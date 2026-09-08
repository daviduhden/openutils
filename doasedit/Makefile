# Makefile for doasedit(1) - openutils fork

PROG = doasedit
SRCS = doasedit.c

CC ?= cc
CFLAGS ?= -O2 -pipe
WARNINGS ?= -Wall -Wextra -Wpedantic
CSTD = -std=c17
CPPFLAGS += -I../compat

# compat shims for non-OpenBSD hosts (they compile to nothing there)
COMPAT_SRCS = ../compat/strtonum.c ../compat/err.c ../compat/bsdstring.c

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/man/man

all: $(PROG)

$(PROG): $(SRCS) $(COMPAT_SRCS) ../compat/bsdcompat.h
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDFLAGS)

# test build: identical code plus test-only hooks used by tests/run.sh
$(PROG)_test: $(SRCS) $(COMPAT_SRCS) ../compat/bsdcompat.h
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(CPPFLAGS) -DDOASEDIT_TEST -o $@ $(SRCS) $(COMPAT_SRCS) $(LDFLAGS)

test: $(PROG)_test
	sh tests/run.sh

clean:
	rm -f $(PROG) $(PROG)_test $(PROG:=.o)

install: $(PROG)
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)1
	install -m 755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	install -m 644 $(PROG).1 $(DESTDIR)$(MANDIR)1/$(PROG).1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG) $(DESTDIR)$(MANDIR)1/$(PROG).1

.PHONY: all clean install uninstall test