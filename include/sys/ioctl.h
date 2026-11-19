/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/* <sys/ioctl.h> on LETS: the C library's (where it has one), then what LETS adds (lets/posix.h). On XC16 tools/xc16-install.sh
   copies it into the compiler; on Cortex-M a project puts lets/include in its include path. A program
   includes only standard headers. */
#ifndef LETS_INCLUDE_SYS_IOCTL_H
#define LETS_INCLUDE_SYS_IOCTL_H

#include "../lets/libc.h"
#if (!defined(__XC16__) && !defined(__NEWLIB__))
#include_next <sys/ioctl.h>
#endif

#if !defined(PORT_CORTEX_M_PRELUDE_H) || defined(LETS_PRELUDE_RENAMED)
#include "../lets/posix.h"
#endif

#endif
