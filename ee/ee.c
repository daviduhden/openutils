#include "bsdcompat.h"

#include "help.h"
#include "utf8.h"

/*
 * Terminal handling is provided by ncursesw.  The editor deliberately
 * uses no colour, no mouse and no panels; only the standard monochrome
 * attributes (standout/underline) are used.
 */
#include <curses.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define TAB 9

/*
 *	SIGINT handler: only set a flag; terminal restoration happens in
 *	normal context (edit_abort() is called from the main loop).
 */
static volatile sig_atomic_t ee_intr_flag;

static void
ee_on_sigint(int sig)
{
	(void)sig;
	ee_intr_flag = 1;
}

/*
 *	Install the SIGINT handler.  SA_RESTART must NOT be set: the
 *	blocking read(2) has to return EINTR so that the main loop can
 *	see ee_intr_flag promptly; otherwise a signal received while
 *	idle would only take effect on the next keystroke.
 */
static void
ee_install_sigint(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = ee_on_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
}

static int
ee_max(int a, int b)
{
	return (a > b ? a : b);
}

static int
ee_min(int a, int b)
{
	return (a < b ? a : b);
}

#define MENU_ITEM_COL	3

struct text {
	unsigned char *line;		/* line of characters		*/
	int line_number;		/* line number			*/
	int line_length;	/* actual number of characters in the line */
	int max_length;	/* maximum number of characters the line handles */
	struct text *next_line;		/* next line of text		*/
	struct text *prev_line;		/* previous line of text	*/
};

struct text *first_line;	/* first line of current buffer		*/
struct text *dlt_line;		/* structure for info on deleted line	*/
struct text *curr_line;		/* current line cursor is on		*/
struct text *tmp_line;		/* temporary line pointer		*/
struct text *srch_line;		/* temporary pointer for search routine */

struct files {		/* structure to store names of files to be edited*/
	unsigned char *name;		/* name of file				*/
	struct files *next_name;
};

struct files *top_of_stack = NULL;

/*
 * Cursor state.  Three different units are involved and must not be
 * confused:
 *
 *   point         pointer to the first byte of the current UTF-8
 *                 sequence within the current line (always on a
 *                 character boundary);
 *   position      1-based byte offset of point from the beginning of
 *                 the line;
 *   scr_horz      display column of the cursor, counted in terminal
 *                 cells (wcwidth units), not bytes or code points;
 *   horiz_offset  leftmost display column currently shown, in cells.
 *
 * scr_vert is the cursor row relative to text_win, and scr_pos is the
 * remembered display column used when moving vertically.  left() and
 * right() move by whole UTF-8 sequences so that point never lands in
 * the middle of a multibyte character.
 */
int d_wrd_len;			/* length of deleted word		*/
int position;			/* byte offset (1-based) in line	*/
int scr_pos;			/* horizontal position			*/
int scr_vert;			/* vertical position on screen		*/
int scr_horz;			/* horizontal position on screen	*/
int absolute_lin;		/* number of lines from top		*/
int tmp_vert, tmp_horz;
int input_file;			/* indicate to read input file		*/
int recv_file;			/* indicate reading a file		*/
int edit;			/* continue executing while true	*/
int gold;			/* 'gold' function key pressed		*/
int case_sen;			/* case sensitive search flag		*/
int last_line;			/* last line for text display		*/
int last_col;			/* last column for text display		*/
int horiz_offset = 0;		/* offset from left edge of text	*/
int clear_com_win;		/* flag to indicate com_win needs clearing */
int text_changes = FALSE;	/* indicate changes have been made to text */
int read_only = FALSE;		/* current file is not writable		*/
int get_fd;			/* file descriptor for reading a file	*/
int info_window = TRUE;		/* flag to indicate if shortcut bar visible */
int expand_tabs = TRUE;		/* flag for expanding tabs		*/
int right_margin = 0;		/* the right margin 			*/
int observ_margins = TRUE;	/* flag for whether margins are observed */
int shell_fork;
int pipe_out[2];		/* pipe file desc for output		*/
int pipe_in[2];			/* pipe file descriptors for input	*/
int out_pipe;			/* flag that info is piped out		*/
int in_pipe;			/* flag that info is piped in		*/
int formatted = FALSE;		/* flag indicating paragraph formatted	*/
int auto_format = FALSE;	/* flag for auto_format mode		*/
int restricted = FALSE;		/* flag to indicate restricted mode	*/
int nohighlight = FALSE;	/* turns off highlighting		*/
int local_LINES = 0;		/* copy of LINES, to detect when win resizes */
int local_COLS = 0;		/* copy of COLS, to detect when win resizes  */
int curses_initialized = FALSE;	/* flag indicating if curses has been started*/
int emacs_keys_mode = FALSE;	/* mode for if emacs key binings are used    */

/*
 * Total number of lines in the buffer.  Maintained incrementally by
 * the routines that split or join lines and recomputed after a whole
 * file is read, so that the status bar can show a percentage without
 * walking the list on every keystroke.
 */
long total_lines = 1;

/* Name the editor was invoked as ("ee", "ree" or "edit"). */
const char *prog_name = "ee";

/*
 * Visual regions.  The layout is computed in one place (set_up_term)
 * from the terminal size; the rest of the editor only refers to the
 * derived windows and to the editor-area bounds (text_top,
 * text_rows, last_line).  No scattered LINES-1/COLS-2 arithmetic.
 *
 *   title_win   the top status/title bar (always present when it fits)
 *   text_win    the editable text area
 *   com_win     the prompt/message/status-input line
 *   key_win     the contextual shortcut bar (the "info window")
 *   help_win    full-screen help overlay
 */
int text_top;			/* first editor row (screen coordinates) */
int text_rows;			/* number of editor rows		*/

unsigned char *point;		/* points to current position in line	*/
unsigned char *srch_str;	/* pointer for search string		*/
unsigned char *u_srch_str;	/* pointer to non-case sensitive search	*/
unsigned char *srch_1;		/* pointer to start of suspect string	*/
unsigned char *srch_2;		/* pointer to next character of string	*/
unsigned char *srch_3;
unsigned char *in_file_name = NULL;	/* name of input file		*/
char *tmp_file;	/* temporary file name			*/
unsigned char d_char[5];	/* deleted character			*/
unsigned char *d_word;		/* deleted word				*/
unsigned char *d_line;		/* deleted line				*/

unsigned char *print_command = (unsigned char *)"lpr";	/* string to use for the print command 	*/
unsigned char *start_at_line = NULL;	/* move to this line at start of session*/
int in;				/* input character			*/

FILE *temp_fp;			/* temporary file pointer		*/
FILE *bit_bucket;		/* file pointer to /dev/null		*/

char *table[] = {
	"^@", "^A", "^B", "^C", "^D", "^E", "^F", "^G", "^H", "\t", "^J",
	"^K", "^L", "^M", "^N", "^O", "^P", "^Q", "^R", "^S", "^T", "^U",
	"^V", "^W", "^X", "^Y", "^Z", "^[", "^\\", "^]", "^^", "^_"
};

WINDOW *com_win;
WINDOW *text_win;
WINDOW *help_win;
WINDOW *title_win;
WINDOW *key_win;

/*
 * Contextual shortcut bar.  The current set of labels is chosen by the
 * operation in progress (editing, prompt, search, confirmation) and
 * rendered left to right, dropping items that do not fit.
 */
#define SHORTCUT_MAX 14
struct shortcut_bar {
	const char *items[SHORTCUT_MAX];
	int count;
};
static struct shortcut_bar shortcut_bar;

/*
 * Thin adapters over the strict UTF-8 module (utf8.c).  The names are
 * historical; all of the decisions live in utf8.c so that the editor
 * has exactly one implementation of "what is a character".
 */

/* Byte length of the character at s (1 for an invalid lead byte). */
static int
utf8_len(const unsigned char *s)
{
	size_t len = ee_utf8_seqlen(s);

	return ((len == 0) ? 1 : (int)len);
}

/* Pointer to the first byte of the previous character. */
static unsigned char *
utf8_prev(const unsigned char *start, const unsigned char *ptr)
{
	return ((unsigned char *)ee_utf8_prev(start, ptr));
}

/* Display width of the character at s (1 if it cannot be decoded). */
static int
utf8_width(const unsigned char *s)
{
	return (ee_utf8_width(s));
}

/* Return the number of terminal columns occupied by a UTF-8 string. */
static int
utf8_strwidth(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	int width = 0;

	while (*p != '\0') {
		size_t len = ee_utf8_seqlen(p);

		if (*p < 0x80) {
			width++;
			p++;
		} else if (len == 0) {
			width++;
			p++;
		} else {
			width += utf8_width(p);
			p += len;
		}
	}
	return (width);
}

/*
 * Allocate n bytes for an internal buffer whose failure cannot be
 * reported usefully.  On exhaustion the terminal is restored and the
 * editor exits rather than continuing with a NULL pointer.
 */
static void *
xmalloc(size_t n)
{
	void *p = malloc(n);

	if (p == NULL) {
		endwin();
		fprintf(stderr, "ee: out of memory\n");
		exit(1);
	}
	return (p);
}

/* Recount the lines in the buffer (used after a whole file is read). */
static void
recount_lines(void)
{
	struct text *line;

	total_lines = 0;
	for (line = first_line; line != NULL; line = line->next_line)
		total_lines++;
	if (total_lines < 1)
		total_lines = 1;
}

/*
 |	The following structure allows menu items to be flexibly declared.
 |	The first item is the string describing the selection, the second
 |	is the address of the procedure to call when the item is selected,
 |	and the third is the argument for the procedure.
 |
 |	The first menu item will be the title of the menu, with NULL
 |	parameters for the procedure and argument, followed by the menu items.
 |
 |	If the procedure value is NULL, the menu item is displayed, but no
 |	procedure is called when the item is selected.  The number of the
 |	item will be returned.  If the third (argument) parameter is -1, no
 |	argument is given to the procedure when it is called.
 */

struct menu_entries {
	char *item_string;
	int (*procedure)(struct menu_entries *);
	struct menu_entries *ptr_argument;
	int (*iprocedure)(int);
	void (*nprocedure)(void);
	int argument;
};

static unsigned char *resiz_line(int factor, struct text *rline, int rpos);
static void insert(int character);
static void insert_utf8(const unsigned char *mb, int len);
static void insert_code(int code);
static void delete(int disp);
static void scanline(unsigned char *pos);
static int tabshift(int temp_int);
static int out_char(WINDOW *window, int character, int column);
static int len_char(int character, int column);
static void draw_line(int vertical, int horiz, unsigned char *ptr, int t_pos,
    int length);
static void insert_line(int disp);
static struct text *txtalloc(void);
static struct files *name_alloc(void);
static char *next_word(char *string);
static void prev_word(void);
static void control(void);
static void emacs_control(void);
static void bottom(void);
static void top(void);
static void nextline(void);
static void prevline(void);
static void left(int disp);
static void right(int disp);
static void find_pos(void);
static void up(void);
static void down(void);
static void function_key(void);
static void print_buffer(void);
static void command_prompt(void);
static void command(char *cmd_str1);
static int scan(char *line, int offset, int column);
static char *get_string(const char *prompt, int advance);
static int compare(const char *string1, const char *string2, int sensitive);
static void goto_line(char *cmd_str);
static void midscreen(int line, unsigned char *pnt);
static void get_options(int numargs, char *arguments[]);
static void check_fp(void);
static void get_file(char *file_name);
static int read_all(int fd, unsigned char **out, size_t *outlen);
static void get_line(int length, unsigned char *input, int *append);
static void draw_screen(void);
static void finish(void);
static int quit(int noverify);
static void edit_abort(int arg);
static void delete_text(void);
static int write_file(char *file_name, int warn_if_exists);
static int search(int display_message);
static void search_prompt(void);
static void del_char(void);
static void undel_char(void);
static void del_word(void);
static void undel_word(void);
static void del_line(void);
static void undel_line(void);
static void adv_word(void);
static void move_rel(int direction, int lines);
static void eol(void);
static void bol(void);
static void adv_line(void);
static void sh_command(char *string);
static void set_up_term(void);
static void resize_check(void);
static int menu_op(struct menu_entries *);
static void paint_menu(struct menu_entries menu_list[], int max_width,
    int max_height, int list_size, int top_offset, WINDOW *menu_win,
    int off_start, int vert_size, int selection);
static void help(void);
static void paint_status_line(void);
static void paint_shortcut_bar(void);
static void set_shortcuts(const char *const *items, int count);
static void default_shortcuts(void);
static void no_info_window(void);
static void create_info_window(void);
static int file_op(int arg);
static int save_op(void);
static void shell_op(void);
static void leave_op(void);
static void redraw(void);
static int confirm(const char *question);
static int utf8_strwidth(const char *s);
static void *xmalloc(size_t n);
static void recount_lines(void);
static int Blank_Line(struct text *test_line);
static void Format(void);
static void ee_init(void);
static void dump_ee_conf(void);
static void echo_string(char *string);
static void spell_op(void);
static void ispell_op(void);
static int first_word_len(struct text *test_line);
static void Auto_Format(void);
static void modes_op(void);
static int append_mem(char **buf, size_t *len, size_t *cap, const char *s,
    size_t n);
static char *resolve_name(char *name);
static int restrict_mode(void);
static int unique_test(const char *string, const char *list[]);
static int locale_is_configured(void);
static void select_utf8_locale(void);
static void strings_init(void);

#undef P_
/*
 |	allocate space here for the strings that will be in the menu
 */

/*
 * Menu labels are static data.  The seven mode entries are filled with
 * generated strings at run time (mode_strings[]); everything else is a
 * literal in the initializer, so there is no run-time string wiring.
 */
struct menu_entries modes_menu[] = {
	{"modes menu", NULL, NULL, NULL, NULL, 0},	/* title */
	{"", NULL, NULL, NULL, NULL, -1},		/* 1. tabs */
	{"", NULL, NULL, NULL, NULL, -1},		/* 2. case */
	{"", NULL, NULL, NULL, NULL, -1},		/* 3. margins */
	{"", NULL, NULL, NULL, NULL, -1},		/* 4. autoformat */
	{"", NULL, NULL, NULL, NULL, -1},		/* 5. info window */
	{"", NULL, NULL, NULL, NULL, -1},		/* 6. emacs keys */
	{"", NULL, NULL, NULL, NULL, -1},		/* 7. right margin */
	{"save editor configuration", NULL, NULL, NULL, dump_ee_conf, -1},
	{NULL, NULL, NULL, NULL, NULL, -1}		/* terminator */
};

const char *mode_strings[9];

#define NUM_MODES_ITEMS 8
#define MODES_ITEM_SIZE 80

struct menu_entries config_dump_menu[] = {
	{"save ee configuration", NULL, NULL, NULL, NULL, 0},
	{"save in current directory", NULL, NULL, NULL, NULL, -1},
	{"save in home directory", NULL, NULL, NULL, NULL, -1},
	{NULL, NULL, NULL, NULL, NULL, -1}
};

struct menu_entries leave_menu[] = {
	{"leave menu", NULL, NULL, NULL, NULL, -1},
	{"save changes", NULL, NULL, NULL, finish, -1},
	{"no save", NULL, NULL, quit, NULL, TRUE},
	{NULL, NULL, NULL, NULL, NULL, -1}
};

#define READ_FILE 1
#define WRITE_FILE 2
#define SAVE_FILE 3

struct menu_entries file_menu[] = {
	{"file menu", NULL, NULL, NULL, NULL, -1},
	{"read a file", NULL, NULL, file_op, NULL, READ_FILE},
	{"write a file", NULL, NULL, file_op, NULL, WRITE_FILE},
	{"save file", NULL, NULL, file_op, NULL, SAVE_FILE},
	{"print editor contents", NULL, NULL, NULL, print_buffer, -1},
	{NULL, NULL, NULL, NULL, NULL, -1}
};

struct menu_entries search_menu[] = {
	{"search menu", NULL, NULL, NULL, NULL, 0},
	{"search for ...", NULL, NULL, NULL, search_prompt, -1},
	{"search", NULL, NULL, search, NULL, TRUE},
	{NULL, NULL, NULL, NULL, NULL, -1}
};

struct menu_entries spell_menu[] = {
	{"spell menu", NULL, NULL, NULL, NULL, -1},
	{"use 'spell'", NULL, NULL, NULL, spell_op, -1},
	{"use 'ispell'", NULL, NULL, NULL, ispell_op, -1},
	{NULL, NULL, NULL, NULL, NULL, -1}
};

struct menu_entries misc_menu[] = {
	{"miscellaneous menu", NULL, NULL, NULL, NULL, -1},
	{"format paragraph", NULL, NULL, NULL, Format, -1},
	{"shell command", NULL, NULL, NULL, shell_op, -1},
	{"check spelling", menu_op, spell_menu, NULL, NULL, -1},
	{NULL, NULL, NULL, NULL, NULL, -1}
};

struct menu_entries main_menu[] = {
	{"main menu", NULL, NULL, NULL, NULL, -1},
	{"leave editor", NULL, NULL, NULL, leave_op, -1},
	{"help", NULL, NULL, NULL, help, -1},
	{"file operations", menu_op, file_menu, NULL, NULL, -1},
	{"redraw screen", NULL, NULL, NULL, redraw, -1},
	{"settings", NULL, NULL, NULL, modes_op, -1},
	{"search", menu_op, search_menu, NULL, NULL, -1},
	{"miscellaneous", menu_op, misc_menu, NULL, NULL, -1},
	{NULL, NULL, NULL, NULL, NULL, -1}
};


#define MENU_WARN 1

#define max_alpha_char 36

/*
 |	Interface strings (hard-coded U.S. English).
 |
 |	Plain text is a static const array.  Text that is used as a printf
 |	format is a macro so that the format is a compile-time literal at
 |	every call site (the compiler can then check the arguments, and
 |	there is no runtime format string or mutable global).  None of
 |	this is localizable: the interface is English (United States)
 |	only, with no message catalogs.
 */

/* Formatted messages (used as printf formats). */
#define printer_msg_str "sending contents of buffer to \"%s\" "
#define unkn_cmd_str "unknown command \"%s\""
#define line_num_str "line %d  "
#define line_len_str "length = %d"
#define current_file_str "current file is \"%s\" "
#define char_str "character = %d"
#define file_is_dir_msg "\"%s\" is a directory"
#define new_file_msg "new file \"%s\""
#define cant_open_msg "cannot open \"%s\""
#define not_text_file_msg "\"%s\" is not a text file (contains a NUL byte)"
#define invalid_utf8_msg "\"%s\" is not valid UTF-8 at byte %lu"
#define file_read_fin_msg "finished reading file \"%s\""
#define reading_file_msg "reading file \"%s\""
#define file_read_lines_msg "file \"%s\", %d lines"
#define conf_dump_success_msg "ee configuration saved in file %s"
#define str_not_found_msg "string \"%s\" not found"

/* Plain messages and prompts. */
static const char com_win_message[] = "    press Escape (^[) for menu";
static const char no_file_string[] = "no file";
static const char ascii_code_str[] = "Character code (decimal): ";
static const char command_str[] = "Command: ";
static const char file_write_prompt_str[] = "File name to write: ";
static const char file_read_prompt_str[] = "File name to read: ";
static const char non_unique_cmd_msg[] = "entered command is not unique";
static const char read_only_msg[] = ", read only";
static const char save_file_name_prompt[] = "File name: ";
static const char file_not_saved_msg[] = "no filename entered: file not saved";
static const char searching_msg[] = "           ...searching";
static const char search_prompt_str[] = "Search for: ";
static const char continue_msg[] = "press return to continue ";
static const char menu_cancel_msg[] = "press Esc to cancel";
static const char shell_prompt[] = "Shell command: ";
static const char formatting_msg[] = "...formatting paragraph...";
static const char spell_in_prog_msg[] =
    "sending contents of edit buffer to 'spell'";
static const char margin_prompt[] = "Right margin: ";
static const char restricted_msg[] =
    "restricted mode: unable to perform requested operation";
static const char conf_dump_err_msg[] =
    "unable to open .init.ee for writing, no configuration saved!";
static const char conf_not_saved_msg[] = "ee configuration not saved";
static const char ree_no_file_msg[] = "must specify a file when invoking ree";
static const char menu_too_lrg_msg[] = "menu too large for window";
static const char more_above_str[] = "^^more^^";
static const char more_below_str[] = "VVmoreVV";

/* Mode and command names. */
static const char ON[] = "ON";
static const char OFF[] = "OFF";
static const char HELP[] = "HELP";
static const char WRITE[] = "WRITE";
static const char READ[] = "READ";
static const char LINE[] = "LINE";
static const char FILE_str[] = "FILE";
static const char CHARACTER[] = "CHARACTER";
static const char REDRAW[] = "REDRAW";
static const char RESEQUENCE[] = "RESEQUENCE";
static const char AUTHOR[] = "AUTHOR";
static const char CASE[] = "CASE";
static const char NOCASE[] = "NOCASE";
static const char EXPAND[] = "EXPAND";
static const char NOEXPAND[] = "NOEXPAND";
static const char Exit_string[] = "EXIT";
static const char QUIT_string[] = "QUIT";
static const char INFO[] = "INFO";
static const char NOINFO[] = "NOINFO";
static const char MARGINS[] = "MARGINS";
static const char NOMARGINS[] = "NOMARGINS";
static const char AUTOFORMAT[] = "AUTOFORMAT";
static const char NOAUTOFORMAT[] = "NOAUTOFORMAT";
static const char Echo[] = "ECHO";
static const char PRINTCOMMAND[] = "PRINTCOMMAND";
static const char RIGHTMARGIN[] = "RIGHTMARGIN";
static const char HIGHLIGHT[] = "HIGHLIGHT";
static const char NOHIGHLIGHT[] = "NOHIGHLIGHT";
static const char EMACS_string[] = "EMACS";
static const char NOEMACS_string[] = "NOEMACS";

/* A command string that command() may advance through, hence mutable. */
static char shell_echo_msg[] =
    "<!echo 'list of unrecognized words'; echo -=-=-=-=-=-";
const char *commands[] = {
	HELP, WRITE, READ, LINE, FILE_str, REDRAW, RESEQUENCE, AUTHOR,
	CASE, NOCASE, EXPAND, NOEXPAND, Exit_string, QUIT_string,
	"<", ">", "!", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
	CHARACTER, NULL
};

const char *init_strings[] = {
	CASE, NOCASE, EXPAND, NOEXPAND, INFO, NOINFO, MARGINS, NOMARGINS,
	AUTOFORMAT, NOAUTOFORMAT, Echo, PRINTCOMMAND, RIGHTMARGIN,
	HIGHLIGHT, NOHIGHLIGHT, EMACS_string, NOEMACS_string, NULL
};

/* beginning of main program		*/
int
main(int argc, char *argv[])
{
	int counter;

	for (counter = 1; counter < 24; counter++)
		signal(counter, SIG_IGN);

	setprogname(argv[0]);
	prog_name = getprogname();

	/* Always read from (and write to) a terminal. */
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr,
		    "ee's standard input and output must be a terminal\n");
		exit(1);
	}

	/*
	 * Establish the UTF-8 LC_CTYPE invariant before any editor state
	 * is built.  Failure is fatal and leaves the terminal untouched.
	 */
	select_utf8_locale();

	signal(SIGCHLD, SIG_DFL);
	signal(SIGSEGV, SIG_DFL);
	ee_install_sigint();
	d_word = xmalloc(150);
	*d_word = '\0';
	d_line = NULL;
	dlt_line = txtalloc();
	dlt_line->line = d_line;
	dlt_line->line_length = 0;
	curr_line = first_line = txtalloc();
	curr_line->line = point = xmalloc(10);
	curr_line->line_length = 1;
	curr_line->max_length = 10;
	curr_line->prev_line = NULL;
	curr_line->next_line = NULL;
	curr_line->line_number = 1;
	srch_str = NULL;
	u_srch_str = NULL;
	position = 1;
	scr_pos = 0;
	scr_vert = 0;
	scr_horz = 0;
	absolute_lin = 1;
	bit_bucket = fopen("/dev/null", "w");
	edit = TRUE;
	gold = case_sen = FALSE;
	shell_fork = TRUE;
	strings_init();
	ee_init();
	if (argc > 0)
		get_options(argc, argv);
	/*
	 * The editor edits files (rpath/wpath/cpath), uses the terminal
	 * (tty), runs shell commands and spell(1) (proc/exec) and
	 * expands ~ (getpw).  No network, no other privileges.  Since
	 * the editor must be able to edit arbitrary files and spawn
	 * arbitrary commands, unveil(2) cannot usefully restrict it.
	 */
	if (pledge("stdio rpath wpath cpath tty proc exec getpw", NULL) == -1)
		err(1, "pledge");
	set_up_term();
	if (right_margin == 0)
		right_margin = last_col;
	if (top_of_stack == NULL) {
		if (restrict_mode()) {
			wmove(com_win, 0, 0);
			werase(com_win);
			wprintw(com_win, "%s", ree_no_file_msg);
			wrefresh(com_win);
			edit_abort(0);
		}
		wprintw(com_win, "%s", no_file_string);
		wrefresh(com_win);
	} else
		check_fp();

	clear_com_win = TRUE;

	counter = 0;

	while (edit) {
		/*
		 |  refresh the persistent chrome: title/status bar and the
		 |  contextual shortcut bar.  Messages and prompts live in
		 |  com_win and are not touched here.
		 */
		paint_status_line();
		paint_shortcut_bar();

		wrefresh(text_win);
		{
			wint_t win;
			int wret = wget_wch(text_win, &win);
			/*
			 * ERR if the underneath terminal is closed (like network failure on a ssh
			 * session)
			 * Normal exit as this is not an editor's error, but a network connection
			 * issue
			 */
			if (wret == ERR) {
				/* SIGINT seen: leave the editor cleanly */
				if (ee_intr_flag)
					edit_abort(0);
				if (errno == EINTR)
					continue;
				exit(0);
			}

			/* SIGINT may have arrived while not blocked in
			 * input; the flag must also be honoured after a
			 * successful read */
			if (ee_intr_flag)
				edit_abort(0);

			/* ncurses reports a window-size change as KEY_RESIZE;
			 * rebuild the layout and redraw before reading the
			 * next key. */
			if ((wret == KEY_CODE_YES) && (win == KEY_RESIZE)) {
				resize_check();
				continue;
			}

			in = (int)win;

			resize_check();

			if (clear_com_win) {
				clear_com_win = FALSE;
				wmove(com_win, 0, 0);
				werase(com_win);
				if (!info_window) {
					wprintw(com_win, "%s", com_win_message);
				}
				wrefresh(com_win);
			}

			if (wret == KEY_CODE_YES)
				function_key();
			else if ((in == '\10') || (in == 127)) {
				in = 8;
				delete(TRUE);
			} else if (in >= 0x80) {
				unsigned char mb[MB_LEN_MAX + 1];
				mbstate_t mbs;
				memset(&mbs, 0, sizeof(mbs));
				size_t n = wcrtomb((char *)mb, (wchar_t)win,
				    &mbs);
				if (n != (size_t)-1)
					insert_utf8(mb, (int)n);
			} else if ((in > 31) || (in == 9))
				insert(in);
			else if ((in >= 0) && (in <= 31)) {
				if (emacs_keys_mode)
					emacs_control();
				else
					control();
			}
			default_shortcuts();
		}
	}
	return (0);
}

/* resize the line to length + factor*/
static unsigned char *
resiz_line(int factor, struct text *rline, int rpos)
{
	unsigned char *rpoint;
	unsigned char *nb;
	int resiz_var;
	long newlen;

	/*
	 * max_length is an int and has historically been allowed to grow
	 * with the line; refuse an impossible growth rather than let the
	 * arithmetic wrap.
	 */
	if ((factor > 0) && (rline->max_length > (INT_MAX - factor))) {
		endwin();
		fprintf(stderr, "ee: line too long\n");
		exit(1);
	}
	newlen = (long)rline->max_length + factor;
	if (newlen < 1)
		newlen = 1;
	nb = reallocarray(rline->line, (size_t)newlen, 1);
	if (nb == NULL) {
		endwin();
		fprintf(stderr, "ee: out of memory\n");
		exit(1);
	}
	rline->line = nb;
	rline->max_length = (int)newlen;
	rpoint = rline->line;
	for (resiz_var = 1; (resiz_var < rpos); resiz_var++)
		rpoint++;
	return (rpoint);
}

/* insert character into line		*/
static void
insert(int character)
{
	int counter;
	int value;
	unsigned char *temp;	/* temporary pointer			*/
	unsigned char *temp2;	/* temporary pointer			*/

	if ((character == '\011') && (expand_tabs)) {
		counter = len_char('\011', scr_horz);
		for (; counter > 0; counter--)
			insert(' ');
		if (auto_format)
			Auto_Format();
		return;
	}
	text_changes = TRUE;
	if ((curr_line->max_length - curr_line->line_length) < 5)
		point = resiz_line(10, curr_line, position);
	curr_line->line_length++;
	temp = point;
	counter = position;
	while (counter < curr_line->line_length) {	/* find end of line */
		counter++;
		temp++;
	}
	temp++;			/* increase length of line by one	*/
	while (point < temp) {
		temp2 = temp - 1;
		*temp = *temp2;	/* shift characters over by one		*/
		temp--;
	}
	*point = (unsigned char)character;	/* insert new character		*/
	wclrtoeol(text_win);
	if (!isprint((unsigned char)character)) {
		scr_pos = scr_horz += out_char(text_win, character, scr_horz);
		point++;
		position++;
	} else {
		waddch(text_win, (unsigned char)character);
		scr_pos = ++scr_horz;
		point++;
		position++;
	}

	if ((observ_margins) && (right_margin < scr_pos)) {
		counter = position;
		while (scr_pos > right_margin)
			prev_word();
		if (scr_pos == 0) {
			while (position < counter)
				right(TRUE);
		} else {
			counter -= position;
			insert_line(TRUE);
			for (value = 0; value < counter; value++)
				right(TRUE);
		}
	}

	if ((scr_horz - horiz_offset) > last_col) {
		horiz_offset += 8;
		midscreen(scr_vert, point);
	}

	if ((auto_format) && (character == ' ') && (!formatted))
		Auto_Format();
	else if ((character != ' ') && (character != '\t'))
		formatted = FALSE;

	draw_line(scr_vert, scr_horz, point, position, curr_line->line_length);
}

/* insert a complete multi-byte UTF-8 character into line	*/
static void
insert_utf8(const unsigned char *mb, int len)
{
	int counter;
	unsigned char *temp;
	int i;

	text_changes = TRUE;
	if ((curr_line->max_length - curr_line->line_length) < (len + 5))
		point = resiz_line(len + 10, curr_line, position);

	/* shift the tail of the line right by len bytes */
	curr_line->line_length += len;
	temp = point;
	counter = position;
	while (counter < curr_line->line_length) {
		counter++;
		temp++;
	}
	temp += len;
	while (point + len - 1 < temp) {
		unsigned char *temp2;

		temp2 = temp - len;
		*temp = *temp2;
		temp--;
	}

	/* copy all bytes of the UTF-8 character */
	memmove(point, mb, (size_t)len);

	/* display the character before advancing past it */
	wclrtoeol(text_win);
	{
		char buf[5];
		memcpy(buf, point, (size_t)len);
		buf[len] = '\0';
		waddstr(text_win, buf);
	}

	point += len;
	position += len;

	scanline(point);
	scr_pos = scr_horz;

	if ((observ_margins) && (right_margin < scr_pos)) {
		counter = position;
		while (scr_pos > right_margin)
			prev_word();
		if (scr_pos == 0) {
			while (position < counter)
				right(TRUE);
		} else {
			counter -= position;
			insert_line(TRUE);
			for (i = 0; i < counter; i++)
				right(TRUE);
		}
	}

	if ((scr_horz - horiz_offset) > last_col) {
		horiz_offset += 8;
		midscreen(scr_vert, point);
	}

	formatted = FALSE;

	draw_line(scr_vert, scr_horz, point, position, curr_line->line_length);
}

/*
 * Insert a character given its Unicode code point (the value typed at
 * the "Character code" prompt).  The value is encoded as UTF-8, so the
 * editor never inserts a bare byte above 0x7f.
 */
static void
insert_code(int code)
{
	unsigned char mb[4];
	size_t n;

	if (code < 0)
		return;
	if (code < 0x80) {
		in = code;
		wmove(text_win, scr_vert, (scr_horz - horiz_offset));
		insert(code);
		return;
	}
	/*
	 * ee_utf8_encode() rejects surrogates (U+D800..U+DFFF) and code
	 * points above U+10FFFF, so the editor can never insert an
	 * invalid sequence here.
	 */
	n = ee_utf8_encode((uint32_t)code, mb);
	if (n == 0)
		return;
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
	insert_utf8(mb, (int)n);
}

/* delete character		*/
static void
delete(int disp)
{
	unsigned char *tp;
	unsigned char *temp2;
	struct text *temp_buff;
	int temp_vert;
	int temp_pos;
	int del_width = 1;

	if (point != curr_line->line) {	/* if not at beginning of line	*/
		text_changes = TRUE;
		temp2 = tp = point;
		unsigned char *prev = utf8_prev(curr_line->line, point);
		del_width = (int)(point - prev);
		tp -= del_width;
		point -= del_width;
		position -= del_width;
		temp_pos = position;
		curr_line->line_length -= del_width;
		scanline(point);
		scr_pos = scr_horz;
		if (in == 8) {
			size_t width = (size_t)ee_min(del_width, 4);

			memcpy(d_char, point, width);
			d_char[width] = '\0';
		}
		while (temp_pos <= curr_line->line_length) {
			temp_pos++;
			*tp = *temp2;
			tp++;
			temp2++;
		}
		if ((scr_horz < horiz_offset) && (horiz_offset > 0)) {
			horiz_offset -= 8;
			midscreen(scr_vert, point);
		}
	} else if (curr_line->prev_line != NULL) {
		text_changes = TRUE;
		if (total_lines > 1)
			total_lines--;
		left(disp);			/* go to previous line	*/
		temp_buff = curr_line->next_line;
		point = resiz_line(temp_buff->line_length, curr_line, position);
		if (temp_buff->next_line != NULL)
			temp_buff->next_line->prev_line = curr_line;
		curr_line->next_line = temp_buff->next_line;
		temp2 = temp_buff->line;
		if (in == 8) {
			d_char[0] = '\n';
			d_char[1] = '\0';
		}
		tp = point;
		temp_pos = 1;
		while (temp_pos < temp_buff->line_length) {
			curr_line->line_length++;
			temp_pos++;
			*tp = *temp2;
			tp++;
			temp2++;
		}
		*tp = '\0';
		free(temp_buff->line);
		free(temp_buff);
		temp_buff = curr_line;
		temp_vert = scr_vert;
		scr_pos = scr_horz;
		if (scr_vert < last_line) {
			wmove(text_win, scr_vert + 1, 0);
			wdeleteln(text_win);
		}
		while ((temp_buff != NULL) && (temp_vert < last_line)) {
			temp_buff = temp_buff->next_line;
			temp_vert++;
		}
		if ((temp_vert == last_line) && (temp_buff != NULL)) {
			tp = temp_buff->line;
			wmove(text_win, last_line, 0);
			wclrtobot(text_win);
			draw_line(last_line, 0, tp, 1, temp_buff->line_length);
			wmove(text_win, scr_vert, (scr_horz - horiz_offset));
		}
	}
	draw_line(scr_vert, scr_horz, point, position, curr_line->line_length);
	formatted = FALSE;
}

/* find the proper horizontal position for the pointer	*/
static void
scanline(unsigned char *pos)
{
	int temp;
	unsigned char *ptr;

	ptr = curr_line->line;
	temp = 0;
	while (ptr < pos) {
		if (*ptr <= 8)
			temp += 2;
		else if (*ptr == 9)
			temp += tabshift(temp);
		else if ((*ptr >= 10) && (*ptr <= 31))
			temp += 2;
		else if ((*ptr >= 32) && (*ptr < 127))
			temp++;
		else if (*ptr == 127)
			temp += 2;
		else if (*ptr >= 0x80) {
			temp += utf8_width(ptr);
			ptr += utf8_len(ptr);
			continue;
		} else
			temp++;
		ptr++;
	}
	scr_horz = temp;
	if ((scr_horz - horiz_offset) > last_col) {
		horiz_offset = (scr_horz - (scr_horz % 8)) -
		    ee_max(1, last_col - 7);
		if (horiz_offset < 0)
			horiz_offset = 0;
		midscreen(scr_vert, point);
	} else if (scr_horz < horiz_offset) {
		horiz_offset = ee_max(0, (scr_horz - (scr_horz % 8)));
		midscreen(scr_vert, point);
	}
}

/* give the number of spaces to shift	*/
static int
tabshift(int temp_int)
{
	int leftover;

	leftover = ((temp_int + 1) % 8);
	if (leftover == 0)
		return (1);
	else
		return (9 - leftover);
}

/* output non-printing character */
static int
out_char(WINDOW *window, int character, int column)
{
	int i1, i2;
	char *string;

	if (character == TAB) {
		i1 = tabshift(column);
		for (i2 = 0;
		    (i2 < i1) && (((column + i2 + 1) - horiz_offset) <
		    last_col); i2++) {
			waddch(window, ' ');
		}
		return (i1);
	} else if ((character >= '\0') && (character < ' ')) {
		string = table[(int)character];
	} else if ((character < 0) || (character >= 127)) {
		if (character == 127) {
			string = "^?";
		} else {
			/*
			 * A byte outside the printable range should not
			 * reach here any more: valid UTF-8 is rendered by
			 * draw_line() and only control characters use this
			 * path.  Print the raw byte defensively.
			 */
			waddch(window, (unsigned char)character);
			return (1);
		}
	} else {
		waddch(window, (unsigned char)character);
		return (1);
	}
	for (i2 = 0;
	    (string[i2] != '\0') &&
	    (((column + i2 + 1) - horiz_offset) < last_col); i2++)
		waddch(window, (unsigned char)string[i2]);
	return ((int)strlen(string));
}

/* return the length of the character	*/
static int
len_char(int character, int column)
{
	int length;

	if (character == '\t')
		length = tabshift(column);
	else if ((character >= 0) && (character < 32))
		length = 2;
	else if ((character >= 32) && (character <= 126))
		length = 1;
	else if (character == 127)
		length = 2;
	else
		length = 1;

	return (length);
}

/* redraw line from current position */
static void
draw_line(int vertical, int horiz, unsigned char *ptr, int t_pos, int length)
{
	int d;		/* partial length of special or tab char to display  */
	unsigned char *temp;	/* temporary pointer to position in line	     */
	int abs_column;	/* offset in screen units from begin of line	     */
	int column;	/* horizontal position on screen		     */
	int row;	/* vertical position on screen			     */
	int posit;	/* temporary position indicator within line	     */

	abs_column = horiz;
	column = horiz - horiz_offset;
	row = vertical;
	temp = ptr;
	d = 0;
	posit = t_pos;
	if (column < 0) {
		wmove(text_win, row, 0);
		wclrtoeol(text_win);
	}
	while (column < 0) {
		if (*temp >= 0x80) {
			d = utf8_width(temp);
			abs_column += d;
			column += d;
			posit += utf8_len(temp);
			temp += utf8_len(temp);
		} else {
			d = len_char(*temp, abs_column);
			abs_column += d;
			column += d;
			posit++;
			temp++;
		}
	}
	wmove(text_win, row, column);
	wclrtoeol(text_win);
	while ((posit < length) && (column <= last_col)) {
		if (*temp >= 0x80) {
			int clen = utf8_len(temp);
			int dw = utf8_width(temp);
			char buf[5];

			/*
			 * Never start a character that would not fit
			 * entirely inside the usable columns: a width-2
			 * character is not drawn half-way at the edge.
			 */
			if ((column + dw) > (last_col + 1))
				break;
			if (clen > (int)sizeof(buf) - 1)
				clen = (int)sizeof(buf) - 1;
			memcpy(buf, temp, (size_t)clen);
			buf[clen] = '\0';
			waddstr(text_win, buf);
			abs_column += dw;
			column += dw;
			posit += clen;
			temp += clen;
		} else if (!isprint(*temp)) {
			column += len_char(*temp, abs_column);
			abs_column += out_char(text_win, *temp, abs_column);
			posit++;
			temp++;
		} else {
			abs_column++;
			column++;
			waddch(text_win, *temp);
			posit++;
			temp++;
		}
	}
	if (column < last_col)
		wclrtoeol(text_win);
	wmove(text_win, vertical, (horiz - horiz_offset));
}

/* insert new line		*/
static void
insert_line(int disp)
{
	int temp_pos;
	int temp_pos2;
	unsigned char *temp;
	unsigned char *extra;
	struct text *temp_nod;

	text_changes = TRUE;
	total_lines++;
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
	wclrtoeol(text_win);
	temp_nod = txtalloc();
	temp_nod->line = extra = xmalloc(10);
	temp_nod->line_length = 1;
	temp_nod->max_length = 10;
	temp_nod->line_number = curr_line->line_number + 1;
	temp_nod->next_line = curr_line->next_line;
	if (temp_nod->next_line != NULL)
		temp_nod->next_line->prev_line = temp_nod;
	temp_nod->prev_line = curr_line;
	curr_line->next_line = temp_nod;
	temp_pos2 = position;
	temp = point;
	if (temp_pos2 < curr_line->line_length) {
		temp_pos = 1;
		while (temp_pos2 < curr_line->line_length) {
			if ((temp_nod->max_length - temp_nod->line_length) < 5)
				extra = resiz_line(10, temp_nod, temp_pos);
			temp_nod->line_length++;
			temp_pos++;
			temp_pos2++;
			*extra = *temp;
			extra++;
			temp++;
		}
		temp = point;
		*temp = '\0';
		temp = resiz_line((1 - temp_nod->line_length), curr_line,
		    position);
		curr_line->line_length = 1 + (int)(temp - curr_line->line);
	}
	curr_line->line_length = position;
	absolute_lin++;
	curr_line = temp_nod;
	*extra = '\0';
	position = 1;
	point = curr_line->line;
	if (disp) {
		if (scr_vert < last_line) {
			scr_vert++;
			wclrtoeol(text_win);
			wmove(text_win, scr_vert, 0);
			winsertln(text_win);
		} else {
			wmove(text_win, 0, 0);
			wdeleteln(text_win);
			wmove(text_win, last_line, 0);
			wclrtobot(text_win);
		}
		scr_pos = scr_horz = 0;
		if (horiz_offset) {
			horiz_offset = 0;
			midscreen(scr_vert, point);
		}
		draw_line(scr_vert, scr_horz, point, position,
		    curr_line->line_length);
	}
}

/* allocate space for line structure	*/
static struct text *
txtalloc(void)
{
	return ((struct text *)xmalloc(sizeof(struct text)));
}

/* allocate space for file name list node */
static struct files *
name_alloc(void)
{
	return ((struct files *)xmalloc(sizeof(struct files)));
}

/* move to next word in string		*/
static char *
next_word(char *string)
{
	while ((*string != '\0') && ((*string != 32) && (*string != 9)))
		string++;
	while ((*string != '\0') && ((*string == 32) || (*string == 9)))
		string++;
	return (string);
}

/* move to start of previous word in text	*/
static void
prev_word(void)
{
	if (position != 1) {
		if ((position != 1) &&
		    ((point[-1] == ' ') ||
		     (point[-1] ==
		      '\t'))) {	/* if at the start of a word	*/
			while ((position != 1) &&
			    ((*point != ' ') && (*point != '\t')))
				left(TRUE);
		}
		while ((position != 1) && ((*point == ' ') || (*point == '\t')))
			left(TRUE);
		while ((position != 1) && ((*point != ' ') && (*point != '\t')))
			left(TRUE);
		if ((position != 1) && ((*point == ' ') || (*point == '\t')))
			right(TRUE);
	} else
		left(TRUE);
}

/* use control for commands		*/
static void
control(void)
{
	char *string;

	if (in == 1) {		/* control a	*/
		string = get_string(ascii_code_str, TRUE);
		if (string != NULL) {
			if (*string != '\0')
				insert_code((int)strtol(string, NULL, 10));
			free(string);
		}
	} else if (in == 2)	/* control b	*/
		bottom();
	else if (in == 3) {	/* control c	*/
		command_prompt();
	} else if (in == 4)	/* control d	*/
		down();
	else if (in == 5)	/* control e	*/
		search_prompt();
	else if (in == 6)	/* control f	*/
		undel_char();
	else if (in == 7)	/* control g	*/
		bol();
	else if (in == 8)	/* control h	*/
		delete(TRUE);
	else if (in == 9)	/* control i	*/
		;
	else if (in == 10)	/* control j	*/
		insert_line(TRUE);
	else if (in == 11)	/* control k	*/
		del_char();
	else if (in == 12)	/* control l	*/
		left(TRUE);
	else if (in == 13)	/* control m	*/
		insert_line(TRUE);
	else if (in == 14)	/* control n	*/
		move_rel('d', ee_max(5, (last_line - 5)));
	else if (in == 15)	/* control o	*/
		eol();
	else if (in == 16)	/* control p	*/
		move_rel('u', ee_max(5, (last_line - 5)));
	else if (in == 17)	/* control q	*/
		leave_op();
	else if (in == 18)	/* control r	*/
		right(TRUE);
	else if (in == 19)	/* control s	*/
		save_op();
	else if (in == 20)	/* control t	*/
		top();
	else if (in == 21)	/* control u	*/
		up();
	else if (in == 22)	/* control v	*/
		undel_word();
	else if (in == 23)	/* control w	*/
		del_word();
	else if (in == 24)	/* control x	*/
		search(TRUE);
	else if (in == 25)	/* control y	*/
		del_line();
	else if (in == 26)	/* control z	*/
		undel_line();
	else if (in == 27) {	/* control [ (escape)	*/
		menu_op(main_menu);
	}
}

/*
 |	Emacs control-key bindings
 */

static void
emacs_control(void)
{
	char *string;

	if (in == 1)		/* control a	*/
		bol();
	else if (in == 2)	/* control b	*/
		left(TRUE);
	else if (in == 3) {	/* control c	*/
		command_prompt();
	} else if (in == 4)	/* control d	*/
		del_char();
	else if (in == 5)	/* control e	*/
		eol();
	else if (in == 6)	/* control f	*/
		right(TRUE);
	else if (in == 7)	/* control g	*/
		move_rel('u', ee_max(5, (last_line - 5)));
	else if (in == 8)	/* control h	*/
		delete(TRUE);
	else if (in == 9)	/* control i	*/
		;
	else if (in == 10)	/* control j	*/
		undel_char();
	else if (in == 11)	/* control k	*/
		del_line();
	else if (in == 12)	/* control l	*/
		undel_line();
	else if (in == 13)	/* control m	*/
		insert_line(TRUE);
	else if (in == 14)	/* control n	*/
		down();
	else if (in == 15) {	/* control o	*/
		string = get_string(ascii_code_str, TRUE);
		if (string != NULL) {
			if (*string != '\0')
				insert_code((int)strtol(string, NULL, 10));
			free(string);
		}
	} else if (in == 16)	/* control p	*/
		up();
	else if (in == 17)	/* control q	*/
		leave_op();
	else if (in == 18)	/* control r	*/
		undel_word();
	else if (in == 19)	/* control s	*/
		save_op();
	else if (in == 20)	/* control t	*/
		top();
	else if (in == 21)	/* control u	*/
		bottom();
	else if (in == 22)	/* control v	*/
		move_rel('d', ee_max(5, (last_line - 5)));
	else if (in == 23)	/* control w	*/
		del_word();
	else if (in == 24)	/* control x	*/
		search(TRUE);
	else if (in == 25)	/* control y	*/
		search_prompt();
	else if (in == 26)	/* control z	*/
		adv_word();
	else if (in == 27) {	/* control [ (escape)	*/
		menu_op(main_menu);
	}
}

/* go to bottom of file			*/
static void
bottom(void)
{
	while (curr_line->next_line != NULL) {
		curr_line = curr_line->next_line;
		absolute_lin++;
	}
	point = curr_line->line;
	if (horiz_offset)
		horiz_offset = 0;
	position = 1;
	midscreen(last_line, point);
	scr_pos = scr_horz;
}

/* go to top of file			*/
static void
top(void)
{
	while (curr_line->prev_line != NULL) {
		curr_line = curr_line->prev_line;
		absolute_lin--;
	}
	point = curr_line->line;
	if (horiz_offset)
		horiz_offset = 0;
	position = 1;
	midscreen(0, point);
	scr_pos = scr_horz;
}

/* move pointers to start of next line	*/
static void
nextline(void)
{
	curr_line = curr_line->next_line;
	absolute_lin++;
	point = curr_line->line;
	position = 1;
	if (scr_vert == last_line) {
		wmove(text_win, 0, 0);
		wdeleteln(text_win);
		wmove(text_win, last_line, 0);
		wclrtobot(text_win);
		draw_line(last_line, 0, point, 1, curr_line->line_length);
	} else
		scr_vert++;
}

/* move pointers to start of previous line*/
static void
prevline(void)
{
	curr_line = curr_line->prev_line;
	absolute_lin--;
	point = curr_line->line;
	position = 1;
	if (scr_vert == 0) {
		winsertln(text_win);
		draw_line(0, 0, point, 1, curr_line->line_length);
	} else
		scr_vert--;
	while (position < curr_line->line_length) {
		position++;
		point++;
	}
}

/* move left one character	*/
static void
left(int disp)
{
	if (point != curr_line->line) {	/* if not at begin of line	*/
		unsigned char *prev = utf8_prev(curr_line->line, point);
		int char_bytes = (int)(point - prev);
		point = prev;
		position -= char_bytes;
		scanline(point);
		wmove(text_win, scr_vert, (scr_horz - horiz_offset));
		scr_pos = scr_horz;
	} else if (curr_line->prev_line != NULL) {
		if (!disp) {
			absolute_lin--;
			curr_line = curr_line->prev_line;
			point = curr_line->line + curr_line->line_length;
			position = curr_line->line_length;
			return;
		}
		position = 1;
		prevline();
		scanline(point);
		scr_pos = scr_horz;
		wmove(text_win, scr_vert, (scr_horz - horiz_offset));
	}
}

/* move right one character	*/
static void
right(int disp)
{
	if (position < curr_line->line_length) {
		int char_bytes = utf8_len(point);
		if (position + char_bytes > curr_line->line_length)
			char_bytes = curr_line->line_length - position;
		point += char_bytes;
		position += char_bytes;
		scanline(point);
		wmove(text_win, scr_vert, (scr_horz - horiz_offset));
		scr_pos = scr_horz;
	} else if (curr_line->next_line != NULL) {
		if (!disp) {
			absolute_lin++;
			curr_line = curr_line->next_line;
			point = curr_line->line;
			position = 1;
			return;
		}
		nextline();
		scr_pos = scr_horz = 0;
		if (horiz_offset) {
			horiz_offset = 0;
			midscreen(scr_vert, point);
		}
		wmove(text_win, scr_vert, (scr_horz - horiz_offset));
		position = 1;
	}
}

/* move to the same column as on other line	*/
static void
find_pos(void)
{
	scr_horz = 0;
	position = 1;
	while ((scr_horz < scr_pos) && (position < curr_line->line_length)) {
		if (*point == 9)
			scr_horz += tabshift(scr_horz);
		else if (*point < ' ')
			scr_horz += 2;
		else if (*point >= 0x80) {
			int clen = utf8_len(point);
			int dw = utf8_width(point);
			if (scr_horz + dw > scr_pos)
				break;
			scr_horz += dw;
			point += clen;
			position += clen;
			continue;
		} else
			scr_horz++;
		position++;
		point++;
	}
	if ((scr_horz - horiz_offset) > last_col) {
		horiz_offset = (scr_horz - (scr_horz % 8)) -
		    ee_max(1, last_col - 7);
		if (horiz_offset < 0)
			horiz_offset = 0;
		midscreen(scr_vert, point);
	} else if (scr_horz < horiz_offset) {
		horiz_offset = ee_max(0, (scr_horz - (scr_horz % 8)));
		midscreen(scr_vert, point);
	}
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
}

/* move up one line		*/
static void
up(void)
{
	if (curr_line->prev_line != NULL) {
		prevline();
		point = curr_line->line;
		find_pos();
	}
}

/* move down one line		*/
static void
down(void)
{
	if (curr_line->next_line != NULL) {
		nextline();
		find_pos();
	}
}

/* process function key		*/
static void
function_key(void)
{
	if (in == KEY_HELP)
		help();
	else if (in == KEY_LEFT)
		left(TRUE);
	else if (in == KEY_RIGHT)
		right(TRUE);
	else if (in == KEY_HOME)
		bol();
	else if (in == KEY_END)
		eol();
	else if (in == KEY_UP)
		up();
	else if (in == KEY_DOWN)
		down();
	else if (in == KEY_NPAGE)
		move_rel('d', ee_max(5, (last_line - 5)));
	else if (in == KEY_PPAGE)
		move_rel('u', ee_max(5, (last_line - 5)));
	else if (in == KEY_DL)
		del_line();
	else if (in == KEY_DC)
		del_char();
	else if (in == KEY_BACKSPACE)
		delete(TRUE);
	else if (in ==
	    KEY_IL) {		/* insert a line before current line	*/
		insert_line(TRUE);
		left(TRUE);
	} else if (in == KEY_F(1))
		gold = !gold;
	else if (in == KEY_F(2)) {
		if (gold) {
			gold = FALSE;
			undel_line();
		} else
			undel_char();
	} else if (in == KEY_F(3)) {
		if (gold) {
			gold = FALSE;
			undel_word();
		} else
			del_word();
	} else if (in == KEY_F(4)) {
		if (gold) {
			gold = FALSE;
			redraw();
		} else
			adv_word();
	} else if (in == KEY_F(5)) {
		if (gold) {
			gold = FALSE;
			search_prompt();
		} else
			search(TRUE);
	} else if (in == KEY_F(6)) {
		if (gold) {
			gold = FALSE;
			bottom();
		} else
			top();
	} else if (in == KEY_F(7)) {
		if (gold) {
			gold = FALSE;
			eol();
		} else
			bol();
	} else if (in == KEY_F(8)) {
		if (gold) {
			gold = FALSE;
			command_prompt();
		} else
			adv_line();
	}
}

static void
print_buffer(void)
{
	size_t len = strlen((char *)print_command);
	char *buffer;

	if (len > (SIZE_MAX - 3))
		return;
	buffer = malloc(len + 3);
	if (buffer == NULL)
		return;
	buffer[0] = '>';
	buffer[1] = '!';
	memcpy(buffer + 2, print_command, len + 1);

	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	wprintw(com_win, printer_msg_str, print_command);
	wrefresh(com_win);
	command(buffer);
	free(buffer);
}

static void
command_prompt(void)
{
	char *cmd_str;
	int result;

	cmd_str = get_string(command_str, TRUE);
	if (cmd_str == NULL)
		return;
	if ((result = unique_test(cmd_str, commands)) != 1) {
		werase(com_win);
		wmove(com_win, 0, 0);
		if (result == 0)
			wprintw(com_win, unkn_cmd_str, cmd_str);
		else
			wprintw(com_win, "%s", non_unique_cmd_msg);

		wrefresh(com_win);
		clear_com_win = TRUE;
		free(cmd_str);
		return;
	}
	command(cmd_str);
	wrefresh(com_win);
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
	free(cmd_str);
}

/* process commands from keyboard	*/
static void
command(char *cmd_str1)
{
	char *cmd_str2 = NULL;
	char *cmd_str = cmd_str1;

	clear_com_win = TRUE;
	if (compare(cmd_str, HELP, FALSE))
		help();
	else if (compare(cmd_str, WRITE, FALSE)) {
		if (restrict_mode()) {
			return;
		}
		cmd_str = (char *)next_word(cmd_str);
		if (*cmd_str == '\0') {
			cmd_str = cmd_str2 = (char *)get_string(file_write_prompt_str,
			    TRUE);
			if (cmd_str == NULL)
				return;
		}
		tmp_file = (char *)resolve_name(cmd_str);
		write_file(tmp_file, 1);
		if (tmp_file != cmd_str)
			free(tmp_file);
	} else if (compare(cmd_str, READ, FALSE)) {
		if (restrict_mode()) {
			return;
		}
		cmd_str = (char *)next_word(cmd_str);
		if (*cmd_str == '\0') {
			cmd_str = cmd_str2 = (char *)get_string(file_read_prompt_str,
			    TRUE);
			if (cmd_str == NULL)
				return;
		}
		tmp_file = cmd_str;
		recv_file = TRUE;
		tmp_file = (char *)resolve_name(cmd_str);
		check_fp();
		if (tmp_file != cmd_str)
			free(tmp_file);
	} else if (compare(cmd_str, LINE, FALSE)) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, line_num_str, curr_line->line_number);
		wprintw(com_win, line_len_str, curr_line->line_length);
	} else if (compare(cmd_str, FILE_str, FALSE)) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		if (in_file_name == NULL)
			wprintw(com_win, "%s", no_file_string);
		else
			wprintw(com_win, current_file_str, in_file_name);
	} else if ((*cmd_str >= '0') && (*cmd_str <= '9'))
		goto_line(cmd_str);
	else if (compare(cmd_str, CHARACTER, FALSE)) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, char_str, *point);
	} else if (compare(cmd_str, REDRAW, FALSE))
		redraw();
	else if (compare(cmd_str, RESEQUENCE, FALSE)) {
		tmp_line = first_line->next_line;
		while (tmp_line != NULL) {
			tmp_line->line_number = tmp_line->prev_line->line_number +
			    1;
			tmp_line = tmp_line->next_line;
		}
	} else if (compare(cmd_str, AUTHOR, FALSE)) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, "written by Hugh Mahon");
	} else if (compare(cmd_str, CASE, FALSE))
		case_sen = TRUE;
	else if (compare(cmd_str, NOCASE, FALSE))
		case_sen = FALSE;
	else if (compare(cmd_str, EXPAND, FALSE))
		expand_tabs = TRUE;
	else if (compare(cmd_str, NOEXPAND, FALSE))
		expand_tabs = FALSE;
	else if (compare(cmd_str, Exit_string, FALSE))
		finish();
	else if (compare(cmd_str, QUIT_string, FALSE))
		leave_op();
	else if (*cmd_str == '!') {
		cmd_str++;
		if ((*cmd_str == ' ') || (*cmd_str == 9))
			cmd_str = (char *)next_word(cmd_str);
		sh_command(cmd_str);
	} else if ((*cmd_str == '<') && (!in_pipe)) {
		in_pipe = TRUE;
		shell_fork = FALSE;
		cmd_str++;
		if ((*cmd_str == ' ') || (*cmd_str == '\t'))
			cmd_str = (char *)next_word(cmd_str);
		command(cmd_str);
		in_pipe = FALSE;
		shell_fork = TRUE;
	} else if ((*cmd_str == '>') && (!out_pipe)) {
		out_pipe = TRUE;
		cmd_str++;
		if ((*cmd_str == ' ') || (*cmd_str == '\t'))
			cmd_str = (char *)next_word(cmd_str);
		command(cmd_str);
		out_pipe = FALSE;
	} else {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, unkn_cmd_str, cmd_str);
	}
	if (cmd_str2 != NULL)
		free(cmd_str2);
}

/* determine horizontal position for get_string	*/
static int
scan(char *line, int offset, int column)
{
	char *stemp;
	int i;
	int j;

	stemp = line;
	i = 0;
	j = column;
	while (i < offset) {
		if (*(unsigned char *)stemp >= 0x80) {
			int clen = utf8_len((const unsigned char *)stemp);
			j += utf8_width((const unsigned char *)stemp);
			stemp += clen;
			i += clen;
		} else {
			j += len_char(*stemp, j);
			stemp++;
			i++;
		}
	}
	return (j);
}

/*
 * Redraw the prompt line showing prompt followed by the input buffer.
 * The view scrolls left when the cursor would leave the last column, so
 * arbitrarily long input remains editable without any fixed limit.
 */
static void
show_prompt_line(const char *prompt, const char *buf, size_t len, size_t cur)
{
	size_t start = 0;
	int prompt_width = (int)strlen(prompt);
	int cursor_col;
	int col;
	size_t i;

	cursor_col = scan((char *)buf, (int)cur, prompt_width);
	while ((cursor_col > (last_col - 1)) && (start < cur)) {
		start += (size_t)utf8_len((const unsigned char *)buf + start);
		cursor_col = scan((char *)buf, (int)cur, prompt_width);
	}

	werase(com_win);
	wmove(com_win, 0, 0);
	waddstr(com_win, prompt);
	col = (start == 0) ? prompt_width :
	    scan((char *)buf, (int)start, prompt_width);
	wmove(com_win, 0, col);

	for (i = start; i < len; ) {
		unsigned char c = (unsigned char)buf[i];
		int clen, width;

		if (c >= 0x80) {
			clen = utf8_len((const unsigned char *)buf + i);
			width = utf8_width((const unsigned char *)buf + i);
		} else {
			clen = 1;
			width = len_char((int)c, col);
		}
		if ((col + width) > last_col)
			break;
		if ((c >= 0x20) && (c != 127)) {
			char tmp[8];

			if (clen > (int)sizeof(tmp) - 1)
				clen = (int)sizeof(tmp) - 1;
			memcpy(tmp, buf + i, (size_t)clen);
			tmp[clen] = '\0';
			waddstr(com_win, tmp);
		} else
			out_char(com_win, (int)c, col);
		col += width;
		i += (size_t)clen;
	}

	wmove(com_win, 0, scan((char *)buf, (int)cur, prompt_width));
	wrefresh(com_win);
}

/* read string from input on command line; NULL if cancelled */
static char *
get_string(const char *prompt, int advance)
{
	static const char *const prompt_keys[] = {
		"[Enter] Accept", "[Esc] Cancel", "[^V] Literal"
	};
	char *buf;
	size_t bufsize = 128;
	size_t len = 0;
	size_t cur = 0;
	int cancelled = FALSE;
	int done = FALSE;

	buf = malloc(bufsize);
	if (buf == NULL)
		return (NULL);
	buf[0] = '\0';

	set_shortcuts(prompt_keys,
	    (int)(sizeof(prompt_keys) / sizeof(prompt_keys[0])));
	clear_com_win = TRUE;
	show_prompt_line(prompt, buf, len, cur);

	while (!done) {
		wint_t win;
		int wret;

		wret = wget_wch(com_win, &win);
		if (wret == ERR) {
			/* SIGINT leaves the editor through the normal,
			 * terminal-restoring path */
			if (ee_intr_flag) {
				free(buf);
				edit_abort(0);
			}
			free(buf);
			exit(0);
		}
		in = (int)win;

		if ((wret == KEY_CODE_YES) && (win == KEY_ENTER)) {
			done = TRUE;
			continue;
		}
		if (wret == KEY_CODE_YES) {
			switch ((int)win) {
			case KEY_RESIZE:
				resize_check();
				break;
			case KEY_BACKSPACE:
				if (cur > 0) {
					size_t prev = (size_t)(utf8_prev(
					    (const unsigned char *)buf,
					    (const unsigned char *)buf + cur) -
					    (const unsigned char *)buf);
					memmove(buf + prev, buf + cur,
					    len - cur);
					len -= cur - prev;
					cur = prev;
				}
				break;
			case KEY_LEFT:
				if (cur > 0) {
					const unsigned char *p = utf8_prev(
					    (const unsigned char *)buf,
					    (const unsigned char *)buf + cur);
					cur = (size_t)(p - (const unsigned char *)buf);
				}
				break;
			case KEY_RIGHT:
				if (cur < len)
					cur += (size_t)utf8_len(
					    (const unsigned char *)buf + cur);
				break;
			case KEY_HOME:
				cur = 0;
				break;
			case KEY_END:
				cur = len;
				break;
			case KEY_DC:
				if (cur < len) {
					size_t n = (size_t)utf8_len(
					    (const unsigned char *)buf + cur);
					if (cur + n > len)
						n = len - cur;
					memmove(buf + cur, buf + cur + n,
					    len - cur - n);
					len -= n;
				}
				break;
			default:
				break;
			}
			show_prompt_line(prompt, buf, len, cur);
			continue;
		}

		if ((in == 27) || (in == 3)) {	/* Esc or ^C */
			cancelled = TRUE;
			done = TRUE;
			continue;
		}
		if ((in == '\n') || (in == '\r')) {
			done = TRUE;
			continue;
		}
		if ((in == 8) || (in == 127)) {
			if (cur > 0) {
				size_t prev = (size_t)(utf8_prev(
				    (const unsigned char *)buf,
				    (const unsigned char *)buf + cur) -
				    (const unsigned char *)buf);
				memmove(buf + prev, buf + cur, len - cur);
				len -= cur - prev;
				cur = prev;
			}
			show_prompt_line(prompt, buf, len, cur);
			continue;
		}

		{
			char mb[MB_LEN_MAX + 1];
			size_t n = 0;

			if (in == '\026') {	/* control-v: literal */
				wret = wget_wch(com_win, &win);
				if (wret == ERR) {
					if (ee_intr_flag) {
						free(buf);
						edit_abort(0);
					}
					free(buf);
					exit(0);
				}
				in = (int)win;
				if ((in >= 0) && (in < 0x80)) {
					mb[0] = (char)in;
					n = 1;
				}
			}
			if (n == 0) {
				if (in >= 0x80) {
					mbstate_t mbs;

					memset(&mbs, 0, sizeof(mbs));
					n = wcrtomb(mb, (wchar_t)win, &mbs);
					if (n == (size_t)-1)
						n = 0;
				} else if ((in >= 0) && (in < 0x80)) {
					mb[0] = (char)in;
					n = 1;
				}
			}
			if (n > 0) {
				if ((len + n + 1) > bufsize) {
					size_t newsize = bufsize * 2;
					char *nb;

					if (newsize < (len + n + 1))
						newsize = len + n + 1;
					nb = realloc(buf, newsize);
					if (nb == NULL) {
						cancelled = TRUE;
						done = TRUE;
						continue;
					}
					buf = nb;
					bufsize = newsize;
				}
				memmove(buf + cur + n, buf + cur, len - cur);
				memcpy(buf + cur, mb, n);
				len += n;
				cur += n;
			}
		}
		show_prompt_line(prompt, buf, len, cur);
	}

	default_shortcuts();
	paint_shortcut_bar();

	if (cancelled) {
		wmove(com_win, 0, 0);
		werase(com_win);
		wrefresh(com_win);
		free(buf);
		return (NULL);
	}

	buf[len] = '\0';
	{
		char *src = buf;
		char *string;

		if (((*src == ' ') || (*src == '\t')) && (advance))
			src = next_word(src);
		string = malloc(strlen(src) + 1);
		if (string == NULL) {
			free(buf);
			return (NULL);
		}
		strlcpy(string, src, strlen(src) + 1);
		free(buf);
		return (string);
	}
}

/* compare two strings	*/
static int
compare(const char *string1, const char *string2, int sensitive)
{
	const char *strng1;
	const char *strng2;
	int equal;

	strng1 = string1;
	strng2 = string2;
	if ((strng1 == NULL) || (strng2 == NULL) || (*strng1 == '\0') ||
	    (*strng2 == '\0'))
		return (FALSE);
	equal = TRUE;
	while (equal) {
		if (sensitive) {
			if (*strng1 != *strng2)
				equal = FALSE;
		} else {
			if (toupper((unsigned char)*strng1) !=
			    toupper((unsigned char)*strng2))
				equal = FALSE;
		}
		strng1++;
		strng2++;
		if ((*strng1 == '\0') || (*strng2 == '\0') ||
		    (*strng1 == ' ') || (*strng2 == ' '))
			break;
	}
	return (equal);
}

static void
goto_line(char *cmd_str)
{
	int number;
	int i;
	char *ptr;
	char direction = '\0';
	struct text *t_line;

	ptr = cmd_str;
	i = 0;
	while ((*ptr >= '0') && (*ptr <= '9')) {
		i = i * 10 + (*ptr - '0');
		ptr++;
	}
	number = i;
	i = 0;
	t_line = curr_line;
	while ((t_line->line_number > number) && (t_line->prev_line != NULL)) {
		i++;
		t_line = t_line->prev_line;
		direction = 'u';
	}
	while ((t_line->line_number < number) && (t_line->next_line != NULL)) {
		i++;
		direction = 'd';
		t_line = t_line->next_line;
	}
	if ((i < 30) && (i > 0)) {
		move_rel(direction, i);
	} else {
		if (direction != 'd') {
			absolute_lin += i;
		} else {
			absolute_lin -= i;
		}
		curr_line = t_line;
		point = curr_line->line;
		position = 1;
		midscreen((last_line / 2), point);
		scr_pos = scr_horz;
	}
	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	wprintw(com_win, line_num_str, curr_line->line_number);
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
}

/* put current line in middle of screen	*/
static void
midscreen(int line, unsigned char *pnt)
{
	struct text *mid_line;
	int i;

	line = ee_min(line, last_line);
	mid_line = curr_line;
	for (i = 0; ((i < line) && (curr_line->prev_line != NULL)); i++)
		curr_line = curr_line->prev_line;
	scr_vert = scr_horz = 0;
	wmove(text_win, 0, 0);
	draw_screen();
	scr_vert = i;
	curr_line = mid_line;
	scanline(pnt);
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
}

/* get arguments from command line	*/
static void
get_options(int numargs, char *arguments[])
{
	char *buff;
	int count;
	struct files *temp_names = NULL;
	char *name;
	char *ptr;
	int no_more_opts = FALSE;

	/*
	 |	see if editor was invoked as 'ree' (restricted mode)
	 */

	if (!(name = strrchr(arguments[0], '/')))
		name = arguments[0];
	else
		name++;
	if (!strcmp(name, "ree"))
		restricted = TRUE;

	top_of_stack = NULL;
	input_file = FALSE;
	recv_file = FALSE;
	count = 1;
	while ((count < numargs) && (!no_more_opts)) {
		buff = arguments[count];
		if (!strcmp("-i", buff)) {
			info_window = FALSE;
		} else if (!strcmp("-e", buff)) {
			expand_tabs = FALSE;
		} else if (!strcmp("-h", buff)) {
			nohighlight = TRUE;
		} else if (!strcmp("-?", buff)) {
			fprintf(stderr,
			    "usage: %s [-i] [-e] [-h] [+line_number] [file(s)]\n",
			    arguments[0]);
			fputs("       -i   turn off info window\n", stderr);
			fputs("       -e   do not convert tabs to spaces\n",
			    stderr);
			fputs("       -h   do not use reverse video\n", stderr);
			exit(1);
		} else if ((*buff == '+') && (start_at_line == NULL)) {
			buff++;
			start_at_line = (unsigned char *)buff;
		} else if (!(strcmp("--", buff)))
			no_more_opts = TRUE;
		else {
			count--;
			no_more_opts = TRUE;
		}
		count++;
	}
	while (count < numargs) {
		buff = arguments[count];
		if (top_of_stack == NULL) {
			temp_names = top_of_stack = name_alloc();
		} else {
			temp_names->next_name = name_alloc();
			temp_names = temp_names->next_name;
		}
		temp_names->name = (unsigned char *)xmalloc(strlen(buff) + 1);
		ptr = (char *)temp_names->name;
		while (*buff != '\0') {
			*ptr = *buff;
			buff++;
			ptr++;
		}
		*ptr = '\0';
		temp_names->next_name = NULL;
		input_file = TRUE;
		recv_file = TRUE;
		count++;
	}
}

/* open or close files according to flags */
static void
check_fp(void)
{
	int line_num;
	int temp;
	struct stat buf;

	clear_com_win = TRUE;
	tmp_vert = scr_vert;
	tmp_horz = scr_horz;
	tmp_line = curr_line;
	if (input_file) {
		in_file_name = (unsigned char *)(tmp_file =
		    (char *)top_of_stack->name);
		top_of_stack = top_of_stack->next_name;
	}
	temp = stat(tmp_file, &buf);
	buf.st_mode &= (mode_t)~07777;
	if ((temp != -1) && (buf.st_mode != 0100000) && (buf.st_mode != 0)) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, file_is_dir_msg, tmp_file);
		wrefresh(com_win);
		if (input_file) {
			quit(0);
			return;
		} else
			return;
	}
	if ((get_fd = open(tmp_file, O_RDONLY)) == -1) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		if (input_file)
			wprintw(com_win, new_file_msg, tmp_file);
		else
			wprintw(com_win, cant_open_msg, tmp_file);
		wrefresh(com_win);
		wmove(text_win, scr_vert, (scr_horz - horiz_offset));
		wrefresh(text_win);
		recv_file = FALSE;
		input_file = FALSE;
		return;
	} else
		get_file(tmp_file);

	recv_file = FALSE;
	line_num = curr_line->line_number;
	scr_vert = tmp_vert;
	scr_horz = tmp_horz;
	if (input_file)
		curr_line = first_line;
	else
		curr_line = tmp_line;
	point = curr_line->line;
	draw_screen();
	if (input_file) {
		input_file = FALSE;
		if (start_at_line != NULL) {
			line_num = (int)strtol((char *)start_at_line, NULL,
			    10) - 1;
			move_rel('d', line_num);
			line_num = 0;
			start_at_line = NULL;
		}
	} else {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		text_changes = TRUE;
		if ((tmp_file != NULL) && (*tmp_file != '\0'))
			wprintw(com_win, file_read_fin_msg, tmp_file);
	}
	wrefresh(com_win);
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
	wrefresh(text_win);
}

/*
 * Read the whole of fd into a freshly allocated buffer.  The buffer is
 * grown geometrically with reallocarray(3); overflow and allocation
 * failure are reported as an error rather than silently truncating.
 */
static int
read_all(int fd, unsigned char **out, size_t *outlen)
{
	unsigned char *buf = NULL;
	size_t cap = 0;
	size_t len = 0;

	for (;;) {
		ssize_t n;

		if (len == cap) {
			size_t ncap = (cap == 0) ? 8192 : cap * 2;
			unsigned char *nb;

			if (ncap < cap) {
				free(buf);
				return (-1);
			}
			nb = reallocarray(buf, ncap, 1);
			if (nb == NULL) {
				free(buf);
				return (-1);
			}
			buf = nb;
			cap = ncap;
		}
		n = read(fd, buf + len, cap - len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			return (-1);
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	*out = buf;
	*outlen = len;
	return (0);
}

/*
 * Read specified file into current buffer.
 *
 * The whole input is read and validated before any of it is copied into
 * the buffer.  This is what makes the "UTF-8 only" policy enforceable:
 * a file that contains a NUL byte or a malformed UTF-8 sequence is
 * rejected as a whole, so the buffer can never end up holding a mix of
 * valid text and stray bytes, and a failed read leaves the previous
 * contents untouched.
 */
static void
get_file(char *file_name)
{
	int append;		/* should text be appended to current line */
	int can_read;		/* file has at least one character	*/
	struct text *temp_line;
	char ro_flag = FALSE;
	unsigned char *data = NULL;
	size_t len = 0;
	size_t bad;

	if (recv_file) {		/* if reading a file			*/
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, reading_file_msg, file_name);
		if (access(file_name, 2)) {	/* check permission to write */
			if ((errno == ENOTDIR) || (errno == EACCES) ||
			    (errno == EROFS) || (errno == ETXTBSY) ||
			    (errno == EFAULT)) {
				wprintw(com_win, "%s", read_only_msg);
				ro_flag = TRUE;
			}
		}
		wrefresh(com_win);
	}

	if (read_all(get_fd, &data, &len) < 0) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, cant_open_msg, file_name);
		wrefresh(com_win);
		clear_com_win = TRUE;
		return;
	}

	if ((len > 0) && (memchr(data, 0, len) != NULL)) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, not_text_file_msg, file_name);
		wrefresh(com_win);
		clear_com_win = TRUE;
		free(data);
		if (input_file)
			quit(0);
		return;
	}

	bad = ee_utf8_validate(data, len);
	if (bad != len) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, invalid_utf8_msg, file_name,
		    (unsigned long)bad);
		wrefresh(com_win);
		clear_com_win = TRUE;
		free(data);
		if (input_file)
			quit(0);
		return;
	}

	can_read = (len > 0);
	if (curr_line->line_length > 1) {	/* if current line not blank */
		insert_line(FALSE);
		left(FALSE);
		append = FALSE;
	} else
		append = TRUE;
	{
		size_t pos = 0;

		while (pos < len) {
			size_t chunk = len - pos;

			if (chunk > 512)
				chunk = 512;
			get_line((int)chunk, data + pos, &append);
			pos += chunk;
		}
	}
	free(data);

	if ((can_read) && (curr_line->line_length == 1)) {
		temp_line = curr_line->prev_line;
		temp_line->next_line = curr_line->next_line;
		if (temp_line->next_line != NULL)
			temp_line->next_line->prev_line = temp_line;
		if (curr_line->line != NULL)
			free(curr_line->line);
		free(curr_line);
		curr_line = temp_line;
	}
	if (input_file) {	/* if this is the file to be edited display number of lines	*/
		read_only = ro_flag;
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, file_read_lines_msg, in_file_name,
		    curr_line->line_number);
		if (ro_flag)
			wprintw(com_win, "%s", read_only_msg);
		wrefresh(com_win);
	} else if (can_read)	/* not input_file and file is non-zero size */
		text_changes = TRUE;

	recount_lines();

	if (recv_file) {		/* if reading a file			*/
		in = EOF;
	}
}

/* read string and split into lines */
static void
get_line(int length, unsigned char *input, int *append)
{
	unsigned char *str1;
	unsigned char *str2;
	int num;		/* offset from start of string		*/
	int char_count;		/* length of new line (or added portion	*/
	int temp_counter;	/* temporary counter value		*/
	struct text *tline;	/* temporary pointer to new line	*/
	int first_time;		/* if TRUE, the first time through the loop */

	str2 = input;
	num = 0;
	first_time = TRUE;
	while (num < length) {
		if (!first_time) {
			if (num < length) {
				str2++;
				num++;
			}
		} else
			first_time = FALSE;
		str1 = str2;
		char_count = 1;
		/* find end of line	*/
		while ((*str2 != '\n') && (num < length)) {
			str2++;
			num++;
			char_count++;
		}
		if (!(*append)) {	/* if not append to current line, insert new one */
			tline = txtalloc();	/* allocate data structure for next line */
			tline->line_number = curr_line->line_number + 1;
			tline->next_line = curr_line->next_line;
			tline->prev_line = curr_line;
			curr_line->next_line = tline;
			if (tline->next_line != NULL)
				tline->next_line->prev_line = tline;
			curr_line = tline;
			curr_line->line = point = (unsigned char *)xmalloc(
			    (size_t)char_count);
			curr_line->line_length = char_count;
			curr_line->max_length = char_count;
		} else {
			point = resiz_line(char_count, curr_line,
			    curr_line->line_length);
			curr_line->line_length += (char_count - 1);
		}
		for (temp_counter = 1; temp_counter < char_count;
		    temp_counter++) {
			*point = *str1;
			point++;
			str1++;
		}
		*point = '\0';
		*append = FALSE;
		if ((num == length) && (*str2 != '\n'))
			*append = TRUE;
	}
}

static void
draw_screen(void)	/* redraw the screen from current position */
{
	struct text *temp_line;
	unsigned char *line_out;
	int temp_vert;

	temp_line = curr_line;
	temp_vert = scr_vert;
	wclrtobot(text_win);
	while ((temp_line != NULL) && (temp_vert <= last_line)) {
		line_out = temp_line->line;
		draw_line(temp_vert, 0, line_out, 1, temp_line->line_length);
		temp_vert++;
		temp_line = temp_line->next_line;
	}
	wmove(text_win, temp_vert, 0);
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
}

/*
 * "leave and save" (the historical "exit" command and Esc-Enter).
 * The save path is shared with ^S; nothing is written if the file
 * name prompt is cancelled.
 */
static void
finish(void)
{
	if (save_op())
		quit(TRUE);
}

/*
 * Exit the editor.  Confirmation for a modified buffer is handled by
 * the caller (leave_op opens the save/discard/cancel menu), so this
 * routine never destroys unsaved data on its own.
 */
static int
quit(int noverify)
{
	(void)noverify;

	touchwin(text_win);
	wrefresh(text_win);
	if (top_of_stack == NULL) {
		wrefresh(com_win);
		endwin();
		putchar('\n');
		exit(0);
	} else {
		delete_text();
		recv_file = TRUE;
		input_file = TRUE;
		check_fp();
	}
	return (0);
}

static void
edit_abort(int arg)
{
	(void)arg;
	wrefresh(com_win);
	endwin();
	putchar('\n');
	exit(1);
}

static void
delete_text(void)
{
	while (curr_line->next_line != NULL)
		curr_line = curr_line->next_line;
	while (curr_line != first_line) {
		free(curr_line->line);
		curr_line = curr_line->prev_line;
		absolute_lin--;
		free(curr_line->next_line);
	}
	curr_line->next_line = NULL;
	*curr_line->line = '\0';
	curr_line->line_length = 1;
	curr_line->line_number = 1;
	point = curr_line->line;
	scr_pos = scr_vert = scr_horz = 0;
	position = 1;
	total_lines = 1;
}

/*
 * Ask a yes/no question with an explicit, unambiguous set of keys.
 * Returns 1 for yes, 0 for no and -1 when the user cancels with Esc.
 */
static int
confirm(const char *question)
{
	static const char *const keys[] = {
		"[Y] Yes", "[N] No", "[Esc] Cancel"
	};
	int result = -1;
	wint_t win;
	int wret;

	set_shortcuts(keys, (int)(sizeof(keys) / sizeof(keys[0])));

	for (;;) {
		wmove(com_win, 0, 0);
		werase(com_win);
		if (!nohighlight)
			wstandout(com_win);
		waddstr(com_win, question);
		if (!nohighlight)
			wstandend(com_win);
		wrefresh(com_win);
		paint_shortcut_bar();

		wret = wget_wch(com_win, &win);
		if (wret == ERR) {
			if (ee_intr_flag) {
				default_shortcuts();
				edit_abort(0);
			}
			default_shortcuts();
			exit(0);
		}
		if (wret == KEY_CODE_YES) {
			if (win == KEY_RESIZE) {
				resize_check();
				continue;
			}
			result = -1;
			break;
		}
		if ((win == 'y') || (win == 'Y'))
			result = 1;
		else if ((win == 'n') || (win == 'N'))
			result = 0;
		else
			result = -1;
		break;
	}
	default_shortcuts();
	paint_shortcut_bar();
	return (result);
}

static int
write_file(char *file_name, int warn_if_exists)
{
	struct text *out_line;
	int lines;
	int write_flag = TRUE;
	int write_ok = TRUE;

	lines = 0;
	if (warn_if_exists &&
	    ((in_file_name == NULL) ||
	     strcmp((char *)in_file_name, file_name))) {
		if ((temp_fp = fopen(file_name, "r"))) {
			fclose(temp_fp);
			if (confirm("File exists.  Overwrite it?") != 1)
				write_flag = FALSE;
		}
	}

	clear_com_win = TRUE;

	if (!write_flag)
		return (FALSE);

	if ((temp_fp = fopen(file_name, "w")) == NULL) {
		clear_com_win = TRUE;
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, "unable to create file \"%s\"", file_name);
		wrefresh(com_win);
		clear_com_win = TRUE;
		return (FALSE);
	}

	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	wprintw(com_win, "writing file \"%s\"", file_name);
	wrefresh(com_win);

	/*
	 * The buffer is written byte for byte and the result is only
	 * reported as successful once fflush(3) and fclose(3) have
	 * confirmed that no deferred error occurred.
	 */
	out_line = first_line;
	while ((out_line != NULL) && write_ok) {
		size_t n = (size_t)(out_line->line_length - 1);

		if ((n > 0) && (fwrite(out_line->line, 1, n, temp_fp) != n))
			write_ok = FALSE;
		else if (fputc('\n', temp_fp) == EOF)
			write_ok = FALSE;
		out_line = out_line->next_line;
		lines++;
	}
	if (write_ok && (fflush(temp_fp) != 0))
		write_ok = FALSE;
	if (fclose(temp_fp) != 0)
		write_ok = FALSE;
	temp_fp = NULL;

	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	if (!write_ok) {
		wprintw(com_win, "error writing \"%s\"", file_name);
		wrefresh(com_win);
		clear_com_win = TRUE;
		return (FALSE);
	}
	wprintw(com_win, "\"%s\", %d lines written", file_name, lines);
	wrefresh(com_win);
	return (TRUE);
}

/* search for string in srch_str	*/
static int
search(int display_message)
{
	int lines_moved;
	int iter;
	int found;

	if ((srch_str == NULL) || (*srch_str == '\0'))
		return (FALSE);
	if (display_message) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, "%s", searching_msg);
		wrefresh(com_win);
		clear_com_win = TRUE;
	}
	lines_moved = 0;
	found = FALSE;
	srch_line = curr_line;
	srch_1 = point;
	if (position < curr_line->line_length)
		srch_1++;
	iter = position + 1;
	while ((!found) && (srch_line != NULL)) {
		while ((iter < srch_line->line_length) && (!found)) {
			srch_2 = srch_1;
			if (case_sen) {	/* if case sensitive		*/
				srch_3 = srch_str;
				while ((*srch_2 == *srch_3) &&
				    (*srch_3 != '\0')) {
					found = TRUE;
					srch_2++;
					srch_3++;
				}	/* end while	*/
			} else		/* if not case sensitive	*/ {
				srch_3 = u_srch_str;
				while (*srch_3 != '\0') {
					wchar_t wc_text, wc_srch;
					mbstate_t mbs;
					int len_text, len_srch;
					memset(&mbs, 0, sizeof(mbs));
					len_text = (int)mbrtowc(&wc_text,
					    (char *)srch_2,
					    MB_CUR_MAX, &mbs);
					if (len_text <= 0)
						len_text = 1;
					memset(&mbs, 0, sizeof(mbs));
					len_srch = (int)mbrtowc(&wc_srch,
					    (char *)srch_3,
					    MB_CUR_MAX, &mbs);
					if (len_srch <= 0)
						len_srch = 1;
					if (towupper((wint_t)wc_text) !=
					    towupper((wint_t)wc_srch))
						break;
					found = TRUE;
					srch_2 += len_text;
					srch_3 += len_srch;
				}
			}	/* end else	*/
			if (!((*srch_3 == '\0') && (found))) {
				found = FALSE;
				if (iter < srch_line->line_length)
					srch_1++;
				iter++;
			}
		}
		if (!found) {
			srch_line = srch_line->next_line;
			if (srch_line != NULL)
				srch_1 = srch_line->line;
			iter = 1;
			lines_moved++;
		}
	}
	if (found) {
		if (display_message) {
			wmove(com_win, 0, 0);
			wclrtoeol(com_win);
			wrefresh(com_win);
		}
		if (lines_moved == 0) {
			while (position < iter)
				right(TRUE);
		} else {
			if (lines_moved < 30) {
				move_rel('d', lines_moved);
				while (position < iter)
					right(TRUE);
			} else {
				absolute_lin += lines_moved;
				curr_line = srch_line;
				point = srch_1;
				position = iter;
				scanline(point);
				scr_pos = scr_horz;
				midscreen((last_line / 2), point);
			}
		}
	} else {
		if (display_message) {
			wmove(com_win, 0, 0);
			wclrtoeol(com_win);
			wprintw(com_win, str_not_found_msg, srch_str);
			wrefresh(com_win);
		}
		wmove(text_win, scr_vert, (scr_horz - horiz_offset));
	}
	return (found);
}

/* prompt and read search string (srch_str)	*/
static void
search_prompt(void)
{
	char *query;
	size_t alloc, used, qlen;

	if (srch_str != NULL) {
		free(srch_str);
		srch_str = NULL;
	}
	if (u_srch_str != NULL) {
		free(u_srch_str);
		u_srch_str = NULL;
	}
	query = get_string(search_prompt_str, FALSE);
	if (query == NULL)
		return;			/* cancelled */

	srch_str = (unsigned char *)query;
	gold = FALSE;

	/*
	 * Build the upper-cased form used for case-insensitive search.
	 * The buffer is sized generously (one full multibyte character
	 * per input byte at most) so that no expansion of a character
	 * can overflow it.
	 */
	qlen = strlen(query);
	if (qlen > ((SIZE_MAX - 1) / (size_t)MB_LEN_MAX)) {
		/* An impossible query length; treat as "not found". */
		search(TRUE);
		return;
	}
	alloc = qlen * (size_t)MB_LEN_MAX + 1;
	u_srch_str = malloc(alloc);
	if (u_srch_str == NULL) {
		search(TRUE);
		return;
	}
	srch_3 = srch_str;
	srch_1 = u_srch_str;
	used = 0;
	while ((*srch_3 != '\0') && ((used + MB_LEN_MAX) < alloc)) {
		if (*srch_3 >= 0x80) {
			wchar_t wc;
			mbstate_t mbs;
			int clen;
			size_t n;

			memset(&mbs, 0, sizeof(mbs));
			clen = (int)mbrtowc(&wc, (char *)srch_3,
			    (size_t)utf8_len(srch_3), &mbs);
			if (clen > 0) {
				wc = (wchar_t)towupper((wint_t)wc);
				memset(&mbs, 0, sizeof(mbs));
				n = wcrtomb((char *)srch_1, wc, &mbs);
				if (n != (size_t)-1) {
					srch_1 += n;
					used += n;
				}
				srch_3 += clen;
			} else {
				*srch_1++ = *srch_3++;
				used++;
			}
		} else {
			*srch_1 = (unsigned char)toupper((unsigned char)*srch_3);
			srch_1++;
			srch_3++;
			used++;
		}
	}
	*srch_1 = '\0';
	search(TRUE);
}

/* delete current character	*/
static void
del_char(void)
{
	in = 8;  /* backspace */
	if (position <
	    curr_line->line_length) {	/* if not end of line	*/
		int clen = utf8_len(point);
		if (position + clen > curr_line->line_length)
			clen = curr_line->line_length - position;
		point += clen;
		position += clen;
		scanline(point);
		delete(TRUE);
	} else {
		right(TRUE);
		delete(TRUE);
	}
}

/* undelete last deleted character	*/
static void
undel_char(void)
{
	if (d_char[0] == '\n')	/* insert line if last del_char deleted eol */
		insert_line(TRUE);
	else if ((unsigned char)d_char[0] >= 0x80)
		insert_utf8(d_char, (int)strlen((char *)d_char));
	else {
		in = d_char[0];
		insert(in);
	}
}

/* delete word in front of cursor	*/
static void
del_word(void)
{
	int tposit;
	int difference;
	unsigned char *d_word2;
	unsigned char *d_word3;
	unsigned char tmp_char[5];

	if (d_word != NULL)
		free(d_word);
	d_word = xmalloc((size_t)curr_line->line_length);
	memcpy(tmp_char, d_char, sizeof(tmp_char));
	d_word3 = point;
	d_word2 = d_word;
	tposit = position;
	while ((tposit < curr_line->line_length) &&
	    ((*d_word3 != ' ') && (*d_word3 != '\t'))) {
		tposit++;
		*d_word2 = *d_word3;
		d_word2++;
		d_word3++;
	}
	while ((tposit < curr_line->line_length) &&
	    ((*d_word3 == ' ') || (*d_word3 == '\t'))) {
		tposit++;
		*d_word2 = *d_word3;
		d_word2++;
		d_word3++;
	}
	*d_word2 = '\0';
	d_wrd_len = difference = (int)(d_word2 - d_word);
	d_word2 = point;
	while (tposit < curr_line->line_length) {
		tposit++;
		*d_word2 = *d_word3;
		d_word2++;
		d_word3++;
	}
	curr_line->line_length -= difference;
	*d_word2 = '\0';
	draw_line(scr_vert, scr_horz, point, position, curr_line->line_length);
	memcpy(d_char, tmp_char, sizeof(d_char));
	text_changes = TRUE;
	formatted = FALSE;
}

/* undelete last deleted word		*/
static void
undel_word(void)
{
	int temp;
	int tposit;
	unsigned char *tmp_old_ptr;
	unsigned char *tmp_space;
	unsigned char *tmp_ptr;
	unsigned char *d_word_ptr;

	/*
	 |	resize line to handle undeleted word
	 */
	if ((curr_line->max_length - (curr_line->line_length + d_wrd_len)) < 5)
		point = resiz_line(d_wrd_len, curr_line, position);
	tmp_ptr = tmp_space = xmalloc((size_t)curr_line->line_length +
	    (size_t)d_wrd_len);
	d_word_ptr = d_word;
	temp = 1;
	/*
	 |	copy d_word contents into temp space
	 */
	while (temp <= d_wrd_len) {
		temp++;
		*tmp_ptr = *d_word_ptr;
		tmp_ptr++;
		d_word_ptr++;
	}
	tmp_old_ptr = point;
	tposit = position;
	/*
	 |	copy contents of line from curent position to eol into
	 |	temp space
	 */
	while (tposit < curr_line->line_length) {
		temp++;
		tposit++;
		*tmp_ptr = *tmp_old_ptr;
		tmp_ptr++;
		tmp_old_ptr++;
	}
	curr_line->line_length += d_wrd_len;
	tmp_old_ptr = point;
	*tmp_ptr = '\0';
	tmp_ptr = tmp_space;
	tposit = 1;
	/*
	 |	now copy contents from temp space back to original line
	 */
	while (tposit < temp) {
		tposit++;
		*tmp_old_ptr = *tmp_ptr;
		tmp_ptr++;
		tmp_old_ptr++;
	}
	*tmp_old_ptr = '\0';
	free(tmp_space);
	draw_line(scr_vert, scr_horz, point, position, curr_line->line_length);
}

/* delete from cursor to end of line	*/
static void
del_line(void)
{
	unsigned char *dl1;
	unsigned char *dl2;
	int tposit;

	if (d_line != NULL)
		free(d_line);
	d_line = xmalloc((size_t)curr_line->line_length);
	dl1 = d_line;
	dl2 = point;
	tposit = position;
	while (tposit < curr_line->line_length) {
		*dl1 = *dl2;
		dl1++;
		dl2++;
		tposit++;
	}
	dlt_line->line_length = 1 + tposit - position;
	*dl1 = '\0';
	*point = '\0';
	curr_line->line_length = position;
	wclrtoeol(text_win);
	if (curr_line->next_line != NULL) {
		right(FALSE);
		delete(FALSE);
	}
	text_changes = TRUE;
}

/* undelete last deleted line		*/
static void
undel_line(void)
{
	unsigned char *ud1;
	unsigned char *ud2;
	int tposit;

	if (dlt_line->line_length == 0)
		return;

	insert_line(TRUE);
	left(TRUE);
	point = resiz_line(dlt_line->line_length, curr_line, position);
	curr_line->line_length += dlt_line->line_length - 1;
	ud1 = point;
	ud2 = d_line;
	tposit = 1;
	while (tposit < dlt_line->line_length) {
		tposit++;
		*ud1 = *ud2;
		ud1++;
		ud2++;
	}
	*ud1 = '\0';
	draw_line(scr_vert, scr_horz, point, position, curr_line->line_length);
}

/* advance to next word		*/
static void
adv_word(void)
{
	while ((position < curr_line->line_length) &&
	    ((*point != 32) && (*point != 9)))
		right(TRUE);
	while ((position < curr_line->line_length) &&
	    ((*point == 32) || (*point == 9)))
		right(TRUE);
}

/* move relative to current line	*/
static void
move_rel(int direction, int lines)
{
	int i;
	char *tmp;

	if (direction == 'u') {
		scr_pos = 0;
		while (position > 1)
			left(TRUE);
		for (i = 0; i < lines; i++) {
			up();
		}
		if ((last_line > 5) && (scr_vert < 4)) {
			tmp = (char *)point;
			tmp_line = curr_line;
			for (i = 0; (i < 5) && (curr_line->prev_line != NULL);
			    i++) {
				up();
			}
			scr_vert = scr_vert + i;
			curr_line = tmp_line;
			absolute_lin += i;
			point = (unsigned char *)tmp;
			scanline(point);
		}
	} else {
		if ((position != 1) && (curr_line->next_line != NULL)) {
			nextline();
			scr_pos = scr_horz = 0;
			if (horiz_offset) {
				horiz_offset = 0;
				midscreen(scr_vert, point);
			}
		} else
			adv_line();
		for (i = 1; i < lines; i++) {
			down();
		}
		if ((last_line > 10) && (scr_vert > (last_line - 5))) {
			tmp = (char *)point;
			tmp_line = curr_line;
			for (i = 0; (i < 5) && (curr_line->next_line != NULL);
			    i++) {
				down();
			}
			absolute_lin -= i;
			scr_vert = scr_vert - i;
			curr_line = tmp_line;
			point = (unsigned char *)tmp;
			scanline(point);
		}
	}
	wmove(text_win, scr_vert, (scr_horz - horiz_offset));
}

/* go to end of line			*/
static void
eol(void)
{
	if (position < curr_line->line_length) {
		while (position < curr_line->line_length)
			right(TRUE);
	} else if (curr_line->next_line != NULL) {
		right(TRUE);
		while (position < curr_line->line_length)
			right(TRUE);
	}
}

/* move to beginning of line	*/
static void
bol(void)
{
	if (point != curr_line->line) {
		while (point != curr_line->line)
			left(TRUE);
	} else if (curr_line->prev_line != NULL) {
		scr_pos = 0;
		up();
	}
}

/* advance to beginning of next line	*/
static void
adv_line(void)
{
	if ((point != curr_line->line) || (scr_pos > 0)) {
		while (position < curr_line->line_length)
			right(TRUE);
		right(TRUE);
	} else if (curr_line->next_line != NULL) {
		scr_pos = 0;
		down();
	}
}

static void
from_top(void)
{
	struct text *tmpline = first_line;
	int x = 1;

	while ((tmpline != NULL) && (tmpline != curr_line)) {
		x++;
		tmpline = tmpline->next_line;
	}
	absolute_lin = x;
}

/* execute shell command			*/
static void
sh_command(char *string)
{
	char *temp_point;
	char *last_slash;
	char *path;		/* directory path to executable		*/
	int parent;		/* zero if child, child's pid if parent	*/
	int value;
	int return_val;
	struct text *line_holder;

	if (restrict_mode()) {
		return;
	}

	if (!(path = getenv("SHELL")))
		path = "/bin/sh";
	last_slash = temp_point = path;
	while (*temp_point != '\0') {
		if (*temp_point == '/')
			last_slash = ++temp_point;
		else
			temp_point++;
	}

	/*
	 |	if in_pipe is true, then output of the shell operation will be
	 |	read by the editor, and curses doesn't need to be turned off
	 */

	if (!in_pipe) {
		keypad(com_win, FALSE);
		keypad(text_win, FALSE);
		echo();
		nl();
		noraw();
		resetty();
		endwin();
	}

	if (in_pipe) {
		if (pipe(pipe_in) != 0) {
			fprintf(stderr, "ee: pipe: %s\n", strerror(errno));
			return;
		}
		parent = fork();
		if (parent < 0) {
			close(pipe_in[0]);
			close(pipe_in[1]);
			fprintf(stderr, "ee: fork: %s\n", strerror(errno));
			return;
		}
		if (!parent) {		/* if the child		*/
/*
 |  child process which will fork and exec shell command (if shell output is
 |  to be read by editor)
 */
			in_pipe = FALSE;
/*
 |  redirect stdout and stderr to the pipe
 */
			close(1);
			if (dup(pipe_in[1]) < 0)
				_exit(127);
			close(2);
			if (dup(pipe_in[1]) < 0)
				_exit(127);
			close(pipe_in[1]);
			/*
			 |	child will now continue down 'if (!in_pipe)'
			 |	path below
			 */
		} else  /* if the parent	*/ {
/*
 |  prepare editor to read from the pipe
 */
			signal(SIGCHLD, SIG_IGN);
			line_holder = curr_line;
			tmp_vert = scr_vert;
			close(pipe_in[1]);
			get_fd = pipe_in[0];
			get_file("");
			close(pipe_in[0]);
			scr_vert = tmp_vert;
			scr_horz = scr_pos = 0;
			position = 1;
			curr_line = line_holder;
			from_top();
			point = curr_line->line;
			out_pipe = FALSE;
			signal(SIGCHLD, SIG_DFL);
/*
 |  since flag "in_pipe" is still TRUE, the path which waits for the child
 |  process to die will be avoided.
 |  (the pipe is closed, no more output can be expected)
 */
		}
	}
	if (!in_pipe) {
		signal(SIGINT, SIG_IGN);
		if (out_pipe && (pipe(pipe_out) != 0)) {
			out_pipe = FALSE;
			fprintf(stderr, "ee: pipe: %s\n", strerror(errno));
		}
/*
 |  fork process which will exec command
 */
		parent = fork();
		if (parent < 0) {
			if (out_pipe) {
				close(pipe_out[0]);
				close(pipe_out[1]);
				out_pipe = FALSE;
			}
			ee_install_sigint();
			fprintf(stderr, "ee: fork: %s\n", strerror(errno));
			return;
		}
		if (!parent) {		/* if the child	*/
			if (shell_fork)
				putchar('\n');
			if (out_pipe) {
/*
 |  prepare the child process (soon to exec a shell command) to read from the
 |  pipe (which will be output from the editor's buffer)
 */
				close(0);
				if (dup(pipe_out[0]) < 0)
					_exit(127);
				close(pipe_out[0]);
				close(pipe_out[1]);
			}
			for (value = 1; value < 24; value++)
				signal(value, SIG_DFL);
			execl(path, last_slash, "-c", string, NULL);
			fprintf(stderr,
			    "unable to execute command %s\n", path);
			_exit(127);
		} else	/* if the parent	*/ {
			if (out_pipe) {
/*
 |  output the contents of the buffer to the pipe (to be read by the
 |  process forked and exec'd above as stdin)
 */
				close(pipe_out[0]);
				line_holder = first_line;
				while (line_holder != NULL) {
					write(pipe_out[1], line_holder->line,
					    (size_t)(line_holder->line_length - 1));
					write(pipe_out[1], "\n", 1);
					line_holder = line_holder->next_line;
				}
				close(pipe_out[1]);
				out_pipe = FALSE;
			}
			for (;;) {
				return_val = waitpid(parent, NULL, 0);
				if ((return_val == parent) || (return_val < 0 &&
				    errno != EINTR))
					break;
			}
/*
 |  if this process is actually the child of the editor, exit.  Here's how it
 |  works:
 |  The editor forks a process.  If output must be sent to the command to be
 |  exec'd another process is forked, and that process (the child's child)
 |  will exec the command.  In this case, "shell_fork" will be FALSE.  If no
 |  output is to be performed to the shell command, "shell_fork" will be TRUE.
 |  If this is the editor process, shell_fork will be true, otherwise this is
 |  the child of the edit process.
 */
			if (!shell_fork)
				exit(0);
		}
		ee_install_sigint();
	}
	if (shell_fork) {
		fputs(continue_msg, stdout);
		fflush(stdout);
		while ((in = getchar()) != '\n')
			;
	}

	if (!in_pipe) {
		fixterm();
		noecho();
		nonl();
		raw();
		keypad(text_win, TRUE);
		keypad(com_win, TRUE);
		/*
		 * The terminal may have been resized while the shell was
		 * running.  Force a layout rebuild so that the restored
		 * screen matches the current size.
		 */
		local_LINES = -1;
		local_COLS = -1;
		resize_check();
	}

	redraw();
}

/*
 * Discard every derived window.  Called before (re)building the
 * layout so that resizing never leaks or reuses a stale window.
 */
static void
free_windows(void)
{
	if (title_win != NULL) {
		delwin(title_win);
		title_win = NULL;
	}
	if (key_win != NULL) {
		delwin(key_win);
		key_win = NULL;
	}
	if (com_win != NULL) {
		delwin(com_win);
		com_win = NULL;
	}
	if (text_win != NULL) {
		delwin(text_win);
		text_win = NULL;
	}
	if (help_win != NULL) {
		delwin(help_win);
		help_win = NULL;
	}
}

/*
 * Central layout computation.  One title/status row at the top, one
 * prompt/message row and one optional shortcut row at the bottom, and
 * the text area in between.  The layout degrades gracefully on very
 * small terminals instead of using negative sizes.
 */
static void
set_up_term(void)
{
	int rows, cols;
	int bar = 0;
	int title = 0;

	if (!curses_initialized) {
		if (initscr() == NULL) {
			fprintf(stderr,
			    "ee: unable to initialize the terminal\n");
			exit(1);
		}
		savetty();
		noecho();
		raw();
		nonl();
		keypad(stdscr, TRUE);
		curses_initialized = TRUE;
	}

	free_windows();

	rows = LINES;
	cols = COLS;
	if (rows < 1)
		rows = 1;
	if (cols < 1)
		cols = 1;

	if (info_window && (rows >= 5))
		bar = 1;
	if (rows >= (bar + 3))
		title = 1;

	text_rows = rows - title - bar - 1;
	if (text_rows < 1)
		text_rows = 1;
	text_top = title;

	last_line = text_rows - 1;
	last_col = cols - 1;

	idlok(stdscr, TRUE);

	/*
	 * Create only the windows the layout actually uses.  On a tiny
	 * terminal the title or the shortcut bar may not fit; creating
	 * them anyway would make two windows overlap.
	 */
	if (title) {
		title_win = newwin(1, cols, 0, 0);
		if (title_win != NULL) {
			keypad(title_win, TRUE);
			idlok(title_win, TRUE);
		}
	}

	text_win = newwin(text_rows, cols, text_top, 0);
	if (text_win != NULL) {
		keypad(text_win, TRUE);
		idlok(text_win, TRUE);
	}

	com_win = newwin(1, cols, rows - 1 - bar, 0);
	if (com_win != NULL) {
		keypad(com_win, TRUE);
		idlok(com_win, TRUE);
		wrefresh(com_win);
	}

	if (bar) {
		key_win = newwin(1, cols, rows - 1, 0);
		if (key_win != NULL) {
			keypad(key_win, TRUE);
			idlok(key_win, TRUE);
		}
	}

	help_win = newwin(rows, cols, 0, 0);
	if (help_win != NULL) {
		keypad(help_win, TRUE);
		idlok(help_win, TRUE);
	}

	if ((text_win == NULL) || (com_win == NULL) || (help_win == NULL)) {
		endwin();
		fprintf(stderr, "ee: unable to create terminal windows\n");
		exit(1);
	}

	if (shortcut_bar.count == 0)
		default_shortcuts();
	paint_status_line();
	paint_shortcut_bar();
	wrefresh(text_win);

	local_LINES = rows;
	local_COLS = cols;
}

/* React to SIGWINCH (delivered as KEY_RESIZE) and size changes. */
static void
resize_check(void)
{
	if ((LINES == local_LINES) && (COLS == local_COLS))
		return;

	set_up_term();

	/* keep the cursor inside the new text area */
	if (scr_vert > last_line)
		scr_vert = last_line;
	if (scr_vert < 0)
		scr_vert = 0;

	redraw();
	wrefresh(text_win);
}

static char item_alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789 ";

/*
 * Geometry of a pop-up menu.  Computed from the current terminal size so
 * that a menu always fits: on a narrow terminal the item text is clipped
 * rather than the menu being refused.
 */
struct menu_geometry {
	int list_size;
	int max_width, max_height;
	int x_off, y_off;
	int top_offset, vert_size;
};

static void
menu_geometry(struct menu_entries menu_list[], struct menu_geometry *g)
{
	int counter, length;

	g->list_size = 1;
	while (menu_list[g->list_size + 1].item_string != NULL)
		g->list_size++;
	g->max_width = 0;
	for (counter = 0; counter <= g->list_size; counter++) {
		length = (int)strlen(menu_list[counter].item_string);
		if (length > g->max_width)
			g->max_width = length;
	}
	g->max_width += 3;
	length = (int)strlen(menu_cancel_msg);
	if (length > g->max_width)
		g->max_width = length;
	length = (int)strlen(more_above_str);
	if (length > g->max_width)
		g->max_width = length;
	length = (int)strlen(more_below_str);
	if (length > g->max_width)
		g->max_width = length;
	g->max_width += 6;

	if (g->max_width > COLS)
		g->max_width = COLS;
	if (g->max_width < 1)
		g->max_width = 1;

	g->top_offset = 0;
	if (g->list_size > LINES) {
		g->max_height = LINES;
		g->vert_size = (g->max_height > 11) ?
		    g->max_height - 8 : g->max_height;
	} else {
		g->vert_size = g->list_size;
		g->max_height = g->list_size;
	}
	if (LINES >= (g->vert_size + 8)) {
		if (menu_list[0].argument != MENU_WARN)
			g->max_height = g->vert_size + 8;
		else
			g->max_height = g->vert_size + 7;
		g->top_offset = 4;
	}
	if (g->max_height > LINES)
		g->max_height = LINES;
	if (g->max_height < 1)
		g->max_height = 1;

	g->x_off = (COLS - g->max_width) / 2;
	if (g->x_off < 0)
		g->x_off = 0;
	g->y_off = (LINES - g->max_height - 1) / 2;
	if (g->y_off < 0)
		g->y_off = 0;
}

static int
menu_op(struct menu_entries menu_list[])
{
	WINDOW *temp_win;
	struct menu_geometry g;
	int max_width, max_height;
	int counter;
	int input;
	int temp = 0;
	int list_size;
	int top_offset;		/* offset from top where menu items start */
	int vert_size;		/* vertical size for menu list item display */
	int off_start = 1;	/* offset from start of menu items to start display */

	menu_geometry(menu_list, &g);
	list_size = g.list_size;
	max_width = g.max_width;
	max_height = g.max_height;
	top_offset = g.top_offset;
	vert_size = g.vert_size;

	temp_win = newwin(max_height, max_width, g.y_off, g.x_off);
	if (temp_win == NULL) {
		wmove(com_win, 0, 0);
		werase(com_win);
		wprintw(com_win, "%s", menu_too_lrg_msg);
		wrefresh(com_win);
		clear_com_win = TRUE;
		return (0);
	}
	keypad(temp_win, TRUE);
	curs_set(0);

	counter = 1;
	paint_menu(menu_list, max_width, max_height, list_size, top_offset,
	    temp_win, off_start, vert_size, counter);

	do {
		if (off_start > 2)
			wmove(temp_win, (1 + counter + top_offset - off_start),
			    MENU_ITEM_COL);
		else
			wmove(temp_win, (counter + top_offset - off_start),
			    MENU_ITEM_COL);

		wrefresh(temp_win);
		{
			wint_t win;
			int wret = wget_wch(temp_win, &win);

			if (wret == ERR) {
				if (ee_intr_flag)
					edit_abort(0);
				exit(0);
			}
			if ((wret == KEY_CODE_YES) && (win == KEY_RESIZE)) {
				/*
				 * Rebuild the menu for the new terminal
				 * size, keeping the current selection and
				 * scroll position.
				 */
				delwin(temp_win);
				resize_check();
				menu_geometry(menu_list, &g);
				list_size = g.list_size;
				max_width = g.max_width;
				max_height = g.max_height;
				top_offset = g.top_offset;
				vert_size = g.vert_size;
				if (counter > list_size)
					counter = list_size;
				if (counter < 1)
					counter = 1;
				if (off_start > list_size)
					off_start = 1;
				temp_win = newwin(max_height, max_width,
				    g.y_off, g.x_off);
				if (temp_win == NULL) {
					wmove(com_win, 0, 0);
					werase(com_win);
					wprintw(com_win, "%s",
					    menu_too_lrg_msg);
					wrefresh(com_win);
					clear_com_win = TRUE;
					return (0);
				}
				keypad(temp_win, TRUE);
				curs_set(0);
				paint_menu(menu_list, max_width, max_height,
				    list_size, top_offset, temp_win, off_start,
				    vert_size, counter);
				continue;
			}
			if ((wret == KEY_CODE_YES) && (win == KEY_ENTER))
				in = input = '\n';
			else
				in = input = (int)win;
		}

		/*
		 * Menu entries are selected by their first letter or
		 * digit, compared with explicit ASCII ranges: the menu
		 * is an ASCII grammar and must not depend on the
		 * locale's character classification.
		 */
		if ((input >= 'a' && input <= 'z') ||
		    (input >= 'A' && input <= 'Z') ||
		    (input >= '0' && input <= '9')) {
			int c = (input >= 'A' && input <= 'Z') ?
			    input + ('a' - 'A') : input;

			if (c >= 'a' && c <= 'z')
				temp = 1 + c - 'a';
			else
				temp = (2 + 'z' - 'a') + (c - '0');

			if (temp <= list_size) {
				input = '\n';
				counter = temp;
			}
		} else {
			switch (input) {
			case ' ':	/* space	*/
			case '\004':	/* ^d, down	*/
			case KEY_RIGHT:
			case KEY_DOWN:
				counter++;
				if (counter > list_size)
					counter = 1;
				break;
			case '\010':	/* ^h, backspace*/
			case '\025':	/* ^u, up	*/
			case 127:	/* ^?, delete	*/
			case KEY_BACKSPACE:
			case KEY_LEFT:
			case KEY_UP:
				counter--;
				if (counter == 0)
					counter = list_size;
				break;
			case '\033':	/* escape key	*/
				if (menu_list[0].argument != MENU_WARN)
					counter = 0;
				break;
			case '\014':	/* ^l       	*/
			case '\022':	/* ^r, redraw	*/
				paint_menu(menu_list, max_width, max_height,
				    list_size, top_offset, temp_win,
				    off_start, vert_size, counter);
				break;
			default:
				break;
			}
		}

		if (((list_size - off_start) >= (vert_size - 1)) &&
		    (counter > (off_start + vert_size - 3)) &&
		    (off_start > 1)) {
			if (counter == list_size)
				off_start = (list_size - vert_size) + 2;
			else
				off_start++;

			paint_menu(menu_list, max_width, max_height,
			    list_size, top_offset, temp_win, off_start,
			    vert_size, counter);
		} else if ((list_size != vert_size) &&
		    (counter > (off_start + vert_size - 2))) {
			if (counter == list_size)
				off_start = 2 + (list_size - vert_size);
			else if (off_start == 1)
				off_start = 3;
			else
				off_start++;

			paint_menu(menu_list, max_width, max_height,
			    list_size, top_offset, temp_win, off_start,
			    vert_size, counter);
		} else if (counter < off_start) {
			if (counter <= 2)
				off_start = 1;
			else
				off_start = counter;

			paint_menu(menu_list, max_width, max_height,
			    list_size, top_offset, temp_win, off_start,
			    vert_size, counter);
		}

		paint_menu(menu_list, max_width, max_height, list_size,
		    top_offset, temp_win, off_start, vert_size, counter);
	} while ((input != '\r') && (input != '\n') && (counter != 0));

	werase(temp_win);
	wrefresh(temp_win);
	delwin(temp_win);
	curs_set(1);

	/* dispatch on the exact, non-NULL entry of each menu item */
	if (menu_list[counter].argument != -1 &&
	    menu_list[counter].iprocedure != NULL)
		(*menu_list[counter].iprocedure)(menu_list[counter].argument);
	else if (menu_list[counter].ptr_argument != NULL &&
	    menu_list[counter].procedure != NULL)
		(*menu_list[counter].procedure)(
		    menu_list[counter].ptr_argument);
	else if (menu_list[counter].nprocedure != NULL)
		(*menu_list[counter].nprocedure)();

	paint_status_line();
	paint_shortcut_bar();
	redraw();

	return (counter);
}

static void
paint_menu_item(struct menu_entries menu_list[], int item, int list_size,
    WINDOW *menu_win, int row, int max_width, int highlight)
{
	int column, avail;

	wmove(menu_win, row, MENU_ITEM_COL);
	if (!nohighlight && highlight) {
		wstandout(menu_win);
		for (column = MENU_ITEM_COL; column < (max_width - 2);
		    column++)
			waddch(menu_win, ' ');
		wmove(menu_win, row, MENU_ITEM_COL);
	}
	avail = max_width - MENU_ITEM_COL - 1;
	if (avail < 0)
		avail = 0;
	if (list_size > 1) {
		char prefix[8];
		int plen;

		snprintf(prefix, sizeof(prefix), "%c) ",
		    item_alpha[ee_min((item - 1), max_alpha_char)]);
		plen = (int)strlen(prefix);
		waddnstr(menu_win, prefix, avail);
		if (plen < avail)
			waddnstr(menu_win, menu_list[item].item_string,
			    avail - plen);
	} else
		waddnstr(menu_win, menu_list[item].item_string, avail);
	if (!nohighlight && highlight)
		wstandend(menu_win);
}

static void
paint_menu(struct menu_entries menu_list[], int max_width, int max_height,
    int list_size, int top_offset, WINDOW *menu_win, int off_start,
    int vert_size, int selection)
{
	int counter, temp_int;

	werase(menu_win);

	/*
	 |	output the title and the separating rules only if the
	 |	window is large enough for the framed layout
	 */

	if (max_height > vert_size) {
		wmove(menu_win, 1, MENU_ITEM_COL);
		if (!nohighlight)
			wstandout(menu_win);
		waddstr(menu_win, menu_list[0].item_string);
		if (!nohighlight)
			wstandend(menu_win);

		wmove(menu_win, 2, 1);
		for (counter = 0; counter < (max_width - 2); counter++)
			waddch(menu_win, '-');

		if (menu_list[0].argument != MENU_WARN) {
			wmove(menu_win, (max_height - 4), 1);
			for (counter = 0; counter < (max_width - 2);
			    counter++)
				waddch(menu_win, '-');
			wmove(menu_win, (max_height - 3), MENU_ITEM_COL);
			waddstr(menu_win, menu_cancel_msg);
		}
	}

	if (list_size > vert_size) {
		if (off_start >= 3) {
			temp_int = 1;
			wmove(menu_win, top_offset, MENU_ITEM_COL);
			waddstr(menu_win, more_above_str);
		} else
			temp_int = 0;

		for (counter = off_start;
		    ((temp_int + counter - off_start) < (vert_size - 1));
		    counter++) {
			paint_menu_item(menu_list, counter, list_size,
			    menu_win,
			    (top_offset + temp_int + (counter - off_start)),
			    max_width, (counter == selection));
		}

		if (counter == list_size)
			paint_menu_item(menu_list, counter, list_size,
			    menu_win, (top_offset + (vert_size - 1)),
			    max_width, (counter == selection));
		else {
			wmove(menu_win, (top_offset + (vert_size - 1)),
			    MENU_ITEM_COL);
			wprintw(menu_win, "%s", more_below_str);
		}
	} else {
		for (counter = 1; counter <= list_size; counter++) {
			paint_menu_item(menu_list, counter, list_size,
			    menu_win, (top_offset + counter - 1),
			    max_width, (counter == selection));
		}
	}
}


static void
paint_help_hint(int row, int top, int total, int pagesize)
{
	int page = (pagesize > 0) ? (top / pagesize) + 1 : 1;
	int pages = (pagesize > 0) ? ((total + pagesize - 1) / pagesize) : 1;

	wmove(help_win, row, 0);
	wclrtoeol(help_win);
	if (!nohighlight)
		wstandout(help_win);
	if (total > pagesize)
		wprintw(help_win,
		    " Space/^N: next   ^P: previous   Esc: close   page %d/%d ",
		    page, pages);
	else
		wprintw(help_win, " Press Esc to close ");
	if (!nohighlight)
		wstandend(help_win);
}

static void
help(void)
{
	char line[256];
	int total, top = 0, rows, cols, pagesize;
	int i, done = 0, header;

	total = ee_help_count(emacs_keys_mode);

	for (;;) {
		rows = getmaxy(help_win);
		cols = getmaxx(help_win);
		pagesize = rows - 1;
		if (pagesize < 1)
			pagesize = 1;
		if (top > total - 1)
			top = (total > pagesize) ? total - pagesize : 0;
		if (top < 0)
			top = 0;

		werase(help_win);
		clearok(help_win, TRUE);
		for (i = 0; (i < pagesize) && ((top + i) < total); i++) {
			ee_help_line(emacs_keys_mode, top + i, line,
			    sizeof(line), &header);
			wmove(help_win, i, 0);
			if (header && !nohighlight)
				wstandout(help_win);
			waddnstr(help_win, line, cols - 1);
			if (header && !nohighlight)
				wstandend(help_win);
		}
		paint_help_hint(rows - 1, top, total, pagesize);
		wrefresh(help_win);

		{
			wint_t win;
			int wret = wget_wch(help_win, &win);

			if (wret == ERR) {
				if (ee_intr_flag)
					edit_abort(0);
				exit(0);
			}
			if ((wret == KEY_CODE_YES) && (win == KEY_RESIZE)) {
				resize_check();
				continue;
			}
			if (wret == KEY_CODE_YES) {
				if (win == KEY_DOWN) {
					if (top + pagesize < total)
						top += pagesize;
				} else if (win == KEY_UP) {
					top -= pagesize;
					if (top < 0)
						top = 0;
				} else if (win == KEY_ENTER) {
					done = 1;
					break;
				}
				continue;
			}
			switch ((int)win) {
			case 27:	/* Esc */
			case 'q':
			case '\n':
			case '\r':
				done = 1;
				break;
			case ' ':
			case '\016':	/* ^N */
				if (top + pagesize < total)
					top += pagesize;
				break;
			case '\020':	/* ^P */
				top -= pagesize;
				if (top < 0)
					top = 0;
				break;
			default:
				break;
			}
		}
		if (done)
			break;
	}

	werase(help_win);
	wrefresh(help_win);
	redraw();
}

/*
 * The title/status bar is persistent and monochrome.  It shows the
 * program name, the file, the cursor position, a progress percentage
 * and the buffer state.  Reverse video (unless -h) provides the only
 * visual emphasis.
 */
static void
paint_status_line(void)
{
	char right[120];
	const char *name;
	int cols, width, right_col, avail;
	int percent;

	if (title_win == NULL)
		return;

	cols = getmaxx(title_win);
	name = ((in_file_name != NULL) && (*in_file_name != '\0')) ?
	    (const char *)in_file_name : no_file_string;

	/*
	 * The percentage runs from 0 on the first line to 100 on the last
	 * line of a multi-line file; a single-line (or empty) buffer is
	 * reported as 100%.  This is line-based, not byte-based.
	 */
	percent = 100;
	if (total_lines > 1)
		percent = (int)(((long)(curr_line->line_number - 1) * 100) /
		    (total_lines - 1));
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;

	if (read_only)
		snprintf(right, sizeof(right),
		    "Ln %d, Col %d  %d%%  [Read Only]",
		    curr_line->line_number, scr_horz + 1, percent);
	else if (text_changes)
		snprintf(right, sizeof(right), "Ln %d, Col %d  %d%%  [Modified]",
		    curr_line->line_number, scr_horz + 1, percent);
	else
		snprintf(right, sizeof(right), "Ln %d, Col %d  %d%%",
		    curr_line->line_number, scr_horz + 1, percent);

	width = utf8_strwidth(right);
	right_col = cols - width - 1;
	if (right_col < 0)
		right_col = 0;

	werase(title_win);
	if (!nohighlight) {
		int c;

		wstandout(title_win);
		for (c = 0; c < cols; c++)
			waddch(title_win, ' ');
		wstandend(title_win);
	}
	/*
	 * Draw the program and file name directly, clipped against the
	 * right-aligned status, so that no fixed-size path buffer is
	 * involved and a long name cannot overrun the bar.
	 */
	wmove(title_win, 0, 0);
	avail = right_col - 2;
	if (avail < 0)
		avail = 0;
	waddnstr(title_win, prog_name, avail);
	avail -= (int)strlen(prog_name);
	if (avail > 0) {
		waddch(title_win, ' ');
		waddch(title_win, ' ');
		avail -= 2;
	}
	if (avail > 0)
		waddnstr(title_win, name, avail);
	if (right_col > 0) {
		wmove(title_win, 0, right_col);
		if (!nohighlight)
			wstandout(title_win);
		waddstr(title_win, right);
		if (!nohighlight)
			wstandend(title_win);
	}
	wrefresh(title_win);
}

static void
set_shortcuts(const char *const *items, int count)
{
	int i;

	if (count > SHORTCUT_MAX)
		count = SHORTCUT_MAX;
	if (count < 0)
		count = 0;
	for (i = 0; i < count; i++)
		shortcut_bar.items[i] = items[i];
	shortcut_bar.count = count;
}

static void
default_shortcuts(void)
{
	static const char *const normal[] = {
		"^[ Menu", "^S Save", "^Q Quit", "^E Search", "^X Find",
		"^W Cut word", "^V Paste word", "^Y Cut line",
		"^Z Paste line", "^U Up", "^D Down", "^C Command"
	};

	set_shortcuts(normal, (int)(sizeof(normal) / sizeof(normal[0])));
}

static void
paint_shortcut_bar(void)
{
	int i, col, cols;

	if (key_win == NULL)
		return;

	cols = getmaxx(key_win);
	werase(key_win);
	if (!nohighlight) {
		int c;

		wstandout(key_win);
		for (c = 0; c < cols; c++)
			waddch(key_win, ' ');
		wstandend(key_win);
	}

	col = 0;
	for (i = 0; i < shortcut_bar.count; i++) {
		const char *item = shortcut_bar.items[i];
		const char *sp;
		int w;

		if (item == NULL)
			continue;
		w = utf8_strwidth(item);
		if ((col + w + 2) > cols)
			break;
		sp = strchr(item, ' ');
		wmove(key_win, 0, col);
		if (!nohighlight)
			wattron(key_win, A_BOLD);
		if (sp != NULL) {
			waddnstr(key_win, item, (int)(sp - item));
			if (!nohighlight)
				wattroff(key_win, A_BOLD);
			waddstr(key_win, sp);
		} else {
			waddstr(key_win, item);
			if (!nohighlight)
				wattroff(key_win, A_BOLD);
		}
		col += w + 2;
	}
	wrefresh(key_win);
}

static void
no_info_window(void)
{
	if (!info_window)
		return;
	info_window = FALSE;
	set_up_term();
	midscreen(ee_min(scr_vert, last_line), point);
	clear_com_win = TRUE;
}

static void
create_info_window(void)
{
	if (info_window)
		return;
	info_window = TRUE;
	set_up_term();
	midscreen(ee_min(scr_vert, last_line), point);
	clear_com_win = TRUE;
}

/*
 * Save the buffer.  Used by the "save file" menu entry, the "write"
 * command and the ^S shortcut.  If the buffer has no file name yet one
 * is requested.  A cancelled prompt leaves the buffer untouched.
 */
static int
save_op(void)
{
	char *string;
	int have_name;

	if (restrict_mode())
		return (FALSE);

	have_name = ((in_file_name != NULL) && (*in_file_name != '\0'));
	string = (char *)in_file_name;
	if (!have_name) {
		string = get_string(save_file_name_prompt, TRUE);
		if (string == NULL)
			return (FALSE);		/* cancelled */
	}
	if ((string == NULL) || (*string == '\0')) {
		wmove(com_win, 0, 0);
		wprintw(com_win, "%s", file_not_saved_msg);
		wclrtoeol(com_win);
		wrefresh(com_win);
		clear_com_win = TRUE;
		if ((string != NULL) && (string != (char *)in_file_name))
			free(string);
		return (FALSE);
	}
	if (!have_name) {
		tmp_file = resolve_name(string);
		if (tmp_file != string) {
			free(string);
			string = tmp_file;
		}
	}
	if (write_file(string, 1)) {
		in_file_name = (unsigned char *)string;
		text_changes = FALSE;
		read_only = FALSE;
		return (TRUE);
	}
	if (!have_name)
		free(string);
	return (FALSE);
}

static int
file_op(int arg)
{
	char *string;

	if (restrict_mode()) {
		return (0);
	}

	if (arg == READ_FILE) {
		string = get_string(file_read_prompt_str, TRUE);
		if (string == NULL)
			return (0);
		recv_file = TRUE;
		tmp_file = resolve_name(string);
		check_fp();
		if (tmp_file != string)
			free(tmp_file);
		free(string);
	} else if (arg == WRITE_FILE) {
		string = get_string(file_write_prompt_str, TRUE);
		if (string == NULL)
			return (0);
		tmp_file = resolve_name(string);
		write_file(tmp_file, 1);
		if (tmp_file != string)
			free(tmp_file);
		free(string);
	} else if (arg == SAVE_FILE)
		(void)save_op();
	return (0);
}

static void
shell_op(void)
{
	char *string;

	string = get_string(shell_prompt, TRUE);
	if (string == NULL)
		return;
	if (*string != '\0')
		sh_command(string);
	free(string);
}

static void
leave_op(void)
{
	if (text_changes) {
		menu_op(leave_menu);
	} else
		quit(TRUE);
}

static void
redraw(void)
{
	clearok(text_win, TRUE);
	paint_status_line();
	paint_shortcut_bar();
	midscreen(scr_vert, point);
}

/*
 |	The following routines will "format" a paragraph (as defined by a
 |	block of text with blank lines before and after the block).
 */

/* test if line has any non-space characters	*/
static int
Blank_Line(struct text *test_line)
{
	unsigned char *line;
	int length;

	if (test_line == NULL)
		return (TRUE);

	length = 1;
	line = test_line->line;

	/*
	 |	To handle troff/nroff documents, consider a line with a
	 |	period ('.') in the first column to be blank.  To handle mail
	 |	messages with included text, consider a line with a '>' blank.
	 */

	if ((*line == '.') || (*line == '>'))
		return (TRUE);

	while (((*line == ' ') || (*line == '\t')) &&
	    (length < test_line->line_length)) {
		length++;
		line++;
	}
	if (length != test_line->line_length)
		return (FALSE);
	else
		return (TRUE);
}

/* format the paragraph according to set margins	*/
static void
Format(void)
{
	int string_count;
	int offset;
	int temp_case;
	int status;
	int tmp_af;
	int counter;
	unsigned char *line;
	unsigned char *tmp_srchstr;
	unsigned char *temp1, *temp2;
	unsigned char *temp_dword;
	unsigned char temp_d_char[5];

	memcpy(temp_d_char, d_char, sizeof(temp_d_char));

/*
 |	if observ_margins is not set, or the current line is blank,
 |	do not format the current paragraph
 */

	if ((!observ_margins) || (Blank_Line(curr_line)))
		return;

/*
 |	save the currently set flags, and clear them
 */

	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	wprintw(com_win, "%s", formatting_msg);
	wrefresh(com_win);

/*
 |	get current position in paragraph, so after formatting, the cursor
 |	will be in the same relative position
 */

	tmp_af = auto_format;
	auto_format = FALSE;
	offset = position;
	if (position != 1)
		prev_word();
	temp_dword = d_word;
	d_word = NULL;
	temp_case = case_sen;
	case_sen = TRUE;
	tmp_srchstr = srch_str;
	{
		size_t srch_len = (curr_line->line_length > position) ?
		    (size_t)(curr_line->line_length - position) : 0;

		temp2 = srch_str = xmalloc(srch_len + 1);
	}
	if ((*point == ' ') || (*point == '\t'))
		adv_word();
	offset -= position;
	counter = position;
	line = temp1 = point;
	while ((*temp1 != '\0') && (*temp1 != ' ') && (*temp1 != '\t') &&
	    (counter < curr_line->line_length)) {
		*temp2 = *temp1;
		temp2++;
		temp1++;
		counter++;
	}
	*temp2 = '\0';
	if (position != 1)
		bol();
	while (!Blank_Line(curr_line->prev_line))
		bol();
	string_count = 0;
	status = TRUE;
	while ((line != point) && (status)) {
		status = search(FALSE);
		string_count++;
	}

	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	wprintw(com_win, "%s", formatting_msg);
	wrefresh(com_win);

/*
 |	now get back to the start of the paragraph to start formatting
 */

	if (position != 1)
		bol();
	while (!Blank_Line(curr_line->prev_line))
		bol();

	observ_margins = FALSE;

/*
 |	Start going through lines, putting spaces at end of lines if they do
 |	not already exist.  Append lines together to get one long line, and
 |	eliminate spacing at begin of lines.
 */

	while (!Blank_Line(curr_line->next_line)) {
		eol();
		left(TRUE);
		if (*point != ' ') {
			right(TRUE);
			insert(' ');
		} else
			right(TRUE);
		del_char();
		if ((*point == ' ') || (*point == '\t'))
			del_word();
	}

/*
 |	Now there is one long line.  Eliminate extra spaces within the line
 |	after the first word (so as not to blow away any indenting the user
 |	may have put in).
 */

	bol();
	adv_word();
	while (position < curr_line->line_length) {
		if ((*point == ' ') && (*(point + 1) == ' '))
			del_char();
		else
			right(TRUE);
	}

/*
 |	Now make sure there are two spaces after a '.'.
 */

	bol();
	while (position < curr_line->line_length) {
		if ((*point == '.') && (*(point + 1) == ' ')) {
			right(TRUE);
			insert(' ');
			insert(' ');
			while (*point == ' ')
				del_char();
		}
		right(TRUE);
	}

	observ_margins = TRUE;
	bol();

	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	wprintw(com_win, "%s", formatting_msg);
	wrefresh(com_win);

/*
 |	create lines between margins
 */

	while (position < curr_line->line_length) {
		while ((scr_pos < right_margin) &&
		    (position < curr_line->line_length))
			right(TRUE);
		if (position < curr_line->line_length) {
			prev_word();
			if (position == 1)
				adv_word();
			insert_line(TRUE);
		}
	}

/*
 |	go back to begin of paragraph, put cursor back to original position
 */

	bol();
	while (!Blank_Line(curr_line->prev_line))
		bol();

/*
 |	find word cursor was in
 */

	while ((status) && (string_count > 0)) {
		search(FALSE);
		string_count--;
	}

/*
 |	offset the cursor to where it was before from the start of the word
 */

	while (offset > 0) {
		offset--;
		right(TRUE);
	}

/*
 |	reset flags and strings to what they were before formatting
 */

	if (d_word != NULL)
		free(d_word);
	d_word = temp_dword;
	case_sen = temp_case;
	free(srch_str);
	srch_str = tmp_srchstr;
	memcpy(d_char, temp_d_char, sizeof(d_char));
	auto_format = tmp_af;

	midscreen(scr_vert, point);
	werase(com_win);
	wrefresh(com_win);
}

static char *init_name[3] = {
	"/usr/share/misc/init.ee",
	NULL,
	".init.ee"
};

/* check for init file and read it if it exists	*/
static void
ee_init(void)
{
	FILE *init_file;
	const char *home_dir;
	char *home;
	char *line = NULL;
	char *p;
	size_t linecap = 0;
	size_t home_size;
	ssize_t linelen;
	int counter;
	int temp_int;

	home_dir = getenv("HOME");
	if (home_dir == NULL)
		home_dir = "/tmp";
	home_size = strlen(home_dir) + sizeof("/.init.ee");
	home = xmalloc(home_size);
	strlcpy(home, home_dir, home_size);
	strlcat(home, "/.init.ee", home_size);
	init_name[1] = home;

	for (counter = 0; counter < 3; counter++) {
		if (access(init_name[counter], R_OK) != 0)
			continue;
		init_file = fopen(init_name[counter], "r");
		if (init_file == NULL)
			continue;
		while ((linelen = getline(&line, &linecap, init_file)) != -1) {
			if ((linelen > 0) && (line[linelen - 1] == '\n'))
				line[--linelen] = '\0';
			p = line;

			if (unique_test(p, init_strings) != 1)
				continue;

			if (compare(p, CASE, FALSE))
				case_sen = TRUE;
			else if (compare(p, NOCASE, FALSE))
				case_sen = FALSE;
			else if (compare(p, EXPAND, FALSE))
				expand_tabs = TRUE;
			else if (compare(p, NOEXPAND, FALSE))
				expand_tabs = FALSE;
			else if (compare(p, INFO, FALSE))
				info_window = TRUE;
			else if (compare(p, NOINFO, FALSE))
				info_window = FALSE;
			else if (compare(p, MARGINS, FALSE))
				observ_margins = TRUE;
			else if (compare(p, NOMARGINS, FALSE))
				observ_margins = FALSE;
			else if (compare(p, AUTOFORMAT, FALSE)) {
				auto_format = TRUE;
				observ_margins = TRUE;
			} else if (compare(p, NOAUTOFORMAT, FALSE))
				auto_format = FALSE;
			else if (compare(p, Echo, FALSE)) {
				p = next_word(p);
				if (*p != '\0')
					echo_string(p);
			} else if (compare(p, PRINTCOMMAND, FALSE)) {
				p = next_word(p);
				print_command = xmalloc(strlen(p) + 1);
				strlcpy((char *)print_command, p, strlen(p) + 1);
			} else if (compare(p, RIGHTMARGIN, FALSE)) {
				p = next_word(p);
				if ((*p >= '0') && (*p <= '9')) {
					temp_int = (int)strtol(p, NULL, 10);
					if (temp_int > 0)
						right_margin = temp_int;
				}
			} else if (compare(p, HIGHLIGHT, FALSE))
				nohighlight = FALSE;
			else if (compare(p, NOHIGHLIGHT, FALSE))
				nohighlight = TRUE;
			else if (compare(p, EMACS_string, FALSE))
				emacs_keys_mode = TRUE;
			else if (compare(p, NOEMACS_string, FALSE))
				emacs_keys_mode = FALSE;
		}
		free(line);
		line = NULL;
		linecap = 0;
		fclose(init_file);
	}
	free(line);
	free(home);
}

/*
 |	Save current configuration to .init.ee file in the current directory.
 */

static void
dump_ee_conf(void)
{
	FILE *init_file;
	FILE *old_init_file = NULL;
	const char *file_name = ".init.ee";
	const char *home_dir = "~/.init.ee";
	char *resolved = NULL;
	char *old_name = NULL;
	struct stat buf;
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	int option = 0;
	int write_ok = TRUE;

	if (restrict_mode())
		return;

	option = menu_op(config_dump_menu);

	werase(com_win);
	wmove(com_win, 0, 0);

	if (option == 0) {
		waddstr(com_win, conf_not_saved_msg);
		wrefresh(com_win);
		return;
	}
	if (option == 2) {
		resolved = resolve_name((char *)home_dir);
		if (resolved != home_dir)
			file_name = resolved;
	}

	/*
	 |	If a .init.ee file exists, move it to .init.ee.old.
	 */

	if (stat(file_name, &buf) != -1) {
		size_t n = strlen(file_name) + sizeof(".old");

		old_name = xmalloc(n);
		snprintf(old_name, n, "%s.old", file_name);
		unlink(old_name);
		link(file_name, old_name);
		unlink(file_name);
		old_init_file = fopen(old_name, "r");
	}

	init_file = fopen(file_name, "w");
	if (init_file == NULL) {
		waddstr(com_win, conf_dump_err_msg);
		wrefresh(com_win);
		free(old_name);
		if ((resolved != NULL) && (resolved != home_dir))
			free(resolved);
		return;
	}

	if (old_init_file != NULL) {
		/*
		 |	Copy non-configuration info into new .init.ee file.
		 */
		while ((linelen = getline(&line, &linecap, old_init_file)) != -1) {
			if ((linelen > 0) && (line[linelen - 1] == '\n'))
				line[--linelen] = '\0';

			if ((unique_test(line, init_strings) == 1) &&
			    !compare(line, Echo, FALSE))
				continue;
			if (fprintf(init_file, "%s\n", line) < 0)
				write_ok = FALSE;
		}
		free(line);
		fclose(old_init_file);
	}

	if (fprintf(init_file, "%s\n", case_sen ? CASE : NOCASE) < 0)
		write_ok = FALSE;
	if (fprintf(init_file, "%s\n", expand_tabs ? EXPAND : NOEXPAND) < 0)
		write_ok = FALSE;
	if (fprintf(init_file, "%s\n", info_window ? INFO : NOINFO) < 0)
		write_ok = FALSE;
	if (fprintf(init_file, "%s\n", observ_margins ? MARGINS : NOMARGINS) < 0)
		write_ok = FALSE;
	if (fprintf(init_file, "%s\n", auto_format ? AUTOFORMAT : NOAUTOFORMAT) < 0)
		write_ok = FALSE;
	if (fprintf(init_file, "%s %s\n", PRINTCOMMAND, print_command) < 0)
		write_ok = FALSE;
	if (fprintf(init_file, "%s %d\n", RIGHTMARGIN, right_margin) < 0)
		write_ok = FALSE;
	if (fprintf(init_file, "%s\n", nohighlight ? NOHIGHLIGHT : HIGHLIGHT) < 0)
		write_ok = FALSE;
	if (fprintf(init_file, "%s\n",
	    emacs_keys_mode ? EMACS_string : NOEMACS_string) < 0)
		write_ok = FALSE;

	if (fflush(init_file) != 0)
		write_ok = FALSE;
	if (fclose(init_file) != 0)
		write_ok = FALSE;

	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	if (!write_ok) {
		waddstr(com_win, conf_dump_err_msg);
	} else {
		wprintw(com_win, "ee configuration saved in file %s",
		    file_name);
	}
	wrefresh(com_win);
	clear_com_win = TRUE;

	free(old_name);
	if ((resolved != NULL) && (resolved != home_dir))
		free(resolved);
}

/* echo the given string	*/
static void
echo_string(char *string)
{
	char *temp;
	int Counter;

	temp = (char *)string;
	while (*temp != '\0') {
		if (*temp == '\\') {
			temp++;
			if (*temp == 'n')
				putchar('\n');
			else if (*temp == 't')
				putchar('\t');
			else if (*temp == 'b')
				putchar('\b');
			else if (*temp == 'r')
				putchar('\r');
			else if (*temp == 'f')
				putchar('\f');
			else if ((*temp == 'e') || (*temp == 'E'))
				putchar('\033');	/* escape */
			else if (*temp == '\\')
				putchar('\\');
			else if (*temp == '\'')
				putchar('\'');
			else if ((*temp >= '0') && (*temp <= '9')) {
				Counter = 0;
				while ((*temp >= '0') && (*temp <= '9')) {
					Counter = (8 * Counter) + (*temp - '0');
					temp++;
				}
				putchar(Counter);
				temp--;
			}
			temp++;
		} else {
			putchar(*temp);
			temp++;
		}
	}

	fflush(stdout);
}

/* check spelling of words in the editor	*/
static void
spell_op(void)
{
	if (restrict_mode()) {
		return;
	}
	top();			/* go to top of file		*/
	insert_line(FALSE);	/* create two blank lines	*/
	insert_line(FALSE);
	top();
	command(shell_echo_msg);
	adv_line();
	wmove(com_win, 0, 0);
	wclrtoeol(com_win);
	wprintw(com_win, "%s", spell_in_prog_msg);
	wrefresh(com_win);
	command("<>!spell");	/* send contents of buffer to command 'spell'
				   and read the results back into the editor */
}

static void
ispell_op(void)
{
	const char *tmpdir;
	char *name;
	char *cmd;
	size_t len;
	int fd;

	if (restrict_mode())
		return;

	/*
	 * A private temporary file created with mkstemp(3): the name is
	 * unpredictable and the file is created with mode 0600, so the
	 * document contents are not exposed.  It is removed on every path
	 * out of the function.
	 */
	tmpdir = getenv("TMPDIR");
	if ((tmpdir == NULL) || (*tmpdir == '\0'))
		tmpdir = "/tmp";
	len = strlen(tmpdir) + sizeof("/ee.XXXXXXXX");
	name = xmalloc(len);
	snprintf(name, len, "%s/ee.XXXXXXXX", tmpdir);
	fd = mkstemp(name);
	if (fd < 0) {
		wmove(com_win, 0, 0);
		wclrtoeol(com_win);
		wprintw(com_win, "unable to create file \"%s\"", name);
		wrefresh(com_win);
		free(name);
		return;
	}
	close(fd);

	if (write_file(name, 0)) {
		len = strlen("ispell ") + strlen(name) + 1;
		cmd = xmalloc(len);
		snprintf(cmd, len, "ispell %s", name);
		sh_command(cmd);
		free(cmd);
		delete_text();
		tmp_file = name;
		recv_file = TRUE;
		check_fp();
	}
	unlink(name);
	if (tmp_file == name)
		tmp_file = NULL;
	free(name);
}

static int
first_word_len(struct text *test_line)
{
	int counter;
	unsigned char *pnt;

	if (test_line == NULL)
		return (0);

	pnt = test_line->line;
	if ((pnt == NULL) || (*pnt == '\0') ||
	    (*pnt == '.') || (*pnt == '>'))
		return (0);

	if ((*pnt == ' ') || (*pnt == '\t')) {
		pnt = (unsigned char *)next_word((char *)pnt);
	}

	if (*pnt == '\0')
		return (0);

	counter = 0;
	while ((*pnt != '\0') && ((*pnt != ' ') && (*pnt != '\t'))) {
		pnt++;
		counter++;
	}
	while ((*pnt != '\0') && ((*pnt == ' ') || (*pnt == '\t'))) {
		pnt++;
		counter++;
	}
	return (counter);
}

/* format the paragraph according to set margins	*/
static void
Auto_Format(void)
{
	int string_count;
	int offset;
	int temp_case;
	int word_len;
	int temp_dwl;
	int tmp_d_line_length;
	int leave_loop = FALSE;
	int status;
	int counter;
	char not_blank;
	unsigned char *line;
	unsigned char *tmp_srchstr;
	unsigned char *temp1, *temp2;
	unsigned char *temp_dword;
	unsigned char temp_d_char[5];
	unsigned char *tmp_d_line;

	memcpy(temp_d_char, d_char, sizeof(temp_d_char));

/*
 |	if observ_margins is not set, or the current line is blank,
 |	do not format the current paragraph
 */

	if ((!observ_margins) || (Blank_Line(curr_line)))
		return;

/*
 |	get current position in paragraph, so after formatting, the cursor
 |	will be in the same relative position
 */

	tmp_d_line = d_line;
	tmp_d_line_length = dlt_line->line_length;
	d_line = NULL;
	auto_format = FALSE;
	offset = position;
	if ((position != 1) &&
	    ((*point == ' ') || (*point == '\t') ||
	     (position == curr_line->line_length) || (*point == '\0')))
		prev_word();
	temp_dword = d_word;
	temp_dwl = d_wrd_len;
	d_wrd_len = 0;
	d_word = NULL;
	temp_case = case_sen;
	case_sen = TRUE;
	tmp_srchstr = srch_str;
	{
		size_t srch_len = (curr_line->line_length > position) ?
		    (size_t)(curr_line->line_length - position) : 0;

		temp2 = srch_str = xmalloc(srch_len + 1);
	}
	if ((*point == ' ') || (*point == '\t'))
		adv_word();
	offset -= position;
	counter = position;
	line = temp1 = point;
	while ((*temp1 != '\0') && (*temp1 != ' ') && (*temp1 != '\t') &&
	    (counter < curr_line->line_length)) {
		*temp2 = *temp1;
		temp2++;
		temp1++;
		counter++;
	}
	*temp2 = '\0';
	if (position != 1)
		bol();
	while (!Blank_Line(curr_line->prev_line))
		bol();
	string_count = 0;
	status = TRUE;
	while ((line != point) && (status)) {
		status = search(FALSE);
		string_count++;
	}

/*
 |	now get back to the start of the paragraph to start checking
 */

	if (position != 1)
		bol();
	while (!Blank_Line(curr_line->prev_line))
		bol();

/*
 |	Start going through lines, putting spaces at end of lines if they do
 |	not already exist.  Check line length, and move words to the next line
 |	if they cross the margin.  Then get words from the next line if they
 |	will fit in before the margin.
 */

	counter = 0;

	while (!leave_loop) {
		if (position != curr_line->line_length)
			eol();
		left(TRUE);
		if (*point != ' ') {
			right(TRUE);
			insert(' ');
		} else
			right(TRUE);

		not_blank = FALSE;

		/*
		 |	fill line if first word on next line will fit
		 |	in the line without crossing the margin
		 */

		while ((curr_line->next_line != NULL) &&
		    ((word_len = first_word_len(curr_line->next_line)) > 0) &&
		    ((scr_pos + word_len) < right_margin)) {
			adv_line();
			if ((*point == ' ') || (*point == '\t'))
				adv_word();
			del_word();
			if (position != 1)
				bol();

			/*
			 |	We know this line was not blank before, so
			 |	make sure that it doesn't have one of the
			 |	leading characters that indicate the line
			 |	should not be modified.
			 |
			 |	We also know that this character should not
			 |	be left as the first character of this line.
			 */

			if ((Blank_Line(curr_line)) &&
			    (curr_line->line[0] != '.') &&
			    (curr_line->line[0] != '>')) {
				del_line();
				not_blank = FALSE;
			} else
				not_blank = TRUE;

			/*
			 |   go to end of previous line
			 */
			left(TRUE);
			undel_word();
			eol();
			/*
			 |   make sure there's a space at the end of the line
			 */
			left(TRUE);
			if (*point != ' ') {
				right(TRUE);
				insert(' ');
			} else
				right(TRUE);
		}

		/*
		 |	make sure line does not cross right margin
		 */

		while (right_margin <= scr_pos) {
			prev_word();
			if (position != 1) {
				del_word();
				if (Blank_Line(curr_line->next_line))
					insert_line(TRUE);
				else
					adv_line();
				if ((*point == ' ') || (*point == '\t'))
					adv_word();
				undel_word();
				not_blank = TRUE;
				if (position != 1)
					bol();
				left(TRUE);
			}
		}

		if ((!Blank_Line(curr_line->next_line)) || (not_blank)) {
			adv_line();
			counter++;
		} else
			leave_loop = TRUE;
	}

/*
 |	go back to begin of paragraph, put cursor back to original position
 */

	if (position != 1)
		bol();
	while ((counter-- > 0) || (!Blank_Line(curr_line->prev_line)))
		bol();

/*
 |	find word cursor was in
 */

	status = TRUE;
	while ((status) && (string_count > 0)) {
		status = search(FALSE);
		string_count--;
	}

/*
 |	offset the cursor to where it was before from the start of the word
 */

	while (offset > 0) {
		offset--;
		right(TRUE);
	}

	if ((string_count > 0) && (offset < 0)) {
		while (offset < 0) {
			offset++;
			left(TRUE);
		}
	}

/*
 |	reset flags and strings to what they were before formatting
 */

	if (d_word != NULL)
		free(d_word);
	d_word = temp_dword;
	d_wrd_len = temp_dwl;
	case_sen = temp_case;
	free(srch_str);
	srch_str = tmp_srchstr;
	memcpy(d_char, temp_d_char, sizeof(d_char));
	auto_format = TRUE;
	dlt_line->line_length = tmp_d_line_length;
	d_line = tmp_d_line;

	formatted = TRUE;
	midscreen(scr_vert, point);
}

static void
modes_op(void)
{
	int ret_value;
	int counter;
	char *string;

	do {
		snprintf(modes_menu[1].item_string, MODES_ITEM_SIZE, "%s %s",
		    mode_strings[1], (expand_tabs ? ON : OFF));
		snprintf(modes_menu[2].item_string, MODES_ITEM_SIZE, "%s %s",
		    mode_strings[2], (case_sen ? ON : OFF));
		snprintf(modes_menu[3].item_string, MODES_ITEM_SIZE, "%s %s",
		    mode_strings[3], (observ_margins ? ON : OFF));
		snprintf(modes_menu[4].item_string, MODES_ITEM_SIZE, "%s %s",
		    mode_strings[4], (auto_format ? ON : OFF));
		snprintf(modes_menu[5].item_string, MODES_ITEM_SIZE, "%s %s",
		    mode_strings[5], (info_window ? ON : OFF));
		snprintf(modes_menu[6].item_string, MODES_ITEM_SIZE, "%s %s",
		    mode_strings[6], (emacs_keys_mode ? ON : OFF));
		snprintf(modes_menu[7].item_string, MODES_ITEM_SIZE, "%s %d",
		    mode_strings[7], right_margin);

		ret_value = menu_op(modes_menu);

		switch (ret_value) {
		case 1:
			expand_tabs = !expand_tabs;
			break;
		case 2:
			case_sen = !case_sen;
			break;
		case 3:
			observ_margins = !observ_margins;
			break;
		case 4:
			auto_format = !auto_format;
			if (auto_format)
				observ_margins = TRUE;
			break;
		case 5:
			if (info_window)
				no_info_window();
			else
				create_info_window();
			break;
		case 6:
			emacs_keys_mode = !emacs_keys_mode;
			if (info_window)
				paint_shortcut_bar();
			break;
		case 7:
			string = get_string(margin_prompt, TRUE);
			if (string != NULL) {
				counter = (int)strtol(string, NULL, 10);
				if (counter > 0)
					right_margin = counter;
				free(string);
			}
			break;
		default:
			break;
		}
	} while (ret_value != 0);
}

/*
 * Append n bytes of s to a NUL-terminated growable buffer.  Returns 0 on
 * success and -1 on allocation failure.  Length arithmetic is checked so
 * that a hostile environment cannot make the buffer size wrap.
 */
static int
append_mem(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
	char *nb;
	size_t need, ncap;

	if (n == 0)
		return (0);
	if (n > (SIZE_MAX - *len - 1))
		return (-1);
	need = *len + n + 1;
	if (need > *cap) {
		ncap = (*cap == 0) ? 64 : *cap;
		while (ncap < need) {
			if (ncap > (SIZE_MAX / 2)) {
				ncap = need;
				break;
			}
			ncap *= 2;
		}
		nb = reallocarray(*buf, ncap, 1);
		if (nb == NULL)
			return (-1);
		*buf = nb;
		*cap = ncap;
	}
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = '\0';
	return (0);
}

/*
 |	handle names of the form "~/file", "~user/file",
 |	"$HOME/foo", "~/$FOO", etc.
 |
 |	The result is dynamically allocated with no fixed size limit; the
 |	original name pointer is returned unchanged when nothing needs to
 |	be expanded.  A failed expansion returns the original name rather
 |	than a truncated one.
 */

static char *
resolve_name(char *name)
{
	char *buffer;
	char *buffer_owned = NULL;
	char *out = NULL;
	char *slash;
	char *const_end;
	size_t outlen = 0, outcap = 0;
	struct passwd *user;
	size_t homelen, restlen;

	if (name[0] == '~') {
		if (name[1] == '/') {
			user = (struct passwd *)getpwuid(getuid());
			slash = name + 1;
		} else {
			char *uname;

			slash = strchr(name, '/');
			if (slash == NULL)
				return (name);
			uname = malloc((size_t)(slash - name));
			if (uname == NULL)
				return (name);
			memcpy(uname, name + 1, (size_t)(slash - name - 1));
			uname[slash - name - 1] = '\0';
			user = (struct passwd *)getpwnam(uname);
			free(uname);
		}
		if (user == NULL)
			return (name);
		homelen = strlen(user->pw_dir);
		restlen = strlen(slash);
		if (homelen > (SIZE_MAX - restlen - 1))
			return (name);
		buffer = malloc(homelen + restlen + 1);
		if (buffer == NULL)
			return (name);
		memcpy(buffer, user->pw_dir, homelen);
		memcpy(buffer + homelen, slash, restlen + 1);
		buffer_owned = buffer;
	} else
		buffer = name;

	if ((buffer_owned == NULL) && (strchr(buffer, '$') == NULL))
		return (name);

	{
		char *p = buffer;

		while (*p != '\0') {
			char *start;
			char *vstart;
			char *vend;
			char *varname;
			char *value;
			size_t vlen;

			if (*p != '$') {
				char *q = p;

				while ((*q != '\0') && (*q != '$'))
					q++;
				if (append_mem(&out, &outlen, &outcap, p,
				    (size_t)(q - p)) < 0)
					goto fail;
				p = q;
				continue;
			}

			start = p;
			if (p[1] == '{') {
				vstart = p + 2;
				const_end = strchr(vstart, '}');
				if (const_end == NULL) {
					/* Unterminated: keep '$' literally. */
					if (append_mem(&out, &outlen, &outcap,
					    "$", 1) < 0)
						goto fail;
					p++;
					continue;
				}
				vend = const_end;
				vlen = (size_t)(vend - vstart);
				p = vend + 1;
			} else {
				vstart = p + 1;
				vend = vstart;
				while ((*vend != '\0') && (*vend != '/') &&
				    (*vend != '$'))
					vend++;
				vlen = (size_t)(vend - vstart);
				p = vend;
			}

			varname = malloc(vlen + 1);
			if (varname == NULL)
				goto fail;
			memcpy(varname, vstart, vlen);
			varname[vlen] = '\0';
			value = getenv(varname);
			free(varname);
			if ((value != NULL) && (*value != '\0')) {
				if (append_mem(&out, &outlen, &outcap, value,
				    strlen(value)) < 0)
					goto fail;
			} else {
				/* Unknown variable: keep the text as-is. */
				if (append_mem(&out, &outlen, &outcap, start,
				    (size_t)(p - start)) < 0)
					goto fail;
			}
		}
	}

	free(buffer_owned);
	return (out);

fail:
	free(buffer_owned);
	free(out);
	return (name);
}

static int
restrict_mode(void)
{
	if (!restricted)
		return (FALSE);

	wmove(com_win, 0, 0);
	wprintw(com_win, "%s", restricted_msg);
	wclrtoeol(com_win);
	wrefresh(com_win);
	clear_com_win = TRUE;
	return (TRUE);
}

/*
 |	The following routine tests the input string against the list of
 |	strings, to determine if the string is a unique match with one of the
 |	valid values.
 */

static int
unique_test(const char *string, const char *list[])
{
	int counter;
	int num_match;
	int result;

	num_match = 0;
	counter = 0;
	while (list[counter] != NULL) {
		result = compare(string, list[counter], FALSE);
		if (result)
			num_match++;
		counter++;
	}
	return (num_match);
}

/*
 * Establish the invariant "the active LC_CTYPE is UTF-8".
 *
 * ee has a single text semantics: UTF-8.  There is deliberately no
 * byte-oriented fallback, because mbrtowc(3), wcrtomb(3), wcwidth(3)
 * and wget_wch(3) would then have a meaning incompatible with the
 * buffer's UTF-8 invariant.  The editor therefore refuses to start
 * unless a UTF-8 LC_CTYPE can be selected.
 *
 * Policy:
 *
 *   - If the environment names a locale (LC_ALL, LC_CTYPE or LANG is
 *     set), that choice is honoured: it must resolve to a UTF-8 codeset
 *     or ee refuses to start.  ee does not silently override an
 *     explicit non-UTF-8 choice.
 *   - If no locale is configured at all, a small set of well-known
 *     UTF-8 locales is tried, so that ee works out of the box on a
 *     system whose default is the "C" locale.
 *
 * Only LC_CTYPE is touched: LC_MESSAGES is never consulted and cannot
 * change the compiled-in U.S. English interface.
 *
 * This runs before any editor state or curses initialization, so a
 * failure leaves the terminal untouched.
 */
static int
locale_is_configured(void)
{
	const char *v;

	v = getenv("LC_ALL");
	if ((v != NULL) && (*v != '\0'))
		return (1);
	v = getenv("LC_CTYPE");
	if ((v != NULL) && (*v != '\0'))
		return (1);
	v = getenv("LANG");
	if ((v != NULL) && (*v != '\0'))
		return (1);
	return (0);
}

static void
select_utf8_locale(void)
{
	static const char *const fallback[] = {
		"C.UTF-8", "en_US.UTF-8"
	};
	size_t i;

	if ((setlocale(LC_CTYPE, "") != NULL) &&
	    (strcmp(nl_langinfo(CODESET), "UTF-8") == 0))
		return;
	if (locale_is_configured()) {
		fprintf(stderr, "ee: the configured locale is not UTF-8; "
		    "set LC_CTYPE to a UTF-8 locale\n");
		exit(1);
	}
	for (i = 0; i < sizeof(fallback) / sizeof(fallback[0]); i++) {
		if ((setlocale(LC_CTYPE, fallback[i]) != NULL) &&
		    (strcmp(nl_langinfo(CODESET), "UTF-8") == 0))
			return;
	}
	fprintf(stderr, "ee: no UTF-8 locale available; "
	    "set LC_CTYPE to a UTF-8 locale\n");
	exit(1);
}

/*
 *	All messages are hard-coded U.S. English: ee is not localized,
 *	and no message catalogs are read or installed.  The message text
 *	itself is defined as static data above; this routine only wires
 *	the menu labels and the command/init name tables together.
 */

static void
strings_init(void)
{
	int counter;

	mode_strings[1] = "tabs to spaces       ";
	mode_strings[2] = "case sensitive search";
	mode_strings[3] = "margins observed     ";
	mode_strings[4] = "auto-paragraph format";
	mode_strings[5] = "info window          ";
	mode_strings[6] = "emacs key bindings   ";
	mode_strings[7] = "right margin         ";

	/*
	 |	allocate space for strings here for settings menu
	 */

	for (counter = 1; counter < NUM_MODES_ITEMS; counter++)
		modes_menu[counter].item_string = xmalloc(MODES_ITEM_SIZE);
}
