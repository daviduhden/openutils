/*
 * openutils portability shims.
 *
 * Every translation unit includes this header first.  On OpenBSD it
 * provides nothing: all of the interfaces below are native to the
 * base system.  On other hosts it does two things:
 *
 *   - it defines the feature-test macros that expose the POSIX/XSI
 *     interfaces used by the sources (glibc in strict ISO C17 mode
 *     hides them unless the macros are defined before the first
 *     system header);
 *
 *   - it supplies replacements for the small number of OpenBSD
 *     interfaces that other libcs do not provide (pledge, strtonum,
 *     strlcpy/strlcat, reallocarray, setprogname/getprogname and the
 *     err(3) family where needed), so that the code can be built and
 *     tested on other systems.  These shims compile to nothing on
 *     OpenBSD.
 *
 * The header deliberately does not touch locale handling: the
 * interface language (U.S. English) and the character encoding
 * (UTF-8) are decided by the individual programs, never by the
 * environment.
 */
#ifndef OPENUTILS_BSDCOMPAT_H
#define OPENUTILS_BSDCOMPAT_H

/*
 * Feature-test macros must be defined before the first system
 * header.  On OpenBSD they must NOT be defined: they would hide the
 * BSD-visible interfaces the project relies on.
 */
#ifndef __OpenBSD__
#define _XOPEN_SOURCE	700
#define _DEFAULT_SOURCE	1
#endif

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* the BSDs provide everything below natively */
#if defined(__OpenBSD__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define OPENUTILS_BSD_LIBC	1
#else
#define OPENUTILS_BSD_LIBC	0
#endif

/*
 * pledge(2) and unveil(2) are OpenBSD kernel interfaces.  On other
 * systems they are no-ops, for testing only: the security model they
 * enforce does not exist outside OpenBSD.
 */
#if !defined(__OpenBSD__)
static inline int
pledge(const char *promises, const char *execpromises)
{
	(void)promises;
	(void)execpromises;
	return (0);
}

static inline int
unveil(const char *path, const char *permissions)
{
	(void)path;
	(void)permissions;
	return (0);
}
#endif

/* mark functions that never return */
#if !defined(__OpenBSD__)
#if defined(__GNUC__) || defined(__clang__)
#define __dead	__attribute__((__noreturn__))
#else
#define __dead
#endif
#endif

/*
 * setprogname(3)/getprogname(3) for libcs that lack them (glibc).
 * glibc's err(3) family does not consult getprogname(), so on glibc
 * this is cosmetic; it exists so the sources compile and run.
 */
#if !OPENUTILS_BSD_LIBC && (defined(__GLIBC__) || !defined(__linux__))
static inline char *
openutils_progname(void)
{
	static char buf[64];	/* zero-initialized */

	return (buf);
}

static inline void
setprogname(const char *name)
{
	char *buf = openutils_progname();
	const char *base;
	size_t len;

	base = strrchr(name, '/');
	base = (base != NULL) ? base + 1 : name;
	len = strlen(base);
	if (len >= 64)
		len = 63;
	memcpy(buf, base, len);
	buf[len] = '\0';
}

static inline const char *
getprogname(void)
{
	return (openutils_progname());
}
#endif

/* strtonum(3): absent from glibc and musl */
#if !OPENUTILS_BSD_LIBC
long long strtonum(const char *, long long, long long, const char **);
#endif

/* strlcpy(3)/strlcat(3): native in glibc 2.38 and later */
#if !OPENUTILS_BSD_LIBC && \
    (!defined(__GLIBC__) || \
    !((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)))
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
#endif

/* reallocarray(3): native in glibc 2.26 and later */
#if !OPENUTILS_BSD_LIBC && \
    (!defined(__GLIBC__) || \
    !((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 26)))
void *reallocarray(void *, size_t, size_t);
#endif

#endif /* OPENUTILS_BSDCOMPAT_H */
