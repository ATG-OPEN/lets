/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The POSIX line on a PC: what a program sees of the kernel through /proc (VFS_PROC 1) or
 * through vfs_proc_line() (VFS_PROC 0, built a second time), kill(), reboot() through the port's
 * hook, getcwd() / chdir(), isatty(), and `top` / `top mem` / `kill` as before: the same lines in
 * the same columns.
 */
#include "../kernel/task.h"
#include "../kernel/alloc.h"
#include "../bin/shell.h"
#include "../vfs/vfs.h"
#include "../lib/posix.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ALLOC_INIT_HEAP(8192);

static int failures;
#define CHECK(c)                                                                     \
	do {                                                                         \
		if (!(c)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			failures++;                                                  \
		}                                                                    \
	} while (0)

static uint64_t fake_us = 1000000u;

static uint64_t fake_clock(void)
{
	return fake_us += 37u;
}

/* the console: what is written to it */
static char sink[2048];
static unsigned int sink_len;

static int sink_write(void *args, void *buffer, unsigned int length)
{
	if (length > sizeof(sink) - 1u - sink_len) {
		length = sizeof(sink) - 1u - sink_len;
	}
	memcpy(sink + sink_len, buffer, length);
	sink_len += length;
	sink[sink_len] = '\0';
	return (int)length;
}

static const vfs_handler_t sink_node = {.write_handler = sink_write};

static int worker(void *arg, uint64_t now)
{
	return 50;
}

/* opens a RAM file and keeps it open (VFS_FD_TASK: closed when it is killed) */
static int opener_fd = -1;

static int opener(void *arg, uint64_t now)
{
	if (opener_fd < 0) {
		opener_fd = open("/tmp/held", O_WRONLY | O_CREAT, 0);
	}
	return 1000;
}

static int resets;

/* reboot() ends in the port's reset */
void port_wdt_reset_now(void)
{
	resets++;
	fprintf(stderr, "proc: %s\n", failures ? "FAILED" : "ok");
	exit(failures != 0 || resets != 1);
}

/* the slot of the task called @p name, by its row in /proc (or vfs_proc_line()) */
static int slot_of(const char *name)
{
	char row[96];

	for (int id = 0; id < TASKS_MAX_COUNT; id++) {
#if VFS_PROC
		char path[16];
		snprintf(path, sizeof(path), "/proc/%d", id);
		int fd = open(path, O_RDONLY, 0);
		int n = (fd < 0) ? -1 : read(fd, row, sizeof(row) - 1);
		close(fd);
		row[n < 0 ? 0 : n] = '\0';
#else
		int n = vfs_proc_line(2u + (unsigned int)id, row, sizeof(row));
#endif
		if (n > 0 && strncmp(row + 4, name, strlen(name)) == 0 && row[4 + strlen(name)] == ' ') {
			return id;
		}
	}
	return -1;
}

/* a row of `top`: "%3u %-15s %-5s %5s %7s %7s %7s %5s" without the blanks at its end */
static bool row_ok(const char *row, int slot, const char *name)
{
	char expect[24];

	snprintf(expect, sizeof(expect), "%3d %-15s ", slot, name);

	return strncmp(row, expect, 20) == 0 && strlen(row) > 50 && row[strlen(row) - 1] == '\n' &&
	       row[strlen(row) - 2] != ' ' && (strstr(row, " run ") || strstr(row, " sleep "));
}

static int step;

static int checks(void *arg, uint64_t now)
{
	char buf[256];

	if (step == 0) {
		/* stat and meminfo: top's first two lines */
		int fd = open("/proc/stat", O_RDONLY, 0);
		int n = (fd < 0) ? -1 : read(fd, buf, sizeof(buf));
		close(fd);
#if VFS_PROC
		CHECK(n > 30 && strncmp(buf, "up 0d 00:00:01  tasks 3  loop ", 30) == 0);
		fd = open("/proc/mem", O_RDONLY, 0);
		CHECK(fd >= 0 && read(fd, buf, 5) == 5 && strncmp(buf, "heap ", 5) == 0 && close(fd) == 0);
		CHECK(open("/proc/stat", O_WRONLY, 0) < 0 && open("/proc/99", O_RDONLY, 0) < 0);
		CHECK(remove("/proc/stat") == -1 && umount("/proc") == 0 && mount("proc", "/proc", "proc", 0, NULL) == 0);
#else
		CHECK(n == -1); /* no /proc */
#endif
		CHECK(vfs_proc_line(0, buf, sizeof(buf)) > 0 && strncmp(buf, "up 0d 00:00:01  tasks 3  loop ", 30) == 0);
		CHECK(strstr(buf, "  reset -\n") != NULL);
		CHECK(vfs_proc_line(1, buf, sizeof(buf)) > 0 && strncmp(buf, "heap ", 5) == 0 &&
		      strstr(buf, " of 8192 B used (") != NULL);
		CHECK(vfs_proc_line(2u + TASKS_MAX_COUNT, buf, sizeof(buf)) == -1);

		/* a task's row, and kill() by its ID */
		int w = slot_of("worker");
		CHECK(w >= 0 && vfs_proc_line(2u + (unsigned int)w, buf, sizeof(buf)) > 0 &&
		      row_ok(buf, w, "worker"));
		CHECK(kill(w, 0) == 0 && kill(w, SIGTERM) == 0 && kill(w, 0) == -1 && kill(w, SIGTERM) == -1);
		CHECK(slot_of("worker") == -1 && vfs_proc_line(2u + (unsigned int)w, buf, sizeof(buf)) == 0);
		CHECK(kill(-1, 0) == -1 && kill(TASKS_MAX_COUNT, 0) == -1);
#if VFS_PROC
		snprintf(buf, sizeof(buf), "/proc/%d", w);
		CHECK(open(buf, O_RDONLY, 0) < 0 && open("/proc/x1", O_RDONLY, 0) < 0); /* a free slot */
#endif

		/* a task that ends leaves its handles open, or (VFS_FD_TASK) they are closed with it */
		task_create("opener", worker, NULL);
		CHECK(kill(slot_of("opener"), SIGTERM) == 0);
		task_create("opener", opener, NULL);
		step = -1;
		return 1;
	}

	if (step == -1) {
		if (opener_fd < 0) {
			return 1;
		}
		CHECK(remove("/tmp/held") == -1); /* open: busy */
		CHECK(kill(slot_of("opener"), SIGTERM) == 0);
#if VFS_FD_TASK
		CHECK(remove("/tmp/held") == 0);
#else
		CHECK(remove("/tmp/held") == -1 && close(opener_fd) == 0 && remove("/tmp/held") == 0);
#endif

		/* the command: by name, through the rows */
		task_create("victim", worker, NULL);
		CHECK(slot_of("victim") >= 0 && system("kill victim") == 0 && slot_of("victim") == -1);
		CHECK(system("kill victim") == -1 && system("kill") == -1);

		/* getcwd, chdir, isatty */
		CHECK(chdir("/tmp") == 0 && getcwd(buf, sizeof(buf)) == buf && strcmp(buf, "/tmp") == 0);
		CHECK(getcwd(buf, 4) == NULL && chdir("/nowhere") == -1 && chdir("/") == 0);
		CHECK(mkdir("/tmp/d", 0) == 0 && rename("/tmp/d", "/tmp/e") == 0 && remove("/tmp/e") == 0);
		CHECK(isatty(0) == 0 && isatty(1) == 0 && isatty(7) == 0);

		/* stat, fstat, opendir / readdir / telldir / seekdir, ioctl */
		struct stat st, st2;
		CHECK(stat("/", &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & S_IXUSR));
		CHECK(stat("/bin/ls", &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0700) == S_IXUSR);
		CHECK(stat("/dev/sink", &st) == 0 && S_ISCHR(st.st_mode) && (st.st_mode & 0700) == S_IWUSR);
		CHECK(stat("sink", &st2) == 0 && st2.st_ino == st.st_ino && st.st_dev == 0);
		CHECK(stat("/nothing", &st) == -1 && stat("/bin/nothing", &st) == -1);
		int f = open("/tmp/r", O_WRONLY | O_CREAT, 0);
		CHECK(f >= 0 && write(f, "abc", 3) == 3 && fstat(f, &st) == 0 && S_ISREG(st.st_mode));
		CHECK(close(f) == 0 && stat("/tmp/r", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 3);
		CHECK(fstat(1, &st) == 0 && S_ISCHR(st.st_mode) && fstat(99, &st) == -1);
		CHECK(ioctl(99, LETS_IOC_ABORT) == -1 && remove("/tmp/r") == 0);
		DIR *d = opendir("/");
		struct dirent *e;
		int entries = 0, dirs = 0;
		long second = 0;
		while (d != NULL && (e = readdir(d)) != NULL) {
			entries++;
			dirs += (e->d_type == DT_DIR);
			if (entries == 1) {
				second = telldir(d);
			}
		}
		CHECK(d != NULL && entries == dirs && entries >= 5);
		seekdir(d, second);
		CHECK((e = readdir(d)) != NULL && strcmp(e->d_name, "dev") == 0 && closedir(d) == 0);
		CHECK(opendir("/bin/ls") == NULL && opendir("/nothing") == NULL);
		d = opendir("/bin");
		bool hidden = false;
		while (d != NULL && (e = readdir(d)) != NULL) {
			hidden |= (strcmp(e->d_name, "ls") == 0 && e->d_hidden && e->d_type == DT_REG);
		}
		CHECK(hidden && closedir(d) == 0);
		CHECK(system("std -t sink") == 0 && isatty(1) == 1 && isatty(3) == 0);

		/* top: in a task, line by line */
		sink_len = 0;
		CHECK(system("top") == 0);
		step = 1;
		return 1;
	}

	if (step == 1) {
		if (slot_of("top") >= 0) {
			return 1; /* still writing */
		}

		/* "up ...\r\n" "heap ...\r\n" header, a row a task (shell, checks, top) */
		const char *p = sink;
		int lines = 0;

		CHECK(strncmp(p, "up 0d ", 6) == 0);
		while ((p = strstr(p, "\r\n")) != NULL) {
			p += 2;
			lines++;
		}
		CHECK(lines == 6);
		CHECK(strstr(sink, "\r\nheap ") != NULL);
		CHECK(strstr(sink, "\r\n ID NAME            STATE   RAM    LAST     MAX  PERIOD   CPU\r\n") != NULL);
		CHECK(strstr(sink, "\r\n  0 shell           ") != NULL && strstr(sink, " top             run ") != NULL);

		sink_len = 0;
		CHECK(system("top mem") == 0 && strncmp(sink, "heap 8192 B used ", 17) == 0 &&
		      strstr(sink, "\r\nblocks ") != NULL && strstr(sink, "\r\nreset 0000\r\n") != NULL);
		CHECK(system("top x") == -1);

		/* reboot: the command syncs, waits for the ttys, resets through the port's hook */
		CHECK(system("reboot") == 0 && resets == 0);
		step = 2;
		return 1;
	}

	if (fake_us > 60000000u) {
		CHECK(resets == 1); /* reboot never came */
		fprintf(stderr, "proc: FAILED\n");
		exit(1);
	}

	return 1;
}

int main(void)
{
	task_initialize(fake_clock);
	CHECK(vfs_initialize() == 0);
	CHECK(vfs_node_create("sink", false, NULL, &sink_node) >= 0);
	shell_initialize();
	vfs_bin_initialize();
	CHECK(system("std sink") == 0);
	task_create("checks", checks, NULL);
	task_create("worker", worker, NULL);
	tasks_start();
	return 1;
}
