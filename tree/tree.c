// clang-format off
#include "bsdcompat.h"

#include <sys/stat.h>

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
// clang-format on

/*
 * tree: recursive directory listing in text or JSON form.
 *
 * The walk is split into two layers.  collect() reads one directory with
 * readdir(3), attaches lstat(2)/stat(2) data (following a symlink to
 * classify its target), applies the -d, -P and -I filters, and sorts the
 * surviving entries.  dirwalk()/jsonentries() then own that array, print
 * or emit it, and recurse into directories.  freeents() releases the
 * entry names and targets; the array itself is consumed by the walk.
 *
 * The text and JSON walkers are separate because their output shape and
 * bookkeeping differ, but they share collect(), the sorting rules, the
 * visited-directory set and the -L/--filelimit/--prune logic.  Visited
 * directories are recorded by (device, inode), never by path, so
 * symlink loops are detected without relying on string comparison.
 *
 * Display depth is tracked with dirbars[]: one slot per ancestor level
 * records whether that ancestor was the last sibling, which selects the
 * vertical line drawn by the text walker.  The array is global and
 * indexed by depth because the walk is strictly depth-first; keeping it
 * out of the per-call frame avoids a large array on every recursion
 * level.
 *
 * The interface language and encoding are fixed: LC_CTYPE is set to
 * en_US.UTF-8 (when available) for multibyte names, all other locale
 * categories stay in "C", and the program degrades to byte-oriented
 * ASCII output with a warning when that locale data is missing.
 *
 * Output errors and unreadable directories set had_error, which makes
 * main() return 2.  A failed command-line option returns 1 through
 * err(3)/errx(3) before the walk starts.
 */

/* display flags */
static int	   aflag;     /* -a: include hidden entries */
static int	   dflag;     /* -d: directories only */
static int	   fflag;     /* -f: print full path */
static int	   Fflag;     /* -F: classify entries */
static int	   iflag;     /* -i: no indentation */
static int	   lflag;     /* -l: follow directory symlinks */
static int	   rflag;     /* -r: reverse sort */
static int	   xflag;     /* -x: stay on filesystem */
static int	   sflag;     /* -s: print sizes */
static int	   hflag;     /* -h: human sizes (1024) */
static int	   siflag;    /* --si: human sizes (1000) */
static int	   pflag;     /* -p: permissions */
static int	   uflag;     /* -u: user */
static int	   gflag;     /* -g: group */
static int	   Dflag;     /* -D: dates */
static int	   cflag;     /* -c: use ctime with -D */
static int	   inoflag;   /* --inodes */
static int	   Nflag;     /* -N: raw non-printable chars */
static int	   Qflag;     /* -Q: quote names */
static int	   qflag;     /* -q: non-printable as '?' */
static int	   Jflag;     /* -J: JSON output */
static int	   Uflag;     /* -U: unsorted */
static int	   pruneflag; /* --prune: prune empty directories */
static int	   dirsfirst; /* --dirsfirst */
static int	   noreport;  /* --noreport */
static int	   filelimit; /* --filelimit */
static int	   maxdepth;  /* -L */
static int	   sortflag;  /* 0 name, 1 mtime, 2 size, 3 ctime */
static const char *timefmt;   /* --timefmt */
static const char *outpath;   /* -o */
static FILE	  *outfile;

static char **patterns; /* -P */
static size_t npatterns;
static char **ipatterns; /* -I */
static size_t nipatterns;

/* counters */
static long nfiles;
static long ndirs;
static int  had_error;

/* (dev, ino) set of every directory already visited in the walk */
struct seen {
	dev_t dev;
	ino_t ino;
};
static struct seen *seen_dirs;
static size_t	    seen_cnt;
static size_t	    seen_alloc;

/*
 * Output encoding capabilities, decided once in main() from the selected
 * locale: use_unicode selects the UTF-8 box-drawing tables, multibyte
 * enables wide-character interpretation and iswprint() checks.
 */
static int use_unicode;
static int multibyte;

/*
 * Sibling state of the directory ancestors, indexed by depth.  The
 * walk is strictly depth-first, so a single array serves every level:
 * a frame only writes its own slot and reads the slots filled in by
 * its ancestors.  Keeping it off the stack avoids exhausting the stack
 * on a pathologically deep tree while preserving the recursion limit.
 */
#define MAX_DEPTH 4096
static int dirbars[MAX_DEPTH];

static const char *u_hier[] = {"\342\224\234\342\224\200\342\224\200 ",
    "\342\224\224\342\224\200\342\224\200 "};
static const char *u_vline[] = {
    "\342\224\202\302\240\302\240 ", "\302\240\302\240\302\240\302\240"};
static const char *a_hier[] = {"|-- ", "`-- "};
static const char *a_vline[] = {"|   ", "    "};

/*
 * HIER/VLINE are indexed by the 0/1 "is the last sibling" flag, so
 * every line-drawing table must have exactly two entries.
 */
static_assert(sizeof(a_hier) / sizeof(a_hier[0]) == 2 &&
	sizeof(a_vline) / sizeof(a_vline[0]) == 2 &&
	sizeof(u_hier) / sizeof(u_hier[0]) == 2 &&
	sizeof(u_vline) / sizeof(u_vline[0]) == 2,
    "the line-drawing tables are indexed by the 0/1 sibling flag");

#define HIER (use_unicode ? u_hier : a_hier)
#define VLINE (use_unicode ? u_vline : a_vline)

/* one directory entry */
struct ent {
	char	   *name;
	char	   *target; /* symlink target or nullptr */
	struct stat lst;    /* lstat(2) */
	struct stat st;	    /* stat(2) of target for symlinks */
	int	    broken; /* symlink whose target is missing */
	int	    isdir;  /* directory or dir symlink */
};

static void report(void);
static void dirwalk(const char *, const char *, int, const int *, dev_t,
    struct ent *, size_t, int);
static void jsonwalk(const char *, const char *, int, dev_t, int);
static void jsonentries(const char *, int, dev_t, int);
[[nodiscard]] static int collect(const char *, struct ent **, size_t *);
static void		 freeents(struct ent *, size_t);
static void entryline(const struct ent *, const char *, int, const int *, int);
static const char *fillinfo(const struct stat *);
static int	   psize(char *, size_t, off_t);
static const char *do_date(time_t);
static const char *prot(mode_t);
static const char *uidtoname(uid_t);
static const char *gidtoname(gid_t);
static const char *jtype(mode_t);
static void	   printname(const char *);
static int	   utf8seqlen(const unsigned char *);
static void	   json_enc(const char *);
static char	   ftype(mode_t);
static void	   closeoutput(void);
static int	   seen(dev_t, ino_t);
static void	   addseen(dev_t, ino_t);
static char	  *joinpath(const char *, const char *);
static char	  *resolvelink(const char *, const char *);

/*
 * Match a name against one pattern.  The whole name is tried first; if
 * it contains path separators, each component after a separator is also
 * tried, so a pattern such as "*.c" matches the final component of a
 * path.  fnmatch(3) is used with flags 0 (no FNM_PATHNAME), so '*' can
 * match '/' as well; this is the historical tree behavior.
 */
static int
patmatch(const char *name, const char *pat)
{
	if (fnmatch(pat, name, 0) == 0)
		return (1);
	for (;;) {
		const char *pc;

		pc = strchr(name, '/');
		if (pc == nullptr || pc[1] == '\0')
			break;
		name = pc + 1;
		if (fnmatch(pat, name, 0) == 0)
			return (1);
	}
	return (0);
}

/*
 * Inclusion is an OR over all -P patterns; an empty pattern list means
 * "include everything" and is handled by the caller.
 */
static int
patinclude(const char *name)
{
	size_t i;

	for (i = 0; i < npatterns; i++)
		if (patmatch(name, patterns[i]))
			return (1);
	return (0);
}

/*
 * Exclusion is an OR over all -I patterns.  Exclusion is applied after
 * inclusion, so -I wins when both match a name; an empty list excludes
 * nothing.
 */
static int
patignore(const char *name)
{
	size_t i;

	for (i = 0; i < nipatterns; i++)
		if (patmatch(name, ipatterns[i]))
			return (1);
	return (0);
}

/*
 * Format a size into buf.  Without -h/--si the exact byte count is
 * printed after a leading space in an 11-character field.  With a human
 * mode, the value is scaled by 1024 (or 1000 for --si), one decimal
 * place is kept while the rounded value is below ten units and none
 * above, and a unit letter is appended to scaled values (plain bytes
 * have no letter).  The field widths keep columns aligned.  LC_NUMERIC
 * is always the "C" locale, so the decimal point is always '.'.
 */
static int
psize(char *buf, size_t bufsize, off_t size)
{
	static const char iec_unit[] = "BKMGTPEZY";
	static const char si_unit[] = "dkMGTPEZY";
	const char	 *unit;
	int		  idx, base;

	unit = siflag ? si_unit : iec_unit;
	base = siflag ? 1000 : 1024;

	if (hflag || siflag) {
		for (idx = size < base ? 0 : 1; size >= base * base;
		    idx++, size /= base)
			;
		if (idx == 0)
			return (snprintf(buf, bufsize, " %4d", (int)size));
		/*
		 * LC_NUMERIC is never set (only LC_CTYPE is, to
		 * en_US.UTF-8), so the "C" locale governs: the decimal
		 * separator is always '.', whatever LANG/LC_* the
		 * user sets.
		 */
		return (snprintf(buf, bufsize,
		    (((size + base / 2) / base) >= 10) ? " %3.0f%c" :
							 " %3.1f%c",
		    (double)size / (double)base, unit[idx]));
	}
	return (snprintf(buf, bufsize, " %11lld", (long long)size));
}

/*
 * Format a timestamp with --timefmt, or with the traditional ls(1)-like
 * form: month, day and time for dates within the last six months, and
 * month, day and year for older or future dates.  The buffer is static
 * and overwritten by the next call, so callers must consume it before
 * formatting another date.  An out-of-range time yields an empty string;
 * with --timefmt, a strftime(3) failure also yields an empty string.
 */
static const char *
do_date(time_t t)
{
	static char buf[256];
	struct tm  *tm;

	tm = localtime(&t);
	if (tm == nullptr)
		return ("");
	if (timefmt != nullptr) {
		if (strftime(buf, sizeof(buf), timefmt, tm) == 0)
			buf[0] = '\0';
	} else {
		time_t c = time(nullptr);

		/*
		 * Compare the difference rather than adding to t: the
		 * addition could overflow a 32-bit time_t for a file
		 * dated in the far future.
		 */
		if (t > c || (c - t) > (time_t)(6 * 31 * 24 * 60 * 60))
			strftime(buf, sizeof(buf), "%b %e  %Y", tm);
		else
			strftime(buf, sizeof(buf), "%b %e %R", tm);
	}
	return (buf);
}

/*
 * Build the ls(1)-style permission string ("drwxr-xr-x") in a static
 * 11-byte buffer.  The file type occupies the first column; the setuid,
 * setgid and sticky bits replace the corresponding execute letters with
 * lowercase or uppercase s/S/t/T, depending on whether that execute bit
 * is also set.
 */
static const char *
prot(mode_t mode)
{
	static char	    buf[11];
	int		    i;
	static const mode_t bits[] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP,
	    S_IWGRP, S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
	static const char   letters[] = "rwxrwxrwx";

	switch (mode & S_IFMT) {
	case S_IFDIR:
		buf[0] = 'd';
		break;
	case S_IFLNK:
		buf[0] = 'l';
		break;
	case S_IFSOCK:
		buf[0] = 's';
		break;
	case S_IFIFO:
		buf[0] = 'p';
		break;
	case S_IFBLK:
		buf[0] = 'b';
		break;
	case S_IFCHR:
		buf[0] = 'c';
		break;
	default:
		buf[0] = '-';
		break;
	}
	for (i = 0; i < 9; i++) {
		if (mode & bits[i])
			buf[i + 1] = letters[i];
		else
			buf[i + 1] = '-';
	}
	/*
	 * The three high bits replace the matching execute position:
	 * lowercase when the execute bit is also set, uppercase when it
	 * is not, as in ls(1).
	 */
	if (mode & S_ISUID)
		buf[3] = (mode & S_IXUSR) ? 's' : 'S';
	if (mode & S_ISGID)
		buf[6] = (mode & S_IXGRP) ? 's' : 'S';
	if (mode & S_ISVTX)
		buf[9] = (mode & S_IXOTH) ? 't' : 'T';
	buf[10] = '\0';
	return (buf);
}

/*
 * Resolve a uid to a user name, or to the decimal uid when no passwd
 * entry exists.  The result is cached in a static buffer: consecutive
 * entries usually share an owner, and getpwuid(3) is comparatively
 * expensive.  The cache makes the return value valid only until the next
 * call, which is sufficient for the one-field-at-a-time formatter.
 */
static const char *
uidtoname(uid_t uid)
{
	static uid_t   cuid = (uid_t)-1;
	static char    cbuf[32];
	struct passwd *pw;

	if (uid != cuid || cbuf[0] == '\0') {
		pw = getpwuid(uid);
		cuid = uid;
		if (pw == nullptr)
			snprintf(cbuf, sizeof(cbuf), "%u", (unsigned)uid);
		else
			strlcpy(cbuf, pw->pw_name, sizeof(cbuf));
	}
	return (cbuf);
}

/*
 * Resolve a gid to a group name, or to the decimal gid when no group
 * entry exists.  As with uidtoname(), the static cache is valid only
 * until the next call.
 */
static const char *
gidtoname(gid_t gid)
{
	static gid_t  cgid = (gid_t)-1;
	static char   cbuf[32];
	struct group *gr;

	if (gid != cgid || cbuf[0] == '\0') {
		gr = getgrgid(gid);
		cgid = gid;
		if (gr == nullptr)
			snprintf(cbuf, sizeof(cbuf), "%u", (unsigned)gid);
		else
			strlcpy(cbuf, gr->gr_name, sizeof(cbuf));
	}
	return (cbuf);
}

/*
 * Append formatted text to a bounded buffer without ever advancing the
 * cursor past the final byte.  snprintf(3) returns the length it would
 * have written, which may exceed the remaining space; accumulating that
 * unchecked would let a later call compute an out-of-range size.
 *
 * C23 has no standard printf-format attribute, so the GCC/Clang one is
 * used to keep the format-string checking enabled for this helper.
 */
static void info_append(char *buf, size_t size, size_t *n, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void
info_append(char *buf, size_t size, size_t *n, const char *fmt, ...)
{
	va_list ap;
	size_t	avail;
	int	written;

	if (*n >= size)
		return;
	avail = size - *n;
	va_start(ap, fmt);
	written = vsnprintf(buf + *n, avail, fmt, ap);
	va_end(ap);
	if (written < 0)
		return;
	if ((size_t)written >= avail)
		*n = size - 1; /* truncated: the buffer is full */
	else
		*n += (size_t)written;
}

/*
 * Assemble the optional metadata prefix for a text entry, in the fixed
 * order inode, permissions, user, group, size, date.  Each field is
 * appended through info_append() so truncation cannot overrun the
 * static 512-byte buffer.  When at least one field was emitted, the
 * leading space is replaced by '[' and a closing ']' is appended; the
 * caller then only has to add the name.
 */
static const char *
fillinfo(const struct stat *st)
{
	static char buf[512];
	char	    nbuf[64];
	size_t	    n = 0;

	buf[0] = '\0';
	if (inoflag)
		info_append(
		    buf, sizeof(buf), &n, " %7lld", (long long)st->st_ino);
	if (pflag)
		info_append(buf, sizeof(buf), &n, " %s", prot(st->st_mode));
	if (uflag)
		info_append(
		    buf, sizeof(buf), &n, " %-8.32s", uidtoname(st->st_uid));
	if (gflag)
		info_append(
		    buf, sizeof(buf), &n, " %-8.32s", gidtoname(st->st_gid));
	if (sflag) {
		psize(nbuf, sizeof(nbuf), st->st_size);
		info_append(buf, sizeof(buf), &n, "%s", nbuf);
	}
	if (Dflag)
		info_append(buf, sizeof(buf), &n, " %s",
		    do_date(cflag ? st->st_ctime : st->st_mtime));
	if (buf[0] == ' ') {
		buf[0] = '[';
		info_append(buf, sizeof(buf), &n, "]");
	}
	return (buf);
}

/*
 * Print a file name with the escaping selected by -N, -Q and -q.
 *
 *   -N  print the name completely raw; -Q still surrounds it in quotes
 *       but nothing inside is escaped.
 *   -Q  surround the name in double quotes and escape '"' and '\\'.
 *   -q  print characters that are not printable as '?' instead of an
 *       octal escape.
 * default: backslash-escape control characters and '\\' with C-style
 *       or three-digit octal escapes, so the output stays unambiguous
 *       and line-oriented.  In the byte-oriented fallback a space is
 *       escaped as "\\ " as well, preserving the historical output.
 *
 * In a multibyte locale the input is decoded as UTF-8 and printable
 * wide characters are emitted unchanged; an invalid sequence falls back
 * to byte-wise handling for that byte.  The option combinations follow
 * the reference tree utility's escaping rules.
 */
static void
printname(const char *s)
{
	if (Nflag) {
		if (Qflag)
			fprintf(outfile, "\"%s\"", s);
		else
			fputs(s, outfile);
		return;
	}
	if (multibyte) {
		wchar_t	    wc;
		size_t	    n;
		mbstate_t   mbs;
		const char *p = s;

		memset(&mbs, 0, sizeof(mbs));
		if (Qflag)
			putc('"', outfile);
		while (*p != '\0') {
			n = mbrtowc(&wc, p, MB_CUR_MAX, &mbs);
			if (n == (size_t)-1 || n == (size_t)-2) {
				int c = (unsigned char)*p++;

				if (isprint(c))
					putc(c, outfile);
				else if (qflag)
					putc('?', outfile);
				else
					fprintf(outfile, "\\%03o", c);
				memset(&mbs, 0, sizeof(mbs));
				continue;
			}
			if (n == 0)
				break;
			p += n;
			if (wc == '\\' || (wc == '"' && Qflag)) {
				putc('\\', outfile);
				putc((int)wc, outfile);
			} else if (Qflag && wc >= 7 && wc <= 13) {
				putc('\\', outfile);
				putc("abtnvfr"[wc - 7], outfile);
			} else if (iswprint((wint_t)wc))
				fprintf(outfile, "%lc", (wint_t)wc);
			else if (qflag)
				putc('?', outfile);
			else
				fprintf(outfile, "\\%03o", (unsigned)wc);
		}
		if (Qflag)
			putc('"', outfile);
		return;
	}
	if (Qflag)
		putc('"', outfile);
	for (; *s != '\0'; s++) {
		int c = (unsigned char)*s;

		if ((c >= 7 && c <= 13) || c == '\\' || (c == '"' && Qflag) ||
		    (c == ' ' && !Qflag)) {
			putc('\\', outfile);
			if (c > 13)
				putc(c, outfile);
			else
				putc("abtnvfr"[c - 7], outfile);
		} else if (isprint(c)) {
			putc(c, outfile);
		} else {
			if (qflag)
				putc('?', outfile);
			else
				fprintf(outfile, "\\%03o", c);
		}
	}
	if (Qflag)
		putc('"', outfile);
}

/*
 * Length of the well-formed UTF-8 sequence at s, or 0 if s does not
 * start one.  No byte past the terminating NUL is examined: every
 * continuation byte is checked before the next one is read.
 */
static int
utf8seqlen(const unsigned char *s)
{
	unsigned char c = s[0], c1, c2, c3;

	if (c < 0x80)
		return (1);
	c1 = s[1];
	if (c >= 0xc2 && c <= 0xdf) {
		if (c1 < 0x80 || c1 > 0xbf)
			return (0);
		return (2);
	}
	if (c >= 0xe0 && c <= 0xef) {
		if (c1 < 0x80 || c1 > 0xbf)
			return (0);
		if (c == 0xe0 && c1 < 0xa0)
			return (0);
		if (c == 0xed && c1 > 0x9f)
			return (0);
		c2 = s[2];
		if (c2 < 0x80 || c2 > 0xbf)
			return (0);
		return (3);
	}
	if (c >= 0xf0 && c <= 0xf4) {
		if (c1 < 0x80 || c1 > 0xbf)
			return (0);
		if (c == 0xf0 && c1 < 0x90)
			return (0);
		if (c == 0xf4 && c1 > 0x8f)
			return (0);
		c2 = s[2];
		if (c2 < 0x80 || c2 > 0xbf)
			return (0);
		c3 = s[3];
		if (c3 < 0x80 || c3 > 0xbf)
			return (0);
		return (4);
	}
	return (0);
}

/*
 * Emit a string as a JSON string body.  Valid UTF-8 passes through
 * unchanged; each byte that does not begin a valid sequence is escaped
 * as \u00xx so that the resulting document is always valid JSON and
 * valid UTF-8.
 */
static void
json_enc(const char *s)
{
	static const char    ctrl[] = "0-------btn-fr";
	const unsigned char *p = (const unsigned char *)s;

	while (*p != '\0') {
		unsigned char c = *p;
		int	      n;

		if (c < 0x80) {
			if (c < 32) {
				/*
				 * ctrl[] only has entries for the control
				 * characters that have a short JSON escape;
				 * every other value in the range must fall
				 * back to the \u00xx form instead of reading
				 * past the table.
				 */
				if ((size_t)c < sizeof(ctrl) - 1 &&
				    ctrl[c] != '-')
					fprintf(outfile, "\\%c", ctrl[c]);
				else
					fprintf(outfile, "\\u%04x", c);
			} else if (c == '"' || c == '\\') {
				fprintf(outfile, "\\%c", c);
			} else {
				putc(c, outfile);
			}
			p++;
			continue;
		}
		n = utf8seqlen(p);
		if (n > 0) {
			fwrite(p, 1, (size_t)n, outfile);
			p += n;
		} else {
			fprintf(outfile, "\\u%04x", c);
			p++;
		}
	}
}

/*
 * Classifier appended by -F: '/' directory, '=' socket, '|' FIFO,
 * '@' symlink, '*' executable regular file.  Returns 0 when no suffix
 * applies.
 */
static char
ftype(mode_t mode)
{
	int m = mode & S_IFMT;

	if (m == S_IFDIR)
		return ('/');
	else if (m == S_IFSOCK)
		return ('=');
	else if (m == S_IFIFO)
		return ('|');
	else if (m == S_IFLNK)
		return ('@');
	else if (m == S_IFREG && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
		return ('*');
	return (0);
}

/* JSON name for a file type; anything not otherwise classified is "file". */
static const char *
jtype(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return ("directory");
	case S_IFLNK:
		return ("link");
	case S_IFSOCK:
		return ("socket");
	case S_IFIFO:
		return ("fifo");
	case S_IFBLK:
		return ("block");
	case S_IFCHR:
		return ("char");
	default:
		return ("file");
	}
}

/*
 * Return 1 if this (device, inode) pair has already been entered during
 * the walk.  The table is a simple linear array that grows geometrically
 * in addseen().  Linear search is deliberate: it avoids a hash function
 * over inode numbers and keeps the structure small for ordinary trees.
 */
static int
seen(dev_t dev, ino_t ino)
{
	size_t i;

	for (i = 0; i < seen_cnt; i++)
		if (seen_dirs[i].dev == dev && seen_dirs[i].ino == ino)
			return (1);
	return (0);
}

/*
 * Record a directory identity in the visited set, growing the array by
 * doubling (starting at 64 entries).  reallocarray(3) failure is fatal:
 * without the set the walk could loop forever on symlinks.
 */
static void
addseen(dev_t dev, ino_t ino)
{
	if (seen_cnt == seen_alloc) {
		seen_alloc = seen_alloc ? seen_alloc * 2 : 64;
		seen_dirs =
		    reallocarray(seen_dirs, seen_alloc, sizeof(*seen_dirs));
		if (seen_dirs == nullptr)
			err(1, "reallocarray");
	}
	seen_dirs[seen_cnt].dev = dev;
	seen_dirs[seen_cnt].ino = ino;
	seen_cnt++;
}

/*
 * qsort(3) comparator for directory entries.  --dirsfirst is a hard
 * partition and is therefore not reversed by -r; within each partition
 * the selected key (name, mtime, size or ctime) decides, with a
 * byte-wise name comparison as the tie-breaker.  Time and size keys put
 * the largest/newest value first, and -r reverses that ordering as
 * well.  Names are compared with strcmp(3), never strcoll(3), so the
 * order cannot change with the locale.
 */
static int
entcmp(const void *va, const void *vb)
{
	const struct ent *a = va, *b = vb;
	int		  r = 0;

	if (dirsfirst && a->isdir != b->isdir)
		return (a->isdir ? -1 : 1);

	switch (sortflag) {
	case 1: /* mtime, newest first */
		if (a->lst.st_mtime != b->lst.st_mtime)
			r = a->lst.st_mtime > b->lst.st_mtime ? -1 : 1;
		break;
	case 2: /* size, largest first */
		if (a->lst.st_size != b->lst.st_size)
			r = a->lst.st_size > b->lst.st_size ? -1 : 1;
		break;
	case 3: /* ctime, newest first */
		if (a->lst.st_ctime != b->lst.st_ctime)
			r = a->lst.st_ctime > b->lst.st_ctime ? -1 : 1;
		break;
	}
	if (r == 0)
		/*
		 * Byte-wise comparison, deliberately not strcoll(3):
		 * the ordering must not depend on locale collation
		 * rules.  For UTF-8 names this is code point order.
		 */
		r = strcmp(a->name, b->name);
	if (rflag)
		r = -r;
	return (r);
}

/*
 * Read and prepare one directory.
 *
 * "." and ".." are always skipped; other dot files only without -a.
 * Each entry gets its full path constructed for lstat(2); a symlink is
 * read with readlink(2) and stat(2) is used once to learn whether it
 * points at a directory (a failed stat marks it broken).  An entry that
 * vanished between readdir(3) and lstat(2)/readlink(2) is skipped
 * silently, which is preferable to aborting the whole listing.
 *
 * Filtering is applied after all metadata is known: -d drops
 * non-directories, -P keeps only matching files (patterns are also
 * tried against a symlink's target), and -I drops matches.  Filtered
 * entries are removed by a stable in-place compaction, preserving the
 * original order for -U.  Surviving entries are sorted with entcmp()
 * unless -U was given.
 *
 * On success the out array holds *nout entries and each entry owns its
 * name and optional target; the caller must release both with
 * freeents().  Returns -1 when opendir(2) fails, in which case the out
 * array is null and *nout is zero.
 */
[[nodiscard]] static int
collect(const char *path, struct ent **out, size_t *nout)
{
	DIR	      *dirp;
	struct dirent *de;
	struct ent    *ents = nullptr;
	size_t	       n = 0, alloc = 0, i;

	*out = nullptr;
	*nout = 0;

	if ((dirp = opendir(path)) == nullptr)
		return (-1);

	while ((de = readdir(dirp)) != nullptr) {
		struct ent *e;
		char	   *join = nullptr;
		size_t	    plen, dlen;

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (!aflag && de->d_name[0] == '.')
			continue;

		plen = strlen(path);
		dlen = strlen(de->d_name);
		join = malloc(plen + dlen + 2);
		if (join == nullptr)
			err(1, "malloc");
		snprintf(join, plen + dlen + 2, "%s%s%s", path,
		    plen != 0 && path[plen - 1] == '/' ? "" : "/", de->d_name);

		if (n == alloc) {
			alloc = alloc ? alloc * 2 : 32;
			ents = reallocarray(ents, alloc, sizeof(*ents));
			if (ents == nullptr)
				err(1, "reallocarray");
		}
		e = &ents[n];
		memset(e, 0, sizeof(*e));
		e->name = strdup(de->d_name);
		if (e->name == nullptr)
			err(1, "strdup");

		if (lstat(join, &e->lst) == -1) {
			/* vanished between readdir(3) and lstat(2) */
			free(e->name);
			free(join);
			continue;
		}
		e->st = e->lst;

		if (S_ISLNK(e->lst.st_mode)) {
			char	lbuf[PATH_MAX + 1];
			ssize_t len;

			len = readlink(join, lbuf, sizeof(lbuf));
			if (len == -1) {
				free(e->name);
				free(join);
				continue;
			}
			/*
			 * readlink(2) truncates silently.  A full buffer
			 * means the target may be incomplete, and following
			 * or displaying a truncated path would be wrong.
			 */
			if ((size_t)len == sizeof(lbuf)) {
				warnx(
				    "%s: symbolic link target too long", join);
				free(e->name);
				free(join);
				continue;
			}
			e->target = malloc((size_t)len + 1);
			if (e->target == nullptr)
				err(1, "malloc");
			memcpy(e->target, lbuf, (size_t)len);
			e->target[(size_t)len] = '\0';
			if (stat(join, &e->st) == -1) {
				e->broken = 1;
				e->st = e->lst;
			} else {
				e->isdir = S_ISDIR(e->st.st_mode);
			}
		} else if (S_ISDIR(e->lst.st_mode)) {
			e->isdir = 1;
		}

		free(join);
		n++;
	}
	closedir(dirp);

	if (n > 0) {
		size_t j;

		for (i = j = 0; i < n; i++) {
			auto e = &ents[i];
			int  drop = 0;

			if (dflag && !e->isdir)
				drop = 1;
			if (!drop && !e->isdir && npatterns > 0 &&
			    !patinclude(e->name) &&
			    !(e->target != nullptr && patinclude(e->target)))
				drop = 1;
			if (!drop && nipatterns > 0 &&
			    (patignore(e->name) ||
				(e->target != nullptr && patignore(e->target))))
				drop = 1;
			if (drop) {
				free(e->name);
				free(e->target);
				continue;
			}
			if (j != i)
				ents[j] = ents[i];
			j++;
		}
		n = j;
	}

	if (n > 0 && !Uflag)
		qsort(ents, n, sizeof(*ents), entcmp);

	*out = ents;
	*nout = n;
	return (0);
}

/*
 * Release every name and symlink target in the array, then the array
 * itself.  Safe with a nullptr array and n == 0.
 */
static void
freeents(struct ent *ents, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		free(ents[i].name);
		free(ents[i].target);
	}
	free(ents);
}

/*
 * Join a directory path and an entry name with exactly one '/'; a root
 * path is handled specially so the result is "/name", not "//name".
 * Returns a newly allocated string and exits on allocation failure.
 */
static char *
joinpath(const char *path, const char *name)
{
	char  *p;
	size_t plen = strlen(path), nlen = strlen(name);

	p = malloc(plen + nlen + 2);
	if (p == nullptr)
		err(1, "malloc");
	if (plen == 1 && path[0] == '/')
		snprintf(p, plen + nlen + 2, "/%s", name);
	else if (plen > 0 && path[plen - 1] == '/')
		snprintf(p, plen + nlen + 2, "%s%s", path, name);
	else
		snprintf(p, plen + nlen + 2, "%s/%s", path, name);
	return (p);
}

/*
 * Turn a symlink target into a path usable by opendir(2)/lstat(2).  An
 * absolute target is returned as a copy.  A relative target is joined
 * to the directory containing the link (path), with root handled as in
 * joinpath().  The result does not canonicalize ".."; the kernel
 * resolves it when the path is opened.
 */
static char *
resolvelink(const char *path, const char *target)
{
	char  *p;
	size_t plen = strlen(path), tlen = strlen(target);

	if (target[0] == '/') {
		p = strdup(target);
		if (p == nullptr)
			err(1, "strdup");
		return (p);
	}
	p = malloc(plen + tlen + 2);
	if (p == nullptr)
		err(1, "malloc");
	if (plen == 1 && path[0] == '/')
		snprintf(p, plen + tlen + 2, "/%s", target);
	else if (plen > 0 && path[plen - 1] == '/')
		snprintf(p, plen + tlen + 2, "%s%s", path, target);
	else
		snprintf(p, plen + tlen + 2, "%s/%s", path, target);
	return (p);
}

/*
 * Print the display line for one entry (without the terminating
 * newline, so that the caller can append a message).
 */
static void
entryline(
    const struct ent *e, const char *path, int depth, const int *bars, int last)
{
	int j;

	if (!iflag) {
		for (j = 0; j < depth; j++)
			fputs(VLINE[bars[j]], outfile);
		fputs(HIER[last], outfile);
	}
	fputs(fillinfo(&e->lst), outfile);
	if (fillinfo(&e->lst)[0] != '\0')
		fputs("  ", outfile);

	if (fflag) {
		if (strcmp(path, "/") == 0)
			fputs("/", outfile);
		else {
			size_t plen = strlen(path);

			printname(path);
			if (plen == 0 || path[plen - 1] != '/')
				fputs("/", outfile);
		}
	}
	printname(e->name);

	if (Fflag && e->target == nullptr) {
		char c = ftype(e->st.st_mode);

		if (c != 0)
			putc(c, outfile);
	}

	if (e->target != nullptr) {
		fputs(" -> ", outfile);
		printname(e->target);
		if (Fflag && !e->broken) {
			char c = ftype(e->st.st_mode);

			if (c != 0)
				putc(c, outfile);
		}
	}
}

/*
 * Recursive text walk.  path is the directory, disp the name to show
 * for it, depth its display depth (0 for the root), bars the sibling
 * state of the ancestors.  ents/n are the directory contents already
 * collected by the caller, open_ok tells whether the opendir(3)
 * succeeded (it failed: entries are invalid).
 */
static void
dirwalk(const char *path, const char *disp, int depth, const int *bars,
    dev_t xdev, struct ent *ents, size_t n, int open_ok)
{
	size_t i;

	if (!open_ok) {
		if (depth == 0) {
			fprintf(outfile, "%s  [error opening dir]\n", disp);
			had_error = 1;
		}
		return;
	}

	if (depth >= MAX_DEPTH)
		errx(1, "directory tree too deep");

	if (depth > 0 && maxdepth > 0 && depth > maxdepth) {
		freeents(ents, n);
		return;
	}

	for (i = 0; i < n; i++) {
		auto	    e = &ents[i];
		int	    last = (i == n - 1);
		int	    d = depth + 1;
		int	    follow = 0;
		char	   *cp = nullptr;
		struct ent *ch = nullptr;
		size_t	    nch = 0;
		int	    opened = 1;

		/* depth filter for files */
		if (!e->isdir && maxdepth > 0 && d > maxdepth)
			continue;

		if (e->isdir) {
			follow = (e->target != nullptr && lflag && !dflag &&
			    !e->broken &&
			    seen(e->st.st_dev, e->st.st_ino) == 0 &&
			    (!xflag || e->st.st_dev == xdev));

			/* symlink shown but not followed */
			if (e->target != nullptr && !follow) {
				if (pruneflag && !dflag && e->isdir &&
				    !e->broken) {
					char *cp2 =
					    resolvelink(path, e->target);
					struct ent *ch2 = nullptr;
					size_t	    nch2 = 0;

					if (collect(cp2, &ch2, &nch2) == 0)
						freeents(ch2, nch2);
					free(cp2);
					if (nch2 == 0)
						continue;
				}
				entryline(e, path, depth, bars, last);
				/*
				 * The visited set is global to the walk, so
				 * this also covers links to directories that
				 * were visited earlier in another branch.
				 */
				if (e->isdir && lflag && !dflag && !e->broken &&
				    seen(e->st.st_dev, e->st.st_ino))
					fputs("  [recursive, not followed]",
					    outfile);
				fputs("\n", outfile);
				ndirs++;
				continue;
			}

			/* show, but do not descend */
			if (maxdepth > 0 && d >= maxdepth) {
				entryline(e, path, depth, bars, last);
				fputs("\n", outfile);
				ndirs++;
				continue;
			}

			/*
			 * -x: the mount point itself is displayed, but its
			 * contents belong to another file system.  Followed
			 * symlinks are handled by the follow test above.
			 */
			if (e->target == nullptr && xflag &&
			    e->lst.st_dev != xdev) {
				entryline(e, path, depth, bars, last);
				fputs("\n", outfile);
				ndirs++;
				continue;
			}

			/* collect children to detect prune/filelimit
			 * conditions and open errors */
			cp = e->target != nullptr ?
			    resolvelink(path, e->target) :
			    joinpath(path, e->name);
			if (collect(cp, &ch, &nch) == -1)
				opened = 0;

			if (filelimit > 0 && opened &&
			    nch > (size_t)filelimit) {
				entryline(e, path, depth, bars, last);
				fprintf(outfile,
				    "  [%d entries exceeds "
				    "filelimit, not opening dir]\n",
				    (int)nch);
				ndirs++;
				freeents(ch, nch);
				free(cp);
				continue;
			}
			if (pruneflag && !dflag && opened && nch == 0) {
				/* nothing printed for pruned dirs */
				freeents(ch, nch);
				free(cp);
				continue;
			}
			entryline(e, path, depth, bars, last);
			if (!opened) {
				fputs("  [error opening dir]\n", outfile);
				had_error = 1;
				ndirs++;
				free(cp);
				continue;
			}
			fputs("\n", outfile);
			ndirs++;
			addseen(e->st.st_dev, e->st.st_ino);
			dirbars[depth] = last;
			dirwalk(cp, e->name, d, dirbars, xdev, ch, nch, 1);
			free(cp);
			continue;
		}

		/* symlink to a non-directory */
		if (e->target != nullptr) {
			entryline(e, path, depth, bars, last);
			fputs("\n", outfile);
			nfiles++;
			continue;
		}

		entryline(e, path, depth, bars, last);
		fputs("\n", outfile);
		nfiles++;
	}
	freeents(ents, n);
}

static void
jindent(int lvl)
{
	int i;

	if (iflag)
		return;
	for (i = 0; i < lvl; i++)
		fputs("    ", outfile);
}

static void
jsep(void)
{
	if (!iflag)
		fputs(",\n", outfile);
	else
		fputs(",", outfile);
}

/*
 * Append the optional stat fields to the current JSON object, each
 * preceded by a comma: inode, mode/prot, user, group, size and time.
 * The fields are only emitted for the corresponding option, so the
 * object remains valid when none are selected.  String values pass
 * through json_enc().
 */
static void
json_info(const struct stat *st)
{
	if (inoflag)
		fprintf(outfile, ",\"inode\":%lld", (long long)st->st_ino);
	if (pflag)
		fprintf(outfile, ",\"mode\":\"%04o\",\"prot\":\"%s\"",
		    (unsigned)(st->st_mode &
			(S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID |
			    S_ISVTX)),
		    prot(st->st_mode));
	if (uflag) {
		fprintf(outfile, ",\"user\":\"");
		json_enc(uidtoname(st->st_uid));
		fprintf(outfile, "\"");
	}
	if (gflag) {
		fprintf(outfile, ",\"group\":\"");
		json_enc(gidtoname(st->st_gid));
		fprintf(outfile, "\"");
	}
	if (sflag)
		fprintf(outfile, ",\"size\":%lld", (long long)st->st_size);
	if (Dflag) {
		fprintf(outfile, ",\"time\":\"");
		json_enc(do_date(cflag ? st->st_ctime : st->st_mtime));
		fprintf(outfile, "\"");
	}
}

/*
 * Emit the JSON members of one directory's contents at indentation
 * level lvl.  The caller has already emitted the opening '[' and is
 * responsible for the closing ']'; this function writes only the
 * separating commas between members, so the first member has no leading
 * comma.
 *
 * Symlinks are always emitted as "link" objects with a "target" string.
 * A link to a directory is expanded recursively only when -l applies,
 * it is not broken, it has not been visited, -x allows it, and the
 * depth limit has not been reached.  Directories on another file
 * system under -x are emitted as empty directory objects, matching the
 * text walker's decision not to descend.
 *
 * The collected entry array is consumed here and freed before return.
 */
static void
jsonentries(const char *path, int depth, dev_t xdev, int lvl)
{
	struct ent *ents;
	size_t	    n, i;
	int	    emitted = 0;

	if (depth >= MAX_DEPTH)
		errx(1, "directory tree too deep");

	if (collect(path, &ents, &n) == -1)
		return;

	/*
	 * --filelimit applies to contents reached through a followed
	 * symbolic link too; jsonwalk() has already handled the
	 * directories it opens itself.
	 */
	if (filelimit > 0 && n > (size_t)filelimit) {
		freeents(ents, n);
		return;
	}

	for (i = 0; i < n; i++) {
		auto e = &ents[i];
		int  d = depth + 1;
		int  follow;

		if (e->target != nullptr) {
			if (emitted)
				jsep();
			jindent(lvl);
			fprintf(outfile, "{\"type\":\"link\",\"name\":\"");
			json_enc(e->name);
			fprintf(outfile, "\",\"target\":\"");
			json_enc(e->target);
			fprintf(outfile, "\"");
			json_info(&e->lst);

			follow = e->isdir && lflag && !dflag && !e->broken &&
			    seen(e->st.st_dev, e->st.st_ino) == 0 &&
			    (!xflag || e->st.st_dev == xdev) &&
			    !(maxdepth > 0 && d >= maxdepth);
			if (follow) {
				char *cp = resolvelink(path, e->target);

				addseen(e->st.st_dev, e->st.st_ino);
				fprintf(outfile, ",\"contents\":[");
				if (!iflag)
					fprintf(outfile, "\n");
				jsonentries(cp, d, xdev, lvl + 1);
				if (!iflag) {
					fprintf(outfile, "\n");
					jindent(lvl);
				}
				fprintf(outfile, "]");
				free(cp);
				ndirs++;
			} else if (e->isdir) {
				ndirs++;
			} else {
				nfiles++;
			}
			fprintf(outfile, "}");
			emitted = 1;
			continue;
		}
		if (e->isdir) {
			char *cp;

			/*
			 * -x: emit the directory object without opening it
			 * when it is on another file system.  Followed
			 * symlinks are filtered by the follow test above.
			 */
			if (xflag && e->lst.st_dev != xdev) {
				if (emitted)
					jsep();
				jindent(lvl);
				fprintf(outfile,
				    "{\"type\":\"directory\",\"name\":\"");
				json_enc(e->name);
				fprintf(outfile, "\"");
				json_info(&e->lst);
				fprintf(outfile, "}");
				ndirs++;
				emitted = 1;
				continue;
			}

			cp = joinpath(path, e->name);

			if (pruneflag && !dflag) {
				struct ent *ch = nullptr;
				size_t	    nch = 0;
				int copen = (collect(cp, &ch, &nch) == 0);

				if (copen)
					freeents(ch, nch);
				/*
				 * Only an empty, readable directory is
				 * pruned; an unreadable one falls through
				 * so that jsonwalk() reports the error.
				 */
				if (copen && nch == 0) {
					free(cp);
					continue;
				}
			}
			if (emitted)
				jsep();
			jindent(lvl);
			addseen(e->st.st_dev, e->st.st_ino);
			jsonwalk(cp, e->name, d, xdev, lvl);
			free(cp);
			emitted = 1;
			continue;
		}
		if (emitted)
			jsep();
		jindent(lvl);
		fprintf(outfile, "{\"type\":\"%s\",\"name\":\"",
		    jtype(e->lst.st_mode));
		json_enc(e->name);
		fprintf(outfile, "\"");
		json_info(&e->lst);
		fprintf(outfile, "}");
		nfiles++;
		emitted = 1;
	}
	freeents(ents, n);
}

/*
 * Emit one JSON directory object at indentation level lvl.  The caller
 * has already positioned the indentation.  disp is the name to show
 * (which may differ from path for a followed symlink).  lstat(2) is
 * repeated here for the object's own metadata; failure is not fatal,
 * the missing fields are simply zeroed.
 *
 * At the -L boundary, or when a directory cannot be opened, is empty,
 * or exceeds --filelimit, the object is closed without a "contents"
 * member.  Only an unreadable directory sets had_error, because a
 * depth/filelimit stop is expected behavior.
 */
static void
jsonwalk(const char *path, const char *disp, int depth, dev_t xdev, int lvl)
{
	struct ent *ents;
	size_t	    n;
	struct stat st;
	int	    opened;

	if (depth >= MAX_DEPTH)
		errx(1, "directory tree too deep");

	fprintf(outfile, "{\"type\":\"directory\",\"name\":\"");
	json_enc(disp);
	fprintf(outfile, "\"");
	if (lstat(path, &st) == -1)
		memset(&st, 0, sizeof(st));
	json_info(&st);

	/*
	 * -L: at the display boundary the directory object is emitted
	 * without opening it, so an unreadable directory at the boundary
	 * is not an error.
	 */
	if (maxdepth > 0 && depth >= maxdepth) {
		fprintf(outfile, "}");
		ndirs++;
		return;
	}

	opened = collect(path, &ents, &n) == 0;
	if (!opened) {
		fprintf(outfile, "}");
		had_error = 1;
		ndirs++;
		return;
	}
	if (n == 0) {
		fprintf(outfile, "}");
		freeents(ents, n);
		ndirs++;
		return;
	}
	if (filelimit > 0 && n > (size_t)filelimit) {
		fprintf(outfile, "}");
		freeents(ents, n);
		ndirs++;
		return;
	}
	fprintf(outfile, ",\"contents\":[");
	if (!iflag)
		fprintf(outfile, "\n");
	jsonentries(path, depth, xdev, lvl + 1);
	if (!iflag) {
		fprintf(outfile, "\n");
		jindent(lvl);
	}
	fprintf(outfile, "]}");
	freeents(ents, n);
	ndirs++;
}

static void
report(void)
{
	if (noreport)
		return;
	fprintf(outfile, "\n%ld director%s", ndirs, ndirs == 1 ? "y" : "ies");
	if (!dflag)
		fprintf(
		    outfile, ", %ld file%s", nfiles, nfiles == 1 ? "" : "s");
	fprintf(outfile, "\n");
}

/*
 * Flush and close the output, recording a write or close error in
 * had_error so that main() exits with status 2.
 */
static void
closeoutput(void)
{
	if (outfile == stdout) {
		if (fflush(outfile) == EOF || ferror(outfile))
			had_error = 1;
		return;
	}
	if (ferror(outfile))
		had_error = 1;
	if (fclose(outfile) == EOF)
		had_error = 1;
}

/*
 * Long-option table entry.  name excludes the leading "--"; hasarg says
 * whether a value is required (given with '=' or as the next argument);
 * val is either the equivalent short option character or a private
 * value in the '1'..'8' range that only setopt() understands.
 */
struct lopt {
	const char *name;
	int	    hasarg;
	int	    val;
};

static const struct lopt longopts[] = {{"all", 0, 'a'}, {"dirsfirst", 0, '1'},
    {"dirs-only", 0, 'd'}, {"filelimit", 1, '2'}, {"fullpath", 0, 'f'},
    {"help", 0, 'h'}, {"inodes", 0, '3'}, {"noindent", 0, 'i'},
    {"noreport", 0, '4'}, {"prune", 0, '5'}, {"si", 0, '6'}, {"sort", 1, '7'},
    {"timefmt", 1, '8'}, {nullptr, 0, 0}};

/*
 * Apply one option.  Short options arrive as their character; the long
 * table maps each long name to a private value ('1'..'8') so that this
 * switch is the single place where an option becomes state.  Options
 * that take an argument check val for nullptr, although the parser
 * already rejects a missing argument.  -C and -n are accepted only for
 * command-line compatibility: tree's output is always monochrome.
 */
static void
setopt(int c, const char *val)
{
	const char *estr;

	switch (c) {
	case 'a':
		aflag = 1;
		break;
	case 'd':
		dflag = 1;
		break;
	case 'f':
		fflag = 1;
		break;
	case 'F':
		Fflag = 1;
		break;
	case 'i':
		iflag = 1;
		break;
	case 'l':
		lflag = 1;
		break;
	case 'r':
		rflag = 1;
		break;
	case 's':
		sflag = 1;
		break;
	case 't':
		sortflag = 1;
		break;
	case 'x':
		xflag = 1;
		break;
	case 'h':
		hflag = 1;
		sflag = 1;
		break;
	case 'p':
		pflag = 1;
		break;
	case 'u':
		uflag = 1;
		break;
	case 'g':
		gflag = 1;
		break;
	case 'D':
		Dflag = 1;
		break;
	case 'c':
		cflag = 1;
		Dflag = 1;
		break;
	case 'C':
	case 'n':
		/* accepted for compatibility; output is always monochrome */
		break;
	case 'N':
		Nflag = 1;
		break;
	case 'Q':
		Qflag = 1;
		break;
	case 'q':
		qflag = 1;
		break;
	case 'J':
		Jflag = 1;
		break;
	case 'U':
		Uflag = 1;
		break;
	case 'L':
		if (val == nullptr)
			errx(1, "missing option argument");
		maxdepth = (int)strtonum(val, 1, INT_MAX, &estr);
		if (estr != nullptr)
			errx(1, "invalid level: %s", val);
		break;
	case 'P':
		if (val == nullptr)
			errx(1, "missing option argument");
		patterns =
		    reallocarray(patterns, npatterns + 1, sizeof(char *));
		if (patterns == nullptr)
			err(1, "reallocarray");
		patterns[npatterns++] = (char *)val;
		break;
	case 'I':
		if (val == nullptr)
			errx(1, "missing option argument");
		ipatterns =
		    reallocarray(ipatterns, nipatterns + 1, sizeof(char *));
		if (ipatterns == nullptr)
			err(1, "reallocarray");
		ipatterns[nipatterns++] = (char *)val;
		break;
	case 'o':
		if (val == nullptr)
			errx(1, "missing option argument");
		outpath = val;
		break;
	case '1':
		dirsfirst = 1;
		break;
	case '2':
		if (val == nullptr)
			errx(1, "missing option argument");
		filelimit = (int)strtonum(val, 1, INT_MAX, &estr);
		if (estr != nullptr)
			errx(1, "invalid file limit: %s", val);
		break;
	case '3':
		inoflag = 1;
		sflag = 1;
		break;
	case '4':
		noreport = 1;
		break;
	case '5':
		pruneflag = 1;
		break;
	case '6':
		siflag = 1;
		sflag = 1;
		break;
	case '7':
		if (val == nullptr)
			errx(1, "missing option argument");
		if (strcmp(val, "name") == 0)
			sortflag = 0;
		else if (strcmp(val, "mtime") == 0)
			sortflag = 1;
		else if (strcmp(val, "size") == 0)
			sortflag = 2;
		else if (strcmp(val, "ctime") == 0)
			sortflag = 3;
		else
			errx(1, "invalid sort type: %s", val);
		break;
	case '8':
		if (val == nullptr)
			errx(1, "missing option argument");
		timefmt = val;
		Dflag = 1;
		break;
	default:
		errx(1, "invalid option: -%c", c);
	}
}

/*
 * Print one entry line (stat fields, name, optional classifier) without
 * a trailing newline, so callers can append a diagnostic.
 */
static void
printentry(const char *name, const struct stat *st)
{
	fputs(fillinfo(st), outfile);
	if (fillinfo(st)[0] != '\0')
		fputs("  ", outfile);
	printname(name);
	if (Fflag) {
		char c = ftype(st->st_mode);

		if (c != 0)
			putc(c, outfile);
	}
}

int
main(int argc, char *argv[])
{
	int		 i, nroots = 0;
	char	       **roots;
	static char	*defroot[] = {".", nullptr};
	static const int zbars[] = {0};

	setprogname(argv[0]);

	/*
	 * The interface language is U.S. English and the only supported
	 * text encoding is UTF-8.  The locale is therefore selected
	 * deliberately, never from the environment: LANG, LANGUAGE and
	 * the LC_* variables cannot change the language, the sorting
	 * order, the decimal separator or the date format.  LC_CTYPE is
	 * set to enable the multibyte interpretation of UTF-8 file
	 * names; every other category stays in the "C" locale.  When
	 * the host lacks the en_US.UTF-8 locale data, the program
	 * degrades deliberately to byte-oriented output with ASCII line
	 * drawing rather than adopting whatever locale the environment
	 * might suggest.
	 */
	if (setlocale(LC_CTYPE, "en_US.UTF-8") == nullptr)
		warnx("en_US.UTF-8 locale unavailable; "
		      "using byte-oriented output");
	outfile = stdout;

	multibyte = MB_CUR_MAX > 1;
	if (nl_langinfo(CODESET) != nullptr &&
	    (strcmp(nl_langinfo(CODESET), "UTF-8") == 0 ||
		strcmp(nl_langinfo(CODESET), "utf8") == 0))
		use_unicode = 1;

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];

		if (strcmp(arg, "--") == 0) {
			i++;
			break;
		}
		if (strncmp(arg, "--", 2) == 0) {
			char  *eq, *val = nullptr;
			int    k, found = 0;
			size_t len;

			eq = strchr(arg, '=');
			if (eq != nullptr) {
				len = (size_t)(eq - arg);
				val = eq + 1;
			} else {
				len = strlen(arg);
			}
			for (k = 0; longopts[k].name != nullptr; k++) {
				if (strncmp(longopts[k].name, arg + 2,
					len - 2) != 0 ||
				    strlen(longopts[k].name) != len - 2)
					continue;
				found = 1;
				if (longopts[k].val == 'h') {
					fprintf(stdout,
					    "usage: tree "
					    "[-acdfFghilnpqrstuxCDJQNU] "
					    "[-L level]\n"
					    "\t[-o file] [-P pattern] "
					    "[-I pattern] [--dirsfirst]\n"
					    "\t[--filelimit n] [--inodes] "
					    "[--noreport] [--prune]\n"
					    "\t[--si] "
					    "[--sort=name|size|mtime|ctime] "
					    "[--timefmt fmt]\n"
					    "\t[--help] [directory ...]\n");
					exit(0);
				}
				if (longopts[k].hasarg && val == nullptr) {
					if (++i >= argc)
						errx(1,
						    "missing argument "
						    "to --%s",
						    longopts[k].name);
					val = argv[i];
				}
				setopt(longopts[k].val, val);
				break;
			}
			if (!found)
				errx(1, "unknown option: %s", arg);
			continue;
		}
		if (arg[0] == '-' && arg[1] != '\0') {
			int    c;
			size_t j;

			for (j = 1; arg[j] != '\0'; j++) {
				c = arg[j];
				switch (c) {
				case 'L':
				case 'P':
				case 'I':
				case 'o': {
					char *val;

					if (arg[j + 1] != '\0')
						val = arg + j + 1;
					else if (++i < argc)
						val = argv[i];
					else
						errx(1,
						    "missing argument "
						    "to -%c",
						    c);
					setopt(c, val);
					j = strlen(arg) - 1;
					break;
				}
				default:
					setopt(c, nullptr);
					break;
				}
			}
			continue;
		}
		break;
	}

	/*
	 * No operand means the current directory.  The vector is
	 * nullptr-terminated, so count it once here; the same count
	 * drives both the text and JSON root loops.
	 */
	if (i >= argc)
		roots = defroot;
	else
		roots = argv + i;
	for (nroots = 0; roots[nroots] != nullptr; nroots++)
		;

	/*
	 * Directory traversal only needs stdio and rpath.  wpath and
	 * cpath are required solely by -o (writing the output file);
	 * getpw is required solely by -u/-g (user/group names).  The
	 * promises are therefore chosen from the parsed options; no
	 * forks, execs or network ever occur.
	 */
	{
		char promises[64];

		snprintf(promises, sizeof(promises), "stdio rpath%s%s",
		    outpath != nullptr ? " wpath cpath" : "",
		    uflag || gflag ? " getpw" : "");
		if (pledge(promises, nullptr) == -1)
			err(1, "pledge");
	}

	if (outpath != nullptr) {
		int fd;

		/*
		 * O_NOFOLLOW: -o must create or truncate a regular file,
		 * never write through a symbolic link planted at outpath.
		 */
		fd = open(
		    outpath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0666);
		if (fd == -1)
			err(1, "%s", outpath);
		outfile = fdopen(fd, "w");
		if (outfile == nullptr) {
			close(fd);
			err(1, "%s", outpath);
		}
	}

	/*
	 * JSON output is a top-level array of root objects followed by an
	 * optional report object.  A root that cannot be lstat(2)ed is
	 * represented as a file object and sets had_error.  As in the
	 * text walk, a symlink root is followed even without -l, because
	 * the command-line operand itself is what the user asked to list;
	 * a link to a non-directory is emitted as a plain file object.
	 */
	if (Jflag) {
		int j, first = 1;

		fputs("[", outfile);
		if (!iflag)
			fputs("\n", outfile);
		for (j = 0; j < nroots; j++) {
			struct stat st;

			if (!first) {
				if (iflag)
					fputs(",", outfile);
				else
					fputs(",\n    ", outfile);
			}
			first = 0;
			if (lstat(roots[j], &st) == -1) {
				fprintf(
				    outfile, "{\"type\":\"file\",\"name\":\"");
				json_enc(roots[j]);
				fprintf(outfile, "\"}");
				nfiles++;
				had_error = 1;
				continue;
			}
			if (S_ISLNK(st.st_mode)) {
				if (stat(roots[j], &st) == -1 ||
				    !S_ISDIR(st.st_mode)) {
					fprintf(outfile,
					    "{\"type\":\"file\","
					    "\"name\":\"");
					json_enc(roots[j]);
					fprintf(outfile, "\"}");
					nfiles++;
					continue;
				}
			} else if (!S_ISDIR(st.st_mode)) {
				fprintf(
				    outfile, "{\"type\":\"file\",\"name\":\"");
				json_enc(roots[j]);
				fprintf(outfile, "\"}");
				nfiles++;
				continue;
			}
			if (!iflag)
				jindent(1);
			addseen(st.st_dev, st.st_ino);
			jsonwalk(roots[j], roots[j], 0, st.st_dev, 1);
		}
		if (!noreport) {
			if (iflag)
				fputs(",", outfile);
			else
				fputs(",\n    ", outfile);
			fprintf(outfile,
			    "{\"type\":\"report\","
			    "\"directories\":%ld",
			    ndirs);
			if (!dflag)
				fprintf(outfile, ",\"files\":%ld", nfiles);
			fprintf(outfile, "}");
		}
		if (!iflag)
			fputs("\n]\n", outfile);
		else
			fputs("]\n", outfile);
		closeoutput();
		return (had_error ? 2 : 0);
	}

	/*
	 * Text mode processes each root in turn.  A non-directory root
	 * is printed on one line and counted as a file.  A directory root
	 * is printed first, then walked; the root's own device is the -x
	 * boundary for the whole walk.  Errors at the root set
	 * had_error but do not stop the remaining roots.
	 */
	for (i = 0; i < nroots; i++) {
		struct stat st;
		struct ent *ents;
		size_t	    n;

		if (lstat(roots[i], &st) == -1) {
			printname(roots[i]);
			fprintf(outfile, "  [error opening dir]\n");
			had_error = 1;
			continue;
		}
		if (S_ISLNK(st.st_mode)) {
			/* a symlink root: follow it like -l */
			if (stat(roots[i], &st) == -1 || !S_ISDIR(st.st_mode)) {
				/* not a directory: display it alone */
				printentry(roots[i], &st);
				fputs("\n", outfile);
				nfiles++;
				continue;
			}
		} else if (!S_ISDIR(st.st_mode)) {
			/* not a directory: display it alone */
			printentry(roots[i], &st);
			fputs("\n", outfile);
			nfiles++;
			continue;
		}

		/* the root line */
		printentry(roots[i], &st);

		if (collect(roots[i], &ents, &n) == -1) {
			fprintf(outfile, "  [error opening dir]\n");
			had_error = 1;
			continue;
		}
		if (filelimit > 0 && n > (size_t)filelimit) {
			fprintf(outfile,
			    "  [%d entries exceeds filelimit, "
			    "not opening dir]\n",
			    (int)n);
			freeents(ents, n);
			ndirs++;
			continue;
		}
		fputs("\n", outfile);
		addseen(st.st_dev, st.st_ino);
		ndirs++;
		dirwalk(roots[i], roots[i], 0, zbars, st.st_dev, ents, n, 1);
	}

	report();

	closeoutput();
	return (had_error ? 2 : 0);
}
