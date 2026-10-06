# Makefile for ee(1) (Easy Editor)
#
# Terminal handling uses ncursesw (the wide-character ncurses), so the
# editor depends on an external curses implementation instead of the
# historical bundled mini-curses.  The code builds warning-free with
# strict flags.

PROG = ee
SRCS = ee.c utf8.c help.c spell/spell.c

CC = clang
CFLAGS ?= -O2 -pipe
WARNINGS ?= -Wall -Wextra -Wpedantic
CSTD = -std=c23
DEBUGGER ?= lldb
DEBUG_CFLAGS ?= -O0 -g

# Locate ncursesw.  pkg-config is used when it is available, but it is
# not required: the wide-character headers live in a well-known
# subdirectory (ncursesw/) and the library is simply -lncursesw, so a
# plain compiler invocation works on OpenBSD and on the Linux
# distributions without pkg-config metadata.
PKG_CONFIG ?= pkg-config
NCURSES_CFLAGS != $(PKG_CONFIG) --cflags-only-I ncursesw 2>/dev/null || \
    echo -I/usr/local/include/ncursesw -I/usr/include/ncursesw
NCURSES_LIBS != $(PKG_CONFIG) --libs ncursesw 2>/dev/null || echo -lncursesw

CPPFLAGS += -I. -I../compat $(NCURSES_CFLAGS)
LDLIBS = $(NCURSES_LIBS)

# compat shims for non-OpenBSD hosts (they compile to nothing there)
COMPAT_SRCS = ../compat/strtonum.c ../compat/err.c ../compat/bsdstring.c

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/man/man

# One of the standard data directories ee searches (see ee.1).  It is
# named after the .aff/.dic convention that the files follow.
DICTDIR ?= $(PREFIX)/share/myspell

all: $(PROG)

$(PROG): $(SRCS) $(COMPAT_SRCS) ../compat/bsdcompat.h ../compat/stdckdint.h Makefile ../Makefile
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDLIBS) $(LDFLAGS)

# debug build: unoptimized binary for use with the debugger
$(PROG)_debug: $(SRCS) $(COMPAT_SRCS) ../compat/bsdcompat.h ../compat/stdckdint.h Makefile ../Makefile
	$(CC) $(DEBUG_CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDLIBS) $(LDFLAGS)

debug: $(PROG)_debug
	$(DEBUGGER) ./$(PROG)_debug

clean:
	rm -f $(PROG) $(PROG)_debug ee.o utf8.o help.o spell/spell.o \
	    tests/test-spell tests/test-dictionary

install: $(PROG)
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)1 $(DESTDIR)$(DICTDIR)
	install -m 755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	install -m 644 $(PROG).1 $(DESTDIR)$(MANDIR)1/$(PROG).1
	install -m 644 spell/en_US.aff spell/en_US.dic \
	    $(DESTDIR)$(DICTDIR)/
	ln -sf $(PROG) $(DESTDIR)$(BINDIR)/ree
	ln -sf $(PROG) $(DESTDIR)$(BINDIR)/edit

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG) $(DESTDIR)$(BINDIR)/ree \
	    $(DESTDIR)$(BINDIR)/edit $(DESTDIR)$(MANDIR)1/$(PROG).1 \
	    $(DESTDIR)$(DICTDIR)/en_US.aff $(DESTDIR)$(DICTDIR)/en_US.dic

test: $(PROG)
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o tests/test-spell \
	    tests/test-spell.c spell/spell.c $(COMPAT_SRCS) $(LDFLAGS)
	./tests/test-spell tests/spell-test.aff tests/spell-test.dic
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) \
	    -o tests/test-dictionary \
	    tests/test-dictionary.c spell/spell.c $(COMPAT_SRCS) $(LDFLAGS)
	./tests/test-dictionary spell/en_US.aff spell/en_US.dic
	perl tests/run.pl
	perl tests/signals.pl
	perl tests/modern.pl
	perl tests/features.pl

.PHONY: all clean install uninstall test debug