#include <stdio.h>
#include <string.h>

#include "bsdcompat.h"
#include "help.h"

/*
 * An entry lists the key in normal mode, the key in emacs mode (NULL if
 * identical) and the description.  All text is U.S. English.
 */
struct help_entry {
	const char *key;
	const char *emacs;
	const char *text;
};

struct help_section {
	const char		*title;
	const struct help_entry *items;
	int			 count;
};

static const struct help_entry help_navigation[] = {
    {"^B", "^U", "Bottom of file"},
    {"^T", "^T", "Top of file"},
    {"^D", "^N", "Down one line"},
    {"^U", "^P", "Up one line"},
    {"^N", "^V", "Next page"},
    {"^P", "^G", "Previous page"},
    {"^L", "^B", "Left one character"},
    {"^R", "^F", "Right one character"},
    {"^G", "^A", "Beginning of line"},
    {"^O", "^E", "End of line"},
    {"Arrows", "Arrows", "Move the cursor"},
    {"Home/End", "Home/End", "Beginning/end of line"},
    {"PgUp/PgDn", "PgUp/PgDn", "Scroll one page"},
};

static const struct help_entry help_editing[] = {
    {"^A", "^O", "Insert a character by code"},
    {"^J", "^M", "Insert a line break"},
    {"^I", "^I", "Insert a tab (or spaces)"},
    {"^K", "^D", "Delete the character at the cursor"},
    {"^F", "^J", "Restore the last deleted character"},
    {"^W", "^W", "Delete the word at the cursor"},
    {"^V", "^R", "Restore the last deleted word"},
    {"^Y", "^K", "Delete from the cursor to end of line"},
    {"^Z", "^L", "Restore the last deleted line"},
    {"Bksp", "Bksp", "Delete the character before the cursor"},
    {"Del", "Del", "Delete the character at the cursor"},
};

static const struct help_entry help_files[] = {
    {"^S", "^S", "Save the buffer to its file"},
    {"^C", "^C", "Command prompt (write, read, exit, ...)"},
    {"^[ then f", "^[ then f", "File menu: read, write, save, print"},
    {"^[ then a", "^[ then a", "Leave: save, discard or cancel"},
};

static const struct help_entry help_search[] = {
    {"^E", "^Y", "Prompt for a search string"},
    {"^X", "^X", "Repeat the last search"},
    {"case", "case", "Command: case sensitive search"},
    {"nocase", "nocase", "Command: ignore case in search"},
};

static const struct help_entry help_cutpaste[] = {
    {"^K", "^D", "Cut the current character"},
    {"^F", "^J", "Paste the last cut character"},
    {"^W", "^W", "Cut the current word"},
    {"^V", "^R", "Paste the last cut word"},
    {"^Y", "^K", "Cut to the end of the line"},
    {"^Z", "^L", "Paste the last cut line"},
};

static const struct help_entry help_exit[] = {
    {"^Q", "^Q", "Quit; asks to save when modified"},
    {"^[ then a", "^[ then a", "Leave editor (save / no save / cancel)"},
    {"Esc Enter", "Esc Enter", "Leave the editor (historical shortcut)"},
};

static const struct help_entry help_commands[] = {
    {"write", NULL, "Command: write the buffer to a file"},
    {"read", NULL, "Command: read a file into the buffer"},
    {"exit", NULL, "Command: save and leave"},
    {"quit", NULL, "Command: leave (asks to save if modified)"},
    {"file", NULL, "Command: print the file name"},
    {"line", NULL, "Command: print the current line number"},
    {"character", NULL, "Command: print the code of the current character"},
    {"0-9", NULL, "Command: go to the given line"},
    {"case", NULL, "Command: case sensitive search"},
    {"nocase", NULL, "Command: ignore case in search"},
    {"expand", NULL, "Command: expand tabs to spaces"},
    {"noexpand", NULL, "Command: keep tabs as tabs"},
    {"help", NULL, "Command: show this screen"},
    {"!cmd", NULL, "Command: run \"cmd\" in the shell"},
    {"<cmd", NULL, "Command: pipe the buffer into \"cmd\""},
    {">cmd", NULL, "Command: pipe the buffer to \"cmd\""},
    {"author", NULL, "Command: print the author"},
    {"redraw", NULL, "Command: repaint the screen"},
    {"resequence", NULL, "Command: renumber the lines"},
};

static const struct help_entry help_advanced[] = {
    {"Esc", "Esc", "Open the main menu"},
    {"F1", "F1", "Select the gold (alternate) function set"},
    {"F2-F8", "F2-F8", "Function keys (gold variants undo)"},
    {"margins", "margins", "Init file: truncate at the right margin"},
    {"nomargins", "nomargins", "Init file: let lines run past the margin"},
    {"autoformat", "autoformat", "Init file: format paragraphs while typing"},
    {"noautoformat", "noautoformat", "Init file: stop automatic formatting"},
    {"printcommand", "printcommand", "Init file: print command (default lpr)"},
    {"rightmargin", "rightmargin", "Init file: right margin column"},
};

static const struct help_section help_sections[] = {
    {"Navigation", help_navigation,
	(int)(sizeof(help_navigation) / sizeof(help_navigation[0]))},
    {"Editing", help_editing,
	(int)(sizeof(help_editing) / sizeof(help_editing[0]))},
    {"Files", help_files, (int)(sizeof(help_files) / sizeof(help_files[0]))},
    {"Search", help_search,
	(int)(sizeof(help_search) / sizeof(help_search[0]))},
    {"Cut and paste", help_cutpaste,
	(int)(sizeof(help_cutpaste) / sizeof(help_cutpaste[0]))},
    {"Exit", help_exit, (int)(sizeof(help_exit) / sizeof(help_exit[0]))},
    {"Commands (press ^C, then type the name)", help_commands,
	(int)(sizeof(help_commands) / sizeof(help_commands[0]))},
    {"Advanced commands and settings", help_advanced,
	(int)(sizeof(help_advanced) / sizeof(help_advanced[0]))},
};

/* The trailing "Command line" block. */
static const char *const help_usage[] = {
    "  ee [+#] [-i] [-e] [-h] [file(s)]",
    "  +#  start at line #        -i  no shortcut bar",
    "  -e  do not expand tabs     -h  no reverse video",
};

int
ee_help_count(int)
{
	int    total = 0;
	size_t s;

	for (s = 0; s < sizeof(help_sections) / sizeof(help_sections[0]); s++)
		total +=
		    1 + help_sections[s].count + 1; /* header, items, blank */
	total += 1 + (int)(sizeof(help_usage) / sizeof(help_usage[0]));
	return (total);
}

void
ee_help_line(
    int emacs_keys_mode, int index, char *out, size_t outsz, int *is_header)
{
	int    i = 0;
	size_t s;

	*is_header = 0;
	out[0] = '\0';

	for (s = 0; s < sizeof(help_sections) / sizeof(help_sections[0]); s++) {
		const struct help_section *sec = &help_sections[s];
		int			   j;

		if (i == index) {
			snprintf(out, outsz, "%s", sec->title);
			*is_header = 1;
			return;
		}
		i++;
		for (j = 0; j < sec->count; j++) {
			const char *key;
			char	    keybuf[24];

			if (i == index) {
				key = sec->items[j].key;
				if (emacs_keys_mode &&
				    (sec->items[j].emacs != NULL))
					key = sec->items[j].emacs;
				/*
				 * Bound the key field so that a long
				 * label can never push the description
				 * out of the caller's buffer.
				 */
				snprintf(keybuf, sizeof(keybuf), "%s", key);
				snprintf(out, outsz, "  %-12s %s", keybuf,
				    sec->items[j].text);
				return;
			}
			i++;
		}
		if (i == index)
			return; /* blank separator line */
		i++;
	}

	if (i == index) {
		snprintf(out, outsz, "Command line");
		*is_header = 1;
		return;
	}
	i++;
	{
		size_t u;

		for (u = 0; u < sizeof(help_usage) / sizeof(help_usage[0]);
		    u++) {
			if (i == index) {
				snprintf(out, outsz, "%s", help_usage[u]);
				return;
			}
			i++;
		}
	}
}
