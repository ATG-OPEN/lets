/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * argscript interpreter (the language is described in argscript.h).
 *
 * A script is a task. Each call runs up to ARGSCRIPT_STEPS lines, then
 * returns, so the other tasks keep running. The interpreter works directly on
 * the source text: `pos` is the offset of the next line, and blocks
 * (if / while / repeat) are a small stack of offsets to jump back to. Nothing
 * is compiled or stored per line, which keeps the RAM use to one as_state_t
 * per running script.
 *
 * Each line goes through three steps:
 *   as_fetch()   copies the line to as_raw, without indentation
 *   as_expand()  replaces $name and $? into as_line
 *   as_step()    runs a keyword, or passes the line to system()
 */

#include "argscript.h"

#include "../kernel/task.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */

#define AS_WAIT_MAX  30000u /* longest single task sleep in ms */
#define AS_FILE_SIZE 128u   /* longest script read from a node without a size */
#ifndef AS_FILE_MAX
#define AS_FILE_MAX 1024u /* longest script read from a file (a node lseek() can size) */
#endif

/* kinds of block on the block stack */
enum {
	AS_IF = 0,
	AS_WHILE,
	AS_REPEAT
};

/* where as_skip() stopped */
enum {
	AS_SKIP_EOF = 0,
	AS_SKIP_ELSE,
	AS_SKIP_END
};

/* a variable; an empty name marks a free entry */
typedef struct {
	char name[ARGSCRIPT_NAME_SIZE];
	char text[ARGSCRIPT_TEXT_SIZE];
} as_var_t;

/* an open if / while / repeat */
typedef struct {
	uint16_t pos;  /* while: its own line, repeat: first body line */
	int16_t count; /* repeat: rounds left */
	uint8_t type;  /* AS_IF, AS_WHILE or AS_REPEAT */
} as_block_t;

/* one running script: the task's argument, freed when the script ends */
typedef struct {
	const char *src; /* the source: text below, or a string in flash */
	uint16_t pos;	 /* next line */
	uint16_t line;	 /* current line, for while and errors */
	uint32_t wait;	 /* ms left to sleep */
	int result;	 /* $? */
	uint8_t depth;	 /* open blocks */
	as_block_t block[ARGSCRIPT_DEPTH];
	as_var_t var[ARGSCRIPT_VARS];
	uint16_t size; /* bytes in text */
	char text[];   /* own copy of the source, when read from a node */
} as_state_t;

/* one script line runs to the end within a task call, so all scripts share these */
static char as_raw[ARGSCRIPT_LINE_SIZE];
static char as_line[ARGSCRIPT_LINE_SIZE];

static int as_task(void *args, uint64_t system_time);

/* ---------------------------------- text ---------------------------------- */
/* letters, digits and '_' make a variable name */
static bool as_name_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       c == '_';
}

/* whether `line` starts with the whole word `word` */
static bool as_is(const char *line, const char *word)
{
	size_t n = strlen(word);

	return strncmp(line, word, n) == 0 &&
	       (line[n] == '\0' || line[n] == ' ' || line[n] == '\t');
}

/* next word, "quoted" words may hold spaces or be empty */
static char *as_word(char **p)
{
	char *s = *p;

	while (*s == ' ' || *s == '\t') {
		s++;
	}

	char end = ' ';

	if (*s == '"') {
		end = '"';
		s++;
	}

	char *w = s;

	while (*s != '\0' && *s != end && !(end == ' ' && *s == '\t')) {
		s++;
	}

	if (*s != '\0') {
		*s++ = '\0';
	}

	*p = s;

	return w;
}

/* `p` without leading blanks */
static char *as_rest(char *p)
{
	while (*p == ' ' || *p == '\t') {
		p++;
	}

	return p;
}

/* whether `t` is a number (and nothing else); its value in *v */
static bool as_number(const char *t, float *v)
{
	char *e;

	*v = (float)strtod(t, &e);

	return e != t && *as_rest(e) == '\0';
}

/* ------------------------------- variables -------------------------------- */
/* the variable `name`; with `create`, a new empty one when there is none */
static as_var_t *as_var(as_state_t *s, const char *name, bool create)
{
	as_var_t *empty = NULL;

	for (int i = 0; i < ARGSCRIPT_VARS; i++) {
		as_var_t *v = &s->var[i];

		if (v->name[0] == '\0') {
			empty = (empty == NULL) ? v : empty;
		} else if (strcmp(v->name, name) == 0) {
			return v;
		}
	}

	if (!create || empty == NULL || name[0] == '\0' || strlen(name) >= ARGSCRIPT_NAME_SIZE) {
		return NULL;
	}

	for (const char *c = name; *c != '\0'; c++) {
		if (!as_name_char(*c)) {
			return NULL;
		}
	}

	strcpy(empty->name, name);
	empty->text[0] = '\0';

	return empty;
}

/* stores text, cut to the variable size */
static void as_var_text(as_var_t *v, const char *text)
{
	strncpy(v->text, text, ARGSCRIPT_TEXT_SIZE - 1);
	v->text[ARGSCRIPT_TEXT_SIZE - 1] = '\0';
}

/* stores a number as text (7 significant digits, as a float holds) */
static void as_var_number(as_var_t *v, float f)
{
	snprintf(v->text, ARGSCRIPT_TEXT_SIZE, "%.7g", (double)f);
}

/* --------------------------------- source --------------------------------- */

/* copy the next line to as_raw without indentation and trailing spaces */
static bool as_fetch(as_state_t *s)
{
	const char *p = s->src + s->pos;

	if (*p == '\0') {
		return false;
	}

	s->line = s->pos;

	while (*p == ' ' || *p == '\t') {
		p++;
	}

	unsigned int n = 0;

	while (*p != '\0' && *p != '\n') {
		if (n < ARGSCRIPT_LINE_SIZE - 1) {
			as_raw[n++] = *p;
		}

		p++;
	}

	if (*p == '\n') {
		p++;
	}

	while (n > 0 && (as_raw[n - 1] == '\r' || as_raw[n - 1] == ' ' || as_raw[n - 1] == '\t')) {
		n--;
	}

	as_raw[n] = '\0';
	s->pos = (uint16_t)(p - s->src);

	return true;
}

/* as_raw to as_line with $name and $? replaced */
static int as_expand(as_state_t *s)
{
	const char *r = as_raw;
	unsigned int n = 0;

	while (*r != '\0') {
		char value[ARGSCRIPT_TEXT_SIZE];
		const char *add = value;

		if (r[0] == '$' && r[1] == '?') {
			snprintf(value, sizeof(value), "%d", s->result);
			r += 2;
		} else if (r[0] == '$' && as_name_char(r[1])) {
			char name[ARGSCRIPT_NAME_SIZE];
			unsigned int k = 0;

			for (r++; as_name_char(*r); r++) {
				if (k < ARGSCRIPT_NAME_SIZE - 1) {
					name[k++] = *r;
				}
			}

			name[k] = '\0';

			as_var_t *v = as_var(s, name, false);
			add = (v == NULL) ? "" : v->text;
		} else {
			value[0] = *r++;
			value[1] = '\0';
		}

		size_t len = strlen(add);

		if (n + len >= ARGSCRIPT_LINE_SIZE) {
			return -1;
		}

		memcpy(as_line + n, add, len);
		n += len;
	}

	as_line[n] = '\0';

	return 0;
}

/* jump past the matching end, or past a matching else when to_else is set */
static int as_skip(as_state_t *s, bool to_else)
{
	int depth = 0;

	while (as_fetch(s)) {
		if (as_is(as_raw, "if") || as_is(as_raw, "while") || as_is(as_raw, "repeat")) {
			depth++;
		} else if (as_is(as_raw, "end")) {
			if (depth == 0) {
				return AS_SKIP_END;
			}

			depth--;
		} else if (to_else && depth == 0 && as_is(as_raw, "else")) {
			return AS_SKIP_ELSE;
		}
	}

	return AS_SKIP_EOF;
}

/* moves to the line after :name; drops the open blocks */
static int as_label(as_state_t *s, const char *name)
{
	s->pos = 0;
	s->depth = 0;

	while (as_fetch(s)) {
		if (as_raw[0] == ':' && strcmp(as_raw + 1, name) == 0) {
			return 0;
		}
	}

	return -1;
}

/* prints the line number and the message; returns -1 (stops the script) */
static int as_error(as_state_t *s, const char *msg)
{
	unsigned int line = 1;

	for (uint16_t i = 0; i < s->line; i++) {
		line += (s->src[i] == '\n');
	}

	printf("-as line %u: %s\n", line, msg);

	return -1;
}

/* ------------------------------- statements ------------------------------- */

/* 1 true, 0 false, -1 bad operator */
static int as_cond(char *p)
{
	char *a = as_word(&p);
	char *op = as_word(&p);
	char *b = as_word(&p);
	float x;
	float y;

	if (op[0] == '\0') {
		return a[0] != '\0' && !(as_number(a, &x) && x == 0);
	}

	int c;

	if (as_number(a, &x) && as_number(b, &y)) {
		c = (x > y) - (x < y);
	} else {
		c = strcmp(a, b);
	}

	if (strcmp(op, "==") == 0)
		return c == 0;
	if (strcmp(op, "!=") == 0)
		return c != 0;
	if (strcmp(op, "<") == 0)
		return c < 0;
	if (strcmp(op, ">") == 0)
		return c > 0;
	if (strcmp(op, "<=") == 0)
		return c <= 0;
	if (strcmp(op, ">=") == 0)
		return c >= 0;

	return -1;
}

/* set NAME text | set NAME number op number */
static int as_set(as_state_t *s, char *p)
{
	as_var_t *v = as_var(s, as_word(&p), true);

	if (v == NULL) {
		return as_error(s, "bad variable");
	}

	p = as_rest(p);

	/* number op number is math, anything else is text */
	char *e;
	float a = (float)strtod(p, &e);
	char *o = as_rest(e);
	char op = o[0];

	if (e == p || op == '\0' || strchr("+-*/%", op) == NULL || (o[1] != ' ' && o[1] != '\t')) {
		as_var_text(v, p);
		return 0;
	}

	float b;

	if (!as_number(o + 1, &b)) {
		as_var_text(v, p);
		return 0;
	}

	if ((op == '/' || op == '%') && b == 0) {
		return as_error(s, "divide by zero");
	}

	switch (op) {
	case '+':
		a += b;
		break;
	case '-':
		a -= b;
		break;
	case '*':
		a *= b;
		break;
	case '/':
		a /= b;
		break;
	default:
		a = (float)((long)a % (long)b);
		break;
	}

	as_var_number(v, a);

	return 0;
}

/* opens a block */
static int as_push(as_state_t *s, uint8_t type, uint16_t pos, int16_t count)
{
	if (s->depth >= ARGSCRIPT_DEPTH) {
		return as_error(s, "too deep");
	}

	as_block_t *b = &s->block[s->depth++];

	b->type = type;
	b->pos = pos;
	b->count = count;

	return 0;
}

/* `end`: a repeat goes round again, a while re-checks, an if closes */
static int as_end(as_state_t *s)
{
	if (s->depth == 0) {
		return as_error(s, "end without block");
	}

	as_block_t *b = &s->block[s->depth - 1];

	if (b->type == AS_REPEAT && --b->count > 0) {
		s->pos = b->pos;
		return 0;
	}

	/* a while goes back to its own line and checks again */
	if (b->type == AS_WHILE) {
		s->pos = b->pos;
	}

	s->depth--;

	return 0;
}

/* leaves the innermost loop, with any ifs inside it */
static int as_break(as_state_t *s)
{
	int k = s->depth - 1;

	while (k >= 0 && s->block[k].type == AS_IF) {
		k--;
	}

	if (k < 0) {
		return as_error(s, "break without loop");
	}

	int ends = s->depth - k;
	s->depth = (uint8_t)k;

	while (ends-- > 0) {
		if (as_skip(s, false) != AS_SKIP_END) {
			return as_error(s, "missing end");
		}
	}

	return 0;
}

/* kind: r read, g get, p put, w write */
static int as_io(as_state_t *s, char kind, char *p)
{
	char *first = as_word(&p);
	char *node = first;
	as_var_t *v = NULL;

	/* read and get name the variable first, put and write the node */
	if (kind == 'r' || kind == 'g') {
		v = as_var(s, first, true);
		node = as_word(&p);

		if (v == NULL) {
			return as_error(s, "bad variable");
		}
	}

	int fd = open(node, 0, 0);

	if (fd < 0) {
		s->result = -1;
		return 0;
	}

	float f;

	if (kind == 'r') {
		int n = read(fd, v->text, ARGSCRIPT_TEXT_SIZE - 1);
		s->result = n;
		n = (n < 0) ? 0 : n;

		while (n > 0 && (v->text[n - 1] == '\n' || v->text[n - 1] == '\r')) {
			n--;
		}

		v->text[n] = '\0';
	} else if (kind == 'g') {
		s->result = read(fd, &f, sizeof(f));

		if (s->result == sizeof(f)) {
			as_var_number(v, f);
		}
	} else if (kind == 'p') {
		f = (float)atof(as_word(&p));
		s->result = write(fd, &f, sizeof(f));
	} else {
		/* decode \n in place, the text only gets shorter */
		char *t = as_rest(p);
		unsigned int n = 0;

		for (char *c = t; *c != '\0'; c++) {
			if (c[0] == '\\' && c[1] == 'n') {
				t[n++] = '\n';
				c++;
			} else {
				t[n++] = *c;
			}
		}

		s->result = write(fd, t, n);
	}

	close(fd);

	return 0;
}

/* run NAME :label - a new script task, starting at the label, with a copy
   of this script's variables */
static int as_run(as_state_t *s, char *p)
{
	char *name = as_word(&p);
	char *label = as_word(&p);

	if (name[0] == '\0' || label[0] != ':') {
		return as_error(s, "bad argument");
	}

	as_state_t *c = malloc(sizeof(as_state_t) + s->size);

	if (c == NULL) {
		return as_error(s, "out of memory");
	}

	memcpy(c, s, sizeof(as_state_t) + s->size);

	if (s->size > 0) {
		c->src = c->text;
	}

	c->wait = 0;

	if (as_label(c, label + 1) < 0) {
		free(c);
		return as_error(s, "not found");
	}

	if (task_create(name, as_task, c) == NULL) {
		free(c);
		return as_error(s, "out of memory");
	}

	s->result = 0;

	return 0;
}

/* the keywords, in the order of the AS_K_* values below */
static const char *const as_keywords[] = {
	"set",	"if",	"else", "end", "while", "repeat", "break", "goto",
	"wait", "read", "get",	"put", "write", "run",	  "exit",
};

enum {
	AS_K_SET = 0,
	AS_K_IF,
	AS_K_ELSE,
	AS_K_END,
	AS_K_WHILE,
	AS_K_REPEAT,
	AS_K_BREAK,
	AS_K_GOTO,
	AS_K_WAIT,
	AS_K_READ,
	AS_K_GET,
	AS_K_PUT,
	AS_K_WRITE,
	AS_K_RUN,
	AS_K_EXIT,
	AS_K_COUNT
};

/* one line from as_raw, < 0 stops the script */
static int as_step(as_state_t *s)
{
	if (as_raw[0] == '\0' || as_raw[0] == '#' || as_raw[0] == ':') {
		return 0;
	}

	if (as_expand(s) < 0) {
		return as_error(s, "line too long");
	}

	int k = 0;

	while (k < AS_K_COUNT && !as_is(as_line, as_keywords[k])) {
		k++;
	}

	if (k == AS_K_COUNT) {
		int r = system(as_line);

		/* the command may have killed this script, s is gone then */
		task_t *t = task_get_data() + task_get_current();

		if (t->state == TASK_UNUSED) {
			return -1;
		}

		s->result = r;

		return 0;
	}

	char *p = as_line;
	as_word(&p);

	int c;

	switch (k) {
	case AS_K_SET:
		return as_set(s, p);

	case AS_K_IF:
		c = as_cond(p);

		if (c < 0) {
			return as_error(s, "bad argument");
		}

		if (c > 0) {
			return as_push(s, AS_IF, 0, 0);
		}

		c = as_skip(s, true);

		if (c == AS_SKIP_EOF) {
			return as_error(s, "missing end");
		}

		return (c == AS_SKIP_ELSE) ? as_push(s, AS_IF, 0, 0) : 0;

	case AS_K_ELSE:
		/* the if part ran, leave over the else part */
		if (s->depth == 0 || s->block[s->depth - 1].type != AS_IF) {
			return as_error(s, "else without if");
		}

		s->depth--;

		return (as_skip(s, false) == AS_SKIP_END) ? 0 : as_error(s, "missing end");

	case AS_K_END:
		return as_end(s);

	case AS_K_WHILE:
		c = as_cond(p);

		if (c < 0) {
			return as_error(s, "bad argument");
		}

		if (c > 0) {
			return as_push(s, AS_WHILE, s->line, 0);
		}

		return (as_skip(s, false) == AS_SKIP_END) ? 0 : as_error(s, "missing end");

	case AS_K_REPEAT:
		c = atoi(as_word(&p));

		if (c > 0) {
			return as_push(s, AS_REPEAT, s->pos, (int16_t)c);
		}

		return (as_skip(s, false) == AS_SKIP_END) ? 0 : as_error(s, "missing end");

	case AS_K_BREAK:
		return as_break(s);

	case AS_K_GOTO:
		return (as_label(s, as_word(&p)) == 0) ? 0 : as_error(s, "not found");

	case AS_K_WAIT:
		s->wait = strtoul(as_word(&p), NULL, 10);
		return 0;

	case AS_K_RUN:
		return as_run(s, p);

	case AS_K_EXIT:
		return -1;

	default:
		return as_io(s, "rgpw"[k - AS_K_READ], p);
	}
}

/* ---------------------------------- task ---------------------------------- */
/* sleeps at most AS_WAIT_MAX at a time: a task's return value is an int */
static int as_sleep(as_state_t *s)
{
	uint32_t ms = (s->wait > AS_WAIT_MAX) ? AS_WAIT_MAX : s->wait;

	s->wait -= ms;

	return (int)ms;
}

/* the script task: a few lines per call */
static int as_task(void *args, uint64_t system_time)
{
	as_state_t *s = args;

	if (s->wait > 0) {
		return as_sleep(s);
	}

	for (int i = 0; i < ARGSCRIPT_STEPS; i++) {
		if (!as_fetch(s) || as_step(s) < 0) {
			return -1;
		}

		if (s->wait > 0) {
			return as_sleep(s);
		}
	}

	return 0;
}

/* a new state with room for `size` bytes of source text */
static as_state_t *as_new(uint16_t size)
{
	as_state_t *s = malloc(sizeof(as_state_t) + size);

	if (s != NULL) {
		memset(s, 0, sizeof(as_state_t));
		s->size = size;
		s->src = s->text;
	}

	return s;
}

/* --------------------------------- public --------------------------------- */

int argscript_start(const char *name, const char *source)
{
	if (name == NULL || source == NULL) {
		return -1;
	}

	as_state_t *s = as_new(0);

	if (s == NULL) {
		return -1;
	}

	s->src = source;

	if (task_create(name, as_task, s) == NULL) {
		free(s);
		return -1;
	}

	return 0;
}

int argscript_execute(void *args, int argc, char *argv[])
{
	if (argc < 2 || argc > 3) {
		printf("bad argument\n");
		return -1;
	}

	if (strcmp(argv[1], "-help") == 0) {
		printf("as NODE [NAME]   run the script stored in NODE\n");
		return 0;
	}

	int fd = open(argv[1], 0, 0);

	if (fd < 0) {
		printf("open failed\n");
		return -1;
	}

	/* a file has a size: read all of it; another node: what one read gives */
	long end = (long)lseek(fd, 0, SEEK_END);
	unsigned int size = AS_FILE_SIZE;

	if (end >= (long)AS_FILE_MAX) {
		close(fd);
		printf("too long\n");
		return -1;
	}

	if (end > 0 && lseek(fd, 0, SEEK_SET) == 0) {
		size = (unsigned int)end + 1u;
	}

	as_state_t *s = as_new((uint16_t)size);

	if (s == NULL) {
		close(fd);
		printf("out of memory\n");
		return -1;
	}

	int n = read(fd, s->text, size - 1u);
	close(fd);

	s->text[(n < 0) ? 0 : n] = '\0';

	if (task_create((argc == 3) ? argv[2] : "as", as_task, s) == NULL) {
		free(s);
		printf("out of memory\n");
		return -1;
	}

	return 0;
}
