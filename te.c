/*
 * Text Editor
 * Copyright (c) 2021 Lone Dynamics Corporation. All rights reserved.
 *
 * Redistribution and use in source, binary or physical forms, with or without
 * modification, is permitted provided that the following condition is met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * THIS HARDWARE, SOFTWARE, DATA AND/OR DOCUMENTATION ("THE ASSETS") IS
 * PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
 * OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE ASSETS OR THE USE OR OTHER
 * DEALINGS IN THE ASSETS. USE AT YOUR OWN RISK.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CURSES
#include <ncurses.h>
#endif

#define VT100_CURSOR_UP       "\e[A"
#define VT100_CURSOR_DOWN     "\e[B"
#define VT100_CURSOR_RIGHT    "\e[C"
#define VT100_CURSOR_LEFT     "\e[D"
#define VT100_CURSOR_HOME     "\e[;H"
#define VT100_CURSOR_MOVE_TO  "\e[%i;%iH"
#define VT100_CURSOR_CRLF     "\e[E"
#define VT100_ERASE_SCREEN    "\e[J"
#define VT100_ERASE_LINE      "\e[K"
#define CH_ESC 0x1b
#define CH_LF  0x0a
#define CH_CR  0x0d
#define CH_FF  0x0c
#define CH_BS  0x08
#define CH_DEL 0x7f

#ifdef EMBEDDED
#include "fs.h"
#endif

#ifndef TE_DEFAULT_ROWS
#define TE_DEFAULT_ROWS 24  /* used as-is on embedded targets; on Linux this
                              * is just the fallback if the terminal size
                              * can't be determined */
#endif
#ifndef TE_DEFAULT_COLS
#define TE_DEFAULT_COLS 80
#endif
#define CONTENT_ROWS (te_rows - 1)  /* last row is reserved for the status bar */

/*
 * -DTE_HOST_IO -- optional I/O indirection, off by default (every
 * existing build -- -DCURSES, plain -DEMBEDDED -- is completely
 * unaffected unless this is also defined).
 *
 * Why this exists: te.c's default embedded contract (README.md,
 * "Embedded targets") talks to a single global getch()/stdout, which
 * is the right assumption for a target that's ENTIRELY this editor.
 * It's the wrong assumption for a caller that's multiplexing more
 * than one input/output stream through one process -- e.g. Zeitlos's
 * `repl` app (sw/apps/repl/repl.c), one process serving several
 * `term` connections at once over a shared port protocol
 * (sw/common/zport.h). Under TE_HOST_IO, every place this file would
 * otherwise call getch()/printf()/fflush(stdout) goes through
 * te_host_getch()/te_host_write()/te_host_flush() instead (declared
 * in te_host_io.h, NOT provided by this file -- the host app
 * implements them; see Zeitlos's sw/apps/repl/te_bridge.c for that
 * side). Every other call site in this file (STATE_ESC0/ESC1/etc's
 * own logic, the document/line-list functions, te_load()/te_save())
 * is untouched -- this only changes where bytes actually come from
 * and go to.
 */
#ifdef TE_HOST_IO
#include "te_host_io.h"
#include <stdarg.h>
#define TE_GETCH()      te_host_getch()
#define TE_FLUSH()      te_host_flush()
// vsnprintf's into a small stack buffer, then hands the formatted
// bytes to te_host_write() -- te.c's own printf() call sites here are
// all short (a VT100 escape sequence, a status line, one line's worth
// of document text up to te_cols) so this stays comfortably inside
// TE_PRINTF_BUFSIZE; truncates defensively (vsnprintf's own contract)
// rather than overrunning if that's ever not true.
#define TE_PRINTF_BUFSIZE 512
static void te_printf(const char *fmt, ...) {
	char buf[TE_PRINTF_BUFSIZE];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0) return;
	if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
	te_host_write(buf, n);
}
#define TE_PRINTF(...) te_printf(__VA_ARGS__)
#else
#define TE_GETCH()      getch()
#define TE_FLUSH()      fflush(stdout)
#define TE_PRINTF(...)  printf(__VA_ARGS__)
#endif

#define MODE_MOVE 0
#define MODE_EDIT 1

#define STATE_NONE 0
#define STATE_ESC0 1
#define STATE_ESC1 2
#define STATE_CMD 3
#define STATE_ESC2 4     /* accumulating a numeric param, e.g. ESC [ 5 ~ (PgUp) */
#define STATE_CMD_NUM 5  /* accumulating a line number, e.g. ESC : 1 <Enter> */

void te_init(void);
void te_redraw(void);
void te_redraw_line(int line);	// see its own comment, below
void te_status(char *notice);
void te_status_bar(int enabled);	// see its own comment, below
int te_yield(void);
int te_edit_start(char *filename);	// see its own comment, below

void te_insert(int l, int pos, char c);
void te_insert_line(int line);
void te_delete(int line, int pos);
int te_line_len(int line);
void te_join_line(int line);
void te_split_line(int line, int pos);
int te_load(void);
int te_save(void);

typedef struct te_lines_t {
	struct te_line_t *first;
	struct te_line_t *last;
} te_lines_t;

typedef struct te_line_t {
	char *text;
	struct te_line_t *prev;
	struct te_line_t *next;
} te_line_t;

static int mode = MODE_MOVE;
static int state = STATE_NONE;
static int esc_num = 0;  /* numeric parameter accumulated in STATE_ESC2 / STATE_CMD_NUM */

// whether te_status()'s line count/input-state/cursor-position
// counters (l%i s%i x%i y%i) are shown -- on by default, so every
// existing caller (desktop, or an embedded target that never calls
// this) sees exactly the same status line as before. A caller with
// its own reason to want the shorter line (te_status_bar(0)) --
// e.g. Zeitlos's `repl` app, see its own te_bridge.c -- can turn it
// off; te_status() still always shows the filename and any notice
// ("SAVED"/"FAILED") either way.
static int te_status_bar_coords = 1;

void te_status_bar(int enabled) {
	te_status_bar_coords = enabled;
}

int te_curs_x, te_curs_y;
static int te_goal_x;  /* remembered column for vim-style sticky vertical movement */
static int scroll_top;
static int hscroll;    /* horizontal scroll offset, keeps the cursor visible on long lines */
static int f_lines;
int te_rows, te_cols;  /* screen size: detected on Linux, hardcoded on embedded */

static char *te_filename;
static te_lines_t *lines;

// starts editing `filename` but does NOT run any input loop itself --
// meant for a caller that drives te_yield() on its own, one byte at a
// time, from its own event loop (see this file's own TE_HOST_IO
// comment above, and te's README.md's "cooperative main loop" note
// on getch()'s contract in that mode). te_edit() below is just this
// plus the blocking `while(te_yield());` loop, for the ordinary
// single-purpose-target case where that's exactly what's wanted.
//
// returns true if the session is now live and te_yield() should be
// called for every subsequent input byte -- false if te_load() failed
// (matches te_load()'s own return convention), in which case the
// caller should NOT call te_yield() at all.
int te_edit_start(char *filename) {
	te_filename = filename;
	te_init();
	if (!te_load()) return 0;
	te_redraw();
	te_status("");
	return 1;
}

void te_edit(char *filename) {
	if (!te_edit_start(filename)) {
#ifdef EMBEDDED
		// no OS to exit(0) to on bare-metal firmware -- that would
		// take the whole device down, not just this editor session.
		// Return to the caller (the CLI) instead, same as any other
		// graceful abort elsewhere in this firmware.
		TE_PRINTF("unable to load file %s\r\n", filename);
#else
		printf("unable to load file %s\n", filename);
		exit(0);
#endif
		return;
	}
	while(te_yield());
}

void te_init(void) {

	te_line_t *line;

	f_lines = 1;

	lines = calloc(1, sizeof(te_lines_t));
	line = calloc(1, sizeof(te_line_t));

	line->text = calloc(1, 1);

	line->prev = NULL;
	line->next = NULL;

	lines->first = line;
	lines->last = line;

#ifdef CURSES
	initscr();
	noecho();
	refresh();

	/* on Linux, get the real terminal size from ncurses instead of
	 * hardcoding it -- LINES/COLS are populated by initscr() */
	te_rows = LINES;
	te_cols = COLS;
	if (te_rows <= 0) te_rows = TE_DEFAULT_ROWS;
	if (te_cols <= 0) te_cols = TE_DEFAULT_COLS;
#else
	/* embedded targets have no way to query terminal size, so it's
	 * fixed at compile time */
	te_rows = TE_DEFAULT_ROWS;
	te_cols = TE_DEFAULT_COLS;
#endif

	te_curs_x = 0;
	te_curs_y = 0;
	te_goal_x = 0;
	scroll_top = 0;
	hscroll = 0;

}

void te_status(char *notice) {
	TE_PRINTF(VT100_CURSOR_MOVE_TO, te_rows, 0);
	TE_PRINTF(VT100_ERASE_LINE);
	if (te_status_bar_coords) {
		TE_PRINTF("te %s l%i s%i x%i y%i %s", te_filename, f_lines, state, te_curs_x, te_curs_y, notice);
	} else {
		TE_PRINTF("te %s %s", te_filename, notice);
	}
	TE_PRINTF(VT100_CURSOR_MOVE_TO, (te_curs_y - scroll_top) + 1, (te_curs_x - hscroll) + 1);
	TE_FLUSH();
}

int te_yield(void) {

	int c = TE_GETCH();

#ifdef CURSES
	/* the terminal may have just been resized -- ncurses catches
	 * SIGWINCH and updates LINES/COLS internally, and getch() reports
	 * it by returning KEY_RESIZE (rather than a real keystroke) once
	 * unblocked. We cache our own copy of the size, so re-sync it here
	 * and repaint if it changed. This also covers the case where a
	 * real keystroke arrives right after a resize that was already
	 * applied silently, without an explicit KEY_RESIZE. */
	{
		int new_rows = (LINES > 0) ? LINES : TE_DEFAULT_ROWS;
		int new_cols = (COLS > 0) ? COLS : TE_DEFAULT_COLS;
		if (new_rows != te_rows || new_cols != te_cols) {
			te_rows = new_rows;
			te_cols = new_cols;
			/* ncurses queues its own resize-triggered screen sync in
			 * its own output buffer, separate from our raw stdio
			 * writes -- flush it now, before our redraw, or it can
			 * land afterward and blank the screen out again */
			refresh();
			te_redraw();
			te_status("");
		}
	}

	/* KEY_RESIZE is not a real keystroke -- consume it here so it can
	 * never fall through and get inserted as text */
	if (c == KEY_RESIZE) return 1;
#endif

	if (c == EOF || c == 0) return 1;

	switch(c) {

		case(CH_ESC):
			state = STATE_ESC0;
			break;

		case(CH_FF):
			te_redraw();
			break;

		case('['):
			if (state == STATE_ESC0)
				state = STATE_ESC1;
			break;

		case(':'):
			if (state == STATE_ESC0)
				state = STATE_CMD;
			break;

		case('^'):
			if (state == STATE_ESC0) {
				/* vi-style: jump to the start of the current line */
				te_curs_x = 0;
				te_goal_x = te_curs_x;
				state = STATE_NONE;
			}
			break;

		case('$'):
			if (state == STATE_ESC0) {
				/* vi-style: jump to the end of the current line */
				int linelen = te_line_len(te_curs_y);
				te_curs_x = (linelen >= 0) ? linelen : 0;
				te_goal_x = te_curs_x;
				state = STATE_NONE;
			}
			break;

		default:

			if (state == STATE_ESC1 && c >= '0' && c <= '9') {
				/* start of a numeric escape sequence, e.g. ESC [ 5 ~ (PgUp) */
				esc_num = c - '0';
				state = STATE_ESC2;
			}

			else if (state == STATE_ESC1) {

				if (c == 'A') {
					/* up: move vertically, restoring the remembered goal column */
					if (te_curs_y > 0) {
						te_curs_y--;
						int linelen = te_line_len(te_curs_y);
						te_curs_x = (te_goal_x < linelen) ? te_goal_x : linelen;
					}
				}

				if (c == 'B') {
					/* down: same, goal column is not disturbed */
					if (te_curs_y < f_lines - 1) {
						te_curs_y++;
						int linelen = te_line_len(te_curs_y);
						te_curs_x = (te_goal_x < linelen) ? te_goal_x : linelen;
					}
				}

				if (c == 'C') {
					/* right: wrap to the start of the next line at end-of-line */
					int linelen = te_line_len(te_curs_y);
					if (te_curs_x < linelen) {
						te_curs_x++;
					} else if (te_curs_y < f_lines - 1) {
						te_curs_y++;
						te_curs_x = 0;
					}
					te_goal_x = te_curs_x;
				}

				if (c == 'D') {
					/* left: wrap to the end of the previous line at start-of-line */
					if (te_curs_x > 0) {
						te_curs_x--;
					} else if (te_curs_y > 0) {
						te_curs_y--;
						te_curs_x = te_line_len(te_curs_y);
					}
					te_goal_x = te_curs_x;
				}

				if (c == 'H') {
					/* Home (xterm-style: ESC [ H): jump to start of line */
					te_curs_x = 0;
					te_goal_x = te_curs_x;
				}

				if (c == 'F') {
					/* End (xterm-style: ESC [ F): jump to end of line */
					int linelen = te_line_len(te_curs_y);
					te_curs_x = (linelen >= 0) ? linelen : 0;
					te_goal_x = te_curs_x;
				}

				state = STATE_NONE;
			}

			else if (state == STATE_ESC2) {

				if (c >= '0' && c <= '9') {
					esc_num = esc_num * 10 + (c - '0');
				} else if (c == '~') {
					if (esc_num == 1) {
						/* Home (VT220-style: ESC [ 1 ~): jump to start of line */
						te_curs_x = 0;
						te_goal_x = te_curs_x;
					} else if (esc_num == 4) {
						/* End (VT220-style: ESC [ 4 ~): jump to end of line */
						int linelen = te_line_len(te_curs_y);
						te_curs_x = (linelen >= 0) ? linelen : 0;
						te_goal_x = te_curs_x;
					} else if (esc_num == 5) {
						/* Page Up: scroll a full page up, not just enough to
						 * keep the cursor visible */
						scroll_top -= CONTENT_ROWS;
						if (scroll_top < 0) scroll_top = 0;
						te_curs_y -= CONTENT_ROWS;
						if (te_curs_y < 0) te_curs_y = 0;
						int linelen = te_line_len(te_curs_y);
						te_curs_x = (te_goal_x < linelen) ? te_goal_x : linelen;
						te_redraw();
					} else if (esc_num == 6) {
						/* Page Down: scroll a full page down, not just enough
						 * to keep the cursor visible */
						int max_scroll = f_lines - CONTENT_ROWS;
						if (max_scroll < 0) max_scroll = 0;
						scroll_top += CONTENT_ROWS;
						if (scroll_top > max_scroll) scroll_top = max_scroll;
						te_curs_y += CONTENT_ROWS;
						if (te_curs_y > f_lines - 1) te_curs_y = f_lines - 1;
						int linelen = te_line_len(te_curs_y);
						te_curs_x = (te_goal_x < linelen) ? te_goal_x : linelen;
						te_redraw();
					}
					state = STATE_NONE;
				} else {
					/* unrecognized escape sequence, bail out */
					state = STATE_NONE;
				}

			}

			else if (state == STATE_CMD) {
				if (c >= '0' && c <= '9') {
					/* start of a "jump to line" command: ESC : 1 <Enter> */
					esc_num = c - '0';
					state = STATE_CMD_NUM;
				} else {
					TE_PRINTF("%c", c);
					if (c == 'q') { return(0); }
					if (c == 'w') {
						if (te_save()) te_status("SAVED"); else te_status("FAILED");
					}
					state = STATE_NONE;
					return(1);
				}
			}

			else if (state == STATE_CMD_NUM) {
				if (c >= '0' && c <= '9') {
					esc_num = esc_num * 10 + (c - '0');
				} else if (c == CH_LF || c == CH_CR) {
					/* jump to the given line, vi-style (1-based, clamped to file) */
					int target = esc_num - 1;
					if (target < 0) target = 0;
					if (target > f_lines - 1) target = f_lines - 1;
					te_curs_y = target;
					te_curs_x = 0;
					te_goal_x = 0;
					state = STATE_NONE;
				} else {
					/* not a digit or Enter -- abandon the command */
					state = STATE_NONE;
				}
			}

			else if (state == STATE_NONE) {
				if (c == CH_BS || c == CH_DEL) {
					if (te_curs_x > 0) {
						te_curs_x--;
						te_delete(te_curs_y, te_curs_x);
						te_goal_x = te_curs_x;
						// same line, same line count -- see
						// te_redraw_line()'s own comment on why this
						// (much cheaper) redraw is correct here.
						te_redraw_line(te_curs_y);
					} else if (te_curs_y > 0) {
						/* at start of line: merge this line into the previous one */
						int prevlen = te_line_len(te_curs_y - 1);
						te_join_line(te_curs_y);
						f_lines--;
						te_curs_y--;
						te_curs_x = prevlen;
						te_goal_x = te_curs_x;
						// line count changed -- every row below this
						// one just shifted up by one, so a partial
						// redraw isn't enough here.
						te_redraw();
					} else {
						te_goal_x = te_curs_x;
					}
				} else if (c == CH_LF || c == CH_CR) {
					/* split the current line at the cursor position */
					te_split_line(te_curs_y, te_curs_x);
					f_lines++;
					te_curs_x = 0;
					te_curs_y++;
					te_goal_x = te_curs_x;
					// line count changed -- same reasoning as the
					// join-line case above.
					te_redraw();
				} else {
					te_insert(te_curs_y, te_curs_x, c);
					// same line, same line count -- see
					// te_redraw_line()'s own comment.
					te_redraw_line(te_curs_y);
					te_curs_x++;
					te_goal_x = te_curs_x;
				}
			}

			break;

	}

	if (te_curs_y < 0) te_curs_y = 0;
	if (te_curs_y > f_lines - 1) te_curs_y = f_lines - 1;

	if (te_curs_x < 0) te_curs_x = 0;
	{
		int linelen = te_line_len(te_curs_y);
		if (linelen >= 0 && te_curs_x > linelen) te_curs_x = linelen;
	}

	/* scroll the view to keep the cursor visible, vertically and horizontally */
	{
		int old_scroll = scroll_top;
		if (te_curs_y < scroll_top) scroll_top = te_curs_y;
		if (te_curs_y > scroll_top + CONTENT_ROWS - 1) scroll_top = te_curs_y - CONTENT_ROWS + 1;
		if (scroll_top < 0) scroll_top = 0;

		int old_hscroll = hscroll;
		if (te_curs_x < hscroll) hscroll = te_curs_x;
		if (te_curs_x > hscroll + te_cols - 1) hscroll = te_curs_x - te_cols + 1;
		if (hscroll < 0) hscroll = 0;

		if (scroll_top != old_scroll || hscroll != old_hscroll) te_redraw();
	}

	te_status("");

	return 1;

}

void te_redraw() {

	int l = 0;

	te_line_t *ptr = lines->first;

	TE_PRINTF(VT100_CURSOR_HOME);
	TE_PRINTF(VT100_ERASE_SCREEN);

	while (ptr) {

		if (l >= scroll_top && l < scroll_top + CONTENT_ROWS) {
			TE_PRINTF(VT100_CURSOR_MOVE_TO, (l - scroll_top) + 1, 1);
			int len = strlen(ptr->text);
			int start = (hscroll < len) ? hscroll : len;
			int outlen = len - start;
			if (outlen > te_cols) outlen = te_cols;
			TE_PRINTF("%.*s\n", outlen, ptr->text + start);
		}

		ptr = ptr->next;
		l++;

	}

}

// redraws just ONE line's content, in place, at whatever screen row
// it currently maps to under the CURRENT scroll_top/hscroll -- much
// cheaper than te_redraw()'s own full erase-and-redraw-every-visible-
// row, and the difference matters: te_redraw() is otherwise called on
// EVERY plain keystroke (te_yield()'s STATE_NONE branch, below), so
// typing into a 24-25 row screen sent a full screen's worth of VT100
// output (every visible line, ~2KB+) per character. Real-world
// finding (Zeitlos's `repl` app, sw/apps/repl/te_bridge.c, where each
// of those bytes also costs a heap allocation on the sending
// process's own tight budget -- see docs/editor.md there): this was
// the actual, dominant cost behind sluggish typing, not the status
// line.
//
// ONLY correct to call in place of te_redraw() when the edit that
// just happened didn't change which lines are visible (no scrolling)
// and didn't change how many lines exist (no line inserted/removed
// above or at this row) -- te_yield()'s STATE_NONE branch below is
// the only caller, and only for the two edits that actually meet
// that bar (a plain character insert, and a same-line backspace); the
// scroll-adjustment block later in te_yield() still falls back to a
// full te_redraw() itself if the edit turns out to have moved the
// cursor out of view after all (a line that just got long enough to
// need horizontal scroll, for example) -- this function doesn't need
// to (and doesn't) guard against that case itself.
void te_redraw_line(int line) {

	if (line < scroll_top || line >= scroll_top + CONTENT_ROWS) return;

	te_line_t *ptr = lines->first;
	int l = 0;
	while (ptr && l < line) { ptr = ptr->next; l++; }
	if (!ptr) return;

	TE_PRINTF(VT100_CURSOR_MOVE_TO, (line - scroll_top) + 1, 1);
	TE_PRINTF(VT100_ERASE_LINE);
	int len = strlen(ptr->text);
	int start = (hscroll < len) ? hscroll : len;
	int outlen = len - start;
	if (outlen > te_cols) outlen = te_cols;
	TE_PRINTF("%.*s", outlen, ptr->text + start);

}

void te_insert(int line, int pos, char c) {

	int l = 0;

	te_line_t *ptr = lines->first;

	while (ptr) {

		if (l == line) {
			int linelen = strlen(ptr->text);
			char *newtext = calloc(1, linelen + 2);
			if (newtext == NULL) return;
			memcpy(newtext, ptr->text, pos);
			newtext[pos] = c;
			memcpy(newtext + pos + 1, ptr->text + pos, linelen - pos);
			free(ptr->text);
			ptr->text = newtext;
			break;
		}

		ptr = ptr->next;
		l++;

	};

}

void te_insert_line(int line) {

	int l = 0;

	te_line_t *ptr = lines->first;

	while (ptr) {

		if (l == line) {

			te_line_t *newline = calloc(1, sizeof(te_line_t));
			char *newtext = calloc(1, 1);

			newline->text = newtext;
			newline->prev = ptr;
			newline->next = ptr->next;

			if (ptr->next) ptr->next->prev = newline;
			ptr->next = newline;
			if (lines->last == ptr) lines->last = newline;

			break;
		}

		ptr = ptr->next;
		l++;

	};

}

// return the length of the given line, or -1 if the line doesn't exist
int te_line_len(int line) {

	int l = 0;
	te_line_t *ptr = lines->first;

	while (ptr) {
		if (l == line) return strlen(ptr->text);
		ptr = ptr->next;
		l++;
	}

	return -1;

}

// merge the given line into the previous line, removing it from the list
void te_join_line(int line) {

	if (line <= 0) return;

	int l = 0;
	te_line_t *ptr = lines->first;

	while (ptr) {
		if (l == line) {

			te_line_t *prev = ptr->prev;
			if (prev == NULL) return;

			int prevlen = strlen(prev->text);
			int curlen = strlen(ptr->text);

			char *newtext = calloc(1, prevlen + curlen + 1);
			if (newtext == NULL) return;
			memcpy(newtext, prev->text, prevlen);
			memcpy(newtext + prevlen, ptr->text, curlen);

			free(prev->text);
			prev->text = newtext;

			prev->next = ptr->next;
			if (ptr->next) ptr->next->prev = prev;
			if (lines->last == ptr) lines->last = prev;

			free(ptr->text);
			free(ptr);

			break;
		}
		ptr = ptr->next;
		l++;
	}

}

// split the given line at pos: text before pos stays, text at/after pos
// moves into a newly-inserted line right after it
void te_split_line(int line, int pos) {

	int l = 0;
	te_line_t *ptr = lines->first;

	while (ptr) {
		if (l == line) {

			int linelen = strlen(ptr->text);
			if (pos < 0) pos = 0;
			if (pos > linelen) pos = linelen;

			int taillen = linelen - pos;

			char *head = calloc(1, pos + 1);
			char *tail = calloc(1, taillen + 1);
			if (head == NULL || tail == NULL) { free(head); free(tail); return; }

			memcpy(head, ptr->text, pos);
			memcpy(tail, ptr->text + pos, taillen);

			free(ptr->text);
			ptr->text = head;

			te_line_t *newline = calloc(1, sizeof(te_line_t));
			newline->text = tail;
			newline->prev = ptr;
			newline->next = ptr->next;

			if (ptr->next) ptr->next->prev = newline;
			ptr->next = newline;
			if (lines->last == ptr) lines->last = newline;

			break;
		}
		ptr = ptr->next;
		l++;
	}

}

void te_delete(int line, int pos) {

	int l = 0;

	te_line_t *ptr = lines->first;

	while (ptr) {

		if (l == line) {
			int linelen = strlen(ptr->text);
			char *newtext = calloc(1, linelen);
			if (newtext == NULL) return;
			if (pos == 0) {
				memcpy(newtext, ptr->text + 1, linelen - pos - 1);
			} else {
				memcpy(newtext, ptr->text, pos);
				memcpy(newtext + pos, ptr->text + pos + 1, linelen - pos - 1);
			}
			free(ptr->text);
			ptr->text = newtext;
			break;
		}

		ptr = ptr->next;
		l++;

	};

}

// load file
int te_load(void) {

	int fs = 0;
	char *buf = NULL;

	if (te_filename != NULL) {
#ifdef EMBEDDED
		// example load
		fs = fs_size(te_filename);
		buf = fs_mallocfile(te_filename);
		if (buf == NULL && fs > 0) return 0;
#else
		FILE *f = fopen(te_filename, "rb");
		if (f == NULL) return 0;
		fseek(f, 0, SEEK_END);
		fs = ftell(f);
		fseek(f, 0, SEEK_SET);
		buf = malloc(fs);
		fread(buf, fs, 1, f);
		fclose(f);
#endif
	}

	int pos = 0;
	char c;

	for (int i = 0; i < fs; i++) {

		c = buf[i];

		if (c == '\r') continue;

		if (c == '\n') {
			pos = 0;
			/* a newline that's the very last byte of the file just
			 * terminates the last line -- it shouldn't create a new
			 * (empty) line after it */
			if (i != fs - 1) {
				te_insert_line(f_lines - 1);
				++f_lines;
			}
			continue;
		}

		te_insert(f_lines - 1, pos, c);
		++pos;

	}

	free(buf);

	return 1;

}

// write file
int te_save(void) {

	int ts = 0;

	char *buf = malloc(1);
	char *p;
	
	te_line_t *ptr = lines->first;

	while (ptr) {

		int ls = strlen(ptr->text);
		p = realloc(buf, ts + ls + 2);
		if (p == NULL) return 0;
		buf = p;
		memcpy(buf + ts, ptr->text, ls);
		ptr = ptr->next;
		ts += ls + 1;
		buf[ts - 1] = '\n';

	};

	buf[ts] = '\0';

#ifdef EMBEDDED
	// example save
	int w = fs_write_file(te_filename, buf, ts);
	if (w != ts) return(0);
#else
	FILE *f = fopen(te_filename, "wb");
	if (f == NULL) return(0);
	int w = fwrite(buf, ts, 1, f);
	if (!w) { fclose(f); return(0); }
	fclose(f);
#endif

	free(buf);
	return(1);

}

#ifdef MAIN
int main(int argc, char *argv[]) {

	if (argc > 1) {
		te_edit(argv[1]);
	} else {
		printf("%s <filename>\n", argv[0]);
	}

#ifdef CURSES
	endwin();
#endif

	return 0;

}
#endif
