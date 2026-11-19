/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Host test for argscript, runs on the PC with fake nodes and a fake task loop.
 *
 *   cc -std=c99 -Wall -Wextra -include fcntl.h -Dopen=t_open -Dread=t_read \
 *      -Dwrite=t_write -Dclose=t_close -Dsystem=t_system -Dlseek=t_lseek \
 *      -Dmalloc=t_malloc -Dfree=t_free -Dprintf=t_printf \
 *      as_test.c ../argscript.c ../../lib/args.c -o as_test && ./as_test
 */

#include "../argscript.h"
#include "../../kernel/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

/* argscript.c gets malloc, free and printf as t_malloc, t_free and t_printf (below); here they
 * are the C library's */
#undef malloc
#undef free
#undef printf
void *malloc(size_t size);
void free(void *p);
int printf(const char *format, ...);

static void log_add(const char *text);

/* ------------------------------- fake heap -------------------------------- */
/* blocks with their size and owner, as the kernel's: a stopped task's blocks are freed */

#define BLOCKS 64

static struct {
	void *p;
	size_t size;
	int owner;
} blocks[BLOCKS];
static size_t heap_used, heap_peak;

void *t_malloc(size_t size)
{
	for (int i = 0; i < BLOCKS; i++) {
		if (blocks[i].p == NULL) {
			blocks[i].p = malloc(size);
			blocks[i].size = size;
			blocks[i].owner = -1;
			heap_used += size;
			heap_peak = (heap_used > heap_peak) ? heap_used : heap_peak;
			return blocks[i].p;
		}
	}

	return NULL;
}

void t_free(void *p)
{
	for (int i = 0; p != NULL && i < BLOCKS; i++) {
		if (blocks[i].p == p) {
			free(p);
			blocks[i].p = NULL;
			heap_used -= blocks[i].size;
			return;
		}
	}
}

int alloc_change_owner(void *p, int owner)
{
	for (int i = 0; p != NULL && i < BLOCKS; i++) {
		if (blocks[i].p == p) {
			blocks[i].owner = owner;
			return 0;
		}
	}

	return -1;
}

static void free_owner(int owner)
{
	for (int i = 0; i < BLOCKS; i++) {
		if (blocks[i].p != NULL && blocks[i].owner == owner) {
			t_free(blocks[i].p);
		}
	}
}

/* what the script prints (errors) goes to the log too, in braces */
int t_printf(const char *format, ...)
{
	char text[128] = "{";
	va_list list;

	va_start(list, format);
	int n = vsnprintf(text + 1, sizeof(text) - 2, format, list);
	va_end(list);

	text[strcspn(text, "\n")] = '\0';
	strcat(text, "}");
	log_add(text);

	return n;
}

/* ------------------------------ fake kernel ------------------------------- */

static task_t tasks[TASKS_MAX_COUNT];
static int current = -1;
static uint64_t now = 0; /* ms */

task_t *task_get_data(void)
{
	return tasks;
}

bool task_stopping(void)
{
	return current >= 0 && tasks[current].state == TASK_STOPPING;
}

int task_get_current(void)
{
	return current;
}

static task_streams_t streams_main = {-1, -1, -1, 0};

task_streams_t *task_streams(void)
{
	return (current >= 0 && current < (int)TASKS_MAX_COUNT) ? &tasks[current].streams : &streams_main;
}

void *task_alloc(size_t size)
{
	void *p = t_malloc(size);

	alloc_change_owner(p, current);

	return p;
}

task_t *task_create(const char *name, int (*entry)(void *arg, uint64_t system_time), void *arg)
{
	for (int i = 0; i < (int)TASKS_MAX_COUNT; i++) {
		if (tasks[i].state == TASK_UNUSED) {
			tasks[i].entry = entry;
			tasks[i].arg = arg;
			tasks[i].state = TASK_RUNNING;
			tasks[i].wake_tick = now;
			tasks[i].streams = *task_streams();
			strncpy(tasks[i].info.name, name, TASK_NAME_SIZE - 1);
			alloc_change_owner(arg, i);
			return &tasks[i];
		}
	}

	return NULL;
}

static void stop(int i)
{
	tasks[i].state = TASK_UNUSED;
	free_owner(i);
}

static void run_tasks(uint64_t until)
{
	for (; now <= until; now++) {
		for (current = 0; current < (int)TASKS_MAX_COUNT; current++) {
			task_t *t = &tasks[current];

			if (t->state == TASK_UNUSED || now < t->wake_tick) {
				continue;
			}

			int r = t->entry(t->arg, now);

			if (r < 0 || t->state == TASK_STOPPING) {
				stop(current);
				continue;
			}

			t->wake_tick = now + r;
		}
	}
}

/* ------------------------------- fake nodes ------------------------------- */

static char out[2048]; /* everything the script did, in order */
static char file_a[128] = "hello file";
static char file_b[128];
static float pot = 0.5f;

static void log_add(const char *text)
{
	strncat(out, text, sizeof(out) - strlen(out) - 1);
}

static long long_pos; /* the read position of the file open now */
static int flaky_fails;
static int flaky_lose; /* what `lose` sets flaky_fails to */

/* the working directory, as a path: directory 5 is /d5 */
char *t_getcwd(char *buf, size_t size)
{
	snprintf(buf, size, "%s", (task_streams()->cwd == 5) ? "/d5" : "/");
	return buf;
}

int t_open(const char *path, int flags, ...)
{
	(void)flags;
	/* a script's file is opened by its whole path, from where it started */
	if (strcmp(path, "/d5/here") == 0 || (strcmp(path, "here") == 0 && task_streams()->cwd == 5))
		return 10;
	path += (path[0] == '/') ? 1 : 0;
	if (strcmp(path, "pot") == 0)
		return 3;
	if (strcmp(path, "led") == 0)
		return 4;
	if (strcmp(path, "fa") == 0)
		return 5;
	if (strcmp(path, "fb") == 0)
		return 6;
	if (strcmp(path, "script") == 0)
		return 7;
	if (strcmp(path, "long") == 0)
		return 8;
	if (strcmp(path, "big") == 0)
		return 9;
	/* opens after flaky_fails more tries (as with a full heap) */
	if (strcmp(path, "flaky") == 0 && flaky_fails > 0) {
		flaky_fails--;
		return -1;
	}
	if (strcmp(path, "flaky") == 0)
		return 11;
	if (strcmp(path, "shrinks") == 0) {
		long_pos = 0;
		return 12;
	}
	return -1;
}

/* files: they have a size, and are read a line at a time */
static const char long_script[] =
	"# a script longer than 127 bytes, read whole because the file has a size\n"
	"set n 0\n"
	"repeat 4\n"
	"    set n $n + 1\n"
	"end\n"
	"echo long $n\n";

/* made shorter by the command `cut` while it runs */
static char shrinks[64];

/* larger than the heap of a small board: built by main() */
static char big_script[4096];

static const char here_script[] = "cd 0\necho a\n  echo b\n";

static const char *file_text(int fd)
{
	static const char script[] = "set n 3\nwhile $n > 0\necho file $n\nset n $n - 1\nend\n";

	static const char flaky[] = "echo one\nlose\necho two\n";

	return (fd == 7) ? script : (fd == 8) ? long_script : (fd == 9) ? big_script : (fd == 10) ? here_script :
	       (fd == 11) ? flaky : (fd == 12) ? shrinks : NULL;
}

off_t t_lseek(int fd, off_t offset, int whence)
{
	if (file_text(fd) == NULL)
		return -1; /* a node without a size */
	long size = (long)strlen(file_text(fd));
	long to = (whence == SEEK_END) ? size + offset : (whence == SEEK_SET) ? offset : long_pos + offset;
	if (to < 0 || to > size)
		return -1; /* as a file system: not past the end */
	long_pos = to;
	return long_pos;
}

int t_close(int fd)
{
	(void)fd;
	return 0;
}

ssize_t t_read(int fd, void *buf, size_t len)
{
	const char *src = NULL;

	if (fd == 3) {
		memcpy(buf, &pot, sizeof(pot));
		return sizeof(pot);
	}

	if (fd == 5)
		src = file_a;
	if (file_text(fd) != NULL)
		src = file_text(fd) + long_pos;

	if (src == NULL) {
		return -1;
	}

	size_t n = strlen(src);
	n = (n < len) ? n : len;
	memcpy(buf, src, n);

	return n;
}

ssize_t t_write(int fd, const void *buf, size_t len)
{
	char line[160];

	if (fd == 4) {
		snprintf(line, sizeof(line), "[led %g]", (double)*(const float *)buf);
		log_add(line);
		return len;
	}

	if (fd == 6) {
		strncat(file_b, buf, len);
		return len;
	}

	return -1;
}

int t_system(const char *cmd)
{
	char line[160];

	if (strncmp(cmd, "kill ", 5) == 0) {
		for (int i = 0; i < (int)TASKS_MAX_COUNT; i++) {
			if (tasks[i].state != TASK_UNUSED &&
			    strcmp(tasks[i].info.name, cmd + 5) == 0) {
				if (i == current) {
					tasks[i].state = TASK_STOPPING;
				} else {
					stop(i);
				}
				return 0;
			}
		}
		return -1;
	}

	if (strncmp(cmd, "cd ", 3) == 0) {
		task_streams()->cwd = (uint8_t)atoi(cmd + 3);
		return 0;
	}

	/* the heap the scripts hold now, against the last `heap` */
	if (strcmp(cmd, "heap") == 0) {
		static size_t last;

		snprintf(line, sizeof(line), "[heap %+d]", (int)(heap_used - last));
		log_add(strstr(out, "[heap") == NULL ? "[heap]" : line);
		last = heap_used;
		return 0;
	}

	if (strcmp(cmd, "lose") == 0) {
		flaky_fails = flaky_lose;
		return 0;
	}

	if (strcmp(cmd, "cut") == 0) {
		strcpy(shrinks, "x\n");
		return 0;
	}

	if (strcmp(cmd, "fail") == 0) {
		log_add("[fail]");
		return -1;
	}

	/* whether the script still holds its state (a block it owns) while this command runs */
	if (strcmp(cmd, "state") == 0) {
		bool held = false;

		for (int i = 0; i < BLOCKS; i++) {
			held |= (current >= 0 && blocks[i].p != NULL && blocks[i].owner == current);
		}
		log_add(held ? "[held]" : "[freed]");
		return 0;
	}

	if (strncmp(cmd, "as ", 3) == 0) {
		char buf[64];
		strcpy(buf, cmd);
		char *argv[3] = {buf, buf + 3, NULL};
		return argscript_execute(NULL, 2, argv);
	}

	snprintf(line, sizeof(line), "[%s]", cmd);
	log_add(line);

	return 0;
}

/* --------------------------------- tests ---------------------------------- */

static int failed = 0;

static void reset(void)
{
	for (int i = 0; i < (int)TASKS_MAX_COUNT; i++) {
		if (tasks[i].state != TASK_UNUSED) {
			stop(i);
		}
	}

	out[0] = '\0';
	file_b[0] = '\0';
	now = 0;
	heap_peak = heap_used;
	streams_main.cwd = 0;
}

static void check(const char *name, const char *script, uint64_t ms, const char *expect)
{
	reset();
	argscript_start("t", script);
	run_tasks(ms);

	if (expect == NULL) {
		return; /* the caller looks at out */
	}

	if (strcmp(out, expect) != 0) {
		printf("FAIL %s\n  got    %s\n  expect %s\n", name, out, expect);
		failed++;
	} else {
		printf("ok   %s\n", name);
	}
}

int main(void)
{
	check("plain commands", "ttyS1 -b 115200\n\n   # comment\nstd ttyS1", 5,
	      "[ttyS1 -b 115200][std ttyS1]");

	check("set and math",
	      "set a 7\nset b $a * 3\nset c $b % 4\nset d $a / 2\necho $a $b $c $d\nset t hi "
	      "there\necho $t!",
	      5, "[echo 7 21 1 3.5][echo hi there!]");

	check("if else",
	      "set x 5\nif $x > 3\necho big\nelse\necho small\nend\nif $x == 9\necho "
	      "nine\nelse\necho other\nend\nif abc == abc\necho text\nend",
	      5, "[echo big][echo other][echo text]");

	check("if empty and quotes",
	      "if \"$nope\" == \"\"\necho empty\nend\nif $nope\necho bad\nend\nif 0\necho "
	      "bad\nend\nif 2\necho two\nend",
	      5, "[echo empty][echo two]");

	check("nested if skip",
	      "if 0\nif 1\necho a\nelse\necho b\nend\necho c\nelse\necho d\nend\necho e", 5,
	      "[echo d][echo e]");

	check("while", "set i 0\nwhile $i < 3\n  echo $i\n  set i $i + 1\nend\necho done", 5,
	      "[echo 0][echo 1][echo 2][echo done]");

	check("repeat nested",
	      "repeat 2\n  repeat 2\n    echo x\n  end\n  echo y\nend\nrepeat 0\necho never\nend",
	      5, "[echo x][echo x][echo y][echo x][echo x][echo y]");

	check("break",
	      "set i 0\nwhile 1\n  set i $i + 1\n  if $i == 3\n    break\n  end\nend\necho "
	      "$i\nrepeat 5\nbreak\nend\necho out",
	      5, "[echo 3][echo out]");

	check("goto", "set n 0\n:top\nset n $n + 1\nif $n < 3\ngoto top\nend\necho $n", 5,
	      "[echo 3]");

	check("wait", "echo a\nwait 100\necho b", 50, "[echo a]");

	check("wait long", "echo a\nwait 40000\necho b", 40002, "[echo a][echo b]");

	check("result", "fail\necho $?\nok\necho $?", 5, "[fail][echo -1][ok][echo 0]");

	check("get put", "get p pot\nset q $p * 4\nput led $q\necho $p", 5, "[led 2][echo 0.5]");

	check("read write files", "read t fa\nwrite fb $t\\n$?\necho $t", 5, "[echo hello file]");
	if (strcmp(file_b, "hello file\n10") != 0) {
		printf("FAIL file copy: %s\n", file_b);
		failed++;
	}

	check("missing node", "read t none\necho $?", 5, "[echo -1]");

	check("run task",
	      "set v 1\nrun blink :blink\nwait 25\nkill blink\necho stop\nexit\n:blink\nwhile "
	      "1\nput led $v\nwait 10\nend",
	      100, "[led 1][led 1][led 1][echo stop]");

	check("exit", "echo a\nexit\necho b", 5, "[echo a]");

	check("self kill", "echo a\nkill t\necho b", 5, "[echo a]");

	check("as from node", "as script", 5, "[echo file 3][echo file 2][echo file 1]");
	check("as long file", "as long", 20, "[echo long 4]");
	check("as needs a file", "as pot\necho $?", 20, "{not a file}[echo -1]");

	/* the last command runs with the script's memory already freed */
	check("last line frees", "echo a\nstate", 5, "[echo a][freed]");
	check("last line, comments after", "state\n\n  # done\n:label\n", 5, "[freed]");
	check("not the last line", "state\necho b", 5, "[held][echo b]");
	check("last line in a block", "if 1\nstate\nend", 5, "[held]");
	check("last line in a loop", "repeat 2\nstate\nend", 5, "[held][held]");
	check("last line kills the script", "echo a\nkill t", 5, "[echo a]");

	check("busy loop yields", "while 1\nend", 3, "");

	check("errors", "set a 1 / 0\necho never", 5, "{-as line 1: divide by zero}");
	check("modulo of a fraction", "set a 5 % 0.5\necho never", 5, "{-as line 1: divide by zero}");
	check("error end", "echo a\n\n  # x\nend", 5, "[echo a]{-as line 4: end without block}");
	check("error missing end", "if 0\necho a", 5, "{-as line 2: missing end}");
	check("error label", "set a 1\ngoto nowhere", 5, "{-as line 2: not found}");

	/* variables take heap one entry at a time, and only once they exist */
	check("variables on the heap", "heap\nset a 1\nheap\nset b 2\nset a 3\nheap\necho $a $b", 5,
	      "[heap][heap +32][heap +32][echo 3 2]");
	check("variables, the most", "set a 1\nset b 2\nset c 3\nset d 4\nset e 5\nset f 6\nset g 7\n"
	      "set h 8\nset i 9\necho $a$h$i", 5, "{-as line 9: bad variable}");
	check("run copies the variables",
	      "set v 7\nrun child :c\nset v 1\nwait 5\necho parent $v\nexit\n:c\necho child $v", 20,
	      "[echo child 7][echo parent 1]");

	/* a file is read a line at a time: a script larger than a small board's heap */
	{
		char *p = big_script;
		char *end = big_script + sizeof(big_script);

		p += snprintf(p, end - p, "heap\nset n 0\n");
		for (int i = 0; i < 50; i++) {
			p += snprintf(p, end - p, "    # line %d of a script far longer than the heap of a small board\n", i);
		}
		p += snprintf(p, end - p, "repeat 3\n\tset n $n + 1\nend\n");
		p += snprintf(p, end - p, "   echo %0120d\n", 7); /* indented, longer than a line: cut */
		snprintf(p, end - p, "echo big $n\nend\n");
	}
	check("as big file", "as big", 20, NULL);
	/* the long line is cut to 95 characters with its indentation, its rest skipped */
	if (strncmp(out, "[heap][echo 0000", 16) != 0 || strlen(out) != 6 + 94 + 12 + 32 ||
	    strstr(out, "0][echo big 3]{-as line 58: end without block}") == NULL) {
		printf("FAIL as big file: %s\n", out);
		failed++;
	}
	if (heap_peak > 160) {
		printf("FAIL as big file: %u B of heap\n", (unsigned)heap_peak);
		failed++;
	} else {
		printf("ok   as big file: %u B of heap for a %u B script\n", (unsigned)heap_peak,
		       (unsigned)strlen(big_script));
	}

	/* a file that cannot be opened for a while (a full heap): the same line later; gone: an error */
	flaky_lose = 5;
	check("as file not opened for a while", "as flaky", 200, "[echo one][echo two]");
	flaky_lose = 1000;
	check("as file gone", "as flaky", 3000, "[echo one]{-as: cannot read}");
	flaky_fails = 0;

	/* a file cut short under the running script: it ends there, it does not start over */
	strcpy(shrinks, "echo a\ncut\necho b\necho c\n");
	check("as file cut short", "as shrinks", 20, "[echo a]");

	/* the path is relative to where the script started, also after a `cd` in it */
	reset();
	streams_main.cwd = 5;
	argscript_start("t", "as here");
	run_tasks(10);
	if (strcmp(out, "[echo a][echo b]") != 0) {
		printf("FAIL as relative path: %s\n", out);
		failed++;
	} else {
		printf("ok   as relative path\n");
	}

	/* the long run tasks left over from the last tests must not leak */
	reset();

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);

	return failed != 0;
}
