/*
 * err(3) family for libcs that declare but do not provide them.
 * OpenBSD, the other BSDs and glibc provide them natively; this file
 * compiles to nothing on all of them.
 */
#include "bsdcompat.h"

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !OPENUTILS_BSD_LIBC && !defined(__GLIBC__)

static void
vwarn_impl(const char *fmt, va_list ap)
{
	fprintf(stderr, "%s: ", getprogname());
	if (fmt != NULL) {
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, ": ");
	}
	fprintf(stderr, "%s\n", strerror(errno));
}

void
warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarn_impl(fmt, ap);
	va_end(ap);
}

void
warnx(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", getprogname());
	va_start(ap, fmt);
	if (fmt != NULL)
		vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void
err(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarn_impl(fmt, ap);
	va_end(ap);
	exit(eval);
}

void
errx(int eval, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", getprogname());
	va_start(ap, fmt);
	if (fmt != NULL)
		vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(eval);
}

#endif
