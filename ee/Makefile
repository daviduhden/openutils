# Makefile for ee(1) (Easy Editor)
#
# Terminal handling uses ncursesw (the wide-character ncurses), so the
# editor depends on an external curses implementation instead of the
# historical bundled mini-curses.  The code builds warning-free with
# strict flags.

PROG = ee
SRCS = ee.c utf8.c help.c

CC = clang
CFLAGS ?= -O2 -pipe
WARNINGS ?= -Wall -Wextra -Wpedantic
CSTD = -std=c17
DEBUGGER ?= lldb
DEBUG_CFLAGS ?= -O0 -g

# Locate ncursesw.  pkg-config knows where the library and its headers
# live on systems that install them outside the default search path;
# when it is unavailable, fall back to plain -lncursesw.
PKG_CONFIG ?= pkg-config
NCURSES_CFLAGS != $(PKG_CONFIG) --cflags-only-I ncursesw 2>/dev/null || \
    echo -I/usr/local/include/ncursesw
NCURSES_LIBS != $(PKG_CONFIG) --libs ncursesw 2>/dev/null || echo -lncursesw

CPPFLAGS += -I. -I../compat $(NCURSES_CFLAGS)
LDLIBS = $(NCURSES_LIBS)

# compat shims for non-OpenBSD hosts (they compile to nothing there)
COMPAT_SRCS = ../compat/strtonum.c ../compat/err.c ../compat/bsdstring.c

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/man/man

all: $(PROG)

$(PROG): $(SRCS) $(COMPAT_SRCS) ../compat/bsdcompat.h Makefile ../Makefile
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDLIBS) $(LDFLAGS)

# debug build: unoptimized binary for use with the debugger
$(PROG)_debug: $(SRCS) $(COMPAT_SRCS) ../compat/bsdcompat.h Makefile ../Makefile
	$(CC) $(DEBUG_CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDLIBS) $(LDFLAGS)

debug: $(PROG)_debug
	$(DEBUGGER) ./$(PROG)_debug

clean:
	rm -f $(PROG) $(PROG)_debug ee.o utf8.o help.o

install: $(PROG)
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)1
	install -m 755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	install -m 644 $(PROG).1 $(DESTDIR)$(MANDIR)1/$(PROG).1
	ln -sf $(PROG) $(DESTDIR)$(BINDIR)/ree
	ln -sf $(PROG) $(DESTDIR)$(BINDIR)/edit

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG) $(DESTDIR)$(BINDIR)/ree \
	    $(DESTDIR)$(BINDIR)/edit $(DESTDIR)$(MANDIR)1/$(PROG).1

test: $(PROG)
	perl tests/run.pl
	perl tests/signals.pl
	perl tests/modern.pl

.PHONY: all clean install uninstall test debug
