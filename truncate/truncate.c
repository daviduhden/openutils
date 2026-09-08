#include "bsdcompat.h"

#include <sys/stat.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Representable bounds of the signed off_t, derived from its width.
 * The shift is performed in uintmax_t (at least 64 bits), so the
 * derivation is well-defined for off_t of at most 64 bits; the
 * assertion documents exactly that precondition (32-bit off_t is
 * fine) rather than any particular ABI width.
 */
_Static_assert(sizeof(off_t) <= 8, "off_t wider than 64 bits is not supported");
#define OFF_MAX	((off_t)(((uintmax_t)1 << (sizeof(off_t) * CHAR_BIT - 1)) - 1))
#define OFF_MIN	(-OFF_MAX - 1)

enum relmode {
	RM_ABS = 0,	/* absolute size */
	RM_REL,		/* '+': extend by, '-': reduce by */
	RM_MIN,		/* '>': at least */
	RM_MAX,		/* '<': at most */
	RM_RDN,		/* '/': round down to multiple of */
	RM_RUP		/* '%': round up to multiple of */
};

static int		 no_create;
static int		 block_mode;
static int		 got_size;
static enum relmode	 rel_mode = RM_ABS;
static off_t		 rsize = -1;
static off_t		 sizev;
static const char	*ref_file;

static void	usage(void) __dead;
static int	parse_size(const char *, off_t *, enum relmode *);
static int	ckd_mul(off_t *, off_t, off_t);
static int	ckd_add(off_t *, off_t, off_t);

static void
usage(void)
{
	fprintf(stderr,
	    "usage: truncate [-c] [-o] -s [+|-]<|>|/|%%SIZE[unit] file ...\n"
	    "       truncate [-c] [-o] -r rfile file ...\n");
	exit(1);
}

static void
help(void)
{
	printf("Usage: truncate OPTION... FILE...\n"
	    "Shrink or extend the size of each FILE to the specified size\n"
	    "\n"
	    "A FILE argument that does not exist is created.\n"
	    "\n"
	    "If a FILE is larger than the specified size, the extra data "
	    "is lost.\n"
	    "If a FILE is shorter, it is extended and the sparse extended "
	    "part (hole)\n"
	    "reads as zero bytes.\n"
	    "\n"
	    "  -c, --no-create        do not create any files\n"
	    "  -o, --io-blocks        treat SIZE as number of IO blocks "
	    "instead of bytes\n"
	    "  -r, --reference=RFILE  base size on RFILE\n"
	    "  -s, --size=SIZE        set or adjust the file size by SIZE "
	    "bytes\n"
	    "      --help     display this help and exit\n"
	    "\n"
	    "SIZE is an integer and optional unit (example: 10K is "
	    "10*1024).\n"
	    "Units are K, M, G, T, P, E, Z, Y, R, Q (powers of 1024) or\n"
	    "KB, MB, ... (powers of 1000).  Binary prefixes can be used,\n"
	    "too: KiB=K, MiB=M, and so on.\n"
	    "\n"
	    "SIZE may also be prefixed by one of the following modifying\n"
	    "characters:\n"
	    "'+' extend by, '-' reduce by, '<' at most, '>' at least,\n"
	    "'/' round down to multiple of, '%%' round up to multiple "
	    "of.\n");
	exit(0);
}

/* b is always positive in the uses below */
static int
ckd_mul(off_t *r, off_t a, off_t b)
{
	uintmax_t	 mm;
	uintmax_t	 ub = (uintmax_t)b;
	int		 neg = 0;

	if (b == 0) {
		*r = 0;
		return (0);
	}
	if (a < 0) {
		neg = 1;
		mm = (uintmax_t)(-(a + 1)) + 1;
	} else {
		mm = (uintmax_t)a;
	}
	if (mm > ((uintmax_t)OFF_MAX + (neg ? 1 : 0)) / ub)
		return (1);
	*r = (off_t)(mm * ub);
	if (neg)
		*r = -*r;
	return (0);
}

static int
ckd_add(off_t *r, off_t a, off_t b)
{
	if (b > 0 && a > OFF_MAX - b)
		return (1);
	if (b < 0 && a < OFF_MIN - b)
		return (1);
	*r = a + b;
	return (0);
}

/*
 * Parse a SIZE argument.  Returns 0 on success and fills in *val and
 * *relp (the relative mode in effect afterwards); -1 on an invalid
 * number (with errno set to ERANGE on overflow); -2 when a sign is
 * combined with a relative modifier.
 */
static int
parse_size(const char *arg, off_t *val, enum relmode *relp)
{
	const char	*p = arg;
	uintmax_t	 acc = 0;
	int		 sign = 1;
	int		 anydigit = 0;

	while (isspace((unsigned char)*p))
		p++;

	switch (*p) {
	case '<':
		*relp = RM_MAX;
		p++;
		break;
	case '>':
		*relp = RM_MIN;
		p++;
		break;
	case '/':
		*relp = RM_RDN;
		p++;
		break;
	case '%':
		*relp = RM_RUP;
		p++;
		break;
	}
	while (isspace((unsigned char)*p))
		p++;

	if (*p == '+' || *p == '-') {
		if (*relp != RM_ABS)
			return (-2);
		*relp = RM_REL;
		if (*p == '-')
			sign = -1;
		p++;
	}

	while (isdigit((unsigned char)*p)) {
		anydigit = 1;
		if (acc > (UINTMAX_MAX - (uintmax_t)(*p - '0')) / 10) {
			errno = ERANGE;
			return (-1);
		}
		acc = acc * 10 + (uintmax_t)(*p - '0');
		p++;
	}

	if (*p != '\0') {
		int	 base = 1024;
		int	 power = -1;

		switch (*p) {
		case 'E':
			power = 6;
			break;
		case 'g':
		case 'G':
			power = 3;
			break;
		case 'k':
		case 'K':
			power = 1;
			break;
		case 'm':
		case 'M':
			power = 2;
			break;
		case 'P':
			power = 5;
			break;
		case 'Q':
			power = 10;
			break;
		case 'R':
			power = 9;
			break;
		case 't':
		case 'T':
			power = 4;
			break;
		case 'Y':
			power = 8;
			break;
		case 'Z':
			power = 7;
			break;
		default:
			return (-1);
		}
		if (!anydigit && !isdigit((unsigned char)*p))
			acc = 1;
		p++;
		if (*p == 'i' && p[1] == 'B') {
			p += 2;
		} else if (*p == 'B' || *p == 'D') {
			base = 1000;
			p++;
		}
		if (*p != '\0')
			return (-1);
		while (power-- > 0) {
			if (acc > UINTMAX_MAX / (uintmax_t)base) {
				errno = ERANGE;
				return (-1);
			}
			acc *= (uintmax_t)base;
		}
	} else if (!anydigit) {
		return (-1);
	}

	if (sign > 0) {
		if (acc > (uintmax_t)OFF_MAX) {
			errno = ERANGE;
			return (-1);
		}
		*val = (off_t)acc;
	} else {
		if (acc > (uintmax_t)OFF_MAX + 1) {
			errno = ERANGE;
			return (-1);
		}
		*val = -(off_t)acc;
	}
	return (0);
}

static int
do_ftruncate(const char *fname, int fd, off_t ssize)
{
	struct stat	 sb;
	off_t		 nsize, fsize = -1;

	if ((block_mode || (rel_mode != RM_ABS && rsize < 0))) {
		if (fstat(fd, &sb) == -1) {
			warn("cannot fstat '%s'", fname);
			return (1);
		}
	}

	if (block_mode) {
		off_t	bs = sb.st_blksize ? sb.st_blksize : 512;

		if (ckd_mul(&ssize, ssize, bs)) {
			warnx("overflow in %lld * %lld byte blocks for file '%s'",
			    (long long)sizev, (long long)bs, fname);
			return (1);
		}
	}

	if (rel_mode != RM_ABS) {
		if (rsize >= 0) {
			fsize = rsize;
		} else if (sb.st_size >= 0) {
			fsize = sb.st_size;
		} else {
			fsize = lseek(fd, 0, SEEK_END);
			if (fsize < 0) {
				warn("cannot get the size of '%s'", fname);
				return (1);
			}
		}

		switch (rel_mode) {
		case RM_MIN:
			nsize = fsize > ssize ? fsize : ssize;
			break;
		case RM_MAX:
			nsize = fsize < ssize ? fsize : ssize;
			break;
		case RM_RDN:
			nsize = fsize - fsize % ssize;
			break;
		case RM_RUP: {
			off_t	r = fsize % ssize;

			nsize = fsize + (r == 0 ? 0 : ssize - r);
			break;
		}
		default:
			if (ckd_add(&nsize, fsize, ssize)) {
				warnx("overflow extending size of file '%s'",
				    fname);
				return (1);
			}
			break;
		}
	} else {
		nsize = ssize;
	}

	if (nsize < 0)
		nsize = 0;

	if (ftruncate(fd, nsize) == -1) {
		warn("failed to truncate '%s' at %lld bytes", fname,
		    (long long)nsize);
		return (1);
	}
	return (0);
}

int
main(int argc, char *argv[])
{
	struct stat	 sb;
	off_t		 file_size = -1;
	int		 i, error = 0;

	setprogname(argv[0]);

	for (i = 1; i < argc; i++) {
		char	*arg = argv[i];

		if (strcmp(arg, "--") == 0) {
			i++;
			break;
		}
		if (strcmp(arg, "--help") == 0)
			help();
		if (strncmp(arg, "--", 2) == 0) {
			char	*eq, *val = NULL;
			char	 name[32];
			size_t	 len;
			int	 opt = -1;

			eq = strchr(arg, '=');
			if (eq != NULL) {
				len = (size_t)(eq - arg - 2);
				val = eq + 1;
			} else {
				len = strlen(arg) - 2;
			}
			if (len == 0 || len >= sizeof(name))
				usage();
			memcpy(name, arg + 2, len);
			name[len] = '\0';
			if (strcmp(name, "no-create") == 0)
				opt = 'c';
			else if (strcmp(name, "io-blocks") == 0)
				opt = 'o';
			else if (strcmp(name, "reference") == 0)
				opt = 'r';
			else if (strcmp(name, "size") == 0)
				opt = 's';
			else
				usage();
			if ((opt == 'c' || opt == 'o') && eq != NULL)
				usage();
			if (opt == 'r' || opt == 's') {
				if (val == NULL) {
					if (++i >= argc)
						errx(1, "missing argument "
						    "to --%s",
						    arg + 2);
					val = argv[i];
				}
			}
			switch (opt) {
			case 'c':
				no_create = 1;
				break;
			case 'o':
				block_mode = 1;
				break;
			case 'r':
				ref_file = val;
				break;
			case 's': {
				int	r;

				r = parse_size(val, &sizev, &rel_mode);
				if (r == -2) {
					warnx("multiple relative modifiers "
					    "specified");
					usage();
				}
				if (r == -1) {
					if (errno == ERANGE)
						err(1, "Invalid number: '%s'",
						    val);
					errx(1, "Invalid number: '%s'", val);
				}
				if ((rel_mode == RM_RDN ||
				    rel_mode == RM_RUP) && sizev == 0)
					errx(1, "division by zero");
				got_size = 1;
				break;
			}
			}
			continue;
		}
		if (arg[0] == '-' && arg[1] != '\0') {
			int	c;
			size_t	j;

			for (j = 1; arg[j] != '\0'; j++) {
				c = arg[j];
				switch (c) {
				case 'c':
					no_create = 1;
					break;
				case 'o':
					block_mode = 1;
					break;
				case 'r':
				case 's': {
					char	*val;

					if (arg[j + 1] != '\0')
						val = arg + j + 1;
					else if (++i < argc)
						val = argv[i];
					else
						errx(1, "missing argument "
						    "to -%c", c);
					if (c == 'r') {
						ref_file = val;
					} else {
						int	r;

						r = parse_size(val, &sizev,
						    &rel_mode);
						if (r == -2) {
							warnx("multiple "
							    "relative "
							    "modifiers "
							    "specified");
							usage();
						}
						if (r == -1) {
							if (errno == ERANGE)
								err(1,
								    "Invalid "
								    "number: " "'%s'",
								    val);
							errx(1, "Invalid "
							    "number: '%s'",
							    val);
						}
						if ((rel_mode == RM_RDN ||
						    rel_mode == RM_RUP) &&
						    sizev == 0)
							errx(1, "division "
							    "by zero");
						got_size = 1;
					}
					j = strlen(arg) - 1;
					break;
				}
				default:
					usage();
				}
			}
			continue;
		}
		break;
	}

	argv += i;
	argc -= i;

	if (!ref_file && !got_size) {
		warnx("you must specify either --size or --reference");
		usage();
	}
	if (ref_file && got_size && rel_mode == RM_ABS) {
		warnx("you must specify a relative --size with --reference");
		usage();
	}
	if (block_mode && !got_size) {
		warnx("--io-blocks was specified but --size was not");
		usage();
	}
	if (argc < 1) {
		warnx("missing file operand");
		usage();
	}

	/*
	 * The operands are opened with O_WRONLY (wpath) and, unless
	 * -c was given, O_CREAT (cpath).  rpath is needed only for the
	 * -r reference file.  No forks, no execs, no network.
	 */
	{
		char promises[32];

		snprintf(promises, sizeof(promises), "stdio wpath%s%s",
		    no_create ? "" : " cpath", ref_file ? " rpath" : "");
		if (pledge(promises, NULL) == -1)
			err(1, "pledge");
	}

	if (ref_file != NULL) {
		if (stat(ref_file, &sb) != 0)
			err(1, "cannot stat '%s'", ref_file);
		if (sb.st_size >= 0) {
			file_size = sb.st_size;
		} else {
			int	ref_fd;
			off_t	file_end;

			ref_fd = open(ref_file, O_RDONLY | O_NONBLOCK);
			if (ref_fd >= 0) {
				file_end = lseek(ref_fd, 0, SEEK_END);
				close(ref_fd);
				if (file_end >= 0)
					file_size = file_end;
			}
		}
		if (file_size < 0)
			err(1, "cannot get the size of '%s'", ref_file);
		if (!got_size)
			sizev = file_size;
		else
			rsize = file_size;
	}

	for (; argc > 0; argc--, argv++) {
		const char	*fname = argv[0];
		int		 fd, oflags;

		oflags = O_WRONLY | (no_create ? 0 : O_CREAT) | O_NONBLOCK;
		fd = open(fname, oflags, 0666);
		if (fd == -1) {
			if (!(no_create && errno == ENOENT)) {
				warn("cannot open '%s' for writing", fname);
				error = 1;
			}
			continue;
		}
		if (do_ftruncate(fname, fd, sizev) == 0) {
			if (close(fd) != 0) {
				warn("failed to close '%s'", fname);
				error = 1;
			}
		} else {
			close(fd);
			error = 1;
		}
	}

	return (error ? 1 : 0);
}
