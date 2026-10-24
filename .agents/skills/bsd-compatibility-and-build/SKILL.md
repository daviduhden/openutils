---
name: bsd-compatibility-and-build
description: "Use when editing compat/, feature-test macros, bsdcompat.h, stdckdint.h, Makefiles, compiler flags, install layout, or OpenBSD-vs-non-OpenBSD build behavior."
---

# BSD compatibility and build

## When to use this

Use this skill for `compat/`, the top-level and per-tool `Makefile`s,
`CSTD`/`CFLAGS`/`CC`, `PREFIX`/`DESTDIR`/`DICTDIR`, and any question
about building or testing on Linux or another non-OpenBSD host. For C
style, pair it with `openutils-c23-standards`.

## Compatibility model

- `compat/bsdcompat.h` is included first by every production C file.
  It enforces C23, selects `OPENUTILS_BSD_LIBC` for OpenBSD/FreeBSD/
  NetBSD/DragonFly, and defines the feature-test macros only on
  non-OpenBSD hosts.
- On OpenBSD, `_XOPEN_SOURCE` and `_DEFAULT_SOURCE` must not be defined
  by the project: they would hide the BSD-visible interfaces the
  sources use.
- `compat/stdckdint.h` is a shim for a libc without `<stdckdint.h>`.
  It uses `#include_next` when a real header is reachable, otherwise
  defines `ckd_add`/`ckd_sub`/`ckd_mul` from compiler overflow builtins.
  Because `compat/` is on the include path, this shim may be active on
  OpenBSD too; do not claim all compat files compile to nothing there.
- `strtonum.c`, `bsdstring.c` and `err.c` supply interfaces that are
  native on BSD and may already exist in glibc. Their `#if` conditions
  are deliberate; do not broaden them without checking for duplicate
  definitions.
- `bsdstring.c` also owns the shared `openutils_progname_buf` used by
  the `setprogname`/`getprogname` shims. It is a single external object
  so `compat/err.c` sees the name set by the program's `main`.
- `pledge`/`unveil` are no-ops outside OpenBSD. A successful non-OpenBSD
  build does not prove the OpenBSD sandbox behavior.

## Build system facts

- Top-level `SUBDIR = doasedit tree ee truncate`; each tool has its own
  `Makefile` and builds one independent binary.
- `CC = clang`, `CFLAGS ?= -O2 -pipe`, `CSTD ?= -std=c23` and
  `WARNINGS ?= -Wall -Wextra -Wpedantic` are the defaults. Override on
  the make command line (`make CC=gcc`, `make CFLAGS=...`).
- `CPPFLAGS` adds `-I../compat`; ee adds `-I.` and ncursesw include
  paths. `LDLIBS` is used by ee for ncursesw.
- `make check` uses `CHECK_CC ?= clang` and the `CHECK_WARNINGS` /
  `EE_WARNINGS` sets. It checks the first-include and `_XOPEN_SOURCE`
  rules, then cleans and rebuilds. It is not `make test`, and it does
  not use `-Werror`.
- `make test` runs compat, tree, truncate and ee. It does not cover
  doasedit. ee's Perl suites require `IO::Pty`.
- `make install` honors `PREFIX` (default `/usr/local`), `BINDIR`,
  `MANDIR`, `DICTDIR` (default `$(PREFIX)/share/myspell`) and
  `DESTDIR`. ee installs `ree` and `edit` symlinks and the en_US
  dictionary files.
- GNU make 4.0 or later is needed if GNU make is used, because
  `ee/Makefile` uses the `!=` shell-assignment operator. OpenBSD make
  works with the native syntax.

## Procedure

1. Identify whether the interface is native on BSD, provided by glibc,
   or missing. Put declarations and implementations under the existing
   `OPENUTILS_BSD_LIBC` / `__GLIBC__` conditions; do not add a new
   portability layer.
2. If you add a new compat `.c` file, add it to `COMPAT_SRCS` in every
   tool Makefile that needs it. Do not add a new binary or multicall
   wrapper.
3. Keep `bsdcompat.h` first in every production source. If a new header
   must be seen before it, stop and reconsider; feature-test macros must
   be defined before the first system header.
4. Update the `Makefile` dependencies when a source starts depending on
   a new header or compat file.
5. For install changes, update both the Makefile and the README/manpage
   that describe installed artifacts.
6. Static-check the result. Build on the host you have if possible;
   otherwise report the build as unverified.

## Validation

- `git diff --check`, `git status`, and path checks for documentation.
- `make -C <tool>` for a normal build; `make check` for the strict
  warning/source-policy gate (it cleans and rebuilds).
- Compat tests:
  ```sh
  sh compat/tests/run.sh
  FORCE=OPENUTILS_STDCKDINT_FORCE_FALLBACK sh compat/tests/stdckdint-compile-fail.sh
  ```
- On a non-OpenBSD host, also run the tool suites; remember that
  `pledge` is a no-op there.
- Do not claim OpenBSD behavior from a Linux build. Pledge/unveil and
  doas are only real on OpenBSD.

## Common problems

- Defining `_XOPEN_SOURCE` in a production source. It belongs only in
  `compat/bsdcompat.h`.
- Assuming `stdckdint.h` compiles to nothing on OpenBSD; the libc may
  lack it and the shim may be active.
- Adding a new `#if !__OpenBSD__` shim that duplicates a glibc
  interface. Check the glibc version conditions first.
- Forgetting to add a new compat source to all four `COMPAT_SRCS` lists.
- Using `CC ?= clang` and getting make's built-in `cc`; the project uses
  `CC = clang` and expects a command-line override.
- Passing `CSTD` only through `CFLAGS`; it is a separate variable so a
  `CFLAGS` override cannot drop `-std=c23`.
- Running `make check` for a docs-only change; it mutates build outputs.
- Assuming `make test` covers doasedit or OpenBSD pledge enforcement.

## References

- `compat/README.md`
- `compat/bsdcompat.h`, `compat/stdckdint.h`
- `compat/tests/run.sh`, `compat/tests/stdckdint-compile-fail.sh`
- `Makefile` and all per-tool `Makefile`s
- Root `README.md`, sections "Language standard" and "Building"
