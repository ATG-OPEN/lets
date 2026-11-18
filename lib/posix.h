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

#endif /* LETS_POSIX_H */
