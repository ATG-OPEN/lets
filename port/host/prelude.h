/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * Host (PC) build: forced in front of every kernel source (-include port/host/prelude.h).
 *
 * On the MCU the kernel *is* the C library's open / read / write / close / lseek / system /
 * malloc / free (XC16 links them from the kernel's .libc.* sections). On a PC those names
 * belong to the system's C library, so the system headers are read first under their real
 * names and the kernel's versions are renamed k_*. The kernel sources stay unchanged.
 */
#ifndef PORT_HOST_PRELUDE_H
#define PORT_HOST_PRELUDE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#define open   k_open
#define read   k_read
#define write  k_write
#define close  k_close
#define lseek  k_lseek
#define system k_system
#define malloc k_malloc
#define free   k_free

int k_open(const char *pathname, int flags, int mode);
int k_close(int handle);
int k_write(int handle, void *buffer, unsigned int len);
int k_read(int handle, void *buffer, unsigned int len);
off_t k_lseek(int handle, off_t offset, int whence);
int k_system(const char *input);
void *k_malloc(size_t size);
void k_free(void *ptr);

/* XC16 puts the replacements in named sections; Mach-O wants "segment,section" names */
#if defined(__APPLE__)
#define __section__(name) __used__
#endif

#endif
