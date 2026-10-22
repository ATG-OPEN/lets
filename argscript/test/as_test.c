/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Host test for argscript, runs on the PC with fake nodes and a fake task loop.
 *
 *   cc -std=c99 -Wall -Wextra -include fcntl.h -Dopen=t_open -Dread=t_read \
 *      -Dwrite=t_write -Dclose=t_close -Dsystem=t_system \
 *      as_test.c ../argscript.c -o as_test && ./as_test
 */

#include "../argscript.h"
#include "../../kernel/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------ fake kernel ------------------------------- */

static task_t tasks[TASKS_MAX_COUNT];
static int current = -1;
static uint64_t now = 0; /* ms */

task_t *task_get_data(void)
{
	return tasks;
}

int task_get_current(void)
{
	return current;
}

task_t *task_create(const char *name, int (*entry)(void *arg, uint64_t system_time), void *arg)
{
	for (int i = 0; i < (int)TASKS_MAX_COUNT; i++) {
		if (tasks[i].state == TASK_UNUSED) {
			tasks[i].entry = entry;
			tasks[i].arg = arg;
			tasks[i].state = TASK_RUNNING;
			tasks[i].wake_tick = now;
			strncpy(tasks[i].info.name, name, TASK_NAME_SIZE - 1);
			return &tasks[i];
		}
	}

	return NULL;
}

static void stop(int i)
{
	tasks[i].state = TASK_UNUSED;
	free(tasks[i].arg);
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

			if (r < 0 || t->state == TASK_UNUSED) {
				if (t->state != TASK_UNUSED) {
					stop(current);
				}
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

int t_open(const char *path, int flags, ...)
{
	(void)flags;
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
	return -1;
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
	if (fd == 7)
		src = "set n 3\nwhile $n > 0\necho file $n\nset n $n - 1\nend\n";

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
				stop(i);
				return 0;
			}
		}
		return -1;
	}

	if (strcmp(cmd, "fail") == 0) {
		log_add("[fail]");
		return -1;
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
}

static void check(const char *name, const char *script, uint64_t ms, const char *expect)
{
	reset();
	argscript_start("t", script);
	run_tasks(ms);

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

	check("busy loop yields", "while 1\nend", 3, "");

	check("errors", "set a 1 / 0\necho never", 5, "");
	check("error end", "end", 5, "");
	check("error missing end", "if 0\necho a", 5, "");
	check("error label", "goto nowhere", 5, "");

	/* the long run tasks left over from the last tests must not leak */
	reset();

	printf(failed ? "%d FAILED\n" : "all passed\n", failed);

	return failed != 0;
}
