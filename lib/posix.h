/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file posix.h
 * @brief The POSIX calls LETS gives a program, beyond the C library's.
 *
 * A program on LETS (a command, a script, an application) uses the C library and these, as on
 * Linux: open / read / write / close / lseek / system / malloc / free, and here the calls for
 * directories, the terminal and the rest. The kernel and the vfs implement them underneath
 * (vfs/posix.c, vfs/vfs.c, kernel/task.c); a program needs none of their headers.
 *
 * XC16's headers declare few of them (no <fcntl.h>, <sys/stat.h>, <dirent.h>), so this header
 * does. On a PC and on Cortex-M the system headers have them too, under the C library's names:
 * the build's prelude (port/host/prelude.h, port/cortex-m/prelude.h) renames the kernel's to
 * k_*, as it does open() and the others.
 */

#ifndef LETS_POSIX_H
#define LETS_POSIX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#if defined(__XC16__)
#define __NEED_mode_t
#define __NEED_ino_t
#define __NEED_dev_t
#include <bits/alltypes.h>
#else
#include <sys/types.h>
#endif

/* open() flags; XC16 has no <fcntl.h>, so the values of its <bits/fcntl.h> */
#ifndef O_WRONLY
#define O_RDONLY 0
#define O_WRONLY 01
#define O_RDWR	 02
#endif
#ifndef O_CREAT
#define O_CREAT	 0400
#define O_EXCL	 02000
#define O_TRUNC	 01000
#define O_APPEND 0010
#endif
#ifndef O_ACCMODE
#define O_ACCMODE 03
#endif

#ifndef VFS_PROC
#define VFS_PROC 1 /**< 1: the kernel's /proc is mounted (vfs/proc.c); 0 leaves it out. */
#endif

#ifndef VFS_FD_TASK
/**
 * 1: what a task opened is closed when it ends or is killed, as a process's files on Linux
 * (vfs.c). 0 (the default): a handle stays open until close(), whoever calls it - code that
 * opens a handle in one task and gives it to another (a command starting a link task on a tty)
 * keeps working; a killed task's handles leak.
 */
#define VFS_FD_TASK 0
#endif

/** mount() flag of LETS: format a device that holds no file system (tfs; `mount -f`). */
#define MS_FORMAT 0x40000000ul

/** @brief Makes @p path the working directory of the running task. @return 0 or -1. */
int chdir(const char *path);

/** @brief The working directory's path in @p buffer (@p size bytes); @p buffer, or NULL. */
char *getcwd(char *buffer, size_t size);

/** @brief Creates a directory (the mode is not kept). @return 0 or -1. */
int mkdir(const char *path, mode_t mode);

/**
 * @brief Mounts @p source (a device, or what the type takes) with the file system type
 * @p type at @p target, as Linux's mount(2); @p target is created when missing. @p flags:
 * MS_FORMAT or 0; @p data is not used. @return 0 or -1.
 */
int mount(const char *source, const char *target, const char *type, unsigned long flags,
	  const void *data);

/** @brief Unmounts the file system at @p target (no file may be open on it). @return 0 or -1. */
int umount(const char *target);

/**
 * @brief Stops the task with ID @p pid (its slot: `top`'s ID, /proc/<ID>); @p sig 0 asks
 * whether it is there, any other stops it (kernel/task.c). pid 0 is a task, not a group.
 * @return 0 or -1.
 */
int kill(pid_t pid, int sig);

#ifndef SIGTERM
#define SIGKILL 9
#define SIGTERM 15
#endif

/** @brief Passes on what stdout holds and makes every block device write back what it holds. */
void sync(void);

/** @brief Passes on what is held for @p fd: stdout's buffered line goes on. @return 0. */
int fsync(int fd);

/**
 * @brief 1 when @p fd (0, 1 or 2) is a terminal - a person typing (`std -t`, `shell -t`): a
 * line ends in "\r\n" there - else 0.
 */
int isatty(int fd);

/** @brief Removes a file, a RAM file or an empty directory (<stdio.h>'s). @return 0 or -1. */
int remove(const char *path);

/** @brief Renames or moves within one file system or within the node table (<stdio.h>'s). @return 0 or -1. */
int rename(const char *from, const char *to);

/* -------------------------------------------------------------------------- */
/* what a path is: stat() */

#ifndef S_IFMT
#define S_IFMT	0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#endif

/**
 * What stat() tells: the type and what can be done (S_IRUSR / S_IWUSR / S_IXUSR from the node's
 * handlers; a file rw-, a directory rwx, a built-in command --x) and the size. A file of a file
 * system and a RAM file are S_IFREG (they end), a node a device: S_IFCHR, S_IFBLK for a block
 * device (vfs_blk_register()).
 */
struct stat {
	mode_t st_mode;
	off_t st_size;
	ino_t st_ino; /**< Which directory or node it is (two paths to one compare equal). */
	dev_t st_dev; /**< A file of a mounted file system: which one (not 0); 0 for a node. */
};

/** @brief What @p path is. @return 0, or -1 when there is nothing there. */
int stat(const char *path, struct stat *st);

/** @brief What the open handle @p fd is (0, 1, 2: the task's streams); no size. @return 0 or -1. */
int fstat(int fd, struct stat *st);

/** ioctl() request of LETS: give up a file opened for writing; the file keeps what it had. */
#define LETS_IOC_ABORT 0x4C01u

/**
 * @brief Device control: @p request LETS_IOC_ABORT gives up @p fd without committing it (a
 * file opened with O_TRUNC keeps its old content; a node's close handler is not called).
 * @return 0, or -1 (another request, or no such handle).
 */
int ioctl(int fd, unsigned long request, ...);

/* -------------------------------------------------------------------------- */
/* directories: opendir(), readdir(), closedir() */

#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_CHR	   2
#define DT_DIR	   4
#define DT_BLK	   6
#define DT_REG	   8
#endif

#define LETS_NAME_MAX 16 /**< A name's bytes in a directory, with its 0. */

/** One entry of a directory, from readdir(). */
struct dirent {
	char d_name[LETS_NAME_MAX];
	unsigned char d_type; /**< DT_DIR, DT_REG, DT_CHR or DT_BLK. */
	/** LETS: a node made hidden (or a file named ".x"); `ls` shows it with -a, and always in /bin. */
	unsigned char d_hidden;
};

typedef struct lets_dir DIR;

/** @brief Opens directory @p path (a heap block, freed by closedir()); NULL when it is none. */
DIR *opendir(const char *path);
/** @brief The next entry, NULL after the last; valid until the next call. */
struct dirent *readdir(DIR *dir);
/** @brief Frees @p dir. @return 0. */
int closedir(DIR *dir);
/** @brief Where readdir() is, for seekdir(): a listing can be closed and opened again there. */
long telldir(DIR *dir);
/** @brief Goes back to where telldir() said. */
void seekdir(DIR *dir, long at);

#endif /* LETS_POSIX_H */
