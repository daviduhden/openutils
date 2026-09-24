#include "bsdcompat.h"

#include "utf8.h"

#include <limits.h>
#include <wchar.h>

/*
 * The encoder/decoder treat unsigned char as an octet: the lead and
 * continuation byte tests (0x80, 0xC0, 0xE0, 0xF0) and the four-byte
 * output buffer only describe UTF-8 when a byte is eight bits.
 */
static_assert(CHAR_BIT == 8, "the UTF-8 helpers assume 8-bit bytes");

/*
 * Strict UTF-8 decoder.  See utf8.h for the contract.
 */
int
ee_utf8_decode(const unsigned char *s, size_t n, uint32_t *cp, size_t *len)
{
	unsigned char c;
	uint32_t value;
	size_t need;
	size_t i;

	if (n == 0)
		return (0);
	c = s[0];
	if (c < 0x80) {
		*cp = c;
		*len = 1;
		return (1);
	}
	/*
	 * 0xC0 and 0xC1 would encode an overlong value below 0x80;
	 * 0xF5..0xFF never appear in valid UTF-8.
	 */
	if (c < 0xC2)
		return (0);
	if (c < 0xE0) {
		need = 2;
		value = c & 0x1FU;
	} else if (c < 0xF0) {
		need = 3;
		value = c & 0x0FU;
	} else if (c < 0xF5) {
		need = 4;
		value = c & 0x07U;
	} else
		return (0);

	if (need > n)
		return (0);
	for (i = 1; i < need; i++) {
		if ((s[i] & 0xC0U) != 0x80U)
			return (0);
		value = (value << 6) | (uint32_t)(s[i] & 0x3FU);
	}

	/* reject overlong encodings and values outside Unicode */
	if ((need == 3) && (value < 0x800U))
		return (0);
	if ((need == 4) && (value < 0x10000U))
		return (0);
	if (value > 0x10FFFFU)
		return (0);
	if ((value >= 0xD800U) && (value <= 0xDFFFU))
		return (0);

	*cp = value;
	*len = need;
	return (1);
}

size_t
ee_utf8_validate(const unsigned char *s, size_t n)
{
	size_t i = 0;

	while (i < n) {
		uint32_t cp;
		size_t len;

		if (!ee_utf8_decode(s + i, n - i, &cp, &len))
			return (i);
		i += len;
	}
	return (n);
}

size_t
ee_utf8_encode(uint32_t cp, unsigned char buf[4])
{
	if (cp < 0x80U) {
		buf[0] = (unsigned char)cp;
		return (1);
	}
	if ((cp >= 0xD800U) && (cp <= 0xDFFFU))
		return (0);
	if (cp < 0x800U) {
		buf[0] = (unsigned char)(0xC0U | (cp >> 6));
		buf[1] = (unsigned char)(0x80U | (cp & 0x3FU));
		return (2);
	}
	if (cp < 0x10000U) {
		buf[0] = (unsigned char)(0xE0U | (cp >> 12));
		buf[1] = (unsigned char)(0x80U | ((cp >> 6) & 0x3FU));
		buf[2] = (unsigned char)(0x80U | (cp & 0x3FU));
		return (3);
	}
	if (cp <= 0x10FFFFU) {
		buf[0] = (unsigned char)(0xF0U | (cp >> 18));
		buf[1] = (unsigned char)(0x80U | ((cp >> 12) & 0x3FU));
		buf[2] = (unsigned char)(0x80U | ((cp >> 6) & 0x3FU));
		buf[3] = (unsigned char)(0x80U | (cp & 0x3FU));
		return (4);
	}
	return (0);
}

int
ee_wcwidth(uint32_t cp)
{
	wchar_t wc = (wchar_t)cp;
	int w;

	/*
	 * wchar_t must be able to hold the whole Unicode range for
	 * wcwidth(3) to classify it.  Where it cannot, fall back to a
	 * single cell rather than guessing.
	 */
	if ((uint32_t)WCHAR_MAX < 0x10FFFFU && cp > (uint32_t)WCHAR_MAX)
		return (1);
	w = wcwidth(wc);
	return ((w >= 0) ? w : 1);
}

size_t
ee_utf8_seqlen(const unsigned char *s)
{
	unsigned char c = s[0];

	if (c < 0x80U)
		return (1);
	if (c < 0xC2U)
		return (0);
	if (c < 0xE0U)
		return (2);
	if (c < 0xF0U)
		return (3);
	if (c < 0xF5U)
		return (4);
	return (0);
}

int
ee_utf8_width(const unsigned char *s)
{
	size_t len = ee_utf8_seqlen(s);
	uint32_t cp;
	size_t used;

	if ((len == 0) || !ee_utf8_decode(s, len, &cp, &used) || (used != len))
		return (1);
	return (ee_wcwidth(cp));
}

const unsigned char *
ee_utf8_prev(const unsigned char *start, const unsigned char *ptr)
{
	if (ptr <= start)
		return (start);
	ptr--;
	while ((ptr > start) && ((*ptr & 0xC0U) == 0x80U))
		ptr--;
	return (ptr);
}
