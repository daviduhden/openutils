# Makefile for ee(1) (Easy Editor)
#
# ee is built against its own bundled terminal library (new_curse.c),
# a self-contained curses subset with a built-in terminfo reader, so
# no external curses library is needed.  The code builds warning-free
# with strict flags.

PROG = ee
SRCS = ee.c new_curse.c

CC = clang
CFLAGS ?= -O2 -pipe
WARNINGS ?= -Wall -Wextra -Wpedantic
CSTD = -std=c17
CPPFLAGS += -I. -I../compat
DEBUGGER ?= lldb
DEBUG_CFLAGS ?= -O0 -g

# compat shims for non-OpenBSD hosts (they compile to nothing there)
COMPAT_SRCS = ../compat/strtonum.c ../compat/err.c ../compat/bsdstring.c

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/man/man

all: $(PROG)

$(PROG): $(SRCS) $(COMPAT_SRCS) new_curse.h ../compat/bsdcompat.h Makefile ../Makefile
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDFLAGS)

# debug build: unoptimized binary for use with the debugger
$(PROG)_debug: $(SRCS) $(COMPAT_SRCS) new_curse.h ../compat/bsdcompat.h Makefile ../Makefile
	$(CC) $(DEBUG_CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDFLAGS)

debug: $(PROG)_debug
	$(DEBUGGER) ./$(PROG)_debug

clean:
	rm -f $(PROG) $(PROG)_debug ee.o new_curse.o

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

.PHONY: all clean install uninstall test debug