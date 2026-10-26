/*
 * err(3) family for libcs that declare but do not provide them.
 * OpenBSD, the other BSDs and glibc provide them natively; this file
 * compiles to nothing on all of them.
 */

// clang-format off
#include "bsdcompat.h"

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// clang-format on

/*
 * The shim is built for non-BSD libcs other than glibc.  glibc is
 * excluded because its implementation is present and its diagnostics
 * are part of the host system; the BSDs provide the family natively.
 */
#if !OPENUTILS_BSD_LIBC && !defined(__GLIBC__)

/*
 * Shared implementation of warn(3) and err(3): "<progname>: <fmt>: <errno
 * text>\n".  The format and its arguments are optional; a nullptr fmt
 * omits both the message and its separating colon.  The system error
 * text is produced from the errno value still in effect when
 * strerror(3) is reached.  This implementation does not save errno
 * around the stdio calls; callers rely on stdio leaving it unchanged,
 * whereas the native BSD versions save and restore it explicitly.
 */
static void
vwarn_impl(const char *fmt, va_list ap)
{
	fprintf(stderr, "%s: ", getprogname());
	if (fmt != nullptr) {
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

/*
 * warnx(3) and errx(3) are the variants without the system error text.
 * They still prefix the program name and append one newline.
 */
void
warnx(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", getprogname());
	va_start(ap, fmt);
	if (fmt != nullptr)
		vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* Report a diagnostic including the system error, then exit(eval). */
void
err(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vwarn_impl(fmt, ap);
	va_end(ap);
	exit(eval);
}

/* Report a message without the system error, then exit(eval). */
void
errx(int eval, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "%s: ", getprogname());
	va_start(ap, fmt);
	if (fmt != nullptr)
		vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(eval);
}

#endif
