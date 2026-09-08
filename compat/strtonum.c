/*
 * strtonum(3) for libcs that do not provide it (glibc, musl, ...).
 * The BSDs provide it natively; this file compiles to nothing there.
 *
 * Based on the OpenBSD implementation.
 */
#include "bsdcompat.h"

#if !OPENUTILS_BSD_LIBC

#include <errno.h>
#include <limits.h>

long long
strtonum(const char *numstr, long long minval, long long maxval,
    const char **errstrp)
{
	long long ll = 0;
	int error = 0;
	char *ep;
	struct errval {
		const char *errstr;
		int err;
	} ev[4] = {
		{ NULL,		0 },
		{ "invalid",	EINVAL },
		{ "too small",	ERANGE },
		{ "too large",	ERANGE },
	};

	ev[0].err = errno;
	errno = 0;
	if (minval > maxval) {
		error = 1;
	} else {
		ll = strtoll(numstr, &ep, 10);
		if (ep == numstr || *ep != '\0')
			error = 1;
		else if ((ll == LLONG_MIN && errno == ERANGE) || ll < minval)
			error = 2;
		else if ((ll == LLONG_MAX && errno == ERANGE) || ll > maxval)
			error = 3;
	}
	if (errstrp != NULL)
		*errstrp = ev[error].errstr;
	errno = ev[error].err;
	if (error)
		ll = 0;
	return (ll);
}

#endif
