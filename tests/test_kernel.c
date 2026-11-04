/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Kernel host test: scheduler timing, task memory ownership, nodes, command dispatch and stdout
 * redirection. tasks_start() never returns, so the checks run as tasks and the last one exits.
 */
#include "../kernel/task.h"
#include "../kernel/alloc.h"
#include "../bin/shell.h"
#include "../bin/kill.h"
#include "../bin/tmp.h"
#include "../bin/top.h"
#include "../vfs/vfs.h"

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

static char sink[2048];
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

static vfs_handler_t sink_node = {.write_handler = sink_write, .read_handler = sink_read};

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

static vfs_handler_t term_node = {.write_handler = sink_write, .read_handler = term_read};

/* a port whose queue is full every other write, as a DRN port is: write() returns 0 */
static char narrow[2048];
static int ls_shown;
static unsigned narrow_len;
static unsigned narrow_writes;

static int narrow_write(void *args, void *buffer, unsigned int length)
{
	(void)args;
	if (narrow_writes++ % 2 == 0 || narrow_len + length >= sizeof(narrow))
		return 0;
	memcpy(narrow + narrow_len, buffer, length);
	narrow_len += length;
	narrow[narrow_len] = '\0';
	return (int)length;
}

static vfs_handler_t narrow_node = {.write_handler = narrow_write};

static unsigned count_of(const char *text, const char *what)
{
	unsigned n = 0;
	for (const char *p = strstr(text, what); p != NULL; p = strstr(p + 1, what))
		n++;
	return n;
}

int shell_loop(void *args, uint64_t system_time); /* bin/shell.c: the shell task */

/* types @p text on the terminal and lets the shell take all of it */
static void type(const char *text)
{
	keys = text;
	while (*keys != '\0')
		shell_loop(NULL, 0);
}

/* -------------------------------------------------------------------------- */
/* a command that leaves a task behind: it goes on on the command's stdout */

static int hold_task(void *arg, uint64_t now)
{
	(void)arg;
	(void)now;
	write(1, "from the task\n", 14);
	return -1;
}

static int cmd_hold(void *args, int argc, char *argv[])
{
	return (task_create("hold", hold_task, NULL) == NULL) ? -1 : 0;
}

static void file_is(const char *path, const char *text)
{
	char got[64] = {0};
	int fd = open(path, 0, 0);

	CHECK(fd > 2);
	read(fd, got, sizeof(got) - 1);
	close(fd);
	CHECK(strcmp(got, text) == 0);
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

static uint8_t where_cwd = 255;

static int where(void *args, uint64_t now)
{
	where_cwd = vfs_cwd();
	return -1;
}

static bool selfstop_done;

static int selfstop(void *args, uint64_t now)
{
	char *memory = task_alloc(32);

	strcpy(memory, "alive");
	CHECK(task_stop_byname("selfstop") == 0 && task_stopping());
	CHECK(task_stop_byname("selfstop") == -1);
	CHECK(strcmp(memory, "alive") == 0); /* nothing freed yet */
	selfstop_done = true;
	return 0;
}

static int beats;
static uint64_t beat_first, beat_last;

static int beat(void *args, uint64_t now)
{
	/* it takes 1 ms of the 5 */
	uint64_t start = now_us();

	while (now_us() - start < 1000) {
	}

	if (beats++ == 0) {
		beat_first = now;
	}

	if (beats == 11) {
		beat_last = now;
	}

	return 5;
}

static int checks(void *args, uint64_t t)
{
	static uint64_t start;
	static int step;
	static int step_waits;
	static size_t heap;
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
		CHECK(vfs_node_create("sink", false, "pong", &sink_node) >= 0);
		int fd = open("sink", O_RDWR, 0);
		CHECK(fd > 2);
		CHECK(write(fd, "ping", 4) == 4);
		CHECK(strcmp(sink, "ping") == 0);
		char buf[8] = {0};
		CHECK(read(fd, buf, sizeof(buf) - 1) == 4 && strcmp(buf, "pong") == 0);
		CHECK(close(fd) == 0);
		CHECK(open("nothing", 0, 0) < 0);

		/* commands: system() parses the line and dispatches */
		CHECK(vfs_register_app("probe", cmd_probe) == 0);
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
		CHECK(vfs_node_create("term", false, NULL, &term_node) >= 0);
		CHECK(system("std -t term") == 0 && vfs_terminal());
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
		CHECK(vfs_register_app("prompt", cmd_probe) == 0);
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
		CHECK(system("std sink") == 0 && !vfs_terminal());
		sink_len = 0;
		CHECK(write(1, "three\n", 6) == 6 && strcmp(sink, "three\n") == 0);
		step = 2;
		return 0;
	}
	if (step == 2) {
		/* the commands (their messages go to the PC's stdout here) */
		vfs_register_app("kill", kill);
		vfs_register_app("top", top);
		vfs_bin_initialize(); /* ls, cat, echo, mv, touch, rm, ... */

		CHECK(system("touch f") == 0 && system("touch f") == 0); /* the second: no-op */
		CHECK(system("echo hello  world > f") == 0 && system("echo again >> f") == 0);
		char text[32] = {0};
		int fd = open("f", 0, 0);
		CHECK(read(fd, text, sizeof(text) - 1) == 18 && close(fd) == 0);
		CHECK(strcmp(text, "hello world\nagain\n") == 0);
		CHECK(system("cat f") == 18 && system("cat -x f") == 18);
		CHECK(system("cat") == -1 && system("cat -y f") == -1 && system("cat nothing") == -1);
		CHECK(system("echo a > new") == 0 && system("rm new") == 0); /* > creates a RAM file */

		/* `<`, `>` and `>>` belong to system(), the command never sees them */
		CHECK(system("echo a > f b") == 0);
		file_is("f", "a b\n"); /* `>` replaces a RAM file too; words go on after it */
		CHECK(system("> f echo first") == 0);
		file_is("f", "first\n");
		CHECK(system("echo no blanks>f") == 0 && system("echo more>>f") == 0);
		file_is("f", "no blanks\nmore\n");
		CHECK(system("echo \"a > b\" > f") == 0);
		file_is("f", "a > b\n"); /* quoted: text */
		CHECK(system("echo \">\" > \"f\"") == 0);
		file_is("f", ">\n");
		CHECK(system("echo x > f > h") == 0);
		file_is("f", "");
		file_is("h", "x\n");
		CHECK(system("cat < h > f") == 2);
		file_is("f", "x\n");
		CHECK(system("cat -x < h") == 2);
		CHECK(system("echo a >") == -1 && system("cat <") == -1 && system("cat < nothing") == -1);
		CHECK(system("rm h") == 0);

		task_streams_t *streams = task_streams();

		CHECK(streams->in == -1 && streams->out == -1 && streams->terminal == -1);

		/* a file is not a terminal, whatever the console is */
		CHECK(system("std -t term") == 0 && system("echo plain > f") == 0 && vfs_terminal());
		file_is("f", "plain\n");
		CHECK(system("std sink") == 0);

		CHECK(system("mv f g") == 0 && open("f", 0, 0) < 0 && (fd = open("g", 0, 0)) > 2 && close(fd) == 0);
		CHECK(system("mv g sink") == -1);		  /* taken */
		CHECK(system("mv g 0123456789abcdef") == -1); /* too long */
		CHECK(system("mv stdout x") == -1);		  /* the standard streams stay */
		CHECK(system("mv nothing x") == -1);

		CHECK(system("rm sink") == -1 && (fd = open("sink", 0, 0)) > 2 && close(fd) == 0); /* not a RAM file */
		CHECK(system("rm g") == 0 && system("rm g") == -1);

		CHECK(system("ls") > 0 && system("ls -l /bin") > 0 && system("ls -a /dev") >= system("ls /dev"));
		CHECK(system("ls -x") == -1 && system("ls a") == -1);
		CHECK(system("kill nothing") == -1 && system("kill") == -1);
		CHECK(system("top") == 0 && system("top x") == -1);

		/* a task started under a redirect stays on it */
		CHECK(vfs_register_app("hold", cmd_hold) == 0);
		CHECK(system("hold > log") == 0);
		step = 3;
		return 0;
	}
	if (step == 3) {
		file_is("log", "from the task\n");

		/* `shell`: its own task on its own streams; the console stays where it is */
		shell_initialize();
		task_stop_byname("shell");
		CHECK(system("shell") == -1 && system("shell -x < term") == -1);
		sink_len = 0;
		sink[0] = '\0';
		keys = "echo mine\r";
		CHECK(system("shell -t < term > term") == 0);
		step = 4;
		return 0;
	}
	if (step == 4) {
		if (strstr(sink, "mine\r\nmine\r\n") == NULL && step_waits++ < 1000) {
			return 1;
		}

		CHECK(strstr(sink, "echo mine\r\nmine\r\n") != NULL); /* an echo, then "\r\n" */

		/* a redirect from inside it, and it stops itself */
		keys = "echo in file > f\rkill shell\r";
		step = 5;
		return 10;
	}
	if (step == 5) {
		if (*keys != '\0') {
			return 1;
		}

		file_is("f", "in file\n");
		CHECK(task_stop_byname("shell") == -1);

		/* a shell gives back all it had, history too */
		heap = alloc_free_size();
		keys = "";
		CHECK(system("shell -t < term > term") == 0);
		step = 6;
		return 30;
	}
	if (step == 6) {
		CHECK(alloc_free_size() < heap); /* it made its history */
		CHECK(system("kill shell") == 0 && alloc_free_size() == heap);

		/* on a link, lines: two in one read, one in two reads, one with no end */
		sink_len = 0;
		sink[0] = '\0';
		keys = "echo one\necho two\necho th";
		CHECK(system("shell < term > term") == 0);
		step = 7;
		return 30;
	}
	if (step == 7) {
		CHECK(strcmp(sink, "one\ntwo\n") == 0);
		keys = "ree\necho no end";
		step = 8;
		return 30;
	}
	if (step == 8) {
		if (strstr(sink, "no end") == NULL && step_waits++ < 2000) {
			return 1;
		}

		CHECK(strcmp(sink, "one\ntwo\nthree\nno end\n") == 0);
		CHECK(system("kill shell") == 0);

		/* each task has its own working directory, and gets its creator's */
		CHECK(system("cd /tmp") == 0 && vfs_cwd() != VFS_ROOT);
		task_create("where", where, NULL);
		CHECK(system("cd /") == 0 && vfs_cwd() == VFS_ROOT);

		/* a task that stops itself keeps its memory until it returns */
		task_create("selfstop", selfstop, NULL);
		task_create("beat", beat, NULL);
		step = 9;
		return 1;
	}
	if (step == 9) {
		if ((!selfstop_done || beats < 11) && step_waits++ < 4000) {
			return 1;
		}

		CHECK(selfstop_done && task_stop_byname("selfstop") == -1);
		CHECK(where_cwd != VFS_ROOT && where_cwd != 255);

		/* a period stays one, whatever the task takes: 10 periods of 5 ms */
		CHECK(beat_last - beat_first >= 49000 && beat_last - beat_first < 52000);
		task_stop_byname("beat");

		/* top into a port that is full every other write: every line, once */
		CHECK(vfs_node_create("narrow", false, NULL, &narrow_node) >= 0);
		CHECK(system("top > narrow") == 0);
		step = 10;
		step_waits = 0;
		return 1;
	}
	if (step == 10) {
		/* until top has ended on its own: its task is gone */
		bool running = false;
		task_t *table = task_get_data();
		for (int i = 0; i < TASKS_MAX_COUNT; i++)
			running |= table[i].state != TASK_UNUSED && strcmp(table[i].info.name, "top") == 0;
		if (running && step_waits++ < 1000) {
			return 1;
		}
		CHECK(!running);

		/* "tasks N" in the first line, then memory, the header and N rows */
		const char *at = strstr(narrow, "tasks ");
		unsigned tasks = at ? (unsigned)atoi(at + 6) : 0;

		CHECK(strncmp(narrow, "up ", 3) == 0 && strstr(narrow, "\nheap ") != NULL);
		CHECK(count_of(narrow, " ID NAME") == 1 && count_of(narrow, "up ") == 1);
		CHECK(tasks > 0 && count_of(narrow, "\n") == tasks + 3);
		CHECK(strstr(narrow, " checks ") != NULL && strstr(narrow, " top ") != NULL);

		/* cat of a file into it: a line at a time from a task of its own, every line once, in order */
		/* a RAM file holds 126 bytes: 20 short lines */
		system("rm /tmp/many");
		CHECK(system("touch /tmp/many") == 0);
		for (int i = 0; i < 20; i++) {
			char line[40];
			snprintf(line, sizeof(line), "echo l%02d >> /tmp/many", i);
			CHECK(system(line) == 0);
		}
		narrow_len = 0;
		narrow[0] = '\0';
		CHECK(system("cat /tmp/many > narrow") > 0);
		step = 30;
		step_waits = 0;
		return 1;
	}
	if (step == 30 || step == 31) {
		const char *name = (step == 30) ? "cat" : "ls";
		bool running = false;
		task_t *table = task_get_data();
		for (int i = 0; i < TASKS_MAX_COUNT; i++)
			running |= table[i].state != TASK_UNUSED && strcmp(table[i].info.name, name) == 0;
		if (running && step_waits++ < 1000) {
			return 1;
		}
		CHECK(!running);

		if (step == 30) {
			bool in_order = count_of(narrow, "\n") == 20;
			for (int i = 0; i < 20 && in_order; i++) {
				char line[40];
				snprintf(line, sizeof(line), "l%02d\n", i);
				in_order = strncmp(narrow + i * (int)strlen(line), line, strlen(line)) == 0;
			}
			CHECK(in_order);

			/* ls -l of /bin, a line a command */
			int shown = system("ls /bin");
			narrow_len = 0;
			narrow[0] = '\0';
			CHECK(shown > 4 && system("ls -l /bin > narrow") == shown);
			step = 31;
			step_waits = 0;
			ls_shown = shown;
			return 1;
		}

		CHECK(count_of(narrow, "\n") == (unsigned)ls_shown && strncmp(narrow, "---x", 4) == 0);

		/* typed on a terminal: the shell waits for top, its prompt comes after the last line */
		sink_len = 0;
		sink[0] = '\0';
		keys = "";
		CHECK(system("shell -t < term > term") == 0);
		step = 11;
		step_waits = 0;
		return 30;
	}
	if (step == 11) {
		/* what the shell shows before anything is typed: its prompt */
		static char prompt[16];

		if (prompt[0] == '\0') {
			CHECK(sink_len > 0 && sink_len < sizeof(prompt));
			strcpy(prompt, sink);
			keys = "top\r";
			return 30;
		}

		if ((count_of(sink, prompt) < 2 || strstr(sink, " top ") == NULL) && step_waits++ < 1000) {
			return 1;
		}

		const char *rows = strstr(sink, " ID NAME");

		CHECK(rows != NULL && count_of(sink, prompt) == 2);
		CHECK(rows != NULL && count_of(rows, prompt) == 1); /* none in between */

		size_t end = strlen(sink) - strlen(prompt);
		CHECK(strcmp(sink + end, prompt) == 0 && sink[end - 2] == '\r' && sink[end - 1] == '\n');
		CHECK(system("kill shell") == 0);

		fprintf(stderr, "kernel: %s\n", failures ? "FAILED" : "ok");
		exit(failures != 0);
	}
	return 1;
}

int main(void)
{
	task_initialize(now_us);
	vfs_initialize();
	task_create("fast", fast, NULL);
	task_create("slow", slow, NULL);
	task_create("checks", checks, NULL);
	tasks_start();
	return 1;
}
