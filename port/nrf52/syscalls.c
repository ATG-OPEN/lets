/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * newlib's ends (--specs=nano.specs): what newlib's own printf / puts / putchar / fputs write to
 * stdout or stderr goes where the kernel's write(1) goes, the running task's stdout or else the
 * console set by `std`; only with neither does it go to the board's tty (nrf52_console, set by
 * the board), as the kernel's k_printf. Everything else (open, read, ...) is the kernel's under
 * k_* names (port/cortex-m/prelude.h): these stubs only satisfy newlib.
 */
#include "nrf52.h"
#include "../../vfs/vfs.h"

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

nrf52_tty_t *nrf52_console;

int _write(int fd, const char *buf, int len)
{
	if ((fd == 1 || fd == 2) && len > 0 && vfs_stream(fd) >= 0) {
		return k_write(fd, (void *)buf, (unsigned int)len);
	}
	if (nrf52_console != NULL && len > 0) {
		nrf52_tty_put(nrf52_console, buf, (unsigned int)len);
	}
	return len;
}

int _read(int fd, char *buf, int len)
{
	(void)fd;
	(void)buf;
	(void)len;
	return 0;
}

int _close(int fd)
{
	(void)fd;
	return -1;
}

int _fstat(int fd, struct stat *st)
{
	(void)fd;
	st->st_mode = S_IFCHR;
	return 0;
}

int _isatty(int fd)
{
	(void)fd;
	return 1;
}

int _lseek(int fd, int off, int whence)
{
	(void)fd;
	(void)off;
	(void)whence;
	return 0;
}

int _getpid(void)
{
	return 1;
}

int _kill(int pid, int sig)
{
	(void)pid;
	(void)sig;
	errno = EINVAL;
	return -1;
}

void _exit(int status)
{
	(void)status;
	nrf52_reset();
	for (;;) {
	}
}

/* newlib-nano's printf may ask for a little memory: from the linker script's heap section */
extern char __HeapBase[], __HeapLimit[];

void *_sbrk(ptrdiff_t increment)
{
	static char *brk = __HeapBase;
	char *prev = brk;

	if (brk + increment > __HeapLimit) {
		errno = ENOMEM;
		return (void *)-1;
	}
	brk += increment;
	return prev;
}
