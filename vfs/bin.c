/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The built-in commands: a table in flash, so they take no RAM.
 */

#include "vfs.h"
#include "../bin/shell.h"
#include "../bin/date.h"
#include "../bin/kill.h"
#include "../bin/tmp.h"
#include "../bin/top.h"
#include "../bin/lines.h"
#include "../kernel/task.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIN_BUFFER_SIZE 64u  /* bytes moved at a time by cat and cp */

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
static unsigned int bin_ls_long(const vfs_dirent_t *e, char *line, unsigned int max)
{
	char mode[5] = "-rw-";

	if (e->kind == VFS_DIR) {
		strcpy(mode, "drwx");
	} else if (e->kind == VFS_NODE && e->node < 0) {
		strcpy(mode, "---x"); /* a built-in command */
	} else if (e->kind == VFS_NODE) {
		vfs_node_t *n = vfs_nodes_get() + e->node;
		const vfs_handler_t *h = n->handlers;
		bool device = (h->read_handler || h->write_handler) && tmp_file_size(n) < 0;

		mode[0] = e->block ? 'b' : device ? 'c' : '-';
		mode[1] = h->read_handler ? 'r' : '-';
		mode[2] = h->write_handler ? 'w' : '-';
		mode[3] = h->execute_handler ? 'x' : '-';
	}

	int n = snprintf(line, max, "%s %6lu %s\n", mode, (unsigned long)e->size, e->name);

	return (n < 0) ? 0 : ((unsigned int)n >= max) ? max - 1u : (unsigned int)n;
}

/*
 * The entries of a directory, a line (ls -l) or a few names at a time
 * (lines.h), so a stream that takes only a few packets at once gets them all.
 */
typedef struct {
	lines_t lines;
	uint16_t index; /* the next entry */
	uint16_t count; /* entries shown so far */
	uint8_t dir;
	bool all, list, slash, done, ended;
} bin_ls_t;

static unsigned int bin_ls_next(lines_t *l, char *piece, unsigned int max)
{
	bin_ls_t *s = (bin_ls_t *)l;
	unsigned int n = 0;
	vfs_dirent_t e;

	while (!s->done) {
		uint16_t before = s->index;

		if (vfs_dir_read(s->dir, &s->index, &e) != 0) {
			s->done = true;
			break;
		}

		/* commands are hidden, but /bin is where they are listed */
		if (e.hidden && !s->all && s->dir != VFS_BIN) {
			continue;
		}

		if (s->list) {
			s->count++;
			return bin_ls_long(&e, piece, max);
		}

		/* "  name/": the next names on one line, as many as the piece takes */
		unsigned int w = (unsigned int)strlen(e.name);
		unsigned int need = (s->count > 0 ? 2u : 0u) + w + (s->slash && e.kind == VFS_DIR) + 1u;

		if (n + need > max) {
			if (n > 0) {
				s->index = before; /* this one starts the next piece */
				return n;
			}

			w = max - 4u; /* a name longer than a piece: cut */
		}

		if (s->count++ > 0) {
			piece[n++] = ' ';
			piece[n++] = ' ';
		}

		memcpy(piece + n, e.name, w);
		n += w;

		if (s->slash && e.kind == VFS_DIR) {
			piece[n++] = '/';
		}
	}

	/* the end of the names' line */
	if (!s->list && s->count > 0 && !s->ended) {
		s->ended = true;
		piece[n++] = '\n';
	}

	return n;
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
		memset(&e, 0, sizeof(e));
		strcpy(e.name, p.name);
		e.kind = p.kind;
		e.node = p.node;

		vfs_info_t t;

		e.block = (p.kind == VFS_NODE && vfs_blk_get(path) != NULL);

		if (vfs_file_info(&p, &t) == 0) {
			e.size = t.size;
		}

		if (p.kind == VFS_NODE) {
			int size = tmp_file_size(&vfs_nodes_get()[p.node]);

			if (size > 0) {
				e.size = (uint32_t)size;
			}
		}

		char line[TFS_NAME_SIZE + 16];

		bin_ls_long(&e, line, sizeof(line));
		printf("%s", line);
		return 1;
	}

	bin_ls_t *s = lines_alloc(sizeof(bin_ls_t), bin_ls_next);

	if (s == NULL) {
		return -1;
	}

	s->dir = p.dir;
	s->all = all;
	s->list = list;
	s->slash = slash;

	/* what it returns: how many entries it shows */
	uint16_t index = 0;

	while (vfs_dir_read(p.dir, &index, &e) == 0) {
		count += !(e.hidden && !all && p.dir != VFS_BIN);
	}

	return (lines_run("ls", &s->lines) < 0) ? -1 : count;
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
		printf(p.kind == VFS_DIR ? "not empty\n" : "busy or not a file\n");
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

/* a file or a RAM file, read to its end; a sensor has no end, it gets one read */
static bool bin_has_end(int fd)
{
	return vfs_file_is(fd) || tmp_file_size(vfs_fd_node(fd)) >= 0;
}

/*
 * cat of a file: a piece at a time, read from the file each time (lines.h),
 * so a stream that takes only a few packets at once gets all of it.
 */
typedef struct {
	lines_t lines;
	uint32_t at;
	uint32_t size;
	uint8_t tries; /* reads that failed in a row (a full heap: open() needs memory) */
	char last;     /* the last byte written: a file without '\n' at its end gets one */
	char path[];
} bin_cat_t;

static unsigned int bin_cat_next(lines_t *l, char *piece, unsigned int max)
{
	bin_cat_t *c = (bin_cat_t *)l;
	int got = 0;

	if (c->at < c->size) {
		int fd = open(c->path, O_RDONLY, 0);

		got = -1;

		if (fd >= 0 && lseek(fd, (off_t)c->at, SEEK_SET) == (off_t)c->at) {
			got = read(fd, piece, max);
		}

		close(fd);

		/* not now (the heap is full): again later, for a while */
		if (got <= 0 && ++c->tries < 200u) {
			return LINES_LATER;
		}
	}

	c->tries = 0;

	if (got <= 0) {
		if (c->last != '\n') {
			c->last = '\n';
			piece[0] = '\n';
			return 1;
		}

		return 0;
	}

	/* a '\n' only at a piece's end */
	const char *nl = memchr(piece, '\n', (size_t)got);

	if (nl != NULL) {
		got = (int)(nl - piece) + 1;
	}

	c->at += (uint32_t)got;
	c->last = piece[got - 1];

	return (unsigned int)got;
}

static int bin_cat_file(const char *path)
{
	int fd = open(path, O_RDONLY, 0);

	if (fd < 0) {
		printf("open failed\n");
		return -1;
	}

	long end = (long)lseek(fd, 0, SEEK_END);

	close(fd);

	size_t length = strlen(path) + 1u;
	bin_cat_t *c = lines_alloc((unsigned int)(sizeof(bin_cat_t) + length), bin_cat_next);

	if (c == NULL) {
		return -1;
	}

	memcpy(c->path, path, length);
	c->last = '\n';
	c->size = (end > 0) ? (uint32_t)end : 0u;

	/* an empty file prints nothing, one with bytes ends in '\n' */
	if (lines_run("cat", &c->lines) < 0) {
		return -1;
	}

	return (end > 0) ? (int)end : 0;
}

/* cat [-x] path: a file to its end; a node with one read. No path: what stdin was sent to */
static int bin_cat(void *args, int argc, char *argv[])
{
	bool hex = (argc > 1 && strcmp(argv[1], "-x") == 0);
	bool in = (argc == (hex ? 2 : 1)) && task_streams()->in >= 0;

	if (argc != (hex ? 3 : 2) && !in) {
		printf("usage: cat [-x] file\n");
		return -1;
	}

	int fd = in ? 0 : open(argv[argc - 1], 0, 0);

	if (fd < 0) {
		printf("open failed\n");
		return -1;
	}

	/* a file (not a device, not stdin) as pieces that are never lost */
	if (!in && !hex && bin_has_end(vfs_stream(fd))) {
		close(fd);
		return bin_cat_file(argv[argc - 1]);
	}

	char *buffer = malloc(BIN_BUFFER_SIZE);

	if (buffer == NULL) {
		if (!in) {
			close(fd);
		}

		printf("out of memory\n");
		return -1;
	}

	bool file = bin_has_end(vfs_stream(fd));
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

	if (!in) {
		close(fd);
	}

	return total;
}

/* echo words */
static int bin_echo(void *args, int argc, char *argv[])
{
	for (int i = 1; i < argc; i++) {
		printf("%s%s", argv[i], (i < argc - 1) ? " " : "");
	}

	return (printf("\n") < 0) ? -1 : 0;
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
		bool file = bin_has_end(in);
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

	/* a copy that failed must not replace what was there */
	if (out >= 0 && total < 0) {
		vfs_file_abort(out);
	} else if (out >= 0) {
		close(out);
	}

	if (in >= 0) {
		close(in);
	}

	free(buffer);

	return (total < 0) ? -1 : 0;
}

/* -------------------------------------------------------------------------- */

/* df: every mounted file system: what is mounted, size, used and free bytes, where */
static int bin_df(void *args, int argc, char *argv[])
{
	char path[VFS_PATH_MAX];
	int count = 0;

	printf("%-10s %-5s %9s %9s %9s  %s\n", "Device", "Type", "Size", "Used", "Avail", "Mounted on");

	for (int i = VFS_DIR_FIRST; i < VFS_DIR_FIRST + (int)VFS_DIRS_MAX; i++) {
		const vfs_fs_type_t *type;
		void *fs = vfs_mount_get((uint8_t)i, &type);
		uint32_t size, avail;

		if (fs == NULL || vfs_dir_path((uint8_t)i, path, sizeof(path)) < 0) {
			continue;
		}

		printf("%-10s %-5s ", type->source(fs), type->name);

		if (type->usage != NULL && type->usage(fs, &size, &avail) == 0) {
			printf("%9lu %9lu %9lu", (unsigned long)size, (unsigned long)(size - avail),
			       (unsigned long)avail);
		} else {
			printf("%9s %9s %9s", "-", "-", "-");
		}

		printf("  %s\n", path);
		count++;
	}

	return count;
}

/* mount [-f] [-t type] source path; no arguments: as df */
static int bin_mount(void *args, int argc, char *argv[])
{
	const char *type = "tfs";
	bool format = false;
	int i = 1;

	if (argc == 1) {
		return bin_df(args, argc, argv);
	}

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-f") == 0) {
			format = true;
		} else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			type = argv[++i];
		} else {
			break;
		}
	}

	if (argc - i != 2) {
		printf("usage: mount [-f] [-t type] source path\n");
		return -1;
	}

	if (vfs_mount_type(type, argv[i], argv[i + 1], format) < 0) {
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
 * the bare names found from anywhere (`std tt` -> ttyS1).
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
		shell_write("~", 1);
		shown = path + home;
	}

	shell_write(shown, strlen(shown));
	shell_write("$ ", 2);
}

const vfs_command_t vfs_commands[] = {
	{"cat", bin_cat},	{"cd", bin_cd},
	{"cp", bin_cp},		{"date", date_execute},
	{"df", bin_df},		{"echo", bin_echo},
	{"kill", kill},		{"ls", bin_ls},
	{"mkdir", bin_mkdir},	{"mount", bin_mount},
	{"mv", bin_mv},		{"pwd", bin_pwd},
	{"rm", bin_rm},		{"std", vfs_std_execute},
	{"top", top},		{"touch", bin_touch},
	{"umount", bin_umount}, {"uptime", date_uptime_execute},
#ifndef VFS_NO_REDIRECT
	{"shell", shell_start},
#endif
#ifndef VFS_NO_TFS
	{"crc", vfs_tfs_crc},	{"mkfs", vfs_tfs_mkfs},
#endif
};

const unsigned int vfs_commands_count = sizeof(vfs_commands) / sizeof(vfs_commands[0]);

int vfs_bin_initialize(void)
{
	shell_names_set(bin_names);
	shell_prompt_set(bin_prompt);

	return date_initialize();
}
