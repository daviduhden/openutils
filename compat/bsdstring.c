/*
 * strlcpy(3), strlcat(3) and reallocarray(3) for libcs that do not
 * provide them.  The BSDs provide them natively; glibc provides
 * strlcpy/strlcat since 2.38 and reallocarray since 2.26.
 *
 * This file also defines the shared setprogname(3)/getprogname(3)
 * buffer declared in bsdcompat.h.
 *
 */

// clang-format off
#include "bsdcompat.h"

#include <errno.h>
// clang-format on

#if !OPENUTILS_BSD_LIBC && (defined(__GLIBC__) || !defined(__linux__))

/*
 * Shared by setprogname(3)/getprogname(3) in bsdcompat.h.  The call to
 * setprogname() is in the program's main source file, while compat/err.c
 * reads the same name through getprogname(); a static buffer in the
 * header would give every translation unit its own copy.
 */
char openutils_progname_buf[64]; /* zero-initialized */

#endif

#if !OPENUTILS_BSD_LIBC &&                                                     \
    (!defined(__GLIBC__) ||                                                    \
	!((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)))

/*
 * Copy src to dst, truncating to fit dsize bytes including the
 * terminating NUL.  dst is always NUL-terminated when dsize is nonzero.
 * Returns strlen(src): a return value of dsize or larger means the copy
 * was truncated.  The scan of src is required because the return value
 * is the length of the source, not of the stored copy.
 */
size_t
strlcpy(char *dst, const char *src, size_t dsize)
{
	const char *osrc = src;
	size_t	    nleft = dsize;

	if (nleft != 0) {
		while (--nleft != 0) {
			if ((*dst++ = *src++) == '\0')
				break;
		}
	}
	if (nleft == 0) {
		if (dsize != 0)
			*dst = '\0';
		while (*src++)
			;
	}
	return ((size_t)(src - osrc) - 1);
}

/*
 * Append src to the NUL-terminated string in dst, truncating so that the
 * result and its terminator fit in dsize bytes.  Returns
 * strlen(dst) + strlen(src), the length the combined string would have
 * had; a return value of dsize or larger means truncation (or that dst
 * was not terminated within dsize).  The destination is not modified
 * when dsize is zero.
 */
size_t
strlcat(char *dst, const char *src, size_t dsize)
{
	const char *odst = dst;
	const char *osrc = src;
	size_t	    n = dsize;
	size_t	    dlen;

	while (n-- != 0 && *dst != '\0')
		dst++;
	dlen = (size_t)(dst - odst);
	n = dsize - dlen;

	if (n-- == 0)
		return (dlen + strlen(src));
	while (*src != '\0') {
		if (n != 0) {
			*dst++ = *src;
			n--;
		}
		src++;
	}
	*dst = '\0';

	return (dlen + (size_t)(src - osrc));
}

#endif

#if !OPENUTILS_BSD_LIBC &&                                                     \
    (!defined(__GLIBC__) ||                                                    \
	!((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 26)))

/*
 * reallocarray(3) needs checked multiplication.  <stdckdint.h> is
 * included only inside this conditional block so that a host that
 * provides reallocarray(3) natively does not pay for the shim header.
 */
// clang-format off
#include <stdckdint.h>
// clang-format on

/*
 * Allocate or resize an array of nmemb elements of size bytes each.
 * The multiplication is checked: on overflow, errno is set to ENOMEM
 * and nullptr is returned.  Otherwise the call is delegated to
 * realloc(3), which sets errno itself when it fails.
 */
void *
reallocarray(void *optr, size_t nmemb, size_t size)
{
	size_t product;

	if (ckd_mul(&product, nmemb, size)) {
		errno = ENOMEM;
		return (nullptr);
	}
	return (realloc(optr, product));
}

#endif
