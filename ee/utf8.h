#ifndef EE_UTF8_H
#define EE_UTF8_H

/*
 * Small, strict UTF-8 helpers used by ee.
 *
 * ee stores text as UTF-8 in byte buffers.  These routines are the only
 * place that knows how to walk or validate UTF-8; every other part of
 * the editor works with the results.  The interface is deliberately
 * tiny: there is no general-purpose Unicode library here, only the
 * operations the editor needs.
 *
 * The routines are strict, not merely tolerant:
 *
 *   - sequences longer than four bytes are rejected;
 *   - overlong encodings are rejected;
 *   - UTF-16 surrogate code points (U+D800..U+DFFF) are rejected;
 *   - code points above U+10FFFF are rejected;
 *   - a truncated sequence, a stray continuation byte or an invalid
 *     lead byte is rejected.
 *
 * A byte that begins an invalid sequence has length one and is not a
 * character; callers that have validated their input never see one.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * Decode the UTF-8 sequence at s, examining at most n bytes.  On
 * success stores the code point in *cp and its byte length in *len and
 * returns 1.  On any invalid or truncated sequence returns 0 and leaves
 * *cp and *len unspecified.  A NUL byte terminates the sequence and is
 * treated as invalid for the purpose of text decoding.
 */
int ee_utf8_decode(const unsigned char *s, size_t n, uint32_t *cp,
    size_t *len);

/*
 * Validate n bytes of UTF-8.  Returns the offset of the first byte that
 * is not part of a valid sequence, or n if the whole buffer is valid.
 * NUL bytes are considered valid code points here; the editor rejects
 * them separately as a binary-file policy.
 */
size_t ee_utf8_validate(const unsigned char *s, size_t n);

/*
 * Encode the code point cp into buf, which must have room for four
 * bytes.  Returns the number of bytes written, or 0 if cp cannot be
 * represented in UTF-8 (surrogate or out of range).
 */
size_t ee_utf8_encode(uint32_t cp, unsigned char buf[4]);

/*
 * Display width of a code point in terminal cells, using wcwidth(3) in
 * the current locale.  Returns 0 for a combining mark, 1 for an
 * ordinary character and 2 for a wide character.  A value wcwidth(3)
 * cannot classify (including control characters) is reported as 1.
 */
int ee_wcwidth(uint32_t cp);

/*
 * Display width of the valid sequence at s (which must be at least
 * ee_utf8_seqlen(s) bytes).  Invalid input is reported as width 1.
 * The caller is responsible for passing a pointer into validated text.
 */
int ee_utf8_width(const unsigned char *s);

/*
 * Byte length of the sequence starting at s.  Returns 0 for an invalid
 * lead byte.  As with ee_utf8_width(), s is expected to point into
 * validated text.
 */
size_t ee_utf8_seqlen(const unsigned char *s);

/*
 * Return a pointer to the first byte of the previous character, given
 * the start of the line and a pointer to the current character.  ptr
 * must be within [start, ...] and aligned to a character boundary.
 */
const unsigned char *ee_utf8_prev(const unsigned char *start,
    const unsigned char *ptr);

#endif /* EE_UTF8_H */
