/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Kernel host test: scheduler timing, task memory ownership, nodes, command dispatch and stdout
 * redirection. tasks_start() never returns, so the checks run as tasks and the last one exits.
 */
#include "../kernel/task.h"
#include "../kernel/alloc.h"
#include "../bin/shell.h"
#include "../bin/cat.h"
#include "../bin/echo.h"
#include "../bin/kill.h"
#include "../bin/ls.h"
#include "../bin/mv.h"
#include "../bin/tmp.h"
#include "../bin/top.h"
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

/* a terminal: read() returns the keys given to type(), once; writes go to sink */
static const char *keys = "";

static int term_read(void *args, void *buffer, unsigned int length)
{
	(void)args;
	unsigned n = (unsigned)strlen(keys);
	if (n > length)
		n = length;
	memcpy(buffer, keys, n);
	keys += n;
	return (int)n;
}

static fsi_handler_t term_node = {.write_handler = sink_write, .read_handler = term_read};

int shell_loop(void *args, uint64_t system_time); /* bin/shell.c: the shell task */

/* types @p text on the terminal and lets the shell take all of it */
static void type(const char *text)
{
	keys = text;
	while (*keys != '\0')
		shell_loop(NULL, 0);
}

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

		/* `std -t`: a person on a terminal - echo, editing, Tab, history, "\r\n" */
		CHECK(fsi_node_create("term", false, NULL, &term_node) == 0);
		CHECK(system("std -t term") == 0 && fsi_terminal());
		sink_len = 0;
		sink[0] = '\0';
		type("pro\t"); /* the only name starting with "pro" */
		CHECK(strcmp(sink, "> probe ") == 0);
		type("x\r");
		CHECK(last_argc == 2 && strcmp(last_arg, "x") == 0);
		CHECK(strstr(sink, "> probe x\r\n") != NULL);
		sink_len = 0;
		type("/bin/prob\t\x15"); /* a path: the part after the last '/' */
		CHECK(strstr(sink, "/bin/probe ") != NULL);
		CHECK(fsi_register_app("prompt", cmd_probe) == 0);
		sink_len = 0;
		type("si\t\x15"); /* the first word: only what executes, so not sink */
		CHECK(strncmp(sink, "si\r", 3) == 0); /* nothing added */
		sink_len = 0;
		type("probe si\t\x15"); /* an argument: any node */
		CHECK(strstr(sink, "probe sink ") != NULL);
		sink_len = 0;
		type("pro\t"); /* probe, prompt: completes the common "pro" */
		type("\t");    /* nothing more in common: lists both */
		CHECK(strstr(sink, "probe  prompt  \r\n\r> pro") != NULL);
		type("\x15probX\x7f"
		     "e y\r\n"); /* Ctrl-U, typo, Backspace, "\r\n" Enter */
		CHECK(last_argc == 2 && strcmp(last_arg, "y") == 0);
		last_argc = 0;
		type("probe zz\x03"); /* Ctrl-C drops the line */
		CHECK(last_argc == 0);
		type("\x1b[A\r"); /* Up: the last line run again */
		CHECK(last_argc == 2 && strcmp(last_arg, "y") == 0);
		sink_len = 0;
		CHECK(write(1, "two\n", 4) == 4);
		CHECK(strcmp(sink, "two\r\n") == 0);
		CHECK(system("std sink") == 0 && !fsi_terminal());
		sink_len = 0;
		CHECK(write(1, "three\n", 6) == 6 && strcmp(sink, "three\n") == 0);
		step = 2;
		return 0;
	}
	if (step == 2) {
		/* the commands (their messages go to the PC's stdout here) */
		fsi_register_app("cat", cat);
		fsi_register_app("echo", echo);
		fsi_register_app("kill", kill);
		fsi_register_app("ls", ls);
		fsi_register_app("mv", mv);
		fsi_register_app("top", top);
		fsi_register_app("touch", tmp_file_create);
		fsi_register_app("rm", tmp_file_delete);

		CHECK(system("touch f") == 0 && system("touch f") == 0); /* the second: no-op */
		CHECK(system("echo hello  world > f") == 0 && system("echo again >> f") == 0);
		char text[32] = {0};
		int fd = open("f", 0, 0);
		CHECK(read(fd, text, sizeof(text) - 1) == 18 && close(fd) == 0);
		CHECK(strcmp(text, "hello world\nagain\n") == 0);
		CHECK(system("cat f") == 18 && system("cat -x f") == 18);
		CHECK(system("cat") == -1 && system("cat -y f") == -1 && system("cat nothing") == -1);
		CHECK(system("echo a > f b") == -1 && system("echo a > nothing") == -1);

		CHECK(system("mv f g") == 0 && open("f", 0, 0) < 0 && open("g", 0, 0) > 2);
		CHECK(system("mv g sink") == -1);		  /* taken */
		CHECK(system("mv g 0123456789abcdef") == -1); /* too long */
		CHECK(system("mv stdout x") == -1);		  /* the standard streams stay */
		CHECK(system("mv nothing x") == -1);

		CHECK(system("rm sink") == -1 && open("sink", 0, 0) > 2); /* not a RAM file */
		CHECK(system("rm g") == 0 && system("rm g") == -1);

		CHECK(system("ls") > 0 && system("ls -al") > system("ls -l"));
		CHECK(system("ls -x") == -1 && system("ls a") == -1);
		CHECK(system("kill nothing") == -1 && system("kill") == -1);
		CHECK(system("top") == 0 && system("top x") == -1);

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
