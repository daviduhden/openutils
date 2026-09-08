#include <sys/stat.h>

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

/* display flags */
static int	aflag;		/* -a: include hidden entries */
static int	dflag;		/* -d: directories only */
static int	fflag;		/* -f: print full path */
static int	Fflag;		/* -F: classify entries */
static int	iflag;		/* -i: no indentation */
static int	lflag;		/* -l: follow directory symlinks */
static int	rflag;		/* -r: reverse sort */
static int	xflag;		/* -x: stay on filesystem */
static int	sflag;		/* -s: print sizes */
static int	hflag;		/* -h: human sizes (1024) */
static int	siflag;		/* --si: human sizes (1000) */
static int	pflag;		/* -p: permissions */
static int	uflag;		/* -u: user */
static int	gflag;		/* -g: group */
static int	Dflag;		/* -D: dates */
static int	cflag;		/* -c: use ctime with -D */
static int	inoflag;	/* --inodes */
static int	Cflag;		/* -C: colours */
static int	nflag;		/* -n: disable colours */
static int	Nflag;		/* -N: raw non-printable chars */
static int	Qflag;		/* -Q: quote names */
static int	qflag;		/* -q: non-printable as '?' */
static int	Jflag;		/* -J: JSON output */
static int	Uflag;		/* -U: unsorted */
static int	pruneflag;	/* --prune: prune empty directories */
static int	dirsfirst;	/* --dirsfirst */
static int	noreport;	/* --noreport */
static int	filelimit;	/* --filelimit */
static int	maxdepth;	/* -L */
static int	sortflag;	/* 0 name, 1 mtime, 2 size, 3 ctime */
static const char *timefmt;	/* --timefmt */
static const char *outpath;	/* -o */
static FILE	*outfile;

static char	**patterns;	/* -P */
static size_t	npatterns;
static char	**ipatterns;	/* -I */
static size_t	nipatterns;

/* counters */
static long	nfiles;
static long	ndirs;
static int	had_error;

/* (dev, ino) set of directories already descended into */
struct seen {
	dev_t	 dev;
	ino_t	 ino;
};
static struct seen *seen_dirs;
static size_t	 seen_cnt;
static size_t	 seen_alloc;

/* colour codes modelled on Linux tree */
#define C_DIR		"\033[01;34m"
#define C_EXE		"\033[01;32m"
#define C_LNK		"\033[01;36m"
#define C_BADLNK	"\033[40;31;01m"
#define C_BADTGT	"\033[01;37;41m"
#define C_RESET		"\033[0m"

static int	use_color;
static int	use_unicode;
static int	multibyte;

static const char *u_hier[] = {
	"\342\224\234\342\224\200\342\224\200 ",
	"\342\224\224\342\224\200\342\224\200 "
};
static const char *u_vline[] = {
	"\342\224\202\302\240\302\240 ",
	"\302\240\302\240\302\240\302\240"
};
static const char *a_hier[] = { "|-- ", "`-- " };
static const char *a_vline[] = { "|   ", "    " };

#define HIER	(use_unicode ? u_hier : a_hier)
#define VLINE	(use_unicode ? u_vline : a_vline)

/* one directory entry */
struct ent {
	char		*name;
	char		*target;	/* symlink target or NULL */
	struct stat	 lst;		/* lstat(2) */
	struct stat	 st;		/* stat(2) of target for symlinks */
	int		 broken;	/* symlink whose target is missing */
	int		 isdir;		/* directory or dir symlink */
};

static void	report(void);
static void	dirwalk(const char *, const char *, int, const int *, dev_t,
		    struct ent *, size_t, int);
static void	jsonwalk(const char *, const char *, int, dev_t, int);
static void	jsonentries(const char *, int, dev_t, int);
static int	collect(const char *, struct ent **, size_t *);
static void	freeents(struct ent *, size_t);
static void	entryline(const struct ent *, const char *, int,
		    const int *, int);
static const char *fillinfo(const struct stat *);
static int	psize(char *, off_t);
static const char *do_date(time_t);
static const char *prot(mode_t);
static const char *uidtoname(uid_t);
static const char *gidtoname(gid_t);
static const char *jtype(mode_t);
static void	printname(const char *);
static void	json_enc(const char *);
static char	ftype(mode_t);
static int	seen(dev_t, ino_t);
static void	addseen(dev_t, ino_t);
static char	*joinpath(const char *, const char *);
static char	*resolvelink(const char *, const char *);

static int
patmatch(const char *name, const char *pat)
{
	if (fnmatch(pat, name, 0) == 0)
		return (1);
	for (;;) {
		const char *pc;

		pc = strchr(name, '/');
		if (pc == NULL || pc[1] == '\0')
			break;
		name = pc + 1;
		if (fnmatch(pat, name, 0) == 0)
			return (1);
	}
	return (0);
}

static int
patinclude(const char *name)
{
	size_t	i;

	for (i = 0; i < npatterns; i++)
		if (patmatch(name, patterns[i]))
			return (1);
	return (0);
}

static int
patignore(const char *name)
{
	size_t	i;

	for (i = 0; i < nipatterns; i++)
		if (patmatch(name, ipatterns[i]))
			return (1);
	return (0);
}

static int
psize(char *buf, off_t size)
{
	static const char iec_unit[] = "BKMGTPEZY";
	static const char si_unit[] = "dkMGTPEZY";
	const char	*unit;
	int		 idx, base;

	unit = siflag ? si_unit : iec_unit;
	base = siflag ? 1000 : 1024;

	if (hflag || siflag) {
		for (idx = size < base ? 0 : 1; size >= base * base;
		    idx++, size /= base)
			;
		if (idx == 0)
			return (sprintf(buf, " %4d", (int)size));
		return (sprintf(buf, (((size + base / 2) / base) >= 10) ?
		    " %3.0f%c" : " %3.1f%c",
		    (double)size / (double)base, unit[idx]));
	}
	return (sprintf(buf, " %11lld", (long long)size));
}

static const char *
do_date(time_t t)
{
	static char	 buf[256];
	struct tm	*tm;

	tm = localtime(&t);
	if (tm == NULL)
		return ("");
	if (timefmt != NULL) {
		if (strftime(buf, sizeof(buf), timefmt, tm) == 0)
			buf[0] = '\0';
	} else {
		time_t	 c = time(NULL);

		if (t > c || (t + 6 * 31 * 24 * 60 * 60) < c)
			strftime(buf, sizeof(buf), "%b %e  %Y", tm);
		else
			strftime(buf, sizeof(buf), "%b %e %R", tm);
	}
	return (buf);
}

static const char *
prot(mode_t mode)
{
	static char	 buf[11];
	int		 i;
	static const mode_t bits[] = {
		S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP, S_IXGRP,
		S_IROTH, S_IWOTH, S_IXOTH
	};
	static const char letters[] = "rwxrwxrwx";

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
	buf[10] = '\0';
	return (buf);
}

static const char *
uidtoname(uid_t uid)
{
	static uid_t	cuid = (uid_t)-1;
	static char	cbuf[32];
	struct passwd	*pw;

	if (uid != cuid || cbuf[0] == '\0') {
		pw = getpwuid(uid);
		cuid = uid;
		if (pw == NULL)
			snprintf(cbuf, sizeof(cbuf), "%u", (unsigned)uid);
		else
			strlcpy(cbuf, pw->pw_name, sizeof(cbuf));
	}
	return (cbuf);
}

static const char *
gidtoname(gid_t gid)
{
	static gid_t	cgid = (gid_t)-1;
	static char	cbuf[32];
	struct group	*gr;

	if (gid != cgid || cbuf[0] == '\0') {
		gr = getgrgid(gid);
		cgid = gid;
		if (gr == NULL)
			snprintf(cbuf, sizeof(cbuf), "%u", (unsigned)gid);
		else
			strlcpy(cbuf, gr->gr_name, sizeof(cbuf));
	}
	return (cbuf);
}

static const char *
fillinfo(const struct stat *st)
{
	static char	 buf[512];
	char		 nbuf[64];
	size_t		 n = 0;

	buf[0] = '\0';
	if (inoflag)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, " %7lld",
		    (long long)st->st_ino);
	if (pflag)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, " %s", prot(st->st_mode));
	if (uflag)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, " %-8.32s",
		    uidtoname(st->st_uid));
	if (gflag)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, " %-8.32s",
		    gidtoname(st->st_gid));
	if (sflag) {
		psize(nbuf, st->st_size);
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s", nbuf);
	}
	if (Dflag)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, " %s",
		    do_date(cflag ? st->st_ctime : st->st_mtime));
	if (buf[0] == ' ') {
		buf[0] = '[';
		snprintf(buf + n, sizeof(buf) - n, "]");
	}
	return (buf);
}

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
		wchar_t		 wc;
		size_t		 n;
		mbstate_t	 mbs;
		const char	*p = s;

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
			if (iswprint((wint_t)wc))
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

		if ((c >= 7 && c <= 13) || c == '\\' ||
		    (c == '"' && Qflag) || (c == ' ' && !Qflag)) {
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

static void
json_enc(const char *s)
{
	static const char ctrl[] = "0-------btn-fr";

	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		if (c < 32) {
			if (ctrl[c] != '-')
				fprintf(outfile, "\\%c", ctrl[c]);
			else
				fprintf(outfile, "\\u%04x", c);
		} else if (c == '"' || c == '\\') {
			fprintf(outfile, "\\%c", c);
		} else {
			putc(c, outfile);
		}
	}
}

static char
ftype(mode_t mode)
{
	int m = mode & S_IFMT;

	if (!dflag && m == S_IFDIR)
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

static const char *
col_entry(const struct stat *lst, const struct stat *st)
{
	if (!use_color)
		return ("");
	if (st == NULL)
		return (C_BADLNK);
	if (S_ISLNK(lst->st_mode))
		return (C_LNK);
	if (S_ISDIR(st->st_mode))
		return (C_DIR);
	if (S_ISREG(st->st_mode) && (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
		return (C_EXE);
	return ("");
}

static const char *
col_target(const struct stat *st)
{
	if (!use_color)
		return ("");
	if (st == NULL)
		return (C_BADTGT);
	if (S_ISDIR(st->st_mode))
		return (C_DIR);
	if (S_ISREG(st->st_mode) && (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
		return (C_EXE);
	return ("");
}

static int
seen(dev_t dev, ino_t ino)
{
	size_t	i;

	for (i = 0; i < seen_cnt; i++)
		if (seen_dirs[i].dev == dev && seen_dirs[i].ino == ino)
			return (1);
	return (0);
}

static void
addseen(dev_t dev, ino_t ino)
{
	if (seen_cnt == seen_alloc) {
		seen_alloc = seen_alloc ? seen_alloc * 2 : 64;
		seen_dirs = reallocarray(seen_dirs, seen_alloc,
		    sizeof(*seen_dirs));
		if (seen_dirs == NULL)
			err(1, "reallocarray");
	}
	seen_dirs[seen_cnt].dev = dev;
	seen_dirs[seen_cnt].ino = ino;
	seen_cnt++;
}

static int
entcmp(const void *va, const void *vb)
{
	const struct ent	*a = va, *b = vb;
	int		 r = 0;

	if (dirsfirst && a->isdir != b->isdir)
		return (a->isdir ? -1 : 1);

	switch (sortflag) {
	case 1:			/* mtime, newest first */
		if (a->lst.st_mtime != b->lst.st_mtime)
			r = a->lst.st_mtime > b->lst.st_mtime ? -1 : 1;
		break;
	case 2:			/* size, largest first */
		if (a->lst.st_size != b->lst.st_size)
			r = a->lst.st_size > b->lst.st_size ? -1 : 1;
		break;
	case 3:			/* ctime, newest first */
		if (a->lst.st_ctime != b->lst.st_ctime)
			r = a->lst.st_ctime > b->lst.st_ctime ? -1 : 1;
		break;
	}
	if (r == 0)
		r = strcoll(a->name, b->name);
	if (rflag)
		r = -r;
	return (r);
}

static int
collect(const char *path, struct ent **out, size_t *nout)
{
	DIR		*dirp;
	struct dirent	*de;
	struct ent	*ents = NULL;
	size_t		 n = 0, alloc = 0, i;

	*out = NULL;
	*nout = 0;

	if ((dirp = opendir(path)) == NULL)
		return (-1);

	while ((de = readdir(dirp)) != NULL) {
		struct ent	*e;
		char		*join = NULL;
		size_t		 plen, dlen;

		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		if (!aflag && de->d_name[0] == '.')
			continue;

		plen = strlen(path);
		dlen = strlen(de->d_name);
		join = malloc(plen + dlen + 2);
		if (join == NULL)
			err(1, "malloc");
		snprintf(join, plen + dlen + 2, "%s%s%s", path,
		    plen != 0 && path[plen - 1] == '/' ? "" : "/",
		    de->d_name);

		if (n == alloc) {
			alloc = alloc ? alloc * 2 : 32;
			ents = reallocarray(ents, alloc, sizeof(*ents));
			if (ents == NULL)
				err(1, "reallocarray");
		}
		e = &ents[n];
		memset(e, 0, sizeof(*e));
		e->name = strdup(de->d_name);
		if (e->name == NULL)
			err(1, "strdup");

		if (lstat(join, &e->lst) == -1) {
			/* vanished between readdir(3) and lstat(2) */
			free(e->name);
			free(join);
			continue;
		}
		e->st = e->lst;

		if (S_ISLNK(e->lst.st_mode)) {
			char		 lbuf[PATH_MAX + 1];
			ssize_t		 len;

			len = readlink(join, lbuf, sizeof(lbuf));
			if (len == -1) {
				free(e->name);
				free(join);
				continue;
			}
			e->target = malloc((size_t)len + 1);
			if (e->target == NULL)
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
		size_t	j;

		for (i = j = 0; i < n; i++) {
			struct ent	*e = &ents[i];
			int		 drop = 0;

			if (dflag && !e->isdir)
				drop = 1;
			if (!drop && !e->isdir && npatterns > 0 &&
			    !patinclude(e->name) &&
			    !(e->target != NULL && patinclude(e->target)))
				drop = 1;
			if (!drop && nipatterns > 0 &&
			    (patignore(e->name) ||
			    (e->target != NULL && patignore(e->target))))
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

static void
freeents(struct ent *ents, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++) {
		free(ents[i].name);
		free(ents[i].target);
	}
	free(ents);
}

static char *
joinpath(const char *path, const char *name)
{
	char	*p;
	size_t	 plen = strlen(path), nlen = strlen(name);

	p = malloc(plen + nlen + 2);
	if (p == NULL)
		err(1, "malloc");
	if (plen == 1 && path[0] == '/')
		snprintf(p, plen + nlen + 2, "/%s", name);
	else
		snprintf(p, plen + nlen + 2, "%s/%s", path, name);
	return (p);
}

static char *
resolvelink(const char *path, const char *target)
{
	char	*p;
	size_t	 plen = strlen(path), tlen = strlen(target);

	if (target[0] == '/')
		return (strdup(target));
	p = malloc(plen + tlen + 2);
	if (p == NULL)
		err(1, "malloc");
	if (plen == 1 && path[0] == '/')
		snprintf(p, plen + tlen + 2, "/%s", target);
	else
		snprintf(p, plen + tlen + 2, "%s/%s", path, target);
	return (p);
}

/*
 * Print the display line for one entry (without the terminating
 * newline, so that the caller can append a message).
 */
static void
entryline(const struct ent *e, const char *path, int depth,
    const int *bars, int last)
{
	int	j;

	if (!iflag) {
		for (j = 0; j < depth; j++)
			fputs(VLINE[bars[j]], outfile);
		fputs(HIER[last], outfile);
	}
	if (use_color)
		fputs(col_entry(&e->lst, e->broken ? NULL : &e->st), outfile);
	fputs(fillinfo(&e->lst), outfile);
	if (fillinfo(&e->lst)[0] != '\0')
		fputs("  ", outfile);

	if (fflag) {
		if (strcmp(path, "/") == 0)
			fputs("/", outfile);
		else {
			printname(path);
			fputs("/", outfile);
		}
	}
	printname(e->name);

	if (Fflag && e->target == NULL) {
		char c = ftype(e->st.st_mode);

		if (c != 0)
			putc(c, outfile);
	}

	if (e->target != NULL) {
		if (use_color)
			fputs(C_RESET, outfile);
		fputs(" -> ", outfile);
		if (use_color)
			fputs(col_target(e->broken ? NULL : &e->st), outfile);
		printname(e->target);
		if (Fflag && !e->broken) {
			char c = ftype(e->st.st_mode);

			if (c != 0)
				putc(c, outfile);
		}
	}
	if (use_color)
		fputs(C_RESET, outfile);
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
	size_t	i;
	int	subbars[4096];

	if (!open_ok) {
		if (depth == 0) {
			fprintf(outfile, "%s  [error opening dir]\n", disp);
			had_error = 1;
		}
		return;
	}

	if (depth >= (int)(sizeof(subbars) / sizeof(subbars[0])))
		errx(1, "directory tree too deep");

	if (depth > 0 && maxdepth > 0 && depth > maxdepth) {
		freeents(ents, n);
		return;
	}

	for (i = 0; i < n; i++) {
		struct ent	*e = &ents[i];
		int		 last = (i == n - 1);
		int		 d = depth + 1;
		int		 follow = 0;
		char		*cp = NULL;
		struct ent	*ch = NULL;
		size_t		 nch = 0;
		int		 opened = 1;

		/* depth filter for files */
		if (!e->isdir && maxdepth > 0 && d > maxdepth)
			continue;

		if (e->isdir) {
			follow = (e->target != NULL && lflag && !dflag &&
			    !e->broken && seen(e->st.st_dev, e->st.st_ino)
			    == 0 && (!xflag || e->st.st_dev == xdev));

			/* symlink shown but not followed */
			if (e->target != NULL && !follow) {
				if (pruneflag && !dflag && e->isdir &&
				    !e->broken) {
					char	*cp2 = resolvelink(path,
					    e->target);
					struct ent *ch2 = NULL;
					size_t	 nch2 = 0;

					if (collect(cp2, &ch2, &nch2) == 0)
						freeents(ch2, nch2);
					free(cp2);
					if (nch2 == 0)
						continue;
				}
				entryline(e, path, depth, bars, last);
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

			/* collect children to detect prune/filelimit
			 * conditions and open errors */
			cp = e->target != NULL ?
			    resolvelink(path, e->target) :
			    joinpath(path, e->name);
			if (collect(cp, &ch, &nch) == -1)
				opened = 0;

			if (filelimit > 0 && opened &&
			    (int)nch > filelimit) {
				entryline(e, path, depth, bars, last);
				fprintf(outfile, "  [%d entries exceeds "
				    "filelimit, not opening dir]\n",
				    (int)nch);
				ndirs++;
				freeents(ch, nch);
				free(cp);
				continue;
			}
			if (pruneflag && !dflag && nch == 0) {
				/* nothing printed for pruned dirs */
				freeents(ch, nch);
				free(cp);
				continue;
			}
			entryline(e, path, depth, bars, last);
			if (!opened) {
				fputs("  [error opening dir]\n", outfile);
				ndirs++;
				free(cp);
				continue;
			}
			fputs("\n", outfile);
			ndirs++;
			addseen(e->st.st_dev, e->st.st_ino);
			memcpy(subbars, bars, (size_t)depth * sizeof(int));
			subbars[depth] = last;
			dirwalk(cp, e->name, d, subbars, xdev, ch, nch, 1);
			free(cp);
			continue;
		}

		/* symlink to a non-directory */
		if (e->target != NULL) {
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
	int	i;

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

/* JSON fields for one entry's stat information */
static void
json_info(const struct stat *st)
{
	if (inoflag)
		fprintf(outfile, ",\"inode\":%lld", (long long)st->st_ino);
	if (pflag)
		fprintf(outfile, ",\"mode\":\"%04o\",\"prot\":\"%s\"",
		    (unsigned)(st->st_mode & (S_IRWXU | S_IRWXG | S_IRWXO |
		    S_ISUID | S_ISGID | S_ISVTX)), prot(st->st_mode));
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
	if (Dflag)
		fprintf(outfile, ",\"time\":\"%s\"",
		    do_date(cflag ? st->st_ctime : st->st_mtime));
}

/*
 * JSON entries of one directory, at indentation level lvl.
 */
static void
jsonentries(const char *path, int depth, dev_t xdev, int lvl)
{
	struct ent	*ents;
	size_t		 n, i;

	if (collect(path, &ents, &n) == -1)
		return;

	for (i = 0; i < n; i++) {
		struct ent	*e = &ents[i];
		int		 d = depth + 1;
		int		 follow;

		jindent(lvl);

		if (e->target != NULL) {
			fprintf(outfile, "{\"type\":\"link\",\"name\":\"");
			json_enc(e->name);
			fprintf(outfile, "\",\"target\":\"");
			json_enc(e->target);
			fprintf(outfile, "\"");
			json_info(&e->lst);

			follow = e->isdir && lflag && !dflag && !e->broken &&
			    seen(e->st.st_dev, e->st.st_ino) == 0 &&
			    (!xflag || e->st.st_dev == xdev) &&
			    !(maxdepth > 0 && d > maxdepth);
			if (follow) {
				char	*cp = resolvelink(path, e->target);

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
			if (i < n - 1)
				jsep();
			continue;
		}
		if (e->isdir) {
			char	*cp = joinpath(path, e->name);

			if (pruneflag && !dflag) {
				struct ent *ch = NULL;
				size_t	 nch = 0;

				if (collect(cp, &ch, &nch) == 0)
					freeents(ch, nch);
				if (nch == 0) {
					free(cp);
					continue;
				}
			}
			addseen(e->st.st_dev, e->st.st_ino);
			jsonwalk(cp, e->name, d, xdev, lvl);
			free(cp);
			if (i < n - 1)
				jsep();
			continue;
		}
		fprintf(outfile, "{\"type\":\"%s\",\"name\":\"",
		    jtype(e->lst.st_mode));
		json_enc(e->name);
		fprintf(outfile, "\"");
		json_info(&e->lst);
		fprintf(outfile, "}");
		nfiles++;
		if (i < n - 1)
			jsep();
	}
	freeents(ents, n);
}

/*
 * JSON directory object at indentation level lvl.  The caller has
 * already positioned the indentation.
 */
static void
jsonwalk(const char *path, const char *disp, int depth, dev_t xdev, int lvl)
{
	struct ent	*ents;
	size_t		 n;
	struct stat	 st;
	int		 opened;

	fprintf(outfile, "{\"type\":\"directory\",\"name\":\"");
	json_enc(disp);
	fprintf(outfile, "\"");
	if (lstat(path, &st) == -1)
		memset(&st, 0, sizeof(st));
	json_info(&st);

	opened = collect(path, &ents, &n) == 0;
	if (!opened || n == 0) {
		fprintf(outfile, "}");
		if (!opened)
			had_error = 1;
		if (opened)
			freeents(ents, n);
		ndirs++;
		return;
	}
	if (maxdepth > 0 && depth >= maxdepth) {
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
	fprintf(outfile, "\n%ld director%s", ndirs,
	    ndirs == 1 ? "y" : "ies");
	if (!dflag)
		fprintf(outfile, ", %ld file%s", nfiles,
		    nfiles == 1 ? "" : "s");
	fprintf(outfile, "\n");
}

struct lopt {
	const char	*name;
	int		 hasarg;
	int		 val;
};

static const struct lopt longopts[] = {
	{ "all",	0,	'a' },
	{ "dirsfirst",	0,	'1' },
	{ "dirs-only",	0,	'd' },
	{ "filelimit",	1,	'2' },
	{ "fullpath",	0,	'f' },
	{ "help",	0,	'h' },
	{ "inodes",	0,	'3' },
	{ "noindent",	0,	'i' },
	{ "noreport",	0,	'4' },
	{ "prune",	0,	'5' },
	{ "si",		0,	'6' },
	{ "sort",	1,	'7' },
	{ "timefmt",	1,	'8' },
	{ NULL,		0,	0 }
};

static void
setopt(int c, const char *val)
{
	const char	*estr;

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
		Cflag = 1;
		break;
	case 'n':
		nflag = 1;
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
		if (val == NULL)
			errx(1, "missing option argument");
		maxdepth = (int)strtonum(val, 1, INT_MAX, &estr);
		if (estr != NULL)
			errx(1, "invalid level: %s", val);
		break;
	case 'P':
		if (val == NULL)
			errx(1, "missing option argument");
		patterns = reallocarray(patterns, npatterns + 1,
		    sizeof(char *));
		if (patterns == NULL)
			err(1, "reallocarray");
		patterns[npatterns++] = (char *)val;
		break;
	case 'I':
		if (val == NULL)
			errx(1, "missing option argument");
		ipatterns = reallocarray(ipatterns, nipatterns + 1,
		    sizeof(char *));
		if (ipatterns == NULL)
			err(1, "reallocarray");
		ipatterns[nipatterns++] = (char *)val;
		break;
	case 'o':
		if (val == NULL)
			errx(1, "missing option argument");
		outpath = val;
		break;
	case '1':
		dirsfirst = 1;
		break;
	case '2':
		if (val == NULL)
			errx(1, "missing option argument");
		filelimit = (int)strtonum(val, 0, INT_MAX, &estr);
		if (estr != NULL)
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
		if (val == NULL)
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
		if (val == NULL)
			errx(1, "missing option argument");
		timefmt = val;
		break;
	default:
		errx(1, "invalid option: -%c", c);
	}
}

int
main(int argc, char *argv[])
{
	int	i, nroots = 0;
	char	**roots;
	static char *defroot[] = { ".", NULL };
	static const int zbars[] = { 0 };


	setlocale(LC_CTYPE, "");
	setprogname(argv[0]);
	setlocale(LC_COLLATE, "");
	outfile = stdout;

	multibyte = MB_CUR_MAX > 1;
	if (nl_langinfo(CODESET) != NULL &&
	    (strcmp(nl_langinfo(CODESET), "UTF-8") == 0 ||
	    strcmp(nl_langinfo(CODESET), "utf8") == 0))
		use_unicode = 1;

	for (i = 1; i < argc; i++) {
		char	*arg = argv[i];

		if (strcmp(arg, "--") == 0) {
			i++;
			break;
		}
		if (strncmp(arg, "--", 2) == 0) {
			char	*eq, *val = NULL;
			int	 k, found = 0;
			size_t	 len;

			eq = strchr(arg, '=');
			if (eq != NULL) {
				len = (size_t)(eq - arg);
				val = eq + 1;
			} else {
				len = strlen(arg);
			}
			for (k = 0; longopts[k].name != NULL; k++) {
				if (strncmp(longopts[k].name, arg + 2,
				    len - 2) != 0 ||
				    strlen(longopts[k].name) !=
				    len - 2)
					continue;
				found = 1;
				if (longopts[k].val == 'h') {
					fprintf(stdout,
					    "usage: tree "
					    "[-adfFgilnpqrstuxACDJQNU] "
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
				if (longopts[k].hasarg && val == NULL) {
					if (++i >= argc)
						errx(1, "missing argument "
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
			int	c;
			size_t	j;

			for (j = 1; arg[j] != '\0'; j++) {
				c = arg[j];
			switch (c) {
			case 'L':
			case 'P':
			case 'I':
			case 'o': {
					char	*val;

					if (arg[j + 1] != '\0')
						val = arg + j + 1;
					else if (++i < argc)
						val = argv[i];
					else
						errx(1, "missing argument "
						    "to -%c", c);
					setopt(c, val);
					j = strlen(arg) - 1;
					break;
				}
				default:
					setopt(c, NULL);
					break;
				}
			}
			continue;
		}
		break;
	}

	if (i >= argc)
		roots = defroot;
	else
		roots = argv + i;
	for (nroots = 0; roots[nroots] != NULL; nroots++)
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
		    outpath != NULL ? " wpath cpath" : "",
		    uflag || gflag ? " getpw" : "");
		if (pledge(promises, NULL) == -1)
			err(1, "pledge");
	}

	if (outpath != NULL) {
		outfile = fopen(outpath, "w");
		if (outfile == NULL)
			err(1, "%s", outpath);
	}

	use_color = Cflag || (isatty(fileno(outfile)) && !nflag);

	if (Jflag) {
		int	j, first = 1;

		fputs("[", outfile);
		if (!iflag)
			fputs("\n", outfile);
		for (j = 0; j < nroots; j++) {
			struct stat	 st;

			if (!first) {
				if (iflag)
					fputs(",", outfile);
				else
					fputs(",\n    ", outfile);
			}
			first = 0;
			if (lstat(roots[j], &st) == -1) {
				fprintf(outfile,
				    "{\"type\":\"file\",\"name\":\"");
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
					had_error = 1;
					continue;
				}
			} else if (!S_ISDIR(st.st_mode)) {
				fprintf(outfile,
				    "{\"type\":\"file\",\"name\":\"");
				json_enc(roots[j]);
				fprintf(outfile, "\"}");
				nfiles++;
				had_error = 1;
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
			fprintf(outfile, "{\"type\":\"report\","
			    "\"directories\":%ld", ndirs);
			if (!dflag)
				fprintf(outfile, ",\"files\":%ld", nfiles);
			fprintf(outfile, "}");
		}
		if (!iflag)
			fputs("\n]\n", outfile);
		else
			fputs("]\n", outfile);
		if (outfile != stdout)
			fclose(outfile);
		return (had_error ? 2 : 0);
	}

	for (i = 0; i < nroots; i++) {
		struct stat	 st;
		struct ent	*ents;
		size_t		 n;

		if (lstat(roots[i], &st) == -1) {
			printname(roots[i]);
			fprintf(outfile, "  [error opening dir]\n");
			had_error = 1;
			continue;
		}
		if (S_ISLNK(st.st_mode)) {
			/* a symlink root: follow it like -l */
			if (stat(roots[i], &st) == -1 || !S_ISDIR(st.st_mode)) {
				printname(roots[i]);
				fprintf(outfile,
				    "  [error opening dir]\n");
				nfiles++;
				had_error = 1;
				continue;
			}
		} else if (!S_ISDIR(st.st_mode)) {
			printname(roots[i]);
			fprintf(outfile, "  [error opening dir]\n");
			nfiles++;
			had_error = 1;
			continue;
		}

		/* the root line */
		fputs(fillinfo(&st), outfile);
		if (fillinfo(&st)[0] != '\0')
			fputs("  ", outfile);
		if (use_color)
			fputs(C_DIR, outfile);
		printname(roots[i]);
		if (Fflag) {
			char c = ftype(st.st_mode);

			if (c != 0)
				putc(c, outfile);
		}
		if (use_color)
			fputs(C_RESET, outfile);

		if (collect(roots[i], &ents, &n) == -1) {
			fprintf(outfile, "  [error opening dir]\n");
			had_error = 1;
			continue;
		}
		if (filelimit > 0 && (int)n > filelimit) {
			fprintf(outfile, "  [%d entries exceeds filelimit, "
			    "not opening dir]\n", (int)n);
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

	if (outfile != stdout)
		fclose(outfile);
	return (had_error ? 2 : 0);
}
