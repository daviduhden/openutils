# doasedit

Edit files with root privileges using your own, unprivileged editor.

A security-hardened fork of doasedit 1.0.9, rewritten in C from the
original shell script.  The shell version validated paths, copied
content and wrote results back all through pathnames, which is a
TOCTOU/symlink race at every step, and its final `doas dd` write
followed symlinks (writing an attacker-chosen destination as root).

This implementation uses file-descriptor based validation (`open(2)`
with `O_NOFOLLOW`, `fstat(2)` identity snapshots), an atomic,
rename-based write-back through `install(1)` that cannot follow a
swapped symlink, a private `mkdtemp(3)` directory, no shell invocation
anywhere, and a `pledge(2)` policy.

| File | Purpose |
| --- | --- |
| [doasedit.c](doasedit.c) | The program. |
| [doasedit.1](doasedit.1) | Manual page. |
| [Makefile](Makefile) | Build, test, debug and install targets. |
| [tests/](tests/) | Behavioural test suite (POSIX shell). See [Tests](#tests). |

## Usage

```
doasedit [-h] [--] file ...
```

`-h`/`--help` prints usage; `--` ends option processing so that a file
name beginning with `-` can be edited.

The editor is taken from `DOAS_EDITOR`, then `VISUAL`, then `EDITOR`.
The command may contain arguments; it is executed directly with
`execvp(3)` and no shell is involved.  `TMPDIR` selects the directory
for the private temporary files (default `/tmp`).

## Building and testing

```
make -C doasedit            # build ./doasedit
make -C doasedit debug      # unoptimized build under the debugger
make -C doasedit test       # POSIX shell behavioural tests
make -C doasedit install    # honours PREFIX/DESTDIR
```

Runtime requirements: `doas(1)`, `install(1)` and a `doas.conf` that
permits them (for example `permit persist :wheel`).  The test build is
compiled with `-DDOASEDIT_TEST`, which adds identity-faking hooks used
by the test suite; the production binary contains none of it.

## Tests

The tests in [tests/](tests/) use a build of doasedit compiled with
`-DDOASEDIT_TEST`, which adds hooks that fake the user identity seen by
the ownership checks, plus fake `doas(1)` and editor programs.  This
exercises the privileged code paths without real root privileges.  The
fake-`doas` flows verify doasedit's own logic — which commands are
invoked, how their results are handled, race detection and cleanup — not
`doas(1)` itself, which exists only on OpenBSD.

| File | Purpose |
| --- | --- |
| [tests/run.sh](tests/run.sh) | Test driver (`/bin/sh`). |
| [tests/fake-doas](tests/fake-doas) | Stand-in for `doas(1)`. |
| [tests/fake-editor](tests/fake-editor) | Stand-in for the user's editor. |

Test-only environment variables understood by the instrumented build:

| Variable | Meaning |
| --- | --- |
| `DOASEDIT_TEST_UID` | UID to report as the invoking user. |
| `DOASEDIT_TEST_UNREADABLE` | Make the identity's permission bits apply even when run as root. |

The suite is POSIX shell and needs no extra packages.  Run it directly:

```
sh doasedit/tests/run.sh
```

See [../README.md](../README.md) for the pledge profile and the known
differences from the reference implementation.
