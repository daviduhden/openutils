# openutils - small Unix utilities for OpenBSD
#
# Top-level build driver.  Works with OpenBSD make(1); no GNU make
# extensions are used.
#
# Build variables are propagated to the per-utility Makefiles through
# the environment (not the make command line) so that the
# per-utility Makefiles can extend them with += as usual.

CC = clang
CFLAGS ?= -O2 -pipe
CPPFLAGS ?=
LDFLAGS ?=
DEBUGGER ?= lldb
DEBUG_CFLAGS ?= -O0 -g

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/man/man
DESTDIR ?=

SUBDIR = doasedit tree ee truncate

MAKE_ENV = env CC="$(CC)" CFLAGS="$(CFLAGS)" CPPFLAGS="$(CPPFLAGS)" \
	LDFLAGS="$(LDFLAGS)" PREFIX="$(PREFIX)" BINDIR="$(BINDIR)" \
	MANDIR="$(MANDIR)" DESTDIR="$(DESTDIR)" \
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

test: all
	$(MAKE_ENV) $(MAKE) -C doasedit doasedit_test
	sh doasedit/tests/run.sh
	sh tree/tests/run.sh
	sh truncate/tests/run.sh
	sh ee/tests/run.sh
	sh ee/tests/signals.sh

# build one utility with -g -O0 and run it under lldb(1)
debug:
	@if [ -z "$(PROG)" ]; then \
		echo "usage: make debug PROG=<doasedit|tree|ee|truncate>" >&2; \
		exit 1; \
	fi
	$(MAKE_ENV) $(MAKE) -C $(PROG) debug

# strict-warning build of every component (developer target).
#
# doasedit, tree and truncate build warning-free under the full set
# below with both compilers.  ee additionally passes -Wshadow
# -Wformat=2 -Wundef -Wstrict-prototypes -Wmissing-prototypes; the
# remaining two classes are reviewed rather than suppressed: gcc's
# -Wformat-nonliteral for tree's user-supplied strftime(3) format
# (the documented purpose of --timefmt) and the intentional
# int<->unsigned char / int<->size_t narrowing of ee's byte-oriented
# text buffer code, which -Wconversion/-Wsign-conversion flag at
# about a hundred sites without indicating real defects.
CHECK_CC ?= clang
CHECK_WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wsign-conversion
EE_WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef \
	-Wstrict-prototypes -Wmissing-prototypes
check:
	@if grep -rq "_XOPEN_SOURCE" doasedit/*.c tree/*.c truncate/*.c ee/*.c; then \
		echo "error: _XOPEN_SOURCE must only be set in compat/bsdcompat.h" >&2; \
		exit 1; \
	fi
	@for d in doasedit tree truncate; do \
		$(MAKE_ENV) $(MAKE) -C $$d CC="$(CHECK_CC)" \
		WARNINGS="$(CHECK_WARNINGS)" clean >/dev/null || exit 1; \
		$(MAKE_ENV) $(MAKE) -C $$d CC="$(CHECK_CC)" \
		WARNINGS="$(CHECK_WARNINGS)" all || exit 1; \
	done
	@$(MAKE_ENV) $(MAKE) -C ee CC="$(CHECK_CC)" \
	    WARNINGS="$(EE_WARNINGS)" clean >/dev/null || exit 1
	@$(MAKE_ENV) $(MAKE) -C ee CC="$(CHECK_CC)" \
	    WARNINGS="$(EE_WARNINGS)" all || exit 1
	@for d in $(SUBDIR); do \
		$(MAKE_ENV) $(MAKE) -C $$d clean >/dev/null || exit 1; \
	done
	@$(MAKE_ENV) $(MAKE) all

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