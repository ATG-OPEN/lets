/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Kernel host test: scheduler timing, task memory ownership, nodes, command dispatch and stdout
 * redirection. tasks_start() never returns, so the checks run as tasks and the last one exits.
 */
#include "../kernel/task.h"
#include "../kernel/alloc.h"
#include "../bin/shell.h"
#include "../fsi/fsi.h"

#include <time.h>

ALLOC_INIT_HEAP(8192);

static int failures;
#define CHECK(c)                                                            \
	do {                                                                \
		if (!(c)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			failures++;                                         \
		}                                                           \
	} while (0)

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* -------------------------------------------------------------------------- */
/* a node that records what is written to it and returns a fixed text when read */

static char sink[256];
static unsigned sink_len;

static int sink_write(void *args, void *buffer, unsigned int length)
{
	(void)args;
	if (sink_len + length < sizeof(sink)) {
		memcpy(sink + sink_len, buffer, length);
		sink_len += length;
		sink[sink_len] = '\0';
	}
	return (int)length;
}

static int sink_read(void *args, void *buffer, unsigned int length)
{
	const char *text = args;
	unsigned n = (unsigned)strlen(text);
	if (n > length)
		n = length;
	memcpy(buffer, text, n);
	return (int)n;
}

static fsi_handler_t sink_node = {.write_handler = sink_write, .read_handler = sink_read};

/* -------------------------------------------------------------------------- */
/* a command */

static int last_argc;
static char last_arg[32];

static int cmd_probe(void *args, int argc, char *argv[])
{
	(void)args;
	last_argc = argc;
	if (argc > 1)
		strncpy(last_arg, argv[1], sizeof(last_arg) - 1);
	return argc == 3 ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* tasks */

static int ticks_fast, ticks_slow;
static uint64_t first_slow, last_slow;

static int fast(void *args, uint64_t t)
{
	ticks_fast++;
	return 0; /* every loop */
}

static int slow(void *args, uint64_t t)
{
	if (ticks_slow == 0)
		first_slow = t;
	last_slow = t;
	ticks_slow++;
	return 20; /* sleep 20 ms */
}

static int ends(void *args, uint64_t t)
{
	return -1; /* stop: its memory is freed */
}

static int checks(void *args, uint64_t t)
{
	static uint64_t start;
	static int step;
	if (start == 0)
		start = t;
	uint64_t ms = (t - start) / 1000;

	if (step == 0 && ms >= 210) {
		/* scheduler: sleeping tasks wake on time, running ones every loop */
		CHECK(ticks_slow >= 9 && ticks_slow <= 12);
		double period = (double)(last_slow - first_slow) / 1000.0 / (ticks_slow - 1);
		CHECK(period >= 19.5 && period <= 23.0);
		CHECK(ticks_fast > ticks_slow * 5);

		/* memory: a task owns its args, a negative return frees them */
		size_t before = alloc_free_size();
		void *mem = malloc(500);
		CHECK(mem != NULL && alloc_free_size() < before);
		CHECK(task_create("ends", ends, mem) != NULL);
		step = 1;
		return 0;
	}
	if (step == 1) {
		CHECK(alloc_free_size() >= 8192 - 2048); /* the 500 bytes came back */
		size_t free_now = alloc_free_size();
		void *a = malloc(100);
		free(a);
		CHECK(alloc_free_size() == free_now);

		/* nodes: open by name, write, read */
		CHECK(fsi_node_create("sink", false, "pong", &sink_node) == 0);
		int fd = open("sink", 0, 0);
		CHECK(fd > 2);
		CHECK(write(fd, "ping", 4) == 4);
		CHECK(strcmp(sink, "ping") == 0);
		char buf[8] = {0};
		CHECK(read(fd, buf, sizeof(buf) - 1) == 4 && strcmp(buf, "pong") == 0);
		CHECK(close(fd) == 0);
		CHECK(open("nothing", 0, 0) < 0);

		/* commands: system() parses the line and dispatches */
		CHECK(fsi_register_app("probe", cmd_probe) == 0);
		CHECK(system("probe alpha \"two words\"") == 0);
		CHECK(last_argc == 3 && strcmp(last_arg, "alpha") == 0);
		CHECK(system("probe") == -1);
		CHECK(system("no_such_command") < 0);

		/* stdout redirection: `std <node>` sends handle 1 to it, line buffered */
		sink_len = 0;
		sink[0] = '\0';
		CHECK(system("std sink") == 0);
		CHECK(write(1, "line one\n", 9) == 9);
		CHECK(strcmp(sink, "line one\n") == 0);
		step = 2;
		return 0;
	}
	if (step == 2) {
		fprintf(stderr, "kernel: %s\n", failures ? "FAILED" : "ok");
		exit(failures != 0);
	}
	return 1;
}

int main(void)
{
	task_initialize(now_us);
	fsi_initialize();
	task_create("fast", fast, NULL);
	task_create("slow", slow, NULL);
	task_create("checks", checks, NULL);
	tasks_start();
	return 1;
}
