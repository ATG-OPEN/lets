/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef LINES_H
#define LINES_H

#include <stdint.h>

#ifndef LINES_SIZE
#define LINES_SIZE 96u /**< the longest piece a command makes at a time, with its '\n' */
#endif

/**
 * @brief Output that is never lost: a command makes it one piece at a time.
 *
 * Some streams take only so much at once: a write returns 0 while their queue
 * is full (a port of a packet router takes a few packets, one per line). A
 * command that printed everything at once lost what came after that. A command
 * that uses lines_t makes its output a piece at a time (one line, or part of a
 * longer one) and lines_run() writes it: piece after piece while the stream
 * takes them, and once a write returns 0, a task of its own goes on with the
 * same piece a little later, so nothing is lost and the other tasks run in
 * between. The shell waits for that task before its next prompt (Ctrl-C stops
 * it on a terminal). Where every write is taken whole (a file, a UART) it all
 * happens inside the command, as before.
 *
 * Only one piece is in memory: next() reads what it needs each time (a file is
 * opened again, at an offset), so a stop at any point leaves nothing open.
 *
 * @code
 * typedef struct { lines_t lines; uint16_t at; } count_t;   // lines_t first
 *
 * static unsigned int count_next(lines_t *l, char *piece, unsigned int max)
 * {
 *     count_t *c = (count_t *)l;
 *     return (c->at < 3) ? (unsigned)snprintf(piece, max, "%u\n", c->at++) : 0;
 * }
 *
 * count_t *c = lines_alloc(sizeof(count_t), count_next);   // then fill in its state
 * return lines_run("count", &c->lines);
 * @endcode
 */
typedef struct lines lines_t;

/** next()'s answer when it can't make its piece now: it is asked again a little later. */
#define LINES_LATER ((unsigned int)-1)

/**
 * Makes the next piece of output in @p piece, at most @p max bytes, and returns
 * its length; 0 when there is no more, LINES_LATER when it can't be made now
 * (the heap is full: open() failed) and is to be asked for again a little
 * later. A '\n' may only be a piece's last byte (it becomes "\r\n" on a
 * terminal).
 */
typedef unsigned int (*lines_next_t)(lines_t *l, char *piece, unsigned int max);

struct lines {
	lines_next_t next;
	uint8_t length; /* of piece, 0: make the next one */
	uint8_t at;	/* written of it so far */
	char piece[LINES_SIZE + 1u];
};

/**
 * @brief A zeroed block of @p size bytes (a struct with lines_t first) for lines_run(), NULL when the heap is full.
 * When there is no room it prints "out of memory".
 */
void *lines_alloc(unsigned int size, lines_next_t next);

/**
 * @brief Writes what @p l makes to stdout, in a task named @p name when the stream is full.
 *
 * @p l is lines_alloc()'s block; lines_run() frees it (or the task does when it
 * ends or is stopped). @return 0, or -1 when stdout is gone or no task was free.
 */
int lines_run(const char *name, lines_t *l);

#endif /* LINES_H */
