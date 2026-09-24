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

All C code is C23 (`-std=c23`, strict ISO mode, no compiler
extensions).  C23 features are used where they express a property the
code already relies on: `static_assert` for compile-time invariants,
`[[noreturn]]` for functions that never return, and `<stdckdint.h>`
for checked integer arithmetic, so no compiler-specific attribute
shim is needed.  Every component builds warning-free with `-Wall
-Wextra -Wpedantic` under both GCC and Clang.  `ee` and its UTF-8 and
help units were additionally audited with `-Wconversion
-Wsign-conversion`; those warnings were fixed rather than suppressed.
OpenBSD interfaces (`strlcpy`, `strlcat`, `strtonum`, `reallocarray`,
`pledge`, `unveil`, `err(3)`, `getprogname`) are used deliberately on
top of that baseline.

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
--sort --timefmt --help`, UTF-8 line
drawing, symlink following with loop detection and JSON output.
Patterns use `fnmatch(3)`; the `|`/`^` extensions of the original
matcher are deliberately not reproduced.  XML/HTML output, `--du`,
`--charset`, `-R`, `-A` and `-S` are not implemented (see
`tree/tree.1`).

### ee (Easy Editor)

The small, friendly screen editor.  Imported from the FreeBSD source
tree (Hugh Mahon's ee 1.5.2) and modernized: terminal handling now uses
`ncursesw`, the editor's historical bundled mini-curses library
(`new_curse`) has been removed entirely.  The editing model, commands,
buffer semantics and configuration file format are unchanged; the
presentation is new, with a monochrome title/status bar, a contextual
shortcut bar, unambiguous prompts and a paged help screen.  The editor
is UTF-8 only and its interface is U.S. English only; text is validated
before it is loaded, so a file containing a NUL byte or malformed UTF-8
is rejected instead of being silently accepted.  Also installed as
`ree` (restricted mode) and `edit`, as upstream does.

### truncate

Shrink or extend the size of files.  Imported from the DragonFlyBSD
source tree (originally by Sheldon Hearn) and extended for command
line compatibility with the truncate utility in common use today:
`-c/--no-create`, `-o/--io-blocks`, `-r/--reference`, `-s/--size`,
`--help`, the `+ - < > / %` size modifiers and the
K/KB/KiB ... suffix set.  The extended behaviour was reimplemented
from the documented semantics of that utility; no GNU code was
copied.

## Locale and encoding

The user interface of every utility is U.S. English only: there is no
translation infrastructure, no message catalogs, and no support for
alternative human languages.  UTF-8 is the only supported text
encoding.

`LANG`, `LANGUAGE`, `LC_ALL`, `LC_MESSAGES` and the other locale
environment variables never change the interface language, the
numeric syntax (the decimal separator is always `.`), the sorting
order or the date format.

- `tree` selects `en_US.UTF-8` itself and ignores the environment;
  on a host without that locale data it degrades (with a warning) to
  byte-oriented output rather than adopting an unknown encoding.
- `ee` requires a UTF-8 `LC_CTYPE`.  If the environment names a
  locale, that choice is honoured and a non-UTF-8 codeset makes `ee`
  refuse to start; if no locale is configured, a well-known UTF-8
  locale is selected automatically.  There is no byte-oriented mode,
  so the editor never operates with a text semantics that contradicts
  its UTF-8 buffer.
- the other utilities never call `setlocale(3)` and always run in the
  deterministic "C" locale.

Legacy encodings (ISO-8859-*, Windows-1252, Shift-JIS, EUC-JP,
KOI8-R, Big-5, ...) are not supported.  User data (file names,
edited text, ...) may contain arbitrary valid Unicode encoded as
UTF-8; only the interface language is restricted to English.

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

- OpenBSD base system (`doas(1)`, `install(1)`, libc).  All components
  need a C23 compiler and library: `-std=c23`, standard attributes,
  and `<stdckdint.h>` (shipped with GCC 14+ and recent Clang, including
  the Clang in current OpenBSD).  `ee` needs the
  wide-character ncurses library (`ncursesw`); on OpenBSD it comes from
  the `ncurses` package (devel/ncurses).  `pkg-config` is used when
  available to locate it, otherwise the build falls back to
  `-lncursesw`.
- For doasedit, a `doas.conf` that permits `dd`, `cat` and `install`
  (e.g. `permit persist :wheel`).
- Perl 5 with the IO::Pty module for the ee behavioural test suite
  (`make test`); install it on OpenBSD with `doas pkg_add p5-IO-Tty`
  (devel/p5-IO-TTY).  The doasedit, tree and truncate test suites are
  POSIX shell and need no extra packages.

## Known differences from the reference utilities

- `tree`: pattern matcher differences, unimplemented output formats
  (documented in `tree/tree.1`); `-s` prints sizes (use
  `--noreport` for the historical "no report" behaviour).
- `truncate`: error messages are similar but not identical; usage
  text differs.
- `ee`: the message-catalog (localization) infrastructure of the
  original is removed; the interface is hard-coded U.S. English and
  text is always UTF-8.  Terminal handling is provided by `ncursesw`;
  the bundled `new_curse` library has been deleted.  The window chrome
  is new (title/status bar, shortcut bar, structured help), and the
  save/quit confirmations are explicit, but the editing commands and
  their semantics are preserved.  `-i` now hides the shortcut bar only
  (the status bar is always shown).  Files that are not valid UTF-8, or
  that contain a NUL byte, are rejected rather than edited.  A
  configured non-UTF-8 locale makes `ee` refuse to start (there is no
  byte-oriented mode).  The historical `eightbit`/`noeightbit` settings
  are accepted but have no effect (text is always UTF-8).
- `doasedit`: the write-back replaces the file atomically (inode is
  not preserved, hard links are broken, file flags are not carried
  over; owner/group/mode are preserved) instead of writing into the
  existing inode — the same trade-off `sudoedit` makes, in exchange
  for closing the symlink race.
