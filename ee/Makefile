# Makefile for ee(1) (Easy Editor)
#
# ee is built against its own bundled terminal library (new_curse.c),
# a self-contained curses subset with a built-in terminfo reader, so
# no external curses library is needed.  Since the C17 modernization
# (see MODERNIZATION.md) the code builds warning-free with strict
# flags.

PROG	= ee
SRCS	= ee.c new_curse.c

CC	?= cc
CFLAGS	?= -O2 -pipe
WARNINGS ?= -Wall -Wextra -Wpedantic
CSTD	= -std=c17
CPPFLAGS+= -I. -I../compat

# compat shims for non-OpenBSD hosts (they compile to nothing there)
COMPAT_SRCS = ../compat/strtonum.c ../compat/err.c ../compat/bsdstring.c

PREFIX	?= /usr/local
BINDIR	?= $(PREFIX)/bin
MANDIR	?= $(PREFIX)/man/man

all: $(PROG)

$(PROG): $(SRCS) $(COMPAT_SRCS) new_curse.h ../compat/bsdcompat.h
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDFLAGS)

clean:
	rm -f $(PROG) ee.o new_curse.o

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
	sh tests/run.sh
	sh tests/signals.sh

.PHONY: all clean install uninstall test
