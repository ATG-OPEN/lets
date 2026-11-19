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
updated: 2026-11-18
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
- **Watchdog** (`-DLETS_WDT=1`, off by default; branch `wdt-all` 2026-11-18): the scheduler kicks
  the hardware watchdog once per loop while no task is late. Every task is watched from
  `task_create()`: once ready (returned 0 / sleep over) it must be called within
  `LETS_WDT_DEFAULT_MS` (1000), sleeping never counts -> `late: T, by: LONGEST_CALL`; a call that
  never returns -> hardware reset, `hang: T` (running slot + task table in no-init RAM).
  `task_watch(t|NULL, ms)` = must also call `task_alive()` within ms (1..32767, sleeps count),
  `task_watch(t, 0)` = not watched at all. `task_busy()` = one long call making progress (flash
  drivers call it before every erase: flash24, sst25, EFR32, nRF52): kicks + checks in, the others
  wait from the call's end; more than `LETS_WDT_BUSY_MS` (30 s) of it = hang. `task_trap(code)`; after a reset `task_reset_reason()`
  and `task_fault()` (kind LATE/HANG/TRAP, task, by, code), shown by `top` and `top mem`.
  Port hooks (all logic in `task.c`): `port_wdt_start(ms)`, `port_wdt_kick()` (every loop, also
  LETS_WDT 0), `port_wdt_reset_now()`, `port_wdt_reset_cause(&raw)`: PIC24 `port/pic24/wdt.c`
  (every PIC24 project must link it; also MCC's `TRAPS_halt_on_error`), EFR32 `port/efr32`, PC weak stubs.
- **Nodes** (`vfs/vfs.h`): `vfs_node_create(name, hidden, args, &handlers)`,
  `vfs_register_app("/opt/name", execute)`, `vfs_blk_register()`; `vfs_peer_new()` - a driver
  signals a new peer on an input (the shell then asks its password again).
- **C library from vfs:** `open`, `read`, `write`, `close`, `lseek`, `system`, `malloc`, `free`;
  the POSIX line (`lib/posix.h`, programs include no kernel/vfs header for these): `chdir`,
  `getcwd`, `mkdir`, `remove`, `rename`, `mount` (`MS_FORMAT`), `umount`, `sync`, `fsync`
  (stdout's line), `isatty` (terminal mode), `reboot` (kernel/task.h). Old names stay as macros
  (`vfs_chdir`, `vfs_mkdir`, `vfs_remove`, `vfs_rename`, `vfs_mount_type`, `vfs_umount`,
  `vfs_terminal`). `kill(pid, sig)` (pid = top's ID = task slot; 0 is a task, not a group).
- **/proc** (`vfs/proc.c`, fs type "proc" mounted by `vfs_initialize()`, `VFS_PROC` 1): `stat`,
  `meminfo` (top's first lines), `mem` (top mem), `<ID>` (a task's row); `top` reads them line by
  line in a task of its own (`lines_spawn()`), `kill NAME` scans the rows. Takes one directory
  slot (VFS_DIRS_MAX default +1). `-DVFS_PROC=0` (PIC24M, flash full) leaves the mount out:
  `top` asks `vfs_proc_line()` / `vfs_proc_mem()` directly, `kill` uses `task_stop_byname()`.
- **Clocks** (`kernel/clock.c`): `clock_gettime` (CLOCK_MONOTONIC = the board's clock,
  CLOCK_REALTIME = UTC), `clock_settime`, `gettimeofday`, `settimeofday`, `adjtime`, `adjtimex`
  (ADJ_OFFSET_SINGLESHOT slew at most 500 us/s, ADJ_FREQUENCY ppm<<16, STA_UNSYNC until set).
  `bin/date.c`'s `date_*()` API (drn time app) is kept, on these calls.
- **stat / dirs**: `stat`, `fstat` (st_mode, st_size, st_ino = which dir/node, st_dev = which fs,
  0 for a node), `opendir`/`readdir`/`closedir`/`telldir`/`seekdir` (dirent: d_name, d_type,
  LETS d_hidden), `ioctl(fd, LETS_IOC_ABORT)` (give up a write, old file stays).
- **errno** set by the POSIX calls (the C library's errno). **poll()** (`VFS_POLL` 1): readiness now;
  with none ready and a timeout, the running task is woken (task_wake()) when one gets ready - it
  returns its sleep: `if (poll(&p, 1, 1000) == 0) return 1000;`. Nodes tell with
  `vfs_handler_t.poll_handler` (PIC24/EFR32/nRF52/host ttys have one; none = always ready).
  /proc/mounts + `statvfs()` (df).
- **VFS_FD_TASK** (0 by default): 1 closes what a task opened when it ends or is killed (each handle
  remembers its opener). Off by default because a command may open a handle and hand it to a task
  it starts; the whole host suite passes with it on. Floor build +117 B, +16 B RAM.
- **Shell:** `std [-t] NODE` moves the shell (terminal or link mode), `;` separates commands,
  redirection `<` `>`; commands `ls cd cat echo top kill mv touch rm cp mount df date uptime passwd
  reboot ...`; password asked once per input stream, again after `vfs_peer_new()`; default
  `123456` (`SHELL_PASSWORD_DEFAULT`), `passwd WORD` / `passwd -` in `/boot/rc`.
  **Link mode prints the prompt (`<cwd>$ `) after every command** (since 8131f4a): programs on the
  other end use it as "command done".
- **Optional nodes** (`opt/`): `gpa` (GPIO/ADC/DAC channel as a node; one native float per read or
  write, in physical units), `tty` (non-blocking PIC24F UART, RS485 direction pin; DPX1/DPX2 swap by
  themselves after 2-4 s (pseudo-random per port, `opt/tty_dpx.h`) of receiving nothing, `-a 0` off,
  `-stat` shows the swaps; `-DTTY_DPX_AUTO=0` builds it out: ~600 B code, 10 B RAM a port).
- **Libraries** (`lib/`): `args` (argv, dotted paths such as `1.2.5`), `byte8` (framing + CRC16, up to
  250 data bytes), `ddata`, `pidlib`, `queue`, `ema_filter`, `medianfilter` (the PIC24 device ID `uuid` is in `port/pic24`).
- **Build flags:** `TASKS_MAX_COUNT` (16), `VFS_NODES_MAX` (24), `MAX_BLOCK_COUNT` (32 heap
  blocks), `VFS_NO_TFS`, `VFS_SYSTEM_STACK` (384 B, XC16), `SHELL_PASSWORD_DEFAULT`, `LETS_WDT` (0),
  `LETS_WDT_MS` (2000; PIC24 uses config bits), `LETS_WDT_DEFAULT_MS` (1000), `LETS_WDT_NO_TRAPS`.

## Where things are

- `kernel/` - scheduler (`task.c`), heap (`alloc.c`).
- `bin/` - the programs: shell and commands (`shell.c`, `bin.c` - the command table and the file
  commands, `top.c`, `kill.c`, `date.c`, `reboot.c`, `sys.c`, `ed.c`, `lines.c`).
- `vfs/` - node table (`node.c`), tree (`vfs.c`), POSIX calls (`posix.c`), RAM files of /tmp (`tmp.c`),
  tiny power-loss-safe file system (`tfs.c`, `tfsvfs.c`), `onefs.c`, memory drivers
  `dev/` (25xx EEPROM, 23xx SRAM, SST25 flash, PIC24FJ flash, RAM disk, partitions).
- `argscript/` - the script language. `lib/` - portable libraries.
- `port/` - `host`, `cortex-m`, `efr32` (ZG23 linker script, flash, syscalls, watchdog), `nrf52`,
  `pic24` (`wdt.c`: watchdog / reset-cause hooks).
- `docs/` - `kernel.md`, `vfs.md`, `argscript.md`, `porting.md`, `linux.md`.
- `examples/host/`, `tests/` - PC demos and host tests.

## Build and test

```
make              # build/lets-demo (VFS_NO_TFS) and build/lets-vfs-demo
make test         # kernel, sleep, wdt, vfs, argscript, nrf52, libraries; AddressSanitizer
make docs         # Doxygen into build/docs/html
make format       # Linux kernel style, 100 columns
```

On an MCU: add `kernel/`, `bin/`, `vfs/node.c`, `vfs/vfs.c`, `vfs/posix.c`, `vfs/tmp.c`, `vfs/proc.c`,
`lib/args.c` (+ `opt/` nodes, `vfs/tfs.c` and `vfs/dev/` drivers when file systems are wanted)
to the project.

## Keep in sync

- [ ] A public function changed: its header documentation and the matching `docs/*.md`.
- [ ] A new source file that another kernel file needs (e.g. `vfs/posix.c` calls `vfs/tmp.c`):
  say so in README "On the MCU" - projects list kernel sources by hand and stop linking.
- [ ] Shell output format (prompt, password prompt, error lines): programs that drive a shell
  over a link parse it; note the change in the commit and README.
- [ ] Every size or default change (`TASKS_MAX_COUNT`, `VFS_NODES_MAX`, heap, stack use): check the
  64 KB / 8 KB reference build still fits.
- [ ] `byte8`, `args`, `vfs.h`, `task.h` are used by code outside the kernel: keep them
  backward compatible or bump and announce.
- [ ] A new port (`port/X`): `docs/porting.md`, tests where possible; `port_wdt_*` hooks.

## Known issues and TODO

The POSIX line (branch `posix-line`, 2026-11-19), still open:
- The shell keeps its kernel calls: `task_streams()` (redirect / terminal of `shell -t`),
  `vfs_stream(0)` (did `std` move the input), `vfs_peer_changed()` (password), `vfs_console()`,
  `task_stop_byname()` for Ctrl-C, the node table for Tab. It is the login and init of LETS.
- The task calls (`task_create`, `task_alloc`, `task_stopping`, `shell_wait`) are LETS's process
  calls; `sys`, `lines`, `top`, `reboot`, argscript use them.
- `VFS_FD_TASK` is off by default.
- PIC24M has 9 B of flash left (with `VFS_PROC=0`); DEMO top: 4.8 ms a call (2.2 ms before).

From the 2026-11 code review, open:
- Nested `system()` (a command run from inside a command): the outer command's `argv` is not
  safe across the inner call.
- Task ids are reused without a generation number: a stale id can address a newer task.

Other:
- Link-mode prompt after every command (8131f4a, 2026-11-16): a program written for the old
  link mode (no prompt after a command) must accept or use it.
- Branch `fixes-2026-11-16` (9905b7a, EFR32 tty receive through LDMA while the flash is busy) is
  not tested on hardware and not merged.
- `tfs` directly (not through the vfs): `tfs_remove`/`tfs_rename` of an open file still frees its blocks;
  a TFS_TRUNC writer's close drops the old file even if another handle reads it.
- Watchdog: `wdt-all` (every task watched, hooks only in ports) tested on the EFR32 kits
  2026-11-18, PIC24 compiled only. A PIC24 board needs `port/pic24/wdt.c` in its project and
  `FWDTEN = ON_SWDTEN`, `WINDIS = OFF`, `WDTCMX = LPRC`, `FWPSA`/`WDTPS` for the period
  (docs/porting.md). A `task_watch()` deadline includes its sleeps. A stuck state machine that
  still returns is caught only with `task_watch()` + `task_alive()`. The `late` record keeps
  slot numbers until the reset (names read from the kept task table at boot): a slot reused in the
  2 s before the reset shows the new name.
- `make test`: `test_kernel` period checks (lines 333, 725) fail on a heavily loaded Mac (real clock), on main too.
- One shared stack: on a full PIC24 board about 720 B; `printf()` takes up to 350 B.
- The default shell password `123456` is public: every product must set its own.

## History

- 2026-11-19 (branch `posix-line`): programs above a POSIX line. Chip code only in `port/*`
  (stack hooks, `reboot()`, PIC24 tty/uuid/flash24 moved); /tmp under vfs; the file commands and
  ed in `bin/`; `lib/posix.h` (chdir, getcwd, mkdir, remove, rename, mount, umount, sync, fsync,
  isatty, kill); /proc (`VFS_PROC`) for top and kill; argscript without kernel internals;
  `VFS_FD_TASK`. Board projects change paths (README "On the MCU").

- 2026-11: published (Apache 2.0); shell password and new-peer signal; `;` in the shell (4926db7);
  review fixes (43f8561); link-mode prompt (8131f4a).
- 2026-11-17 (branch `sweep-2026-11-17`): `opt/tty.c` sets a direction pin from a task with IPL 7 (no lost
  update of a shared LATx; host test `tests/test_tty.c`), `rm`/`mv` refuse an open tfs file, `shell` stops
  only the console shell (`task_stop_arg`).
- 2026-11-17: watchdog in the scheduler (`LETS_WDT`): watched tasks, reset reason and fault
  record across the reset, traps restart the board.
- 2026-11-18 (branch `wdt-all`): every task watched by the scheduler (ready-wait default, hang named
  after the reset, `by:` the long call); watchdog hardware only behind four port hooks, PIC24 ones
  moved to `port/pic24/wdt.c`.
- 2026-11-18: merged to main with `task_busy()` (flash drivers kick between page copies: an `ed`
  rewrite of `/boot/rc` on a PIC24 no longer outlasts the ~2.1 s watchdog), tty transmit IRQ bits as
  single bset/bclr (UART3/4 share IFS5/IEC5), and DPX auto direction in the tty (was a router
  feature on a drn-router-mcu branch, now the port's).
