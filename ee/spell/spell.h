#ifndef EE_SPELL_H
#define EE_SPELL_H

/*
 * Small, original affix-based spell checker for ee.
 *
 * This is an ee component written from scratch in C23; it shares no
 * code or data with any third-party spell checker.  It reads the common
 * ".aff"/".dic" dictionary format so that data files in that format can
 * be used, but the implementation is ee's own.
 *
 * The scope is deliberately tiny: one dictionary (typically en_US),
 * exact word lookup, prefix and suffix stripping, and a small set of
 * one-edit suggestions.  There is no process, no thread and no network
 * use; the dictionary is read once, on request.
 *
 * See spell/README.md for the format subset and spell.c for the algorithm.
 */

#include <stddef.h>

struct ee_spell;

/*
 * Open a dictionary.  `aff_path` may be NULL (only exact words are then
 * recognised).  `dic_path` is required.  Returns NULL on failure.
 */
struct ee_spell *ee_spell_open(const char *aff_path, const char *dic_path);

void ee_spell_close(struct ee_spell *sp);

/*
 * Return 1 when `word` (length `len`, not NUL-terminated) is a known
 * word, 0 otherwise.
 */
int ee_spell_check(struct ee_spell *sp, const char *word, size_t len);

/*
 * Write up to `max` suggestions for `word` into `out`, a caller-owned
 * array of `max` rows of 64 bytes.  Returns the number of suggestions
 * written, or 0 when the word is known or nothing was found.
 */
int ee_spell_suggest(
    struct ee_spell *sp, const char *word, size_t len, char out[][64], int max);

#endif /* EE_SPELL_H */
