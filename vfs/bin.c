/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The vfs commands. ls, cat, echo, rm, mv and touch replace the kernel's
 * (same use with bare names, plus paths); cd, pwd, mkdir, cp, crc, mount,
 * umount, mkfs and df are new.
 */

#include "vfs.h"
#include "../bin/shell.h"
#include "../bin/tmp.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIN_BUFFER_SIZE 64u  /* bytes moved at a time by cat and cp */
#define BIN_LINE_SIZE	128u /* longest echo line */

/* -------------------------------------------------------------------------- */

static int bin_bad(void)
{
	printf("bad argument\n");
	return -1;
}

/*
 * One line of `ls -l`, as Linux shows the type and the permissions:
 * d directory, - file (also a RAM file or a command), c device, b block
 * device; r / w / x from what the file or node can do.
 */
static void bin_ls_long(const vfs_dirent_t *e)
{
	char mode[5] = "-rw-";

	if (e->kind == VFS_DIR) {
		strcpy(mode, "drwx");
	} else if (e->kind == VFS_NODE) {
		fsi_node_t *n = fsi_nodes_get() + e->node;
		fsi_handler_t *h = n->handlers;
		bool device = (h->read_handler || h->write_handler) && tmp_file_size(n) < 0;

		mode[0] = e->block ? 'b' : device ? 'c' : '-';
		mode[1] = h->read_handler ? 'r' : '-';
		mode[2] = h->write_handler ? 'w' : '-';
		mode[3] = h->execute_handler ? 'x' : '-';
	}

	printf("%s %6lu %s\n", mode, (unsigned long)e->size, e->name);
}

/* ls [-a] [-l] [-p | -F] [path] */
static int bin_ls(void *args, int argc, char *argv[])
{
	bool all = false;
	bool list = false;
	bool slash = false; /* -p / -F: a '/' after a directory */
	const char *path = ".";

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			path = argv[i];
			continue;
		}

		for (const char *c = argv[i] + 1; *c != '\0'; c++) {
			if (*c == 'a') {
				all = true;
			} else if (*c == 'l') {
				list = true;
			} else if (*c == 'p' || *c == 'F') {
				slash = true;
			} else {
				return bin_bad();
			}
		}
	}

	vfs_path_t p;

	if (vfs_resolve(path, &p) < 0 || p.kind == VFS_NONE) {
		printf("not found\n");
		return -1;
	}

	vfs_dirent_t e;
	int count = 0;

	if (p.kind != VFS_DIR) {
		/* one file or node */
		tfs_info_t t;

		memset(&e, 0, sizeof(e));
		strcpy(e.name, p.name);
		e.kind = p.kind;
		e.node = p.node;
		e.block = (p.kind == VFS_NODE && vfs_blk_get(path) != NULL);

		if (p.kind == VFS_FILE && tfs_stat(vfs_mount_get(p.dir), p.name, &t) == 0) {
			e.size = t.size;
		} else if (p.kind == VFS_NODE) {
			int size = tmp_file_size(&fsi_nodes_get()[p.node]);

			if (size > 0) {
				e.size = (uint32_t)size;
			}
		}

		bin_ls_long(&e);
		return 1;
	}

	uint16_t index = 0;

	while (vfs_dir_read(p.dir, &index, &e) == 0) {
		/* commands are hidden by fsi, but /bin is where they are listed */
		if (e.hidden && !all && p.dir != VFS_BIN) {
			continue;
		}

		count++;

		if (list) {
			bin_ls_long(&e);
		} else {
			printf("%s%s%s", count > 1 ? "  " : "", e.name,
			       (slash && e.kind == VFS_DIR) ? "/" : "");
		}
	}

	if (!list && count > 0) {
		printf("\n");
	}

	return count;
}

/* cd [path] */
static int bin_cd(void *args, int argc, char *argv[])
{
	if (argc > 2) {
		return bin_bad();
	}

	/* no argument: home, as on Linux */
	if (vfs_chdir(argc == 2 ? argv[1] : VFS_HOME) < 0 && (argc == 2 || vfs_chdir("/") < 0)) {
		printf("not found\n");
		return -1;
	}

	return 0;
}

static int bin_pwd(void *args, int argc, char *argv[])
{
	char path[VFS_PATH_MAX];

	if (vfs_dir_path(vfs_cwd(), path, sizeof(path)) < 0) {
		return -1;
	}

	printf("%s\n", path);

	return 0;
}

static int bin_mkdir(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return bin_bad();
	}

	return vfs_mkdir(argv[1]) < 0 ? bin_bad() : 0;
}

static int bin_rm(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return bin_bad();
	}

	vfs_path_t p;

	if (vfs_resolve(argv[1], &p) < 0 || p.kind == VFS_NONE) {
		printf("not found\n");
		return -1;
	}

	/* devices, commands and mount points stay; a directory must be empty */
	if (vfs_remove(argv[1]) < 0) {
		printf(p.kind == VFS_DIR ? "not empty\n" : "bad argument\n");
		return -1;
	}

	return 0;
}

static int bin_mv(void *args, int argc, char *argv[])
{
	if (argc != 3) {
		return bin_bad();
	}

	return vfs_rename(argv[1], argv[2]) < 0 ? bin_bad() : 0;
}

/* touch path: an empty file, or a RAM file outside a mounted file system */
static int bin_touch(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return bin_bad();
	}

	int fd = open(argv[1], O_CREAT, 0);

	if (fd < 0) {
		printf("open failed\n");
		return -1;
	}

	return close(fd);
}

/* cat [-x] path: a file to its end; a node with one read, as fsi */
static int bin_cat(void *args, int argc, char *argv[])
{
	bool hex = (argc == 3 && strcmp(argv[1], "-x") == 0);

	if (argc != (hex ? 3 : 2)) {
		printf("usage: cat [-x] file\n");
		return -1;
	}

	int fd = open(argv[argc - 1], 0, 0);

	if (fd < 0) {
		printf("open failed\n");
		return -1;
	}

	char *buffer = malloc(BIN_BUFFER_SIZE);

	if (buffer == NULL) {
		close(fd);
		printf("out of memory\n");
		return -1;
	}

	bool file = (vfs_file_get(fd) != NULL);
	int total = 0;
	char last = '\n';
	int n;

	do {
		n = read(fd, buffer, BIN_BUFFER_SIZE);

		for (int i = 0; hex && i < n; i++) {
			printf("%02X ", (unsigned char)buffer[i]);
		}

		if (!hex && n > 0) {
			write(1, buffer, n);
			last = buffer[n - 1];
		}

		total += (n > 0) ? n : 0;
	} while (file && n > 0);

	if (hex || last != '\n') {
		printf("\n");
	}

	free(buffer);
	close(fd);

	return total;
}

/* echo words [> path | >> path] */
static int bin_echo(void *args, int argc, char *argv[])
{
	int end = argc; /* first word not echoed */
	int flags = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
			if (i + 2 != argc) {
				return bin_bad();
			}

			end = i;
			flags = O_WRONLY | O_CREAT | (argv[i][1] ? O_APPEND : O_TRUNC);
			break;
		}
	}

	char *line = malloc(BIN_LINE_SIZE);

	if (line == NULL) {
		printf("out of memory\n");
		return -1;
	}

	line[0] = '\0';

	for (int i = 1; i < end; i++) {
		if (strlen(line) + strlen(argv[i]) + 2u >= BIN_LINE_SIZE) {
			break;
		}

		strcat(line, argv[i]);

		if (i < end - 1) {
			strcat(line, " ");
		}
	}

	strcat(line, "\n");

	int r = 0;

	if (end == argc) {
		printf("%s", line);
	} else {
		int fd = open(argv[argc - 1], flags, 0);

		if (fd < 0) {
			printf("echo: cannot open file\n");
			r = -1;
		} else {
			r = (write(fd, line, strlen(line)) < 0) ? -1 : 0;
			close(fd);
		}
	}

	free(line);

	return r;
}

/* cp from to: `to` may be a directory */
static int bin_cp(void *args, int argc, char *argv[])
{
	if (argc != 3) {
		return bin_bad();
	}

	char *buffer = malloc(BIN_BUFFER_SIZE + VFS_PATH_MAX);

	if (buffer == NULL) {
		printf("out of memory\n");
		return -1;
	}

	char *to = buffer + BIN_BUFFER_SIZE;
	vfs_path_t from, dest;

	strncpy(to, argv[2], VFS_PATH_MAX - 1);
	to[VFS_PATH_MAX - 1] = '\0';

	/* into a directory: under the same name */
	if (vfs_resolve(argv[1], &from) == 0 && vfs_resolve(argv[2], &dest) == 0 &&
	    dest.kind == VFS_DIR && strlen(to) + strlen(from.name) + 2u < VFS_PATH_MAX) {
		strcat(to, "/");
		strcat(to, from.name);
	}

	int in = open(argv[1], 0, 0);
	int out = (in < 0) ? -1 : open(to, O_WRONLY | O_CREAT | O_TRUNC, 0);
	int total = 0;

	if (in >= 0 && out >= 0) {
		bool file = (vfs_file_get(in) != NULL);
		int n;

		do {
			n = read(in, buffer, BIN_BUFFER_SIZE);

			if (n > 0 && write(out, buffer, n) != n) {
				total = -1;
				break;
			}

			total += (n > 0) ? n : 0;
		} while (file && n > 0);
	} else {
		printf("open failed\n");
		total = -1;
	}

	if (out >= 0) {
		close(out);
	}

	if (in >= 0) {
		close(in);
	}

	free(buffer);

	return (total < 0) ? -1 : 0;
}

/* crc path: size and CRC-32 of a file, checked against the stored one */
static int bin_crc(void *args, int argc, char *argv[])
{
	vfs_path_t p;
	tfs_info_t t;

	if (argc != 2 || vfs_resolve(argv[1], &p) < 0 || p.kind != VFS_FILE ||
	    tfs_stat(vfs_mount_get(p.dir), p.name, &t) < 0) {
		return bin_bad();
	}

	int fd = open(argv[1], 0, 0);
	uint8_t *buffer = malloc(BIN_BUFFER_SIZE);
	uint32_t crc = 0;
	int n = -1;

	while (fd >= 0 && buffer != NULL && (n = read(fd, buffer, BIN_BUFFER_SIZE)) > 0) {
		crc = tfs_crc32(crc, buffer, (unsigned int)n);
	}

	free(buffer);

	if (fd >= 0) {
		close(fd);
	}

	if (n < 0) {
		printf("open failed\n");
		return -1;
	}

	const char *check = !(t.attr & TFS_ATTR_CRC) ? "" : (crc == t.crc) ? " ok" : " BAD";

	printf("%lu %08lX%s\n", (unsigned long)t.size, (unsigned long)crc, check);

	return (*check == ' ' && crc != t.crc) ? -1 : 0;
}

/* -------------------------------------------------------------------------- */

/* df: every mounted file system, its size and free bytes */
static int bin_df(void *args, int argc, char *argv[])
{
	char path[VFS_PATH_MAX];
	int count = 0;

	for (int i = VFS_DIR_FIRST; i < VFS_DIR_FIRST + (int)VFS_DIRS_MAX; i++) {
		tfs_t *fs = vfs_mount_get((uint8_t)i);

		if (fs == NULL || vfs_dir_path((uint8_t)i, path, sizeof(path)) < 0) {
			continue;
		}

		printf("%s\t%lu\t%lu\n", path, (unsigned long)fs->blocks * fs->bsize,
		       (unsigned long)tfs_free(fs));
		count++;
	}

	return count;
}

/* mount [-f] dev path; no arguments: as df */
static int bin_mount(void *args, int argc, char *argv[])
{
	bool format = (argc == 4 && strcmp(argv[1], "-f") == 0);

	if (argc == 1) {
		return bin_df(args, argc, argv);
	}

	if (argc != (format ? 4 : 3)) {
		printf("usage: mount [-f] dev path\n");
		return -1;
	}

	if (vfs_mount(argv[argc - 2], argv[argc - 1], format) < 0) {
		printf("mount failed\n");
		return -1;
	}

	return 0;
}

static int bin_umount(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return bin_bad();
	}

	if (vfs_umount(argv[1]) < 0) {
		printf("umount failed\n");
		return -1;
	}

	return 0;
}

/* mkfs dev [block size] [entries] */
static int bin_mkfs(void *args, int argc, char *argv[])
{
	if (argc < 2 || argc > 4) {
		printf("usage: mkfs dev [block] [entries]\n");
		return -1;
	}

	vfs_blk_t *blk = vfs_blk_get(argv[1]);
	int bsize = (argc > 2) ? atoi(argv[2]) : 0; /* 0: by the device size */
	int entries = (argc > 3) ? atoi(argv[3]) : 0;

	if (blk == NULL || bsize < 0 || entries < 0 || entries > 255) {
		return bin_bad();
	}

	for (int i = VFS_DIR_FIRST; i < VFS_DIR_FIRST + (int)VFS_DIRS_MAX; i++) {
		if (vfs_mount_get((uint8_t)i) != NULL && vfs_mount_get((uint8_t)i)->blk == blk) {
			printf("mounted\n");
			return -1;
		}
	}

	if (tfs_format(blk, (uint16_t)bsize, (uint8_t)entries) < 0) {
		return bin_bad();
	}

	return 0;
}

/* -------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------ */
/* Tab completion that follows the tree                                     */

/* offers every entry of directory @p dir; returns how many matched */
static int bin_names_in(uint8_t dir, bool (*candidate)(const char *, bool))
{
	vfs_dirent_t e;
	uint16_t index = 0;
	int matched = 0;

	while (vfs_dir_read(dir, &index, &e) == 0) {
		/* as ls shows them: hidden nodes only in /bin, where the commands are */
		if (e.hidden && dir != VFS_BIN) {
			continue;
		}

		if (candidate(e.name, e.kind == VFS_DIR)) {
			matched++;
		}
	}

	return matched;
}

/*
 * A word with a '/' completes in the directory it names (`../`, `/dev/t`,
 * `logs/a`). The first word without one is a command. Any other word
 * completes in the working directory, or, when nothing there matches, with
 * the bare names fsi finds from anywhere (`std tt` -> ttyS1).
 */
static void bin_names(const char *word, bool command, bool (*candidate)(const char *, bool))
{
	const char *slash = strrchr(word, '/');

	if (slash == NULL) {
		if (command || bin_names_in(vfs_cwd(), candidate) == 0) {
			shell_node_names(word, command, candidate);
		}
		return;
	}

	char dir[VFS_PATH_MAX];
	size_t length = (size_t)(slash - word);

	if (length == 0) {
		strcpy(dir, "/");
	} else if (length < sizeof(dir)) {
		memcpy(dir, word, length);
		dir[length] = '\0';
	} else {
		return;
	}

	vfs_path_t p;

	if (vfs_resolve(dir, &p) == 0 && p.kind == VFS_DIR) {
		bin_names_in(p.dir, candidate);
	}
}

/* the prompt: the working directory, ~ for the home, as a Linux shell ("~$ ", "/tmp$ ") */
static void bin_prompt(void)
{
	char path[VFS_PATH_MAX];
	size_t home = strlen(VFS_HOME);
	const char *shown = path;

	if (vfs_dir_path(vfs_cwd(), path, sizeof(path)) < 0) {
		strcpy(path, "?");
	}

	if (strncmp(path, VFS_HOME, home) == 0 && (path[home] == '\0' || path[home] == '/')) {
		write(2, "~", 1);
		shown = path + home;
	}

	write(2, (void *)shown, strlen(shown));
	write(2, "$ ", 2);
}

int vfs_bin_initialize(void)
{
	shell_names_set(bin_names);
	shell_prompt_set(bin_prompt);

	vfs_app_set("ls", bin_ls);
	vfs_app_set("cat", bin_cat);
	vfs_app_set("echo", bin_echo);
	vfs_app_set("rm", bin_rm);
	vfs_app_set("mv", bin_mv);
	vfs_app_set("touch", bin_touch);
	vfs_app_set("cd", bin_cd);
	vfs_app_set("pwd", bin_pwd);
	vfs_app_set("mkdir", bin_mkdir);
	vfs_app_set("cp", bin_cp);
	vfs_app_set("crc", bin_crc);
	vfs_app_set("mount", bin_mount);
	vfs_app_set("umount", bin_umount);
	vfs_app_set("mkfs", bin_mkfs);
	vfs_app_set("df", bin_df);

	return 0;
}
