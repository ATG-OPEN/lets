/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* Which C library: XC16's (no fcntl.h, poll.h, sys/...), newlib (Cortex-M: no poll.h, sys/timex.h,
   a dirent.h that is an #error), or a PC's. <stddef.h> is the compiler's: <_ansi.h> tells newlib. */
#ifndef LETS_INCLUDE_LIBC_H
#define LETS_INCLUDE_LIBC_H
#include <stddef.h>
#if !defined(__XC16__) && defined(__arm__) && !defined(__linux__) && !defined(__APPLE__)
#include <_ansi.h>   /* newlib: defines __NEWLIB__ */
#endif
#endif
