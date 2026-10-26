/*
 * strtonum(3) for libcs that do not provide it (glibc, musl, ...).
 * The BSDs provide it natively; this file compiles to nothing there.
 *
 */

// clang-format off
#include "bsdcompat.h"

#if !OPENUTILS_BSD_LIBC

#include <errno.h>
#include <limits.h>
// clang-format on

/*
 * Convert the decimal string numstr to a long long in [minval, maxval].
 * strtoll(3) supplies the base-10 syntax (optional leading whitespace and
 * sign), but the whole input must be consumed: any trailing character,
 * including whitespace, makes it invalid.  When errstrp is non-null,
 * *errstrp is set to nullptr on success, or to "invalid", "too small" or
 * "too large" on failure (0 is then returned).  errno is restored to its
 * incoming value on success and set to EINVAL or ERANGE on failure,
 * matching the OpenBSD contract.  An inverted range (minval > maxval) is
 * invalid.
 */
long long
strtonum(const char *numstr, long long minval, long long maxval,
    const char **errstrp)
{
	long long ll = 0;
	int	  error = 0;
	char	 *ep;
	/*
	 * Lookup table indexed by the error code (0 = success).  ev[0]
	 * carries the saved incoming errno so the success path can restore
	 * it; the other entries pair the public message with the errno
	 * value that the caller observes.
	 */
	struct errval {
		const char *errstr;
		int	    err;
	} ev[4] = {
	    {nullptr, 0},
	    {"invalid", EINVAL},
	    {"too small", ERANGE},
	    {"too large", ERANGE},
	};

	ev[0].err = errno;
	errno = 0;
	if (minval > maxval) {
		/* An impossible range is reported like malformed input. */
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
	if (errstrp != nullptr)
		*errstrp = ev[error].errstr;
	errno = ev[error].err;
	if (error)
		ll = 0;
	return (ll);
}

#endif
