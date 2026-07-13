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
#endif

#ifdef EMBEDDED
// example includes
#include "../common/zucker.h"
#include "include/te.h"
#include "include/fs.h"
#endif

#define ROWS 24
#define COLS 80
#define CONTENT_ROWS (ROWS - 1)  /* last row is reserved for the status bar */

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
void te_status(char *notice);
int te_yield(void);

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

int mode = MODE_MOVE;
int state = STATE_NONE;
int esc_num = 0;  /* numeric parameter accumulated in STATE_ESC2 / STATE_CMD_NUM */

int te_curs_x, te_curs_y;
int te_goal_x;  /* remembered column for vim-style sticky vertical movement */
int scroll_top;
int hscroll;    /* horizontal scroll offset, keeps the cursor visible on long lines */
int f_lines;

char *te_filename;
te_lines_t *lines;

void te_edit(char *filename) {
	te_filename = filename;
	te_init();
	if (!te_load()) {
		printf("unable to load file %s\n", te_filename);
		exit(0);
	} else {
		te_redraw();
		te_status("");
		while(te_yield());
	}
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
#endif

	te_curs_x = 0;
	te_curs_y = 0;
	te_goal_x = 0;
	scroll_top = 0;
	hscroll = 0;

}

void te_status(char *notice) {
	printf(VT100_CURSOR_MOVE_TO, 24, 0);
	printf(VT100_ERASE_LINE);
	printf("te %s l%i s%i x%i y%i %s", te_filename, f_lines, state, te_curs_x, te_curs_y, notice);
	printf(VT100_CURSOR_MOVE_TO, (te_curs_y - scroll_top) + 1, (te_curs_x - hscroll) + 1);
	fflush(stdout);
}

int te_yield(void) {

	int c = getch();
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
					printf("%c", c);
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
					} else if (te_curs_y > 0) {
						/* at start of line: merge this line into the previous one */
						int prevlen = te_line_len(te_curs_y - 1);
						te_join_line(te_curs_y);
						f_lines--;
						te_curs_y--;
						te_curs_x = prevlen;
					}
					te_goal_x = te_curs_x;
					te_redraw();
				} else if (c == CH_LF || c == CH_CR) {
					/* split the current line at the cursor position */
					te_split_line(te_curs_y, te_curs_x);
					f_lines++;
					te_curs_x = 0;
					te_curs_y++;
					te_goal_x = te_curs_x;
					te_redraw();
				} else {
					te_insert(te_curs_y, te_curs_x, c);
					te_redraw();
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
		if (te_curs_x > hscroll + COLS - 1) hscroll = te_curs_x - COLS + 1;
		if (hscroll < 0) hscroll = 0;

		if (scroll_top != old_scroll || hscroll != old_hscroll) te_redraw();
	}

	te_status("");

	return 1;

}

void te_redraw() {

	int l = 0;

	te_line_t *ptr = lines->first;

	printf(VT100_CURSOR_HOME);
	printf(VT100_ERASE_SCREEN);

	while (ptr) {

		if (l >= scroll_top && l < scroll_top + CONTENT_ROWS) {
			printf(VT100_CURSOR_MOVE_TO, (l - scroll_top) + 1, 1);
			int len = strlen(ptr->text);
			int start = (hscroll < len) ? hscroll : len;
			int outlen = len - start;
			if (outlen > COLS) outlen = COLS;
			printf("%.*s\n", outlen, ptr->text + start);
		}

		ptr = ptr->next;
		l++;

	}

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
