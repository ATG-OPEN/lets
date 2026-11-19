/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * /proc: what only the kernel knows, as text, for programs to read (vfs.h, "/proc"):
 *
 *   /proc/stat      up 0d 00:00:04  tasks 3  loop 1.56ms (641/s)  reset -
 *   /proc/meminfo   heap 104 of 16384 B used (0%)  stack 638 of 1018 B (62%)
 *   /proc/mem       the heap's blocks, the stack, the reset register, the watched tasks
 *   /proc/<ID>      one task, a row of `top`:  0 shell           sleep    0B     7us ...
 *
 * `top` is these lines; `kill NAME` finds the ID by the name in its row.
 *
 * The text is put together by hand, not with printf(): on a PIC24 a line by snprintf() took
 * 11 ms, long enough for a UART to fill its receive queue while every other task waited.
 *
 * With VFS_PROC (1, the default) it is a file system type mounted at /proc by
 * vfs_initialize(); a file's text is made when it is opened and kept until it is closed, so a
 * read goes on where the last one stopped. VFS_PROC 0 leaves the mount out, for a board short of
 * flash: `top` then takes its lines from vfs_proc_line() and vfs_proc_mem(), the same text.
 */

#include "vfs.h"
#include "../kernel/task.h"
#include "../kernel/alloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* text: counted when buf is NULL */

typedef struct {
	char *buf;
	unsigned int n;
	unsigned int max;
} proc_text_t;

static void proc_char(proc_text_t *t, char c)
{
	if (t->n < t->max) {
		if (t->buf != NULL) {
			t->buf[t->n] = c;
		}
		t->n++;
	}
}

static void proc_put(proc_text_t *t, const char *text)
{
	while (*text != '\0') {
		proc_char(t, *text++);
	}
}

/* @p v in decimal, at least @p width digits wide, padded with @p fill */
static void proc_uint(proc_text_t *t, uint32_t v, uint8_t width, char fill)
{
	char d[10];
	uint8_t k = 0;

	do {
		d[k++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v > 0);

	while (width > k) {
		proc_char(t, fill);
		width--;
	}

	while (k > 0) {
		proc_char(t, d[--k]);
	}
}

/* a time in us as "850us", "1.25ms", "125ms" or "12.5s" */
static void proc_time(proc_text_t *t, uint32_t us)
{
	if (us < 1000u) {
		proc_uint(t, us, 0, ' ');
		proc_put(t, "us");
	} else if (us < 10000u) {
		proc_uint(t, us / 1000u, 0, ' ');
		proc_put(t, ".");
		proc_uint(t, us / 10u % 100u, 2, '0');
		proc_put(t, "ms");
	} else if (us < 1000000u) {
		proc_uint(t, us / 1000u, 0, ' ');
		proc_put(t, "ms");
	} else {
		proc_uint(t, us / 1000000u, 0, ' ');
		proc_put(t, ".");
		proc_uint(t, us / 100000u % 10u, 0, ' ');
		proc_put(t, "s");
	}
}

/* part of whole in %, as "40%" */
static void proc_percent(proc_text_t *t, uint32_t part, uint32_t whole)
{
	proc_uint(t, whole ? part * 100u / whole : 0u, 0, ' ');
	proc_put(t, "%");
}

/* @p text in a column of @p width, to the right or the left; a space after it */
static void proc_column(proc_text_t *line, const char *text, uint8_t length, uint8_t width,
			bool right)
{
	uint8_t pad = (length < width) ? (uint8_t)(width - length) : 0u;

	for (; right && pad > 0; pad--) {
		proc_char(line, ' ');
	}

	for (uint8_t i = 0; i < length && i < width; i++) {
		proc_char(line, text[i]);
	}

	for (; pad > 0; pad--) {
		proc_char(line, ' ');
	}

	proc_put(line, " ");
}

/* why the chip last reset, and the fault the kernel kept, e.g. "watchdog (late: link)" */
static void proc_reset(proc_text_t *l)
{
	static const char *const names[] = {"-",	"power-on", "brown-out", "MCLR pin",
					    "software", "watchdog", "trap"};
	unsigned int r = (unsigned int)task_reset_reason();

	proc_put(l, names[(r < sizeof(names) / sizeof(names[0])) ? r : 0u]);

#if LETS_WDT
	const task_fault_t *f = task_fault();

	if (f == NULL) {
		return;
	}

	if (f->kind == TASK_FAULT_TRAP) {
		proc_put(l, " ");
		proc_uint(l, f->code, 0, ' ');
	}

	proc_put(l, (f->kind == TASK_FAULT_LATE)   ? " (late: "
		    : (f->kind == TASK_FAULT_HANG) ? " (hang: "
						   : " (in: ");
	proc_put(l, (f->task[0] != '\0') ? f->task : "-");

	if (f->by[0] != '\0') {
		proc_put(l, ", by: ");
		proc_put(l, f->by);
	}

	proc_put(l, ")");
#endif
}

/* one task's row, no trailing space */
static void proc_row(proc_text_t *line, uint8_t slot, const task_t *task)
{
	char c[12];
	proc_text_t cell = {c, 0, sizeof(c)};
	const char *state = (task->state == TASK_RUNNING)    ? "run"
			    : (task->state == TASK_SLEEPING) ? "sleep"
							     : "stop";

	proc_uint(&cell, slot, 0, ' ');
	proc_column(line, c, (uint8_t)cell.n, 3, true);
	proc_column(line, task->info.name, (uint8_t)strlen(task->info.name), 15, false);
	proc_column(line, state, (uint8_t)strlen(state), 5, false);

	cell.n = 0;
	proc_uint(&cell, (uint32_t)alloc_get_owner_size(slot), 0, ' ');
	proc_put(&cell, "B");
	proc_column(line, c, (uint8_t)cell.n, 5, true);

	const uint32_t times[3] = {task->info.duration, task->info.worst, task->info.delay};

	for (int i = 0; i < 3; i++) {
		cell.n = 0;
		proc_time(&cell, times[i]);
		proc_column(line, c, (uint8_t)cell.n, 7, true);
	}

	/* its share of the time: its last call over the time between its last two calls, in 0.1% */
	uint32_t d = task->info.duration, p = task->info.delay;
	cell.n = 0;

	if (p > 0) {
		uint32_t pm = (p >= 1000u) ? d / (p / 1000u) : d * 1000u / p;

		pm = (pm > 1000u) ? 1000u : pm;
		proc_uint(&cell, pm / 10u, 0, ' ');
		proc_put(&cell, ".");
		proc_uint(&cell, pm % 10u, 0, ' ');
		proc_put(&cell, "%");
	} else {
		proc_put(&cell, "-");
	}

	/* the last column without the space after it */
	proc_column(line, c, (uint8_t)cell.n, 5, true);
	line->n--;
}

static void proc_stat(proc_text_t *l)
{
	uint32_t up = (uint32_t)(task_get_system_time() / 1000000ull);
	uint32_t loop = (uint32_t)(task_get_loop_time() * 1000.0f);

	proc_put(l, "up ");
	proc_uint(l, up / 86400u, 0, ' ');
	proc_put(l, "d ");
	proc_uint(l, up / 3600u % 24u, 2, '0');
	proc_put(l, ":");
	proc_uint(l, up / 60u % 60u, 2, '0');
	proc_put(l, ":");
	proc_uint(l, up % 60u, 2, '0');
	proc_put(l, "  tasks ");
	proc_uint(l, (uint32_t)task_count(), 0, ' ');
	proc_put(l, "  loop ");
	proc_time(l, loop);

	if (loop > 0) {
		proc_put(l, " (");
		proc_uint(l, 1000000u / loop, 0, ' ');
		proc_put(l, "/s)");
	}

	proc_put(l, "  reset ");
	proc_reset(l);
}

static void proc_meminfo(proc_text_t *l)
{
	uint32_t heap = (uint32_t)alloc_heap_size();
	uint32_t used = heap - (uint32_t)alloc_free_size();
	size_t peak, size;

	proc_put(l, "heap ");
	proc_uint(l, used, 0, ' ');
	proc_put(l, " of ");
	proc_uint(l, heap, 0, ' ');
	proc_put(l, " B used (");
	proc_percent(l, used, heap);
	proc_put(l, ")");

	if (task_stack(&peak, &size) == 0) {
		proc_put(l, "  stack ");
		proc_uint(l, (uint32_t)peak, 0, ' ');
		proc_put(l, " of ");
		proc_uint(l, (uint32_t)size, 0, ' ');
		proc_put(l, " B (");
		proc_percent(l, (uint32_t)peak, (uint32_t)size);
		proc_put(l, ")");
	}
}

#if VFS_PROC
/* printf() into the text (counted when it has no buffer) */
static void proc_printf(proc_text_t *t, const char *format, ...)
{
	va_list list;

	va_start(list, format);

	int n = vsnprintf((t->buf != NULL) ? t->buf + t->n : NULL,
			  (t->buf != NULL) ? t->max + 1u - t->n : 0u, format, list);

	va_end(list);

	if (n > 0) {
		t->n += (unsigned int)n;
		t->n = (t->n > t->max) ? t->max : t->n;
	}
}
#else
/* no /proc: `top mem` prints it (vfs_proc_mem()) */
#define proc_printf(t, ...) printf(__VA_ARGS__)
#endif

/* `top mem`: the heap's blocks, the stack, the reset register, the watched tasks */
static void proc_mem(proc_text_t *l)
{
	size_t used, largest, peak, size;
	unsigned int count;

	alloc_stats(&used, &count, &largest);
	proc_printf(l, "heap %u B used %u B free %u B\nblocks %u/%u largest free %u B\n",
		    (unsigned)alloc_heap_size(), (unsigned)used, (unsigned)(alloc_heap_size() - used),
		    count, (unsigned)MAX_BLOCK_COUNT, (unsigned)largest);

	if (task_stack(&peak, &size) == 0) {
		proc_printf(l, "stack %u of %u B\n", (unsigned)peak, (unsigned)size);
	}

	proc_printf(l, "reset %04X", task_reset_cause());

#if LETS_WDT
	task_t *t = task_get_data();
	uint32_t now = (uint32_t)task_get_system_time();

	proc_printf(l, "\nwatchdog %u ms, ready tasks %u ms", (unsigned)LETS_WDT_MS,
		    (unsigned)LETS_WDT_DEFAULT_MS);

	for (int i = 0; i < TASKS_MAX_COUNT; i++) {
		/* the ones that check in (task_watch()), and the ones not watched */
		if (t[i].state != TASK_UNUSED && (t[i].deadline & TASK_WATCH_ALIVE)) {
			proc_printf(l, "\n  %s %u ms, alive %lu ms ago", t[i].info.name,
				    t[i].deadline & ~TASK_WATCH_ALIVE,
				    (unsigned long)((now - t[i].alive) / 1000u));
		} else if (t[i].state != TASK_UNUSED && t[i].deadline == 0) {
			proc_printf(l, "\n  %s not watched", t[i].info.name);
		}
	}
#endif
}

/* line @p line of `top` but its header: 0 stat, 1 meminfo, 2 + k the task in slot k */
static int proc_line(proc_text_t *t, unsigned int line)
{
	task_t *tasks = task_get_data();
	unsigned int k = line - 2u;

	if (line == 0u) {
		proc_stat(t);
	} else if (line == 1u) {
		proc_meminfo(t);
	} else if (k >= TASKS_MAX_COUNT) {
		return -1;
	} else if (tasks[k].state != TASK_UNUSED) {
		proc_row(t, (uint8_t)k, &tasks[k]);
	} else {
		return 0;
	}

	proc_char(t, '\n');

	if (t->buf != NULL) {
		t->buf[t->n] = '\0';
	}

	return (int)t->n;
}

int vfs_proc_line(unsigned int line, char *buf, unsigned int max)
{
	proc_text_t t = {buf, 0, max - 1u};

	return proc_line(&t, line);
}

#if VFS_PROC
/* /proc/mem in @p buf, or counted (NULL, room 0xFFFF) */
static int proc_mem_text(char *buf, unsigned int max)
{
	proc_text_t t = {buf, 0, max};

	proc_mem(&t);
	proc_printf(&t, "\n");

	return (int)t.n;
}
#else
void vfs_proc_mem(void)
{
	proc_mem(NULL);
	printf("\n");
}
#endif

#if VFS_PROC

/* the line of `top` file @p name is ("stat", "meminfo", a task slot "0" ...), -1 for none */
static int proc_name(const char *name)
{
	unsigned int k = 0;

	if (strcmp(name, "stat") == 0 || strcmp(name, "meminfo") == 0) {
		return (name[0] == 's') ? 0 : 1;
	}

	if (*name == '\0' || strlen(name) > 2u) {
		return -1;
	}

	for (; *name >= '0' && *name <= '9'; name++) {
		k = k * 10u + (unsigned int)(*name - '0');
	}

	return (*name == '\0') ? (int)k + 2 : -1;
}

/* the text of /proc/@p name in @p buf (NULL: counted), with its 0; its length or -1 */
static int proc_text(const char *name, char *buf, unsigned int max)
{
	int line = proc_name(name);
	proc_text_t t = {buf, 0, (buf == NULL) ? 0xFFFFu : max - 1u};

	if (strcmp(name, "mem") == 0) {
		return proc_mem_text(buf, t.max);
	}

	int made = (line < 0) ? -1 : proc_line(&t, (unsigned int)line);

	return (made > 0) ? made : -1;
}

#define PROC_SLACK 8 /* bytes more than counted: the text's numbers may grow meanwhile */

typedef struct {
	uint16_t size;
	uint16_t at;
	char text[];
} proc_file_t;

static void *proc_mount(const char *source, bool format)
{
	(void)source;
	(void)format;

	return &vfs_proc_type; /* any address: there is nothing to keep */
}

static void proc_umount(void *fs)
{
	(void)fs;
}

static int proc_info(void *fs, const char *name, vfs_info_t *info)
{
	int n = proc_text(name, NULL, 0);

	if (n < 0) {
		return -1;
	}

	memset(info, 0, sizeof(*info));
	info->size = (uint32_t)n;

	return 0;
}

/* stat, meminfo, mem, then the task slots in use */
static int proc_dir(void *fs, uint16_t *index, char *name, vfs_info_t *info)
{
	static const char *const files[] = {"stat", "meminfo", "mem"};

	for (;; (*index)++) {
		uint16_t i = *index;

		if (i < 3u) {
			strcpy(name, files[i]);
		} else if (i < 3u + TASKS_MAX_COUNT) {
			unsigned int k = i - 3u;

			name[0] = (char)('0' + k / 10u);
			name[1] = (char)('0' + k % 10u);
			name[2] = '\0';

			if (k < 10u) {
				name[0] = name[1];
				name[1] = '\0';
			}
		} else {
			return -1;
		}

		if (proc_info(fs, name, info) == 0) {
			(*index)++;
			return 0;
		}
	}
}

/*
 * The text is made now, for this handle. It is counted first; the block for it changes what
 * the heap holds, so a few more digits get room.
 */
static void *proc_open(void *fs, const char *name, int flags)
{
	int n = proc_text(name, NULL, 0) + PROC_SLACK;

	if (n < PROC_SLACK || (flags & O_ACCMODE) != O_RDONLY) {
		return NULL;
	}

	proc_file_t *f = malloc(sizeof(proc_file_t) + (size_t)n);

	if (f != NULL) {
		f->at = 0;
		f->size = (uint16_t)proc_text(name, f->text, (unsigned int)n);
	}

	return f;
}

static int proc_read(void *file, void *buffer, unsigned int length)
{
	proc_file_t *f = file;
	unsigned int left = f->size - f->at;

	length = (length > left) ? left : length;
	memcpy(buffer, f->text + f->at, length);
	f->at = (uint16_t)(f->at + length);

	return (int)length;
}

static int proc_write(void *file, const void *buffer, unsigned int length)
{
	return -1;
}

static int32_t proc_seek(void *file, int32_t offset, int whence)
{
	proc_file_t *f = file;
	int32_t to = offset + ((whence == SEEK_CUR) ? f->at : (whence == SEEK_END) ? f->size : 0);

	if (to < 0 || to > (int32_t)f->size) {
		return -1;
	}

	f->at = (uint16_t)to;

	return to;
}

static int proc_close(void *file)
{
	free(file);

	return 0;
}

static int proc_remove(void *fs, const char *name)
{
	return -1;
}

static const char *proc_source(void *fs)
{
	return "proc";
}

vfs_fs_type_t vfs_proc_type = {
	.name = "proc",
	.mount = proc_mount,
	.umount = proc_umount,
	.info = proc_info,
	.dir = proc_dir,
	.open = proc_open,
	.read = proc_read,
	.write = proc_write,
	.seek = proc_seek,
	.close = proc_close,
	.remove = proc_remove,
	.source = proc_source,
};

#endif /* VFS_PROC */
