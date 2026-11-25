// clang-format off
#include "bsdcompat.h"

#include <sys/stat.h>
#include <sys/wait.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// clang-format on

/*
 * doasedit: edit a file with an unprivileged editor and save the result
 * with doas(1).
 *
 * The editor works on a private copy. File descriptors and metadata
 * snapshots provide checks at the read and publication boundaries:
 *
 *   1. Resolve the target's parent directory with realpath(3), keeping
 *      the final component separate so validation can reject symlinks.
 *   2. An lstat(2) snapshot records device, inode, mode, owner, group,
 *      size and modification time for an existing target. The local
 *      copy checks its open descriptor, and the privileged copy checks
 *      the named target after the read.
 *   3. A private mkdtemp(3) directory holds the editable copy and an
 *      untouched copy used to detect "no changes".  Both files are
 *      created with O_EXCL and mode 0600.
 *   4. The editor runs as the invoking user with execvp(3) and a separate
 *      argument vector. The editor command is split on whitespace only.
 *   5. A changed buffer is written back either through the already-open
 *      descriptor (for a user-writable file, preserving the inode) or
 *      with "doas install", which replaces the target atomically by
 *      rename(2) for a stable regular destination. The privileged
 *      operation still relies on trusted paths during authentication
 *      and publication.
 *
 * The doas.conf check runs before the final save: replacing a
 * configuration file that doas itself would reject is a lockout risk,
 * so the user must confirm it explicitly.  The editor and doas must be
 * executed, so the process stays unpledged.
 *
 * The termination handler records a signal. Normal control flow handles
 * child processes and removes temporary files.
 */

/*
 * Prefer the OpenBSD base doas(1). If that exec fails, the fallback
 * searches the invoking user's PATH.
 */
#define DOAS_PATH "/usr/bin/doas"

/* Cancellation requested by the termination handler. */
static volatile sig_atomic_t interrupted;
static sigset_t		     editor_wait_signals;

[[noreturn]] static void usage(void);
[[nodiscard]] static int doas_exec(
    const char *const *argv, int outfd, int infd);
[[nodiscard]] static int run_editor(char *const *editor, const char *file);
[[nodiscard]] static int files_equal(const char *, const char *);
[[nodiscard]] static int copy_file(const char *, const char *);
static int		 is_doas_conf(const char *);
static void		 sighandler(int);

[[noreturn]] static void
usage(void)
{
	fprintf(stderr, "usage: doasedit [-h] [--] file ...\n");
	exit(1);
}

[[noreturn]] static void
help(void)
{
	fprintf(stderr,
	    "doasedit - edit files as root using an unprivileged editor\n"
	    "\n"
	    "usage: doasedit [-h] [--] file ...\n"
	    "\n"
	    "Options:\n"
	    "  -h, --help     display help message and exit\n"
	    "  --             stop processing command line arguments\n");
	exit(0);
}

/* Cleanup happens in the main control flow, after children have exited. */
static void
sighandler(int sig)
{
	interrupted = sig;
}

/* Wake the editor's wait loop when its child changes state. */
static void
child_handler(int sig)
{
	(void)sig;
}

/*
 * Resolve the parent directory of a path with realpath(3) and return
 * "<resolved>/<basename>".  Returns nullptr (with a message printed) on
 * failure.
 *
 * Only the directory part is canonicalized: resolving the final
 * component would follow a symlink, which is exactly what the rest of
 * the program must not do.  Trailing slashes are dropped for the same
 * reason (a trailing slash asks the kernel to follow a symlink), so
 * "name/" is treated as "name".  Empty paths and paths consisting only
 * of slashes are rejected.  The returned string is allocated with
 * malloc(3) and owned by the caller.
 */
static char *
resolve_target(const char *path)
{
	const char *base, *dir, *end;
	char	    dirbuf[PATH_MAX];
	char	   *rdir, *ret;
	size_t	    dlen, blen;

	if (path[0] == '\0')
		return (nullptr);

	end = path + strlen(path);
	while (end > path && end[-1] == '/')
		end--;
	if (end == path)
		return (nullptr); /* "/" or "///" */

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
			return (nullptr);
		memcpy(dirbuf, path, (size_t)dlen);
		dirbuf[(size_t)dlen] = '\0';
		dir = dirbuf;
	}

	blen = (size_t)(end - base);

	{
		char rbuf[PATH_MAX];

		if (realpath(dlen == 0 ? "." : dir, rbuf) == nullptr) {
			warn("%s", dlen == 0 ? "." : dir);
			return (nullptr);
		}
		rdir = strdup(rbuf);
	}
	if (rdir == nullptr) {
		warn("%s", dlen == 0 ? "." : dirbuf);
		return (nullptr);
	}
	dlen = strlen(rdir);
	ret = malloc(dlen + blen + 2);
	if (ret == nullptr)
		err(1, "malloc");
	snprintf(ret, dlen + blen + 2, "%s%s%s", rdir,
	    (dlen == 1 && rdir[0] == '/') ? "" : "/", base);
	free(rdir);
	return (ret);
}

/*
 * Return 1 for the two paths whose replacement can affect doas(1):
 * /etc/doas.conf itself and any path below /etc/doas.d/ whose final
 * component ends in ".conf".  The test is textual by design; the caller
 * already canonicalized the parent directory, so an alternate spelling
 * of the same path has already been folded into this form.  Nested
 * paths are matched conservatively for implementations that support
 * configuration fragments. OpenBSD base loads /etc/doas.conf.
 */
static int
is_doas_conf(const char *path)
{
	if (strcmp(path, "/etc/doas.conf") == 0)
		return (1);
	if (strncmp(path, "/etc/doas.d/", 12) == 0) {
		auto len = strlen(path);

		if (len > 12 && strcmp(path + len - 5, ".conf") == 0)
			return (1);
	}
	return (0);
}

/*
 * Run the editor.  The child process inherits the environment; the
 * editor is started with execvp(3) and a separate argument vector.
 * Returns the editor exit status, or -1 if it could not be started or
 * was terminated by a signal.  A "command not found" in the child is
 * mapped to status 127.
 *
 * The argument vector is rebuilt on the heap so the file operand can be
 * appended to the user-supplied command; it is freed by the parent after
 * the fork (the child replaces its image or exits).
 */
static int
run_editor(char *const *editor, const char *file)
{
	pid_t	 pid, result;
	sigset_t previous;
	sigset_t waiting;
	int	 status;
	char   **argv;
	size_t	 n = 0;

	while (editor[n] != nullptr)
		n++;
	argv = reallocarray(nullptr, n + 2, sizeof(char *));
	if (argv == nullptr)
		err(1, "reallocarray");
	memcpy(argv, editor, n * sizeof(char *));
	argv[n] = (char *)file;
	argv[n + 1] = nullptr;

	if (sigprocmask(SIG_BLOCK, &editor_wait_signals, &previous) == -1) {
		free(argv);
		return (-1);
	}
	if (interrupted) {
		(void)sigprocmask(SIG_SETMASK, &previous, nullptr);
		free(argv);
		return (-1);
	}
	/*
	 * Keep SIGCHLD and cancellation blocked across fork and the
	 * waitpid(2) check. sigsuspend(2) then changes the mask and sleeps
	 * atomically, so an exit between the check and sleep wakes us.
	 */
	pid = fork();
	if (pid == 0) {
		(void)sigprocmask(SIG_SETMASK, &previous, nullptr);
		execvp(argv[0], argv);
		warn("%s", argv[0]);
		_exit(127);
	}
	free(argv);
	result = -1;
	if (pid > 0) {
		waiting = previous;
		sigdelset(&waiting, SIGCHLD);
		for (;;) {
			if (interrupted)
				(void)kill(pid, SIGTERM);
			result = waitpid(pid, &status, WNOHANG);
			if (result == -1 && errno == EINTR)
				continue;
			if (result != 0)
				break;
			(void)sigsuspend(&waiting);
		}
	}
	(void)sigprocmask(SIG_SETMASK, &previous, nullptr);
	if (result == pid && pid > 0 && !interrupted && WIFEXITED(status))
		return (WEXITSTATUS(status));
	return (-1);
}

/*
 * Execute "doas <argv...>", optionally with stdout redirected to
 * outfd and stdin from infd (-1 for no redirection).  Returns the
 * exit status, or -1 on fork/wait failure.  A child that cannot start
 * doas exits 127; a child that fails to set up the redirection exits
 * 126, mirroring the shell convention.
 *
 * /usr/bin/doas is tried first; the PATH lookup is only a fallback for
 * systems that install doas elsewhere.  The caller supplies argv[0]
 * ("doas") separately from the path used for execution.
 */
static int
doas_exec(const char *const *argv, int outfd, int infd)
{
	pid_t pid;
	int   status;

	if (interrupted)
		return (-1);
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
	while (waitpid(pid, &status, 0) == -1) {
		if (errno != EINTR)
			return (-1);
	}
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	return (-1);
}

[[nodiscard]] static int fd_copy_to(int, int);

/*
 * Open an editor-side regular file and transfer its fd to the caller.
 * O_NONBLOCK lets the type check reject a FIFO without waiting for a
 * writer. On failure, preserve the open or validation error.
 */
static int
open_regular(const char *path)
{
	struct stat st;
	int	    fd, saved_errno;

	fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd == -1)
		return (-1);
	if (fstat(fd, &st) == 0) {
		if (S_ISREG(st.st_mode))
			return (fd);
		errno = EINVAL;
	}
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return (-1);
}

/* Fill a block or stop at EOF. Independent short reads can then be compared. */
static ssize_t
read_block(int fd, char *buf, size_t length)
{
	size_t offset = 0;

	while (offset < length) {
		ssize_t n = read(fd, buf + offset, length - offset);

		if (n == 0)
			break;
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		offset += (size_t)n;
	}
	return ((ssize_t)offset);
}

/*
 * Copy src over the existing file dst.  Both opens use O_NOFOLLOW, and
 * dst is truncated before writing.  The loop handles short write(2)
 * results and returns 0 only after the whole source was read and
 * written; any open, read or write error returns -1.  The copy is not
 * synchronized to disk: the caller only needs an identical temporary
 * snapshot for its change comparison.
 */
static int
copy_file(const char *src, const char *dst)
{
	int in, out, rc, saved_errno;

	in = open_regular(src);
	if (in == -1)
		return (-1);
	out = open(dst, O_WRONLY | O_TRUNC | O_NOFOLLOW);
	rc = out == -1 ? -1 : fd_copy_to(in, out);
	saved_errno = errno;
	if (out != -1 && close(out) == -1 && rc == 0) {
		rc = -1;
		saved_errno = errno;
	}
	close(in);
	errno = saved_errno;
	return (rc);
}

/*
 * Compare two files without following symlinks.  Reads both in fixed
 * blocks and stops at the first difference.  Returns 1 when the contents
 * are identical, 0 when they differ, and -1 on an open/read error.
 * Both descriptors are closed on every path.
 */
static int
files_equal(const char *a, const char *b)
{
	char	ba[65536], bb[65536];
	ssize_t na, nb;
	int	fa, fb, rc = 1;

	fa = open_regular(a);
	if (fa == -1)
		return (-1);
	fb = open_regular(b);
	if (fb == -1) {
		close(fa);
		return (-1);
	}
	for (;;) {
		na = read_block(fa, ba, sizeof(ba));
		if (na == -1) {
			rc = -1;
			break;
		}
		nb = read_block(fb, bb, sizeof(bb));
		if (nb == -1) {
			rc = -1;
			break;
		}
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
	char **argv = nullptr;
	size_t n = 0;

	while (*cmd != '\0') {
		char  *word;
		size_t len;

		while (isspace((unsigned char)*cmd))
			cmd++;
		if (*cmd == '\0')
			break;
		len = 0;
		while (cmd[len] != '\0' && !isspace((unsigned char)cmd[len]))
			len++;
		word = strndup(cmd, len);
		if (word == nullptr)
			err(1, "strndup");
		argv = reallocarray(argv, n + 2, sizeof(char *));
		if (argv == nullptr)
			err(1, "reallocarray");
		argv[n++] = word;
		argv[n] = nullptr;
		cmd += len;
	}
	if (argv == nullptr) {
		argv = reallocarray(nullptr, 1, sizeof(char *));
		if (argv == nullptr)
			err(1, "reallocarray");
		argv[0] = nullptr;
	}
	return (argv);
}

/*
 * Metadata snapshot used to detect replacement of the target file.
 * size and mtime catch a replacement that reuses the inode, which FFS
 * does for a file removed and recreated in the same directory.
 */
struct snapshot {
	dev_t		dev;
	ino_t		ino;
	mode_t		mode;
	uid_t		uid;
	gid_t		gid;
	off_t		size;
	struct timespec mtim;
};

/*
 * Fill *snap from lstat(2), which does not follow a symlink at path.
 * Directories, devices, FIFOs and sockets are rejected with EINVAL so
 * that the rest of the program only ever deals with regular files.
 * Returns 0 on success and -1 on failure, leaving errno from the failing
 * system call (or EINVAL for a non-regular file).
 */
static int
snapshot_of(const char *path, struct snapshot *snap)
{
	struct stat st;

	if (lstat(path, &st) == -1)
		return (-1);
	if (!S_ISREG(st.st_mode)) {
		errno = EINVAL;
		return (-1);
	}
	snap->dev = st.st_dev;
	snap->ino = st.st_ino;
	snap->mode = st.st_mode;
	snap->uid = st.st_uid;
	snap->gid = st.st_gid;
	snap->size = st.st_size;
	snap->mtim = st.st_mtim;
	return (0);
}

/*
 * Compare every field recorded by snapshot_of().  Device and inode
 * can identify a replacement. Mode, owner and group reflect permission
 * and ownership changes. Size and nanosecond mtime can reflect content
 * changes without an inode change.
 */
static int
stat_matches_snapshot(const struct stat *st, const struct snapshot *snap)
{
	return (st->st_dev == snap->dev && st->st_ino == snap->ino &&
	    st->st_mode == snap->mode && st->st_uid == snap->uid &&
	    st->st_gid == snap->gid && st->st_size == snap->size &&
	    st->st_mtim.tv_sec == snap->mtim.tv_sec &&
	    st->st_mtim.tv_nsec == snap->mtim.tv_nsec);
}

/*
 * Re-lstat(2) path and compare it with snap.  Returns 1 when the path is
 * still the same regular file with unchanged metadata, 0 otherwise
 * (including a missing path).
 */
static int
snapshot_equal(const char *path, const struct snapshot *snap)
{
	struct stat st;

	if (lstat(path, &st) == -1)
		return (0);
	return (stat_matches_snapshot(&st, snap));
}

/*
 * Copy the content of an already-open source descriptor into an
 * already-open destination, starting at the current offsets.  Short
 * writes are retried.  Returns 0 on success and -1 on a read or write
 * error; neither descriptor is closed and the caller decides whether a
 * partial copy is acceptable.  On a write error the destination may
 * already contain part of the input, and this function does not roll
 * that back.
 */
[[nodiscard]] static int
fd_copy_to(int in, int out)
{
	char	buf[65536];
	ssize_t n, off;

	while ((n = read_block(in, buf, sizeof(buf))) > 0) {
		off = 0;
		while (off < n) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));

			if (w == -1 && errno == EINTR)
				continue;
			if (w <= 0) {
				if (w == 0)
					errno = EIO;
				return (-1);
			}
			off += w;
		}
	}
	return (n == -1 ? -1 : 0);
}

/*
 * Open src read-only without following symlinks and copy all of it into
 * the already-open outfd.  Returns 0 on success and -1 on any open or
 * copy error.  This is the direct-write path, so outfd is left at its
 * final offset for the caller's subsequent ftruncate(2).
 */
[[nodiscard]] static int
file_into_fd(const char *src, int outfd)
{
	int in, rc;

	in = open_regular(src);
	if (in == -1)
		return (-1);
	rc = fd_copy_to(in, outfd);
	close(in);
	return (rc);
}

/*
 * Check whether an executable command exists, either as an absolute
 * or relative path, or through the directories listed in PATH.
 *
 * A name containing '/' is checked directly with access(2).  Otherwise
 * each PATH element is tried in order; an empty element means the
 * current directory, as in the shell.  Candidates that would not fit in
 * the bounded buffer are skipped rather than truncated.  This is a
 * pre-flight check for a useful error message, not a security boundary:
 * the editor is executed later with execvp(3), which repeats the search.
 */
static int
command_exists(const char *cmd)
{
	const char *path, *p;
	char	    buf[PATH_MAX];

	if (strchr(cmd, '/') != nullptr)
		return (access(cmd, X_OK) == 0);
	path = getenv("PATH");
	if (path == nullptr)
		path = "/usr/bin:/bin";
	for (p = path;;) {
		const char *end = strchr(p, ':');
		size_t len = end != nullptr ? (size_t)(end - p) : strlen(p);

		if (len == 0) {
			/* empty PATH element means the current directory */
			if (access(cmd, X_OK) == 0)
				return (1);
		} else if (len + 1 + strlen(cmd) + 1 < sizeof(buf)) {
			memcpy(buf, p, len);
			buf[len] = '/';
			strlcpy(buf + len + 1, cmd, sizeof(buf) - len - 1);
			if (access(buf, X_OK) == 0)
				return (1);
		}
		if (end == nullptr)
			break;
		p = end + 1;
	}
	return (0);
}

/*
 * Run "doas -C <tmpfile>" when the target is a doas configuration file.
 * doas -C parses the named configuration and exits nonzero if it is
 * invalid, so this exercises the edited file before it can replace a
 * working configuration. The check covers syntax only. On failure the
 * user may edit again, overwrite anyway, or abort. Returns 0 to
 * continue and 1 to abort this file. EOF or a read error on stdin
 * aborts the file as well.
 */
static int
check_doas_conf(const char *target, const char *tmpfile, char *const *editor)
{
	const char *doas_argv[] = {"doas", "-C", tmpfile, nullptr};
	char	    line[16];
	int	    status, rc;

	if (!is_doas_conf(target))
		return (0);

	for (;;) {
		status = doas_exec(doas_argv, -1, -1);
		if (interrupted)
			return (1);
		if (status == 0)
			return (0);
		printf("doasedit: Could not validate '%s' "
		       "with doas -C.\n",
		    target);
		printf("(E)dit again, (O)verwrite anyway, (A)bort: "
		       "[E/o/a]? ");
		fflush(stdout);
		if (fgets(line, sizeof(line), stdin) == nullptr)
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
			rc = run_editor(editor, tmpfile);
			if (rc != 0) {
				warnx("editor failed (status %d)", rc);
				return (1);
			}
			break;
		}
	}
}

static char **editor_cmd;

/*
 * Program entry point.
 *
 * Startup validates the invocation, refuses to run as root, probes that
 * "doas dd" is permitted, resolves the editor command and creates the
 * private temporary directory.  The argument loop then processes each
 * operand in this order: resolve, validate, snapshot, copy in, edit,
 * compare, write back. Ordinary operand failures let the loop continue.
 * The aggregate status is 0 after any successful save or unchanged
 * comparison. Cancellation returns 128 plus the signal number, and
 * retaining a failed edit stops processing with status 1.
 * Each operand has its own subdirectory under the session directory.
 */
int
main(int argc, char *argv[])
{
	const char *editor_env;
	const char *env_editor;
	char	   *tmpdir = nullptr;
	char	   *tdir_tmpl;
	const char *tmpenv;
	int	    i, exit_code = 1;

	setprogname(argv[0]);

	/* Stay unpledged: editor and doas execute after this point. */

	/*
	 * Options are recognized only before the first operand; a later
	 * "-h" is therefore a file name.  "--" ends option processing
	 * explicitly so that a file name can begin with '-'.  An empty
	 * or bare "-" argument is an operand, not an option.
	 */
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
		    "usage: doasedit [-h] [--] file ...\n"
		    "\n"
		    "Options:\n"
		    "  -h, --help     display help message and exit\n"
		    "  --             stop processing command line "
		    "arguments\n");
		exit(1);
	}

	/*
	 * Both the real and the effective identity are checked: the
	 * program must run as the unprivileged user whose editor will be
	 * started.  Running it through doas would make every identity
	 * check below compare against root.
	 */
	if (getuid() == 0 || geteuid() == 0)
		errx(1, "using this program as root is not permitted");

	/*
	 * Retain the original zero-byte doas dd probe. Authorization is
	 * decided again for each actual cat, install or configuration
	 * check because each invocation has its own arguments.
	 */
	{
		const char *probe[] = {"doas", "dd", "status=none", "count=0",
		    "of=/dev/null", nullptr};

		if (doas_exec(probe, -1, -1) != 0)
			errx(1, "unable to run 'doas dd'");
	}

	/* editor selection: DOAS_EDITOR, VISUAL, EDITOR, then vi(1) */
	editor_env = getenv("DOAS_EDITOR");
	if (editor_env == nullptr || editor_env[0] == '\0') {
		env_editor = getenv("VISUAL");
		if (env_editor == nullptr || env_editor[0] == '\0')
			editor_env = getenv("EDITOR");
		else
			editor_env = env_editor;
	}
	if (editor_env == nullptr || editor_env[0] == '\0')
		editor_env = "vi";
	editor_cmd = split_editor(editor_env);
	if (editor_cmd[0] == nullptr)
		errx(1, "no editor specified");
	if (!command_exists(editor_cmd[0]))
		errx(1, "invalid editor command: '%s'", editor_cmd[0]);

	/* private temporary directory */
	tmpenv = getenv("TMPDIR");
	if (tmpenv == nullptr || tmpenv[0] == '\0')
		tmpenv = "/tmp";
	{
		static const char suffix[] = "doasedit.XXXXXXXXXX";
		size_t		  dlen = strlen(tmpenv), need;

		/* directory, separating '/', suffix and terminating NUL */
		if (ckd_add(&need, dlen, sizeof(suffix)) ||
		    ckd_add(&need, need, 1))
			errx(1, "TMPDIR too long");
		tdir_tmpl = malloc(need);
		if (tdir_tmpl == nullptr)
			err(1, "malloc");
		memcpy(tdir_tmpl, tmpenv, dlen);
		if (tdir_tmpl[dlen - 1] != '/')
			tdir_tmpl[dlen++] = '/';
		memcpy(tdir_tmpl + dlen, suffix, sizeof(suffix));
	}
	/*
	 * mkdtemp(3) creates the session directory with mode 0700.
	 * TMPDIR supplies its parent, which must remain trusted because
	 * privileged publication reopens the edited copy by pathname.
	 */
	tmpdir = mkdtemp(tdir_tmpl);
	if (tmpdir == nullptr)
		err(1, "mkdtemp");

	{
		static const int termination[] = {
		    SIGINT, SIGTERM, SIGHUP, SIGQUIT};
		struct sigaction action = {.sa_handler = sighandler};

		sigemptyset(&editor_wait_signals);
		sigemptyset(&action.sa_mask);
		for (size_t n = 0;
		    n < sizeof(termination) / sizeof(termination[0]); n++) {
			sigaddset(&editor_wait_signals, termination[n]);
			if (sigaction(termination[n], &action, nullptr) == -1) {
				(void)rmdir(tmpdir);
				err(1, "sigaction");
			}
		}
		sigaddset(&editor_wait_signals, SIGCHLD);
		action.sa_handler = child_handler;
		action.sa_flags = SA_NOCLDSTOP;
		if (sigaction(SIGCHLD, &action, nullptr) == -1) {
			(void)rmdir(tmpdir);
			err(1, "sigaction");
		}
	}

	/*
	 * Process each operand independently.  target is the canonical
	 * parent plus the unchanged final component; tmpfile is the copy
	 * handed to the editor and tmpcopy the pristine copy used to
	 * detect whether the editor changed anything.
	 */
	for (; i < argc && !interrupted; i++) {
		const char     *file = argv[i];
		char	       *target = nullptr;
		char	       *tmpfile = nullptr;
		char	       *tmpcopy = nullptr;
		char	       *editdir = nullptr;
		struct stat	lst;
		struct snapshot snap;
		int		exists = 0, readable = 0, writable = 0;
		int		fd, tmpfd, rc;
		int		keep_edit = 0;
		const char     *base;

		if (file[0] == '\0') {
			warnx(": cannot edit directories");
			continue;
		}
		if (file[strlen(file) - 1] == '/') {
			warnx("%s: cannot edit directories", file);
			continue;
		}

		target = resolve_target(file);
		if (target == nullptr) {
			warnx("%s: no such directory", file);
			continue;
		}

		base = strrchr(target, '/');
		base = base != nullptr ? base + 1 : target;
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
				char	   *dir, *slash;
				struct stat dstat;

				dir = strdup(target);
				if (dir == nullptr)
					err(1, "strdup");
				slash = strrchr(dir, '/');
				if (slash == dir)
					slash[1] = '\0';
				else if (slash != nullptr)
					*slash = '\0';
				if (stat(dir, &dstat) == -1) {
					if (errno == ENOENT ||
					    errno == ENOTDIR) {
						warnx("%s: no such "
						      "directory",
						    dir);
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
				if (dstat.st_uid == getuid()) {
					warnx("%s: creating files in your "
					      "own directory is not "
					      "permitted",
					    file);
					free(dir);
					free(target);
					continue;
				}
				if (access(dir, W_OK) == 0) {
					warnx("%s: creating files in a "
					      "user-writable directory is "
					      "not permitted",
					    file);
					free(dir);
					free(target);
					continue;
				}
				free(dir);
			}
		/*
		 * The target did not exist and its parent directory is not
		 * user-writable (or could not be examined by the
		 * unprivileged user).  Saving therefore takes the
		 * privileged create path, so there is no identity snapshot
		 * yet.  That path rechecks that the target is still absent
		 * immediately before install(1).
		 */
		create_root_only:
			exists = 0;
			memset(&snap, 0, sizeof(snap));
		} else {
			if (!S_ISREG(lst.st_mode)) {
				warnx("%s: not a regular file", file);
				free(target);
				continue;
			}
			if (lst.st_uid == getuid()) {
				warnx("%s: editing your own files is not "
				      "permitted",
				    file);
				free(target);
				continue;
			}
			exists = 1;
			if (snapshot_of(target, &snap) == -1) {
				warn("%s", target);
				free(target);
				continue;
			}

			/*
			 * The privileged paths exist for files the user
			 * cannot fully access.  A file that is both readable
			 * and writable could simply be edited directly, so
			 * editing it through doas would add privilege for no
			 * reason; reject it. Read access determines whether
			 * loading uses doas cat. Write access determines
			 * whether saving can use a local descriptor.
			 */
			readable = open(target, O_RDONLY | O_NOFOLLOW);
			if (readable != -1) {
				close(readable);
				readable = 1;
			}
			writable = (access(target, W_OK) == 0);
			if (readable && writable) {
				warnx("%s: editing user-readable and "
				      "-writable files is not permitted",
				    file);
				free(target);
				continue;
			}
		}

		/*
		 * Isolate editor-created side files across operands. Keep
		 * the original basename so the editor can recognize its
		 * extension, and choose a distinct comparison filename.
		 */
		if (asprintf(&editdir, "%s/edit.XXXXXXXXXX", tmpdir) == -1)
			err(1, "asprintf");
		if (mkdtemp(editdir) == nullptr) {
			warn("%s", editdir);
			free(editdir);
			editdir = nullptr;
			goto next;
		}
		{
			auto	    tlen = strlen(editdir);
			auto	    blen = strlen(base);
			const char *cname = strcmp(base, ".original") == 0 ?
			    ".reference" :
			    ".original";
			auto	    clen = strlen(cname);

			tmpfile = malloc(tlen + blen + 2);
			tmpcopy = malloc(tlen + clen + 2);
			if (tmpfile == nullptr || tmpcopy == nullptr)
				err(1, "malloc");
			snprintf(
			    tmpfile, tlen + blen + 2, "%s/%s", editdir, base);
			snprintf(
			    tmpcopy, tlen + clen + 2, "%s/%s", editdir, cname);
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
					struct stat fst;

					if (fstat(fd, &fst) == -1 ||
					    !stat_matches_snapshot(
						&fst, &snap)) {
						warnx("%s: file changed "
						      "while reading",
						    file);
						close(fd);
						goto next;
					}
				}
				{
					int out =
					    open(tmpfile, O_WRONLY | O_TRUNC);
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
				 * doas. Compare the named target with
				 * the snapshot after cat(1) returns.
				 * This observes metadata at the end of
				 * the read. cat opens its own input.
				 */
				const char *cat_argv[] = {
				    "doas", "cat", target, nullptr};
				int tries;

				for (tries = 0; tries < 3; tries++) {
					int out =
					    open(tmpfile, O_WRONLY | O_TRUNC);
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
					      "reading",
					    file);
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
		if (rc != 0) {
			warnx("editor '%s' failed (status %d)", editor_cmd[0],
			    rc);
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
			keep_edit = 0;
			exit_code = 0;
			goto next;
		}

		keep_edit = 1;
		if (interrupted)
			goto next;

		/* write back */
		if (!exists) {
			const char *inst_argv[] = {"doas", "install", "-m",
			    "0644", tmpfile, target, nullptr};
			struct stat nst;
			int	    tries;

			/* Recheck absence before each authentication attempt.
			 */
			for (tries = 0; tries < 3; tries++) {
				if (lstat(target, &nst) == 0) {
					warnx("%s: file appeared during "
					      "editing, not saving",
					    file);
					goto next;
				}
				if (errno != ENOENT) {
					warn("%s", target);
					goto next;
				}
				if (doas_exec(inst_argv, -1, -1) == 0)
					break;
				if (tries == 2) {
					warnx("unable to save '%s' with "
					      "'doas install'",
					    file);
					goto next;
				}
			}
			keep_edit = 0;
			exit_code = 0;
			goto next;
		}

		/* existing file: refuse to write if it changed */
		if (!snapshot_equal(target, &snap)) {
			warnx("%s: file changed during editing, not "
			      "saving",
			    file);
			goto next;
		}

		/*
		 * For a locally writable target, verify the descriptor
		 * before updating the existing inode. Truncation follows
		 * the copy so it removes the old suffix at the new end.
		 * A failure or interruption can leave partial contents.
		 */
		fd = -1;
		if (access(target, W_OK) == 0)
			fd = open(target, O_WRONLY | O_NOFOLLOW);
		if (fd != -1) {
			struct stat fst;
			off_t	    pos;

			if (fstat(fd, &fst) == -1 ||
			    !stat_matches_snapshot(&fst, &snap)) {
				warnx("%s: file changed during editing, "
				      "not saving",
				    file);
				close(fd);
				goto next;
			}
			if (file_into_fd(tmpfile, fd) == -1) {
				warn("failed to write %s", target);
				close(fd);
				goto next;
			}
			pos = lseek(fd, 0, SEEK_CUR);
			if (pos == -1 || ftruncate(fd, pos) == -1) {
				warn("failed to truncate %s", target);
				close(fd);
				goto next;
			}
			if (close(fd) == -1) {
				warn("failed to close %s", target);
				goto next;
			}
			keep_edit = 0;
			exit_code = 0;
			goto next;
		}

		/*
		 * Privileged write: doas install(1) copies the
		 * temporary file into place atomically (mkstemp(3) in
		 * the destination directory plus rename(2)), which is
		 * intended for a stable regular target and preserves owner,
		 * group and mode. install resolves its operands again after
		 * authentication.
		 */
		{
			const char *inst_argv[] = {"doas", "install", "-o",
			    nullptr, "-g", nullptr, "-m", nullptr, tmpfile,
			    target, nullptr};
			char	    ubuf[32], gbuf[32], mbuf[8];
			int	    tries;

			snprintf(ubuf, sizeof(ubuf), "%u", (unsigned)snap.uid);
			snprintf(gbuf, sizeof(gbuf), "%u", (unsigned)snap.gid);
			snprintf(mbuf, sizeof(mbuf), "%o",
			    (unsigned)(snap.mode & 07777));
			inst_argv[3] = ubuf;
			inst_argv[5] = gbuf;
			inst_argv[7] = mbuf;
			for (tries = 0; tries < 3; tries++) {
				if (!snapshot_equal(target, &snap)) {
					warnx("%s: file changed during "
					      "editing, not saving",
					    file);
					goto next;
				}
				if (doas_exec(inst_argv, -1, -1) == 0)
					break;
				if (tries == 2) {
					warnx("unable to save '%s' with "
					      "'doas install'",
					    file);
					goto next;
				}
			}
			keep_edit = 0;
			exit_code = 0;
			goto next;
		}
	next:
		if (keep_edit && !interrupted) {
			const char *keep = getenv("DOASEDIT_KEEP_FAILED");

			if (keep != nullptr && strcmp(keep, "1") == 0 &&
			    lstat(tmpfile, &lst) == 0 && S_ISREG(lst.st_mode) &&
			    unlink(tmpcopy) == 0) {
				/* The private directory becomes the recovery
				 * location. */
				warnx("edited copy retained at %s", tmpfile);
				free(target);
				free(tmpfile);
				free(tmpcopy);
				free(editdir);
				free(tdir_tmpl);
				return (1);
			}
		}
		if (tmpfile != nullptr)
			(void)unlink(tmpfile);
		if (tmpcopy != nullptr)
			(void)unlink(tmpcopy);
		free(target);
		free(tmpfile);
		free(tmpcopy);
		if (editdir != nullptr && rmdir(editdir) == -1)
			warn("temporary files remain in %s", editdir);
		free(editdir);
	}

	if (rmdir(tmpdir) == -1 && errno != ENOTEMPTY && errno != EEXIST)
		warn("%s", tmpdir);
	free(tdir_tmpl);
	return (interrupted ? 128 + interrupted : exit_code);
}
