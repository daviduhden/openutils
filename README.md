# openutils

A collection of small Unix utilities for OpenBSD, built from
BSD-origin implementations and adapted so that their user-visible
functionality is comparable to the corresponding utilities in common
use today.

| Utility   | Origin       | Purpose                                   |
|-----------|--------------|-------------------------------------------|
| doasedit  | doasedit     | secure privileged file editing via doas(1) |
| tree      | pyr's tree   | directory tree display                     |
| ee        | FreeBSD      | Easy Editor                               |
| truncate  | DragonFlyBSD | file size manipulation                    |

Everything is implemented for OpenBSD first: native libc interfaces,
`pledge(2)` where it fits, no GNU dependencies, no shell wrappers
around C functionality, no large portability layers.  The repository
builds with OpenBSD `make` (GNU make also works for testing on other
systems).

## Language standard

All C code is C17 (`-std=c17`, strict ISO mode, no compiler
extensions).  Every component builds warning-free with `-Wall
-Wextra -Wpedantic` under both GCC and Clang.  OpenBSD interfaces
(`strlcpy`, `strlcat`, `strtonum`, `reallocarray`, `pledge`,
`unveil`, `err(3)`, `getprogname`) are used deliberately on top of
that baseline.

Feature-test macros are deliberately NOT defined in production
sources: on OpenBSD, defining `_XOPEN_SOURCE` would hide the
BSD-visible interfaces (`__BSD_VISIBLE` becomes 0) that this project
relies on, and the OpenBSD headers expose everything by default.  A
host-only `_XOPEN_SOURCE 700` lives inside `compat/bsdcompat.h` (its
non-OpenBSD branch), which every translation unit includes first, so
glibc in strict ISO mode sees the POSIX/XSI interfaces during
Linux-host testing.  `make check` guards against the macro leaking
back into production sources.

## The utilities

### doasedit

Edit files with root privileges using your own, unprivileged editor.
A security-hardened fork of doasedit 1.0.9, rewritten in C from the
original shell script: the shell version validated paths, copied
content and wrote results back all through pathnames, which is a
TOCTOU/symlink race at every step, and the final `doas dd` write
followed symlinks (writing an attacker-chosen destination as root).
The fork uses file-descriptor based validation (`open(2)` with
`O_NOFOLLOW`, `fstat(2)` identity snapshots), an atomic, rename-based
write-back through `install(1)` that cannot follow a swapped symlink,
a private `mkdtemp(3)` directory, no shell invocation anywhere, and a
`pledge(2)` policy.

### tree

Recursive directory listing in tree form.  A fork of Pierre-Yves
Ritschard's tree 0.62 (the historical OpenBSD ports implementation),
extended with the option set of the tree utility in common use today:
`-a -d -f -F -i -l -L -r -t -x -s -h -p -u -g -D -C -n -N -Q -q -J
-U -P -I -o --si --dirsfirst --filelimit --inodes --noreport --prune
--sort --timefmt --help`, colour output, UTF-8/ASCII line
drawing, symlink following with loop detection and JSON output.
Patterns use `fnmatch(3)`; the `|`/`^` extensions of the original
matcher are deliberately not reproduced.  XML/HTML output, `--du`,
`--charset`, `-R`, `-A` and `-S` are not implemented (see
`tree/tree.1`).

### ee (Easy Editor)

The small, friendly screen editor.  Imported from the FreeBSD source
tree (Hugh Mahon's ee 1.5.2) and built against its own bundled
mini-curses library (`new_curse`), which parses the terminfo database
directly, so no ncurses dependency exists; `new_curse` was ported to
termios and given wide-character input support for OpenBSD.  Also
installed as `ree` (restricted mode) and `edit`, as upstream does.

### truncate

Shrink or extend the size of files.  Imported from the DragonFlyBSD
source tree (originally by Sheldon Hearn) and extended for command
line compatibility with the truncate utility in common use today:
`-c/--no-create`, `-o/--io-blocks`, `-r/--reference`, `-s/--size`,
`--help`, the `+ - < > / %` size modifiers and the
K/KB/KiB ... suffix set.  The extended behaviour was reimplemented
from the documented semantics of that utility; no GNU code was
copied.

## Building

```
make             # or: make doasedit tree ee truncate
make test
make check       # strict developer build (extra warnings)
make install      # honours PREFIX (default /usr/local) and DESTDIR
make uninstall
```

The build uses `CC`, `CFLAGS`, `CPPFLAGS` and `LDFLAGS`.  `make
install` installs the four binaries, their manual pages, and the
`ree`/`edit` links for ee.  On non-OpenBSD systems the tiny shims in
`compat/` are linked in automatically (they compile to nothing on
OpenBSD); this exists so the code can be built and tested elsewhere.

### pledge profiles

- `doasedit`: `stdio rpath wpath cpath proc exec` (forks/execs the
  editor and doas; never drops to a shell).
- `tree`: `stdio rpath`, plus `wpath cpath` only with `-o` and
  `getpw` only with `-u`/`-g` — the promises are chosen from the
  parsed options.
- `truncate`: `stdio wpath`, plus `cpath` unless `-c` and `rpath`
  only with `-r` — likewise chosen from the parsed options.
- `ee`: `stdio rpath wpath cpath tty proc exec getpw` (files,
  terminal, shell commands, `~` expansion).  `unveil(2)` is not used
  where an editor must reach arbitrary files.

Note: on non-OpenBSD hosts GNU make is known to work; some third-party
BSD make ports come with embedded toolchain defaults for other
operating systems and may need `make CC=cc CFLAGS="-O2 -pipe"
CPPFLAGS="-I$(pwd)/compat"` or equivalent.  On OpenBSD itself a plain
`make` works as expected.

## Requirements

- OpenBSD base system (`doas(1)`, `install(1)`, libc, curses-free).
- For doasedit, a `doas.conf` that permits `dd`, `cat` and `install`
  (e.g. `permit persist :wheel`).

## Known differences from the reference utilities

- `tree`: pattern matcher differences, unimplemented output formats
  (documented in `tree/tree.1`); `-s` prints sizes (use
  `--noreport` for the historical "no report" behaviour).
- `truncate`: error messages are similar but not identical; usage
  text differs.
- `ee`: no message catalogs installed; terminal handling is provided
  by the bundled `new_curse` library.
- `doasedit`: the write-back replaces the file atomically (inode is
  not preserved, hard links are broken, file flags are not carried
  over; owner/group/mode are preserved) instead of writing into the
  existing inode — the same trade-off `sudoedit` makes, in exchange
  for closing the symlink race.
