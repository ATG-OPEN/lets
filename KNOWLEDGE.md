---
id: lets
kind: library
title: LETS
summary: LETS (Linux-like Embedded Task Scheduler) - a small cooperative kernel for 16-bit MCUs where devices, files and commands are nodes reached with open/read/write/close/system, with a shell and a script language.
repo: github.com/ATG-OPEN/lets
path: ~/Firmware/lets
ports: []
runs_on: []
languages: [c]
tags: [kernel, mcu, shell, scheduler, script]
status: active
updated: 2026-11-17
---

# LETS

A small, Linux-like kernel in C for PIC24 / dsPIC33 (XC16), also ported to Cortex-M (EFR32,
nRF52) and to a PC host build. Hardware and programs alike are nodes in one virtual file system
(`/dev`, `/bin`, `/opt`, `/tmp`, `/mnt`) reached with `open` / `read` / `write` / `close` /
`system`. Tasks are cooperative and run to completion (no per-task stacks, no locks); a built-in
shell and the *argscript* language let a board be configured, tested and driven over a serial
line without reflashing. The floor: it must fit in 64 KB of program memory and 8 KB of RAM
(PIC24FJ64GP202 reference build). Public, Apache 2.0.

## Depends on

Nothing but a C toolchain: XC16 on PIC24/dsPIC33, arm-none-eabi gcc on Cortex-M, a host cc for
the PC build and tests (Doxygen for `make docs`, clang-format for `make format`).

## Used by

## Interfaces

- **Boot:** `task_initialize(system_time_get)`, `vfs_initialize()`, board nodes,
  `shell_initialize()`, `vfs_bin_initialize()`, `vfs_interpreter_set(argscript_execute)`,
  `system("/boot/rc")`, `tasks_start()`; heap `ALLOC_INIT_HEAP(size)`.
- **Tasks** (`kernel/task.h`): `task_create(name, fn, args)`; `fn` returns ms to sleep (`> 0`,
  max 32767), `0` run again, `< 0` stop (owned memory freed); `task_alloc()`, `task_stopping()`,
  `shell_wait(task)`.
- **Watchdog** (`-DLETS_WDT=1`, off by default): the scheduler kicks the hardware watchdog once
  per loop only while every watched task checked in: `task_watch(t|NULL, ms)` (0 = not watched,
  the default), `task_alive()`; `task_trap(code)`; after a reset `task_reset_reason()` (enum) and
  `task_fault()` (late task / trap code + task, kept in no-init RAM), shown by `top` and `top mem`.
  Port hooks `port_wdt_start(ms)`, `port_wdt_kick()`, `port_wdt_reset_cause()`: PIC24 in
  `task.c` (weak; also overrides MCC's weak `TRAPS_halt_on_error`), EFR32 WDOG0 in `port/efr32`.
- **Nodes** (`vfs/vfs.h`): `vfs_node_create(name, hidden, args, &handlers)`,
  `vfs_register_app("/opt/name", execute)`, `vfs_blk_register()`; `vfs_peer_new()` - a driver
  signals a new peer on an input (the shell then asks its password again).
- **C library from vfs:** `open`, `read`, `write`, `close`, `lseek`, `system`, `malloc`, `free`.
- **Shell:** `std [-t] NODE` moves the shell (terminal or link mode), `;` separates commands,
  redirection `<` `>`; commands `ls cd cat echo top kill mv touch rm cp mount df date uptime passwd
  reboot ...`; password asked once per input stream, again after `vfs_peer_new()`; default
  `123456` (`SHELL_PASSWORD_DEFAULT`), `passwd WORD` / `passwd -` in `/boot/rc`.
  **Link mode prints the prompt (`<cwd>$ `) after every command** (since 8131f4a): programs on the
  other end use it as "command done".
- **Optional nodes** (`opt/`): `gpa` (GPIO/ADC/DAC channel as a node; one native float per read or
  write, in physical units), `tty` (non-blocking PIC24F UART, RS485 direction pin).
- **Libraries** (`lib/`): `args` (argv, dotted paths such as `1.2.5`), `byte8` (framing + CRC16, up to
  250 data bytes), `ddata`, `pidlib`, `queue`, `ema_filter`, `medianfilter`, `uuid`.
- **Build flags:** `TASKS_MAX_COUNT` (16), `VFS_NODES_MAX` (24), `MAX_BLOCK_COUNT` (32 heap
  blocks), `VFS_NO_TFS`, `VFS_SYSTEM_STACK` (384 B, XC16), `SHELL_PASSWORD_DEFAULT`, `LETS_WDT` (0),
  `LETS_WDT_MS` (2000; PIC24 uses config bits), `LETS_WDT_NO_TRAPS`.

## Where things are

- `kernel/` - scheduler (`task.c`), heap (`alloc.c`).
- `bin/` - shell and kernel commands (`shell.c`, `top.c`, `kill.c`, `date.c`, `reboot.c`, `tmp.c`, `sys.c`).
- `vfs/` - node table (`node.c`), tree (`vfs.c`), POSIX calls (`posix.c`), file commands (`bin.c`),
  tiny power-loss-safe file system (`tfs.c`, `tfsvfs.c`), `onefs.c`, `ed.c`, memory drivers
  `dev/` (25xx EEPROM, 23xx SRAM, SST25 flash, PIC24FJ flash, RAM disk, partitions).
- `argscript/` - the script language. `lib/` - portable libraries.
- `port/` - `host`, `cortex-m`, `efr32` (ZG23 linker script, flash, syscalls), `nrf52`.
- `docs/` - `kernel.md`, `vfs.md`, `argscript.md`, `porting.md`, `linux.md`.
- `examples/host/`, `tests/` - PC demos and host tests.

## Build and test

```
make              # build/lets-demo (VFS_NO_TFS) and build/lets-vfs-demo
make test         # kernel, sleep, wdt, vfs, argscript, nrf52, libraries; AddressSanitizer
make docs         # Doxygen into build/docs/html
make format       # Linux kernel style, 100 columns
```

On an MCU: add `kernel/`, `bin/`, `vfs/node.c`, `vfs/vfs.c`, `vfs/posix.c`, `vfs/bin.c`,
`lib/args.c` (+ `opt/` nodes, `vfs/tfs.c` and `vfs/dev/` drivers when file systems are wanted)
to the project.

## Keep in sync

- [ ] A public function changed: its header documentation and the matching `docs/*.md`.
- [ ] A new source file that another kernel file needs (e.g. `vfs/bin.c` calls `bin/reboot.c`):
  say so in README "On the MCU" - projects list kernel sources by hand and stop linking.
- [ ] Shell output format (prompt, password prompt, error lines): programs that drive a shell
  over a link parse it; note the change in the commit and README.
- [ ] Every size or default change (`TASKS_MAX_COUNT`, `VFS_NODES_MAX`, heap, stack use): check the
  64 KB / 8 KB reference build still fits.
- [ ] `byte8`, `args`, `vfs.h`, `task.h` are used by code outside the kernel: keep them
  backward compatible or bump and announce.
- [ ] A new port (`port/X`): `docs/porting.md`, tests where possible; `port_wdt_*` hooks.

## Known issues and TODO

From the 2026-11 code review, open:
- Nested `system()` (a command run from inside a command): the outer command's `argv` is not
  safe across the inner call.
- Task ids are reused without a generation number: a stale id can address a newer task.
- No `errno`: callers get -1 without a reason.

Other:
- Link-mode prompt after every command (8131f4a, 2026-11-16): a program written for the old
  link mode (no prompt after a command) must accept or use it.
- Branch `fixes-2026-11-16` (9905b7a, EFR32 tty receive through LDMA while the flash is busy) is
  not tested on hardware and not merged.
- `tfs` directly (not through the vfs): `tfs_remove`/`tfs_rename` of an open file still frees its blocks;
  a TFS_TRUNC writer's close drops the old file even if another handle reads it.
- Watchdog (2026-11-17): host-tested only (fake clock and watchdog, `tests/test_wdt.c`) and
  compiled with XC16 / arm-none-eabi; not yet run on a PIC24 or EFR32. A PIC24 board needs
  `FWDTEN = ON_SWDTEN`, `WINDIS = OFF`, `WDTCMX = LPRC`, `FWPSA`/`WDTPS` for the period
  (docs/porting.md). A watched task's deadline includes its sleeps.
- One shared stack: on a full PIC24 board about 720 B; `printf()` takes up to 350 B.
- The default shell password `123456` is public: every product must set its own.

## History

- 2026-11: published (Apache 2.0); shell password and new-peer signal; `;` in the shell (4926db7);
  review fixes (43f8561); link-mode prompt (8131f4a).
- 2026-11-17 (branch `sweep-2026-11-17`): `opt/tty.c` sets a direction pin from a task with IPL 7 (no lost
  update of a shared LATx; host test `tests/test_tty.c`), `rm`/`mv` refuse an open tfs file, `shell` stops
  only the console shell (`task_stop_arg`).
- 2026-11-17: watchdog in the scheduler (`LETS_WDT`): watched tasks, reset reason and fault
  record across the reset, traps restart the board.
