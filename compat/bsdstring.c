/*
 * strlcpy(3), strlcat(3) and reallocarray(3) for libcs that do not
 * provide them.  The BSDs provide them natively; glibc provides
 * strlcpy/strlcat since 2.38 and reallocarray since 2.26.
 *
 */

/*
 * Copyright (c) 2026 David Uhden Collado <david@uhden.dev>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "bsdcompat.h"

#include <errno.h>
#include <stdint.h>

#if !OPENUTILS_BSD_LIBC && \
    (!defined(__GLIBC__) || \
    !((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 38)))

size_t
strlcpy(char *dst, const char *src, size_t dsize)
{
	const char *osrc = src;
	size_t nleft = dsize;

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

size_t
strlcat(char *dst, const char *src, size_t dsize)
{
	const char *odst = dst;
	const char *osrc = src;
	size_t n = dsize;
	size_t dlen;

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

#if !OPENUTILS_BSD_LIBC && \
    (!defined(__GLIBC__) || \
    !((__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 26)))

#define MUL_NO_OVERFLOW	((size_t)1 << (sizeof(size_t) * 4))

void *
reallocarray(void *optr, size_t nmemb, size_t size)
{
	if ((nmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	    nmemb > 0 && SIZE_MAX / nmemb < size) {
		errno = ENOMEM;
		return (NULL);
	}
	return (realloc(optr, size * nmemb));
}

#endif
