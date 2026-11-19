# XC16 for LETS: standard headers

A program on LETS is written as a POSIX program: it includes only the C library's standard headers
and `<lets-app.h>`, never a kernel or vfs header of LETS and no relative path into the LETS tree:

```c
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lets-app.h>      /* lets_spawn(): the one LETS piece */
```

XC16's C library has no `<fcntl.h>`, `<poll.h>`, `<dirent.h>` and no `<sys/...>` at all, and its
`<unistd.h>` and `<time.h>` do not declare what LETS adds (`chdir`, `sync`, `clock_gettime`, `poll`,
`stat`, `opendir`, ...). `tools/xc16-install.sh` adds them to the compiler, once per installed XC16.

## What it installs

Into `XC16/include/` (copied from this repository's `include/`, which stays the source of truth):

| File | What |
|---|---|
| `fcntl.h`, `poll.h`, `dirent.h` | `open()` flags, `poll()`/`struct pollfd`, `opendir()`/`readdir()` |
| `sys/stat.h`, `sys/statvfs.h`, `sys/time.h`, `sys/timex.h`, `sys/mount.h`, `sys/ioctl.h`, `sys/types.h` | `stat()`, `statvfs()`, `gettimeofday()`/`adjtime()`, `adjtimex()`, `mount()`, `ioctl()`, the types |
| `lets/posix.h` | the declarations themselves (the kernel's `lib/posix.h` includes it too) |
| `lets/libc.h` | which C library is in use (XC16, newlib, a PC) |
| `lets-app.h` | `lets_spawn()` and, for board code, `LETS_COMMAND()` |
| `lets/INSTALLED` | the list of files above (for `--uninstall`) |

Each of the standard-named files includes `lets/posix.h`; where the C library has the header too
(newlib, a PC), it first includes that one (`#include_next`).

XC16's own `unistd.h`, `time.h` and `signal.h` get three lines at their end, between markers:

```c
/* LETS (lets/tools/xc16-install.sh): the POSIX calls LETS adds - begin */
#include <lets/posix.h>
/* LETS (lets/tools/xc16-install.sh) - end */
```

The originals are kept beside them as `unistd.h.lets-orig`, `time.h.lets-orig`, `signal.h.lets-orig`.
The compiler's files may be read-only (root): the script writes a new file in their place, so it needs
write access to the `include/` directory only (`admin` group on macOS; else `sudo`).

**Linker scripts (.gld): nothing.** Checked for the chips the DRN boards use (PIC24FJ1024GA606,
PIC24FJ256GB412, PIC24FJ128GL306): LETS's heap is a static array (`ALLOC_INIT_HEAP`, the link uses
`--heap=0`), the one stack is what the linker leaves (`__SP_init`/`SPLIM` of the stock script), and a
board's `/boot` in program flash is kept free by the board itself (a `noload` array at a fixed address,
`vfs/dev/flash24.c`). So no device script is changed and other chips are not touched.

## Run it

```sh
cd lets
tools/xc16-install.sh               # the newest XC16 (/Applications/microchip/xc16/v* on macOS,
                                    # /opt/microchip/xc16/v* on Linux)
tools/xc16-install.sh 2.10          # a given version
XC16_DIR=/path/xc16/v2.10 tools/xc16-install.sh
tools/xc16-install.sh --check       # 0: the compiler has this LETS's headers; else what differs
make app-check                      # tests/app/posix-app.c compiled for XC16 and Cortex-M
```

It prints every file it copies or patches; a second run changes nothing (only files that differ are
copied again; a patched file is not patched twice). After a LETS update that changes `include/`, run
it again (`--check` says when).

If the directory is not writable it stops and prints the `sudo` command to run.

## Undo it

```sh
tools/xc16-install.sh --uninstall 2.10
```

restores `unistd.h`, `time.h`, `signal.h` from their `.lets-orig` and removes the copied files (a file
the compiler had under one of those names before is restored from its `.lets-orig` too). Checked on
XC16 2.10: after `--uninstall`, `diff -r` against a copy of `include/` made before the first install
shows no difference.

## Cortex-M (newlib) and the PC

newlib has `fcntl.h`, `sys/stat.h`, `time.h`, ... but no `poll.h`, `sys/timex.h`, and a `dirent.h`
that is an `#error`. A Cortex-M project adds `-I lets/include` (and, as before, `-include
port/cortex-m/prelude.h`, which renames the kernel's `open`, `read`, ... to `k_*` and then includes
`lets/posix.h`): the same program compiles unchanged. Host tests keep `port/host/prelude.h`.

## A new program

```c
/* echo.c: answers what comes to /drn/N, a LETS program and a POSIX one */
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <lets-app.h>

static int fd;

static int step(void *ctx)
{
	char b[200];
	int n = read(fd, b, sizeof(b));          /* nothing blocks on LETS: 0 = nothing now */
	struct pollfd p = {fd, POLLIN, 0};

	(void)ctx;
	if (n > 0)
		write(fd, b, n);
	return poll(&p, 1, 1000) ? 0 : 1000;     /* asleep until something comes, at most 1 s */
}

int echo_main(int argc, char *argv[])
{
	if (argc != 2 || (fd = open(argv[1], O_RDWR, 0)) < 0)
		return -1;
	return lets_spawn("echo", step, NULL);   /* the step runs as the task "echo" */
}
```

The board makes it a command (board code may use the kernel's headers):

```c
#include <lets-app.h>
#include "vfs/vfs.h"

LETS_COMMAND(echo_main)
...
vfs_register_app("/opt/echo", LETS_COMMAND_OF(echo_main));
```

Why a step and not a loop in `main()`: LETS tasks are cooperative (one stack), so `main()` returns
after it has started the task; `lets_spawn()` is the one LETS-specific call a program makes.
