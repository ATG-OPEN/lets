/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* <sys/stat.h> on LETS: the C library's (where it has one), then what LETS adds (lets/posix.h). On XC16 tools/xc16-install.sh
   copies it into the compiler; on Cortex-M a project puts lets/include in its include path. A program
   includes only standard headers. */
#ifndef LETS_INCLUDE_SYS_STAT_H
#define LETS_INCLUDE_SYS_STAT_H

#include "../lets/libc.h"
#if defined(__NEWLIB__) || (!defined(__XC16__) && !defined(__NEWLIB__))
#include_next <sys/stat.h>
#endif

#if !(defined(PORT_CORTEX_M_PRELUDE_H) || defined(PORT_HOST_PRELUDE_H)) || defined(LETS_PRELUDE_RENAMED)
#include "../lets/posix.h"
#endif

#endif
