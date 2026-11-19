/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Arm Cortex-M build (GCC, newlib-nano): forced in front of every source
 * (-include port/cortex-m/prelude.h), as port/host/prelude.h on a PC.
 *
 * newlib owns the names open / read / write / close / lseek / system / malloc / free, so its
 * headers are read first under their real names and the kernel's are renamed k_*: every
 * program built with this prelude (the kernel, the libraries, the apps) calls the kernel's.
 * newlib's own puts / putchar / fputs end in _write() (port/<chip>/syscalls.c): the running
 * task's stdout, else the console set by `std`, else the board's tty.
 * The kernel sources stay unchanged.
 */
#ifndef PORT_CORTEX_M_PRELUDE_H
#define PORT_CORTEX_M_PRELUDE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#define open   k_open
#define read   k_read
#define write  k_write
#define close  k_close
#define lseek  k_lseek
#define system k_system
#define printf k_printf
#define malloc k_malloc
#define free   k_free
#define reboot k_reboot
#define chdir  k_chdir
#define getcwd k_getcwd
#define mkdir  k_mkdir
#define rename k_rename
#define remove k_remove
#define mount  k_mount
#define umount k_umount
#define sync   k_sync
#define fsync  k_fsync
#define isatty k_isatty
#define kill   k_kill
#define ioctl  k_ioctl
#define poll   k_poll
#define pollfd k_pollfd
#define nfds_t k_nfds_t
#define statvfs k_statvfs
#define clock_gettime k_clock_gettime
#define clock_settime k_clock_settime
#define gettimeofday  k_gettimeofday
#define settimeofday  k_settimeofday
#define adjtime	      k_adjtime
#define adjtimex      k_adjtimex
#define timex	      k_timex
#define stat   k_stat
#define fstat  k_fstat
#define dirent k_dirent
#define DIR    k_DIR
#define opendir	 k_opendir
#define readdir	 k_readdir
#define closedir k_closedir
#define telldir	 k_telldir
#define seekdir	 k_seekdir

/* the kernel's heap has no calloc / realloc */
#define calloc	k_calloc_is_not_in_the_kernel
#define realloc k_realloc_is_not_in_the_kernel

int k_open(const char *pathname, int flags, int mode);
int k_close(int handle);
int k_write(int handle, void *buffer, unsigned int len);
int k_read(int handle, void *buffer, unsigned int len);
off_t k_lseek(int handle, off_t offset, int whence);
int k_system(const char *input);
int k_printf(const char *format, ...);
void *k_malloc(size_t size);
void k_free(void *ptr);

#endif
