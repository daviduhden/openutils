#include <sys/stat.h>
#include <sys/wait.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Test-only hooks, compiled in only with -DDOASEDIT_TEST.  They allow
 * the behavioural test suite (doasedit/tests) to exercise the
 * privileged code paths without real root privileges by faking the
 * user identity seen by the checks.  The production binary contains
 * none of this.
 */
#ifdef DOASEDIT_TEST
static uid_t
our_uid(void)
{
	const char	*s = getenv("DOASEDIT_TEST_UID");

	if (s != NULL && s[0] != '\0')
		return ((uid_t)strtoul(s, NULL, 10));
	return (getuid());
}

static int
force_unreadable(void)
{
	return (getenv("DOASEDIT_TEST_UNREADABLE") != NULL);
}
#else
#define our_uid()		(getuid())
#define force_unreadable()	(0)
#endif

/*
 * Path of the doas(1) binary.  The absolute path is preferred so that
 * a malicious PATH cannot substitute another program for the
 * privileged one; execvp(3) is used as a fallback for unusual setups
 * and for the test suite.
 */
#define DOAS_PATH	"/usr/bin/doas"

/* state used by the signal handler for cleanup */
static char	*cur_tmpfile;
static char	*cur_tmpcopy;
static char	*cur_tmpdir;
static pid_t	 editor_pid = -1;

static void	usage(void) __dead;
static int	doas_exec(const char *const *argv, int outfd, int infd);
static int	run_editor(char *const *editor, const char *file);
static int	files_equal(const char *, const char *);
static int	copy_file(const char *, const char *);
static int	is_doas_conf(const char *);
static void	cleanup_current(void);
static void	sighandler(int);

static void
usage(void)
{
	fprintf(stderr,
	    "usage: doasedit [-h] [--] file ...\n");
	exit(1);
}

static void
help(void)
{
	fprintf(stderr,
	    "doasedit - edit files as root using an unprivileged editor\n"
	    "\n"
	    "usage: doasedit file...\n"
	    "       doasedit -h\n"
	    "\n"
	    "Options:\n"
	    "  -h, --help     display help message and exit\n"
	    "  --             stop processing command line arguments\n");
	exit(0);
}

static void
sighandler(int sig)
{
	if (editor_pid > 0)
		kill(editor_pid, SIGTERM);
	cleanup_current();
	_exit(128 + sig);
}

static void
cleanup_current(void)
{
	if (cur_tmpfile != NULL)
		(void)unlink(cur_tmpfile);
	if (cur_tmpcopy != NULL)
		(void)unlink(cur_tmpcopy);
	if (cur_tmpdir != NULL)
		(void)rmdir(cur_tmpdir);
}

/*
 * Resolve the parent directory of a path with realpath(3) and return
 * "<resolved>/<basename>".  Returns NULL (with a message printed) on
 * failure.
 */
static char *
resolve_target(const char *path)
{
	const char	*base, *dir, *end;
	char		 dirbuf[PATH_MAX];
	char		*rdir, *ret;
	size_t		 dlen, blen;

	if (path[0] == '\0')
		return (NULL);

	end = path + strlen(path);
	while (end > path && end[-1] == '/')
		end--;
	if (end == path)
		return (NULL);		/* "/" or "///" */

	base = end;
	while (base > path && base[-1] != '/')
		base--;
	/* base >= path, so the pointer difference is non-negative */

	if (base == path) {
		/* no directory part */
		dlen = 0;
	} else {
		dlen = (size_t)(base - path);
		if (dlen >= sizeof(dirbuf))
			return (NULL);
		memcpy(dirbuf, path, (size_t)dlen);
		dirbuf[(size_t)dlen] = '\0';
		dir = dirbuf;
	}

	blen = (size_t)(end - base);

	if (dlen == 0)
		rdir = strdup(".");
	else {
		char	 rbuf[PATH_MAX];

		if (realpath(dir, rbuf) == NULL) {
			warn("%s", dirbuf);
			return (NULL);
		}
		rdir = strdup(rbuf);
	}
	if (rdir == NULL) {
		warn("%s", dlen == 0 ? "." : dirbuf);
		return (NULL);
	}
	dlen = strlen(rdir);
	ret = malloc(dlen + blen + 2);
	if (ret == NULL)
		err(1, "malloc");
	snprintf(ret, dlen + blen + 2, "%s%s%s", rdir,
	    (dlen == 1 && rdir[0] == '/') ? "" : "/", base);
	free(rdir);
	return (ret);
}

static int
is_doas_conf(const char *path)
{
	if (strcmp(path, "/etc/doas.conf") == 0)
		return (1);
	if (strncmp(path, "/etc/doas.d/", 12) == 0) {
		size_t	len = strlen(path);

		if (len > 12 && strcmp(path + len - 5, ".conf") == 0)
			return (1);
	}
	return (0);
}

/*
 * Run the editor.  The child process inherits the environment; the
 * editor is started with execvp(3) directly, never through a shell.
 * Returns the editor exit status, or -1 if it could not be started.
 */
static int
run_editor(char *const *editor, const char *file)
{
	pid_t	  pid;
	int	  status;
	char	**argv;
	size_t	  n = 0;

	while (editor[n] != NULL)
		n++;
	argv = reallocarray(NULL, n + 2, sizeof(char *));
	if (argv == NULL)
		err(1, "reallocarray");
	memcpy(argv, editor, n * sizeof(char *));
	argv[n] = (char *)file;
	argv[n + 1] = NULL;

	pid = fork();
	switch (pid) {
	case -1:
		free(argv);
		return (-1);
	case 0:
		execvp(argv[0], argv);
		warn("%s", argv[0]);
		_exit(127);
	default:
		break;
	}
	editor_pid = pid;
	free(argv);
	while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
		;
	editor_pid = -1;
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	return (-1);
}

/*
 * Execute "doas <argv...>", optionally with stdout redirected to
 * outfd and stdin from infd (-1 for no redirection).  Returns the
 * exit status, or 127 when doas could not be executed.
 */
static int
doas_exec(const char *const *argv, int outfd, int infd)
{
	pid_t	pid;
	int	status;

	pid = fork();
	switch (pid) {
	case -1:
		return (-1);
	case 0:
		if (outfd >= 0 && outfd != STDOUT_FILENO) {
			if (dup2(outfd, STDOUT_FILENO) == -1)
				_exit(126);
		}
		if (infd >= 0 && infd != STDIN_FILENO) {
			if (dup2(infd, STDIN_FILENO) == -1)
				_exit(126);
		}
		execv(DOAS_PATH, (char *const *)argv);
		execvp("doas", (char *const *)argv);
		_exit(127);
	default:
		break;
	}
	while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
		;
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	return (-1);
}

static int
copy_file(const char *src, const char *dst)
{
	char	 buf[65536];
	ssize_t	 n, off;
	int	 in, out;

	in = open(src, O_RDONLY | O_NOFOLLOW);
	if (in == -1)
		return (-1);
	out = open(dst, O_WRONLY | O_TRUNC | O_NOFOLLOW);
	if (out == -1) {
		close(in);
		return (-1);
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		off = 0;
		while (off < n) {
			ssize_t	w = write(out, buf + off,
			    (size_t)(n - off));

			if (w == -1) {
				close(in);
				close(out);
				return (-1);
			}
			off += w;
		}
	}
	close(in);
	close(out);
	return (n == -1 ? -1 : 0);
}

static int
files_equal(const char *a, const char *b)
{
	char	 ba[65536], bb[65536];
	ssize_t	 na, nb;
	int	 fa, fb, rc = 1;

	fa = open(a, O_RDONLY | O_NOFOLLOW);
	if (fa == -1)
		return (-1);
	fb = open(b, O_RDONLY | O_NOFOLLOW);
	if (fb == -1) {
		close(fa);
		return (-1);
	}
	for (;;) {
		na = read(fa, ba, sizeof(ba));
		if (na == -1)
			break;
		nb = read(fb, bb, sizeof(bb));
		if (nb == -1)
			break;
		if (na != nb || (na > 0 && memcmp(ba, bb, (size_t)na) != 0)) {
			rc = 0;
			break;
		}
		if (na == 0)
			break;
	}
	close(fa);
	close(fb);
	return (rc);
}

/*
 * Split an editor command from DOAS_EDITOR/VISUAL/EDITOR into words
 * on whitespace.  No shell is involved, so quoting is not interpreted.
 */
static char **
split_editor(const char *cmd)
{
	char	**argv = NULL;
	size_t	  n = 0;

	while (*cmd != '\0') {
		char	*word;
		size_t	 len;

		while (isspace((unsigned char)*cmd))
			cmd++;
		if (*cmd == '\0')
			break;
		len = 0;
		while (cmd[len] != '\0' && !isspace((unsigned char)cmd[len]))
			len++;
		word = strndup(cmd, len);
		if (word == NULL)
			err(1, "strndup");
		argv = reallocarray(argv, n + 2, sizeof(char *));
		if (argv == NULL)
			err(1, "reallocarray");
		argv[n++] = word;
		argv[n] = NULL;
		cmd += len;
	}
	if (argv == NULL) {
		argv = reallocarray(NULL, 1, sizeof(char *));
		if (argv == NULL)
			err(1, "reallocarray");
		argv[0] = NULL;
	}
	return (argv);
}

/* metadata snapshot used to detect replacement of the target file */
struct snapshot {
	dev_t	dev;
	ino_t	ino;
	mode_t	mode;
	uid_t	uid;
	gid_t	gid;
};

static int
snapshot_of(const char *path, struct snapshot *snap)
{
	struct stat	 st;

	if (lstat(path, &st) == -1)
		return (-1);
	snap->dev = st.st_dev;
	snap->ino = st.st_ino;
	snap->mode = st.st_mode;
	snap->uid = st.st_uid;
	snap->gid = st.st_gid;
	return (0);
}

static int
snapshot_equal(const char *path, const struct snapshot *snap)
{
	struct stat	 st;

	if (lstat(path, &st) == -1)
		return (0);
	return (st.st_dev == snap->dev && st.st_ino == snap->ino &&
	    st.st_mode == snap->mode && st.st_uid == snap->uid &&
	    st.st_gid == snap->gid);
}

/* copy the content of a source fd into an already-open file */
static int
fd_copy_to(int in, int out)
{
	char	 buf[65536];
	ssize_t	 n, off;

	while ((n = read(in, buf, sizeof(buf))) > 0) {
		off = 0;
		while (off < n) {
			ssize_t	w = write(out, buf + off,
			    (size_t)(n - off));

			if (w == -1)
				return (-1);
			off += w;
		}
	}
	return (n == -1 ? -1 : 0);
}

/* write the contents of "src" into the open descriptor "outfd" */
static int
file_into_fd(const char *src, int outfd)
{
	int	in, rc;

	in = open(src, O_RDONLY | O_NOFOLLOW);
	if (in == -1)
		return (-1);
	rc = fd_copy_to(in, outfd);
	close(in);
	return (rc);
}

/*
 * Check whether an executable command exists, either as an absolute
 * or relative path, or through the directories listed in PATH.
 */
static int
command_exists(const char *cmd)
{
	const char	*path, *p;
	char		 buf[PATH_MAX];

	if (strchr(cmd, '/') != NULL)
		return (access(cmd, X_OK) == 0);
	path = getenv("PATH");
	if (path == NULL)
		path = "/usr/bin:/bin";
	for (p = path; *p != '\0';) {
		size_t	len;

		while (*p == ':')
			p++;
		if (*p == '\0')
			break;
		len = strcspn(p, ":");
		if (len == 0) {
			/* empty PATH element means the current directory */
			if (access(cmd, X_OK) == 0)
				return (1);
			continue;
		}
		if (len + 1 + strlen(cmd) + 1 < sizeof(buf)) {
			memcpy(buf, p, len);
			buf[len] = '/';
			strlcpy(buf + len + 1, cmd, sizeof(buf) - len - 1);
			if (access(buf, X_OK) == 0)
				return (1);
		}
		p += len;
	}
	return (0);
}

static int
check_doas_conf(const char *target, const char *tmpfile,
    char *const *editor)
{
	const char	*doas_argv[] = { "doas", "-C", tmpfile, NULL };
	char		 line[16];
	int		 status;

	if (!is_doas_conf(target))
		return (0);

	for (;;) {
		status = doas_exec(doas_argv, -1, -1);
		if (status == 0)
			return (0);
		printf("doasedit: Replacing '%s' would introduce the "
		    "above error and break doas.\n", target);
		printf("(E)dit again, (O)verwrite anyway, (A)bort: "
		    "[E/o/a]? ");
		fflush(stdout);
		if (fgets(line, sizeof(line), stdin) == NULL)
			return (1);
		switch (line[0]) {
		case 'o':
		case 'O':
			return (0);
		case 'a':
		case 'A':
			return (1);
		case 'e':
		case 'E':
		default:
			if (run_editor(editor, tmpfile) == -1) {
				warnx("editor could not be started");
				return (1);
			}
			break;
		}
	}
}

static char	**editor_cmd;

int
main(int argc, char *argv[])
{
	const char	*editor_env;
	const char	*env_editor;
	char		*tmpdir = NULL;
	char		 tdir_tmpl[] = "/tmp/doasedit.XXXXXXXXXX";
	const char	*tmpenv;
	int		 i, exit_code = 1;

	setprogname(argv[0]);

	if (pledge("stdio rpath wpath cpath proc exec", NULL) == -1)
		err(1, "pledge");

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0)
			help();
		if (argv[i][0] == '-' && argv[i][1] != '\0') {
			warnx("invalid option: '%s'", argv[i]);
			usage();
		}
		break;
	}

	/* no arguments */
	if (i >= argc) {
		fprintf(stderr,
		    "doasedit - edit files as root using an unprivileged "
		    "editor\n"
		    "\n"
		    "usage: doasedit file...\n"
		    "       doasedit -h\n"
		    "\n"
		    "Options:\n"
		    "  -h, --help     display help message and exit\n"
		    "  --             stop processing command line "
		    "arguments\n");
		exit(1);
	}

	if (getuid() == 0)
		errx(1, "using this program as root is not permitted");

	/*
	 * Check in advance that modifications can actually be saved:
	 * the same probe as the original implementation.
	 */
	{
		const char *probe[] = { "doas", "dd", "status=none",
			"count=0", "of=/dev/null", NULL };

		if (doas_exec(probe, -1, -1) != 0)
			errx(1, "unable to run 'doas dd'");
	}

	/* editor selection: DOAS_EDITOR, VISUAL, EDITOR, then vi(1) */
	editor_env = getenv("DOAS_EDITOR");
	if (editor_env == NULL || editor_env[0] == '\0') {
		env_editor = getenv("VISUAL");
		if (env_editor == NULL || env_editor[0] == '\0')
			editor_env = getenv("EDITOR");
		else
			editor_env = env_editor;
	}
	if (editor_env == NULL || editor_env[0] == '\0')
		editor_env = "vi";
	editor_cmd = split_editor(editor_env);
	if (editor_cmd[0] == NULL)
		errx(1, "no editor specified");
	if (!command_exists(editor_cmd[0]))
		errx(1, "invalid editor command: '%s'", editor_cmd[0]);

	/* private temporary directory */
	tmpenv = getenv("TMPDIR");
	if (tmpenv != NULL && tmpenv[0] != '\0') {
		size_t	len = strlen(tmpenv);

		if (len < sizeof(tdir_tmpl)) {
			memcpy(tdir_tmpl, tmpenv, len);
			tdir_tmpl[len] = '\0';
			if (tdir_tmpl[len - 1] != '/') {
				tdir_tmpl[len] = '/';
				tdir_tmpl[len + 1] = '\0';
			}
			strlcat(tdir_tmpl, "doasedit.XXXXXXXXXX",
			    sizeof(tdir_tmpl));
		}
	}
	tmpdir = mkdtemp(tdir_tmpl);
	if (tmpdir == NULL)
		err(1, "mkdtemp");
	/* mkdtemp(3) returns its argument (a stack buffer here) */
	tmpdir = strdup(tdir_tmpl);
	if (tmpdir == NULL)
		err(1, "strdup");

	cur_tmpdir = tmpdir;
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGHUP, sighandler);
	signal(SIGQUIT, sighandler);

	for (; i < argc; i++) {
		const char	*file = argv[i];
		char		*target = NULL;
		char		*tmpfile = NULL;
		char		*tmpcopy = NULL;
		struct stat	 lst;
		struct snapshot	 snap;
		int		 exists = 0, readable = 0, writable = 0;
		int		 fd, tmpfd, rc;
		const char	*base;

		cur_tmpfile = cur_tmpcopy = NULL;

		if (file[0] == '\0') {
			warnx(": cannot edit directories");
			continue;
		}
		if (file[strlen(file) - 1] == '/') {
			warnx("%s: cannot edit directories", file);
			continue;
		}

		target = resolve_target(file);
		if (target == NULL) {
			warnx("%s: no such directory", file);
			continue;
		}

		base = strrchr(target, '/');
		base = base != NULL ? base + 1 : target;
		if (base[0] == '\0') {
			warnx("%s: cannot edit directories", file);
			free(target);
			continue;
		}

		/* metadata and accessibility checks */
		if (lstat(target, &lst) == -1) {
			if (errno != ENOENT) {
				warn("%s", target);
				free(target);
				continue;
			}
			/* does not exist: check the parent directory */
			{
				char		*dir, *slash;
				struct stat	 dstat;

				dir = strdup(target);
				if (dir == NULL)
					err(1, "strdup");
				slash = strrchr(dir, '/');
				if (slash == dir)
					slash[1] = '\0';
				else if (slash != NULL)
					*slash = '\0';
				if (stat(dir, &dstat) == -1) {
					if (errno == ENOENT ||
					    errno == ENOTDIR) {
						warnx("%s: no such "
						    "directory", dir);
						free(dir);
						free(target);
						continue;
					}
					/* exists but not accessible to
					 * the user: treat as
					 * root-accessible */
					free(dir);
					goto create_root_only;
				}
				if (dstat.st_uid == our_uid()) {
					warnx("%s: creating files in your "
					    "own directory is not "
					    "permitted", file);
					free(dir);
					free(target);
					continue;
				}
				if (access(dir, W_OK) == 0) {
					warnx("%s: creating files in a "
					    "user-writable directory is "
					    "not permitted", file);
					free(dir);
					free(target);
					continue;
				}
				free(dir);
			}
create_root_only:
			exists = 0;
			memset(&snap, 0, sizeof(snap));
		} else {
			if (!S_ISREG(lst.st_mode)) {
				warnx("%s: not a regular file", file);
				free(target);
				continue;
			}
			if (lst.st_uid == our_uid()) {
				warnx("%s: editing your own files is not "
				    "permitted", file);
				free(target);
				continue;
			}
			exists = 1;
			if (snapshot_of(target, &snap) == -1) {
				warn("%s", target);
				free(target);
				continue;
			}

			readable = open(target, O_RDONLY | O_NOFOLLOW);
			if (readable != -1) {
				close(readable);
				readable = 1;
			}
			if (force_unreadable())
				readable = 0;
			writable = (access(target, W_OK) == 0);
			if (readable && writable) {
				warnx("%s: editing user-readable and "
				    "-writable files is not permitted",
				    file);
				free(target);
				continue;
			}
		}

		/* create the private temporary files */
		{
			size_t	 tlen = strlen(tmpdir);
			size_t	 blen = strlen(base);
			char	*cname;

			cname = malloc(blen + 12);
			if (cname == NULL)
				err(1, "malloc");
			snprintf(cname, blen + 12, "copy-of-%s", base);

			tmpfile = malloc(tlen + blen + 2);
			tmpcopy = malloc(tlen + strlen(cname) + 2);
			if (tmpfile == NULL || tmpcopy == NULL)
				err(1, "malloc");
			snprintf(tmpfile, tlen + blen + 2, "%s/%s", tmpdir,
			    base);
			snprintf(tmpcopy, tlen + strlen(cname) + 2, "%s/%s",
			    tmpdir, cname);
			free(cname);

			cur_tmpfile = tmpfile;
			cur_tmpcopy = tmpcopy;
		}

		tmpfd = open(tmpfile, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (tmpfd == -1) {
			warn("%s", tmpfile);
			goto next;
		}
		close(tmpfd);
		tmpfd = open(tmpcopy, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (tmpfd == -1) {
			warn("%s", tmpcopy);
			goto next;
		}
		close(tmpfd);

		/* load the contents */
		if (exists) {
			if (readable) {
				fd = open(target, O_RDONLY | O_NOFOLLOW);
				if (fd == -1) {
					warn("%s", target);
					goto next;
				}
				{
					struct stat	 fst;

					if (fstat(fd, &fst) == -1 ||
					    fst.st_dev != snap.dev ||
					    fst.st_ino != snap.ino) {
						warnx("%s: file changed "
						    "while reading",
						    file);
						close(fd);
						goto next;
					}
				}
				{
					int	out = open(tmpfile,
					    O_WRONLY | O_TRUNC);
					if (out == -1) {
						warn("%s", tmpfile);
						close(fd);
						goto next;
					}
					rc = fd_copy_to(fd, out);
					close(out);
				}
				close(fd);
				if (rc == -1) {
					warn("%s", target);
					goto next;
				}
			} else {
				/*
				 * Not readable by the user: copy with
				 * doas.  Verify afterwards that the
				 * target was not replaced while cat(1)
				 * read it, and retry a few times.
				 */
				const char	*cat_argv[] = {
					"doas", "cat", target, NULL
				};
				int		 tries;

				for (tries = 0; tries < 3; tries++) {
					int	out = open(tmpfile,
					    O_WRONLY | O_TRUNC);
					if (out == -1) {
						warn("%s", tmpfile);
						rc = -1;
						break;
					}
					rc = doas_exec(cat_argv, out, -1);
					close(out);
					if (rc != 0) {
						if (rc == 127)
							warnx("unable to "
							    "run 'doas "
							    "cat'");
						else
							warnx("you are "
							    "not permitted "
							    "to call "
							    "'doas cat'");
						rc = -1;
						break;
					}
					if (snapshot_equal(target, &snap))
						break;
					if (tries < 2)
						continue;
					warnx("%s: file changed while "
					    "reading", file);
					rc = -1;
					break;
				}
				if (rc != 0)
					goto next;
			}
			/* snapshot of the original content for the
			 * "unchanged" check */
			if (copy_file(tmpfile, tmpcopy) == -1) {
				warn("%s", tmpcopy);
				goto next;
			}
		}

		/* run the editor */
		rc = run_editor(editor_cmd, tmpfile);
		if (rc == -1 || rc == 127) {
			warnx("invalid editor command: '%s'",
			    editor_cmd[0]);
			goto next;
		}

		/* doas.conf sanity check */
		if (check_doas_conf(target, tmpfile, editor_cmd) != 0)
			goto next;

		rc = files_equal(tmpfile, tmpcopy);
		if (rc == -1) {
			warn("%s", tmpfile);
			goto next;
		}
		if (rc == 1) {
			printf("doasedit: %s: unchanged\n", file);
			exit_code = 0;
			goto next;
		}

		/* write back */
		if (!exists) {
			const char	*inst_argv[] = {
				"doas", "install", "-m", "0644", tmpfile,
				target, NULL
			};
			int		 tries;

			/* like the original: retry after failed password
			 * attempts, three tries in total */
			for (tries = 0; tries < 3; tries++) {
				if (doas_exec(inst_argv, -1, -1) == 0)
					break;
				if (tries == 2) {
					warnx("unable to save '%s' with "
					    "'doas install'", file);
					goto next;
				}
			}
			exit_code = 0;
			goto next;
		}

		/* existing file: refuse to write if it changed */
		if (!snapshot_equal(target, &snap)) {
			warnx("%s: file changed during editing, not "
			    "saving", file);
			goto next;
		}

		/* direct write path (write-only files the user can
		 * open): write into the existing inode */
		fd = open(target, O_WRONLY | O_NOFOLLOW);
		if (fd != -1) {
			struct stat	 fst;

			if (fstat(fd, &fst) == -1 ||
			    fst.st_dev != snap.dev || fst.st_ino != snap.ino) {
				warnx("%s: file changed during editing, "
				    "not saving", file);
				close(fd);
				goto next;
			}
			if (file_into_fd(tmpfile, fd) == -1) {
				warn("failed to write %s", target);
				close(fd);
				goto next;
			}
			if (ftruncate(fd, lseek(fd, 0, SEEK_CUR)) == -1) {
				warn("failed to truncate %s", target);
				close(fd);
				goto next;
			}
			close(fd);
			exit_code = 0;
			goto next;
		}

		/*
		 * Privileged write: doas install(1) copies the
		 * temporary file into place atomically (mkstemp(3) in
		 * the destination directory plus rename(2)), which is
		 * immune to symlink replacement of the target and
		 * preserves the target's owner, group and mode.
		 */
		{
			const char	*inst_argv[] = {
				"doas", "install", "-o", NULL, "-g", NULL,
				"-m", NULL, tmpfile, target, NULL
			};
			char		 ubuf[32], gbuf[32], mbuf[8];
			int		 tries;

			snprintf(ubuf, sizeof(ubuf), "%u",
			    (unsigned)snap.uid);
			snprintf(gbuf, sizeof(gbuf), "%u",
			    (unsigned)snap.gid);
			snprintf(mbuf, sizeof(mbuf), "%o",
			    (unsigned)(snap.mode & 07777));
			inst_argv[3] = ubuf;
			inst_argv[5] = gbuf;
			inst_argv[7] = mbuf;
			for (tries = 0; tries < 3; tries++) {
				if (doas_exec(inst_argv, -1, -1) == 0)
					break;
				if (tries == 2) {
					warnx("unable to save '%s' with "
					    "'doas install'", file);
					goto next;
				}
			}
			exit_code = 0;
			goto next;
		}
next:
		if (tmpfile != NULL)
			(void)unlink(tmpfile);
		if (tmpcopy != NULL)
			(void)unlink(tmpcopy);
		free(target);
		free(tmpfile);
		free(tmpcopy);
		cur_tmpfile = cur_tmpcopy = NULL;
	}

	cleanup_current();
	free(tmpdir);
	return (exit_code);
}
