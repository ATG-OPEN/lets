/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The built-in commands: a table in flash, so they take no RAM. system() (vfs/posix.c) finds a
 * command here by its name, after the nodes (vfs.h, vfs_commands).
 */

#include "shell.h"
#include "date.h"
#include "kill.h"
#include "reboot.h"
#include "top.h"
#include "lines.h"

#include "../vfs/vfs.h" /* the command table (vfs_command_t), VFS_HOME, VFS_PATH_MAX */
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
static unsigned int bin_ls_long(const char *name, const struct stat *st, char *line,
				unsigned int max)
{
	char mode[5];

	mode[0] = S_ISDIR(st->st_mode) ? 'd' : S_ISCHR(st->st_mode) ? 'c' : S_ISBLK(st->st_mode) ? 'b' : '-';
	mode[1] = (st->st_mode & S_IRUSR) ? 'r' : '-';
	mode[2] = (st->st_mode & S_IWUSR) ? 'w' : '-';
	mode[3] = (st->st_mode & S_IXUSR) ? 'x' : '-';
	mode[4] = '\0';

	int n = snprintf(line, max, "%s %6lu %s\n", mode, (unsigned long)st->st_size, name);

	return (n < 0) ? 0 : ((unsigned int)n >= max) ? max - 1u : (unsigned int)n;
}

/* @p dir, then '/' unless it ends in one, then @p name, in @p full (VFS_PATH_MAX) */
static char *bin_join(char *full, const char *dir, const char *name)
{
	size_t n = strlen(dir);

	if (n + strlen(name) + 2u > VFS_PATH_MAX) {
		return NULL;
	}

	strcpy(full, dir);

	if (n > 0 && dir[n - 1] != '/') {
		strcat(full, "/");
	}

	return strcat(full, name);
}

/* whether ls shows entry @p e: a hidden one with -a, or in /bin, where the commands are */
static bool bin_shown(const struct dirent *e, bool all, bool bin)
{
	return !e->d_hidden || all || bin;
}

/* whether @p path is /bin: the same directory (st_ino) */
static bool bin_is_bin(const char *path)
{
	struct stat a, b;

	return stat(path, &a) == 0 && stat("/bin", &b) == 0 && a.st_ino == b.st_ino;
}

/*
 * The entries of a directory, a line (ls -l) or a few names at a time
 * (lines.h), so a stream that takes only a few packets at once gets them all.
 * The directory is opened for each piece, and read on from where the last one ended.
 */
typedef struct {
	lines_t lines;
	long at;	/* telldir(): the next entry */
	uint16_t count; /* entries shown so far */
	bool all, list, slash, bin, done, ended;
	char path[];
} bin_ls_t;

static unsigned int bin_ls_next(lines_t *l, char *piece, unsigned int max)
{
	bin_ls_t *s = (bin_ls_t *)l;
	unsigned int n = 0;
	DIR *d = s->done ? NULL : opendir(s->path);
	struct dirent *e;

	if (!s->done && d == NULL) {
		return LINES_LATER; /* no heap now */
	}

	if (d != NULL) {
		seekdir(d, s->at);
	}

	while (!s->done) {
		long before = telldir(d);

		if ((e = readdir(d)) == NULL) {
			s->done = true;
			break;
		}

		if (!bin_shown(e, s->all, s->bin)) {
			continue;
		}

		if (s->list) {
			char full[VFS_PATH_MAX];
			struct stat st;

			s->at = telldir(d);
			s->count++;

			if (bin_join(full, s->path, e->d_name) == NULL || stat(full, &st) < 0) {
				memset(&st, 0, sizeof(st));
			}

			n = bin_ls_long(e->d_name, &st, piece, max);
			closedir(d);
			return n;
		}

		/* "  name/": the next names on one line, as many as the piece takes */
		bool dir = s->slash && e->d_type == DT_DIR;
		unsigned int w = (unsigned int)strlen(e->d_name);
		unsigned int need = (s->count > 0 ? 2u : 0u) + w + dir + 1u;

		if (n + need > max) {
			if (n > 0) {
				s->at = before; /* this one starts the next piece */
				closedir(d);
				return n;
			}

			w = max - 4u; /* a name longer than a piece: cut */
		}

		if (s->count++ > 0) {
			piece[n++] = ' ';
			piece[n++] = ' ';
		}

		memcpy(piece + n, e->d_name, w);
		n += w;

		if (dir) {
			piece[n++] = '/';
		}
	}

	closedir(d);

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

	struct stat st;

	if (stat(path, &st) < 0) {
		printf("not found\n");
		return -1;
	}

	if (!S_ISDIR(st.st_mode)) {
		/* one file or node */
		const char *name = strrchr(path, '/');
		char line[LETS_NAME_MAX + 16];

		bin_ls_long((name != NULL) ? name + 1 : path, &st, line, sizeof(line));
		printf("%s", line);
		return 1;
	}

	size_t length = strlen(path) + 1u;
	bin_ls_t *s = lines_alloc((unsigned int)(sizeof(bin_ls_t) + length), bin_ls_next);

	if (s == NULL) {
		return -1;
	}

	memcpy(s->path, path, length);
	s->all = all;
	s->list = list;
	s->slash = slash;
	s->bin = bin_is_bin(path);

	/* what it returns: how many entries it shows */
	int count = 0;
	DIR *d = opendir(path);
	struct dirent *e;

	while (d != NULL && (e = readdir(d)) != NULL) {
		count += bin_shown(e, all, s->bin);
	}

	closedir(d);

	return (lines_run("ls", &s->lines) < 0) ? -1 : count;
}

/* cd [path] */
static int bin_cd(void *args, int argc, char *argv[])
{
	if (argc > 2) {
		return bin_bad();
	}

	/* no argument: home, as on Linux */
	if (chdir(argc == 2 ? argv[1] : VFS_HOME) < 0 && (argc == 2 || chdir("/") < 0)) {
		printf("not found\n");
		return -1;
	}

	return 0;
}

static int bin_pwd(void *args, int argc, char *argv[])
{
	char path[VFS_PATH_MAX];

	if (getcwd(path, sizeof(path)) == NULL) {
		return -1;
	}

	printf("%s\n", path);

	return 0;
}

/* what the block devices hold back to the memory now */
static int bin_sync(void *args, int argc, char *argv[])
{
	sync();

	return 0;
}

static int bin_mkdir(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return bin_bad();
	}

	return mkdir(argv[1], 0) < 0 ? bin_bad() : 0;
}

static int bin_rm(void *args, int argc, char *argv[])
{
	if (argc != 2) {
		return bin_bad();
	}

	struct stat st;

	if (stat(argv[1], &st) < 0) {
		printf("not found\n");
		return -1;
	}

	/* devices, commands and mount points stay; a directory must be empty */
	if (remove(argv[1]) < 0) {
		printf(S_ISDIR(st.st_mode) ? "not empty\n" : "busy or not a file\n");
		return -1;
	}

	return 0;
}

static int __attribute__((unused)) bin_mv(void *args, int argc, char *argv[]) /* BIN_FILES 0: not in the table */
{
	if (argc != 3) {
		return bin_bad();
	}

	return rename(argv[1], argv[2]) < 0 ? bin_bad() : 0;
}

/* touch path: an empty file, or a RAM file outside a mounted file system */
static int __attribute__((unused)) bin_touch(void *args, int argc, char *argv[]) /* BIN_FILES 0: not in the table */
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
	struct stat st;

	return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
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
	if (!in && !hex && bin_has_end(fd)) {
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

	bool file = bin_has_end(fd);
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
static int __attribute__((unused)) bin_cp(void *args, int argc, char *argv[]) /* BIN_FILES 0: not in the table */
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
	const char *name = strrchr(argv[1], '/');
	struct stat from, dest;

	strncpy(to, argv[2], VFS_PATH_MAX - 1);
	to[VFS_PATH_MAX - 1] = '\0';
	name = (name != NULL) ? name + 1 : argv[1];

	/* into a directory: under the same name */
	if (stat(argv[1], &from) == 0 && stat(argv[2], &dest) == 0 && S_ISDIR(dest.st_mode) &&
	    strlen(to) + strlen(name) + 2u < VFS_PATH_MAX) {
		strcat(to, "/");
		strcat(to, name);
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
		ioctl(out, LETS_IOC_ABORT);
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
	int count = 0;
	int fd = open("/proc/mounts", O_RDONLY, 0);
	off_t size = (fd < 0) ? 0 : lseek(fd, 0, SEEK_END);
	char *text = (size > 0) ? malloc((size_t)size + 1u) : NULL;

	printf("%-10s %-5s %9s %9s %9s  %s\n", "Device", "Type", "Size", "Used", "Avail", "Mounted on");

	if (text != NULL && lseek(fd, 0, SEEK_SET) == 0 && read(fd, text, (unsigned int)size) == size) {
		text[size] = '\0';

		/* "source path type" a line, as Linux's /proc/mounts */
		for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {
			char *source = line;
			char *path = strchr(source, ' ');
			char *type = (path == NULL) ? NULL : strchr(path + 1, ' ');
			struct statvfs v;

			if (type == NULL) {
				continue;
			}

			*path++ = '\0';
			*type++ = '\0';

			/* /proc holds no files of its own, as on Linux */
			if (strcmp(type, "proc") == 0) {
				continue;
			}

			printf("%-10s %-5s ", source, type);

			if (statvfs(path, &v) == 0) {
				printf("%9lu %9lu %9lu", v.f_blocks, v.f_blocks - v.f_bavail, v.f_bavail);
			} else {
				printf("%9s %9s %9s", "-", "-", "-");
			}

			printf("  %s\n", path);
			count++;
		}
	}

	free(text);
	close(fd);

	return count;
}

/* mount [-f] [-t type] source path; no arguments: as df */
static int __attribute__((unused)) bin_mount(void *args, int argc, char *argv[]) /* BIN_FILES 0: not in the table */
{
	const char *type = "tfs";
	unsigned long flags = 0;
	int i = 1;

	if (argc == 1) {
		return bin_df(args, argc, argv);
	}

	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-f") == 0) {
			flags = MS_FORMAT;
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

	if (mount(argv[i], argv[i + 1], type, flags, NULL) < 0) {
		printf("mount failed\n");
		return -1;
	}

	return 0;
}

static int __attribute__((unused)) bin_umount(void *args, int argc, char *argv[]) /* BIN_FILES 0: not in the table */
{
	if (argc != 2) {
		return bin_bad();
	}

	if (umount(argv[1]) < 0) {
		printf("umount failed\n");
		return -1;
	}

	return 0;
}

/* -------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------ */
/* Tab completion that follows the tree                                     */

/* offers every entry of directory @p path; returns how many matched */
static int bin_names_in(const char *path, bool (*candidate)(const char *, bool))
{
	DIR *d = opendir(path);
	bool bin = bin_is_bin(path);
	struct dirent *e;
	int matched = 0;

	while (d != NULL && (e = readdir(d)) != NULL) {
		/* as ls shows them: hidden nodes only in /bin, where the commands are */
		if (bin_shown(e, false, bin) && candidate(e->d_name, e->d_type == DT_DIR)) {
			matched++;
		}
	}

	closedir(d);

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
		if (command || bin_names_in(".", candidate) == 0) {
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

	bin_names_in(dir, candidate);
}

/* the prompt: the working directory, ~ for the home, as a Linux shell ("~$ ", "/tmp$ ") */
static void bin_prompt(void)
{
	char path[VFS_PATH_MAX];
	size_t home = strlen(VFS_HOME);
	const char *shown = path;

	if (getcwd(path, sizeof(path)) == NULL) {
		strcpy(path, "?");
	}

	if (strncmp(path, VFS_HOME, home) == 0 && (path[home] == '\0' || path[home] == '/')) {
		shell_write("~", 1);
		shown = path + home;
	}

	shell_write(shown, strlen(shown));
	shell_write("$ ", 2);
}

/*
 * Small boards (the 64 KB / 8 KB floor) leave out commands they do not use; the full set is the
 * default:
 *   BIN_DATE 0   no `date`, `uptime` and no node date (bin/date.c is not linked; clock_gettime()
 *                and the rest of kernel/clock.c stay for programs)
 *   BIN_TOP 0    no `top` and `kill` (bin/top.c, bin/kill.c and, if nothing else uses it, lines.c)
 *   BIN_FILES 0  no `cp`, `mv`, `df`, `mount`, `umount`, `touch` (cat, ls, rm, mkdir, cd stay)
 */
#ifndef BIN_DATE
#define BIN_DATE 1
#endif
#ifndef BIN_TOP
#define BIN_TOP 1
#endif
#ifndef BIN_FILES
#define BIN_FILES 1
#endif

const vfs_command_t vfs_commands[] = {
	{"cat", bin_cat},	{"cd", bin_cd},
#if BIN_DATE
	{"date", date_execute},	{"uptime", date_uptime_execute},
#endif
#if BIN_TOP
	{"kill", kill_execute},	{"top", top},
#endif
#if BIN_FILES
	{"cp", bin_cp},		{"df", bin_df},
	{"mount", bin_mount},	{"mv", bin_mv},
	{"touch", bin_touch},	{"umount", bin_umount},
#endif
	{"echo", bin_echo},	{"ls", bin_ls},
	{"mkdir", bin_mkdir},
#ifndef SHELL_NO_PASSWORD
	{"passwd", shell_passwd},
#endif
	{"pwd", bin_pwd},
	{"reboot", reboot_execute},	{"rm", bin_rm},
	{"std", vfs_std_execute}, {"sync", bin_sync},
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

#if BIN_DATE
	return date_initialize();
#else
	return 0;
#endif
}
