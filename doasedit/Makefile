# Makefile for doasedit(1) - openutils fork

PROG = doasedit
SRCS = doasedit.c

CC = clang
CFLAGS ?= -O2 -pipe
WARNINGS ?= -Wall -Wextra -Wpedantic
CSTD ?= -std=c23
PROJECT_CPPFLAGS = -I../compat
DEBUGGER ?= lldb
DEBUG_CFLAGS ?= -O0 -g

# Compile the shared shims with the utility. Each shim checks whether libc
# already supplies its interfaces.
COMPAT_SRCS = ../compat/strtonum.c ../compat/err.c ../compat/bsdstring.c

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/man/man

all: $(PROG)

$(PROG): $(SRCS) $(COMPAT_SRCS) ../compat/bsdcompat.h ../compat/stdckdint.h Makefile ../Makefile
	$(CC) $(CFLAGS) $(CSTD) $(WARNINGS) $(PROJECT_CPPFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDFLAGS)

# debug build: unoptimized binary for use with the debugger
$(PROG)_debug: $(SRCS) $(COMPAT_SRCS) ../compat/bsdcompat.h ../compat/stdckdint.h Makefile ../Makefile
	$(CC) $(DEBUG_CFLAGS) $(CSTD) $(WARNINGS) $(PROJECT_CPPFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(COMPAT_SRCS) $(LDFLAGS)

debug: $(PROG)_debug
	$(DEBUGGER) ./$(PROG)_debug

clean:
	rm -f $(PROG) $(PROG)_debug $(PROG:=.o)

# Building the utility is the available smoke check; this directory has no separate test suite.
test: all

# Prepend DESTDIR when staging the installed binary and manual page.
install: $(PROG)
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)1
	install -m 755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	install -m 644 $(PROG).1 $(DESTDIR)$(MANDIR)1/$(PROG).1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG) $(DESTDIR)$(MANDIR)1/$(PROG).1

.PHONY: all clean install uninstall debug test