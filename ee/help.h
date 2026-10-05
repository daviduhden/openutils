#ifndef EE_HELP_H
#define EE_HELP_H

/*
 * Built-in help text for ee.
 *
 * The help content is kept out of the editor proper so that it can be
 * reviewed and translated to the terminal size independently.  The
 * content is U.S. English only.  The caller asks for the number of
 * logical lines and for each line in turn; headers are flagged so that
 * the renderer can emphasise them.
 */

#include <stddef.h>

/*
 * Key binding sets.  The help screen and the editor must agree on these
 * values; they live here so that both translation units share them.
 */
#define EE_KEYS_EE 0
#define EE_KEYS_EMACS 1
#define EE_KEYS_VI 2

/* Number of logical help lines for the active key binding set. */
int ee_help_count(int keys_mode);

/*
 * Fill out with logical help line `index` (0-based), NUL-terminated and
 * truncated to outsz.  Sets *is_header to 1 for a section header.  If
 * index is out of range, out is set to the empty string.
 */
void ee_help_line(
    int keys_mode, int index, char *out, size_t outsz, int *is_header);

#endif /* EE_HELP_H */
