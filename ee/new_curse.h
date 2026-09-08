#ifndef NEW_CURSE_H
#define NEW_CURSE_H

#include <stdio.h>
#include <termios.h>
#include <wchar.h>

#define OK	0
#define ERR	(-1)

/* function keys are reported as KEY_CODE_YES + a code */
#define KEY_CODE_YES	0400

enum nc_key {
	KEY_BREAK =	0401,
	KEY_DOWN =	0402,
	KEY_UP =	0403,
	KEY_LEFT =	0404,
	KEY_RIGHT =	0405,
	KEY_HOME =	0406,
	KEY_BACKSPACE =	0407,
	KEY_F0 =	0410,
	KEY_DL =	0510,
	KEY_IL =	0511,
	KEY_DC =	0512,
	KEY_IC =	0513,
	KEY_EIC =	0514,
	KEY_CLEAR =	0515,
	KEY_EOS =	0516,
	KEY_EOL =	0517,
	KEY_SF =	0520,
	KEY_SR =	0521,
	KEY_NPAGE =	0522,
	KEY_PPAGE =	0523,
	KEY_STAB =	0524,
	KEY_CTAB =	0525,
	KEY_CATAB =	0526,
	KEY_ENTER =	0527,
	KEY_SRESET =	0530,
	KEY_RESET =	0531,
	KEY_PRINT =	0532,
	KEY_LL =	0533,
	KEY_A1 =	0534,
	KEY_A3 =	0535,
	KEY_B2 =	0536,
	KEY_C1 =	0537,
	KEY_C3 =	0540,
	KEY_BTAB =	0541,
	KEY_BEG =	0542,
	KEY_CANCEL =	0543,
	KEY_CLOSE =	0544,
	KEY_COMMAND =	0545,
	KEY_COPY =	0546,
	KEY_CREATE =	0547,
	KEY_END =	0550,
	KEY_EXIT =	0551,
	KEY_FIND =	0552,
	KEY_HELP =	0553,
	KEY_MARK =	0554,
	KEY_MESSAGE =	0555,
	KEY_MOVE =	0556,
	KEY_NEXT =	0557,
	KEY_OPEN =	0560,
	KEY_OPTIONS =	0561,
	KEY_PREVIOUS =	0562,
	KEY_REDO =	0563,
	KEY_REFERENCE =	0564,
	KEY_REFRESH =	0565,
	KEY_REPLACE =	0566,
	KEY_RESTART =	0567,
	KEY_RESUME =	0570,
	KEY_SAVE =	0571,
	KEY_SBEG =	0572,
	KEY_SCANCEL =	0573,
	KEY_SCOMMAND =	0574,
	KEY_SCOPY =	0575,
	KEY_SCREATE =	0576,
	KEY_SDC =	0577,
	KEY_SDL =	0600,
	KEY_SELECT =	0601,
	KEY_SEND =	0602,
	KEY_SEOL =	0603,
	KEY_SEXIT =	0604,
	KEY_SFIND =	0605,
	KEY_SHELP =	0606,
	KEY_SHOME =	0607,
	KEY_SIC =	0610,
	KEY_SLEFT =	0611,
	KEY_SMESSAGE =	0612,
	KEY_SMOVE =	0613,
	KEY_SNEXT =	0614,
	KEY_SOPTIONS =	0615,
	KEY_SPREVIOUS =	0616,
	KEY_SPRINT =	0617,
	KEY_SREDO =	0620,
	KEY_SREPLACE =	0621,
	KEY_SRIGHT =	0622,
	KEY_SRSUME =	0623,
	KEY_SSAVE =	0624,
	KEY_SSUSPEND =	0625,
	KEY_SUNDO =	0626,
	KEY_SUSPEND =	0627,
	KEY_UNDO =	0630
};

#define KEY_F(n)	((enum nc_key)(KEY_F0 + (n)))

#define TRUE	1
#define FALSE	0

#define A_STANDOUT	0001	/* standout mode */
#define A_NC_BIG5	0x0100	/* handle Chinese Big5 characters */
#define SCROLL		1	/* text has been scrolled */
#define CLEAR		2	/* window has been cleared */
#define CHANGE		3	/* window has been changed */
#define UP		1	/* direction of scroll */
#define DOWN		2

struct nc_line {
	struct nc_line	*next_screen;
	struct nc_line	*prev_screen;
	char		*row;
	char		*attributes;
	int		 last_char;
	int		 changed;
	int		 scroll;
	int		 number;
};

typedef struct WIND {
	int		 SR;	/* starting row */
	int		 SC;	/* starting column */
	int		 LC;	/* last column */
	int		 LX;	/* last cursor column position */
	int		 LY;	/* last cursor row position */
	int		 Attrib;	/* attributes active in window */
	int		 Num_lines;	/* number of lines */
	int		 Num_cols;	/* number of columns */
	int		 scroll_up;	/* number of lines moved */
	int		 scroll_down;
	int		 SCROLL_CLEAR;	/* window scrolled or cleared */
	struct nc_line	*first_line;
	struct nc_line	**line_array;
} WINDOW;

extern WINDOW	*curscr;
extern WINDOW	*stdscr;
extern int	 LINES, COLS;

void	copy_window(WINDOW *, WINDOW *);
void	reinitscr(int);
void	initscr(void);
int	Get_int(void);
int	INFO_PARSE(void);
int	AtoI(void);
void	Key_Get(void);
void	keys_vt100(void);
struct nc_line *Screenalloc(int);
WINDOW	*newwin(int, int, int, int);
int	Operation(int *, int);
void	Info_Out(char *, int *, int);
void	wmove(WINDOW *, int, int);
void	clear_line(struct nc_line *, int, int);
void	werase(WINDOW *);
void	wclrtoeol(WINDOW *);
void	wrefresh(WINDOW *);
void	touchwin(WINDOW *);
void	wnoutrefresh(WINDOW *);
void	flushinp(void);
void	ungetch(int);
int	wgetch(WINDOW *);
int	wget_wch(WINDOW *, wint_t *);
void	overwrite(WINDOW *, WINDOW *);
int	Get_key(int);
void	waddch(WINDOW *, int);
void	winsertln(WINDOW *);
void	wdeleteln(WINDOW *);
void	wclrtobot(WINDOW *);
void	wstandout(WINDOW *);
void	wstandend(WINDOW *);
void	waddstr(WINDOW *, const char *);
void	clearok(WINDOW *, int);
void	echo(void);
void	noecho(void);
void	raw(void);
void	noraw(void);
void	nl(void);
void	nonl(void);
void	saveterm(void);
void	fixterm(void);
void	resetterm(void);
void	nodelay(WINDOW *, int);
void	idlok(WINDOW *, int);
void	keypad(WINDOW *, int);
void	savetty(void);
void	resetty(void);
void	endwin(void);
void	delwin(WINDOW *);
void	wprintw(WINDOW *, const char *, ...);
void	iout(WINDOW *, int);
int	Comp_line(struct nc_line *, struct nc_line *);
struct nc_line *Insert_line(int, int, WINDOW *);
struct nc_line *Delete_line(int, int, WINDOW *);
void	CLEAR_TO_EOL(WINDOW *, int, int);
int	check_delete(WINDOW *, int, int, struct nc_line *,
	    struct nc_line *);
int	check_insert(WINDOW *, int, int, struct nc_line *,
	    struct nc_line *);
void	doupdate(void);
void	Position(WINDOW *, int, int);
void	Char_del(char *, char *, int, int);
void	Char_ins(char *, char *, int, int, int, int);
void	attribute_on(void);
void	attribute_off(void);
void	Char_out(int, int, char *, char *, int);
void	nc_setattrib(int);
void	nc_clearattrib(int);

#endif /* NEW_CURSE_H */
