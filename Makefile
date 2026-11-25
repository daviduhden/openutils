# openutils - small Unix utilities for OpenBSD
#
# Top-level build driver.  Works with OpenBSD make(1); no GNU make
# extensions are used.
#
# Shared settings travel through the environment. Recursive make also
# carries command-line overrides (for example, make CC=gcc). Each
# utility owns its compiler inputs and mandatory include directories.

CC = clang
CFLAGS ?= -O2 -pipe
# C23 is declared explicitly at the top level so that tooling which scans
# build metadata recognises the standard for every file below, including
# compat/tests/ whose ancestors carry no other declaration.  It is kept
# separate from CFLAGS so that a command-line CFLAGS override cannot drop
# it.  The per-utility Makefiles also set it for direct builds.
CSTD ?= -std=c23
CPPFLAGS ?=
LDFLAGS ?=
DEBUGGER ?= lldb
DEBUG_CFLAGS ?= -O0 -g

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/man/man
DICTDIR ?= $(PREFIX)/share/openutils/ee
DESTDIR ?=

SUBDIR = doasedit tree ee truncate

MAKE_ENV = env CC="$(CC)" CFLAGS="$(CFLAGS)" CSTD="$(CSTD)" \
	CPPFLAGS="$(CPPFLAGS)" LDFLAGS="$(LDFLAGS)" \
	PREFIX="$(PREFIX)" BINDIR="$(BINDIR)" MANDIR="$(MANDIR)" \
	DICTDIR="$(DICTDIR)" DESTDIR="$(DESTDIR)" \
	DEBUGGER="$(DEBUGGER)" DEBUG_CFLAGS="$(DEBUG_CFLAGS)"

all:
	@for d in $(SUBDIR); do \
		$(MAKE_ENV) $(MAKE) -C $$d all || exit 1; \
	done

# force a full rebuild of every component
rebuild: clean
	@$(MAKE_ENV) $(MAKE) all

doasedit:
	$(MAKE_ENV) $(MAKE) -C doasedit all

tree:
	$(MAKE_ENV) $(MAKE) -C tree all

ee:
	$(MAKE_ENV) $(MAKE) -C ee all

truncate:
	$(MAKE_ENV) $(MAKE) -C truncate all

# The shell drivers use the freshly built binaries and keep their
# fixtures in temporary directories. ee owns its test build and runner.
test: all
	CC="$(CC)" CFLAGS="$(CFLAGS)" CSTD="$(CSTD)" sh compat/tests/run.sh && CC="$(CC)" CFLAGS="$(CFLAGS)" CSTD="$(CSTD)" FORCE=OPENUTILS_STDCKDINT_FORCE_FALLBACK sh compat/tests/stdckdint-compile-fail.sh && sh tree/tests/run.sh && sh truncate/tests/run.sh && $(MAKE_ENV) $(MAKE) -C ee test

# Delegate the unoptimized build and debugger invocation to the utility.
debug:
	@if [ -z "$(PROG)" ]; then \
		echo "usage: make debug PROG=<doasedit|tree|ee|truncate>" >&2; \
		exit 1; \
	fi
	$(MAKE_ENV) $(MAKE) -C $(PROG) debug

# Check source policy, then rebuild with extra warnings. Finish with
# normal builds so later commands use the default warning settings.
#
# CHECK_CC defaults to clang; override it on the command line for a
# build with another compiler.  doasedit, tree and truncate are checked
# with the full CHECK_WARNINGS set below, with one intentional
# exception: GCC's -Wformat-nonliteral (implied by -Wformat=2)
# diagnoses do_date()'s runtime strftime(3) format string, which comes
# from the --timefmt option and is user-supplied by design.  ee is
# checked with EE_WARNINGS, which omits -Wconversion and
# -Wsign-conversion; those two flags are not part of the routine check
# target.  -Werror is deliberately not used, so a newer compiler's new
# diagnostics do not turn the developer build into a failure.
CHECK_CC ?= clang
CHECK_WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wsign-conversion
EE_WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes
check:
	@for f in doasedit/*.c tree/*.c truncate/*.c ee/*.c ee/spell/*.c compat/*.c; do first=$$(grep -m1 '^#include' "$$f"); if [ "$$first" != '#include "bsdcompat.h"' ]; then echo "error: $$f must include bsdcompat.h first (got: $$first)" >&2; exit 1; fi; done && if grep -rq "_XOPEN_SOURCE" doasedit/*.c tree/*.c truncate/*.c ee/*.c ee/*.h ee/spell/*.c ee/spell/*.h compat/*.c compat/stdckdint.h; then echo "error: _XOPEN_SOURCE must only be set in compat/bsdcompat.h" >&2; exit 1; fi
	@for d in doasedit tree truncate; do $(MAKE_ENV) $(MAKE) -C $$d CC="$(CHECK_CC)" WARNINGS="$(CHECK_WARNINGS)" clean >/dev/null && $(MAKE_ENV) $(MAKE) -C $$d CC="$(CHECK_CC)" WARNINGS="$(CHECK_WARNINGS)" all || exit 1; done
	@$(MAKE_ENV) $(MAKE) -C ee CC="$(CHECK_CC)" WARNINGS="$(EE_WARNINGS)" clean >/dev/null && $(MAKE_ENV) $(MAKE) -C ee CC="$(CHECK_CC)" WARNINGS="$(EE_WARNINGS)" all
	@for d in $(SUBDIR); do $(MAKE_ENV) $(MAKE) -C $$d clean >/dev/null || exit 1; done && $(MAKE_ENV) $(MAKE) all

install: all
	@for d in $(SUBDIR); do \
		$(MAKE_ENV) $(MAKE) -C $$d install || exit 1; \
	done

uninstall:
	@for d in $(SUBDIR); do \
		$(MAKE_ENV) $(MAKE) -C $$d uninstall || exit 1; \
	done

clean:
	@for d in $(SUBDIR); do \
		$(MAKE_ENV) $(MAKE) -C $$d clean || exit 1; \
	done

.PHONY: all rebuild doasedit tree ee truncate test debug check install uninstall clean
