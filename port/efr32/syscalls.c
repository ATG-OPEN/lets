/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * newlib's ends (--specs=nano.specs): what newlib's own printf / puts / putchar / fputs write to
 * stdout or stderr goes where the kernel's write(1) goes, the running task's stdout or else the
 * console set by `std`; only with neither does it go to the board's tty (efr32_console, set by
 * the board), as the kernel's k_printf, and only while no handle is open on that tty: once a
 * program has it (a link, `shell < ttyS0`), such text is dropped, as on XC16. Everything else
 * (open, read, ...) is the kernel's under k_* names (port/cortex-m/prelude.h): these stubs only
 * satisfy newlib.
 */
#include "efr32.h"
#include "../../vfs/vfs.h"

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>

efr32_tty_t *efr32_console;

int _write(int fd, const char *buf, int len)
{
	if ((fd == 1 || fd == 2) && len > 0 && vfs_stream(fd) >= 0) {
		return k_write(fd, (void *)buf, (unsigned int)len);
	}
	/* no console: the board's tty, unless a handle has it (a link: text would break its frames) */
	if (efr32_console != NULL && len > 0 && !vfs_device_open(efr32_console)) {
		efr32_tty_put(efr32_console, buf, (unsigned int)len);
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
	efr32_reset();
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
