# Porting

The kernel needs three things from a target: a microsecond clock, a heap and somewhere for
stdout to go, and four small watchdog hooks (below; a PC has empty ones). Everything else is
plain C.

## Clock

`task_initialize()` takes a function returning microseconds since boot, as `uint64_t`. It must
never go backwards. With a 32-bit hardware timer and a software overflow counter, read the
counter, then the timer, and repeat until the counter has not changed - otherwise the time jumps
back when the overflow interrupt lands between the two reads:

```c
volatile uint64_t system_time = 0;

void system_time_callback(void)             // timer overflow interrupt
{
    system_time += 0x0000000100000000;
}

uint64_t system_time_get(void)
{
    uint64_t high, low;
    do
    {
        high = system_time;
        low = (uint64_t)TMR2_Counter32BitGet() & 0x00000000FFFFFFFF;
    }
    while (high != system_time);
    return (high | low) >> 1;               // timer at 2 MHz -> microseconds
}
```

## Heap

`ALLOC_INIT_HEAP(size);` once, in one C file. `malloc` / `free` come from `kernel/alloc.c`
(32 blocks, `-DMAX_BLOCK_COUNT=n`; 2-byte alignment, 8 in a host build). `alloc_info()` (`top mem`) prints it; `top` shows each
task's share.

## stdout and the shell

Handles 0 / 1 / 2 are stdin / stdout / stderr. They go through the running task's streams, and
`std <node>` points the default at a node (a UART, a network port); the shell reads its commands from handle 0. `printf` ends up in `write(1, ...)`,
which is line buffered (128 bytes).

**The shell password.** Every shell asks for a password on its input before it runs anything:
once per stream (a tty once after power-up), and again when the stream's driver calls
`vfs_peer_new()` (a packet port whose packets come from another address). The built-in default
is `SHELL_PASSWORD_DEFAULT`, **"123456", which everyone knows**: put `passwd WORD` in the
board's startup script. `passwd -` switches it off (for a board whose shell is driven by a
program that does not log in); `-DSHELL_PASSWORD_DEFAULT=\"\"` builds without a default and
`-DSHELL_NO_PASSWORD` without the password at all. It guards the shells only, and goes in clear:
the node `sys` (`vfs/sys.c`), or a network service that can write `/boot/rc` or `/home`, runs
commands without it, so keep those off a link a stranger can reach.

## XC16

The kernel's `open`, `read`, `write`, `close`, `lseek`, `system`, `malloc` and `free` are placed
in `.libc.*` sections, so XC16 links them instead of its own library versions. The other POSIX
calls of `lib/posix.h` are not in XC16's library at all; its headers do not declare most of them,
so `lib/posix.h` does. `reboot`
ends in `port_wdt_reset_now()`: the `reset` instruction.

## A PC (host build)

`port/host/prelude.h` is forced in front of every kernel source. It reads the system headers
first, then renames the kernel's C-library functions to `k_open`, `k_read`, ... so they do not
clash with the system's. `port/host/host_tty.c` (compiled without the prelude) is the terminal
as a node, with non-blocking reads. See `examples/host/main.c` and the `Makefile`.

## Another MCU

Nothing in `kernel/`, `vfs/`, `opt/` or `lib/` touches registers or includes a chip header: what
does is in `port/<chip>/`, behind hooks (the clock, the watchdog and stack hooks of `task.h`, the
memory drivers' `vfs_spi_t`, `flash24_hw_*()` of `vfs/dev/flash24.h`, the UART of `opt/tty.h`,
`get_unique_id()` of `port/pic24/uuid.h`). Provide the clock, make sure your
toolchain links the kernel's C-library functions (or rename them as the host build does), and
write the board's device nodes.

## Watchdog

Like the clock, the watchdog is the kernel's: which tasks are watched, their deadlines, late and
hung tasks, the reset reason and the fault kept across the reset, and `top`'s line all live in
`kernel/task.c`. Boards and tasks never touch the watchdog's registers. The port drives the
hardware through four hooks (`kernel/task.h`):

| hook                            | contract                                                       |
|---------------------------------|----------------------------------------------------------------|
| `port_wdt_start(ms)`            | turn the hardware watchdog on, period about `ms` (`LETS_WDT_MS`); once, in `tasks_start()`, LETS_WDT only |
| `port_wdt_kick()`               | clear it; once a scheduler loop (with LETS_WDT 0 too); a no-op before the start; fast (no waiting for a sync) |
| `port_wdt_reset_now()`          | reset the chip now; does not return (`task_trap()`)             |
| `port_wdt_reset_cause(&raw)`    | why the chip reset, as a `task_reset_t`, and the raw register in `raw`; once, in `task_initialize()` |

| port                              | start                        | kick             | reset now          | cause            |
|-----------------------------------|------------------------------|------------------|--------------------|------------------|
| PIC24 / dsPIC33 `port/pic24/wdt.c` | `RCONbits.SWDTEN = 1` (period: config bits) | `clrwdt` | `reset`     | RCON (then clears its flags) |
| EFR32 `port/efr32/efr32.c`        | WDOG0 on the 1 kHz ULFRCO    | `WDOG0->CMD = CLEAR` unless syncing | `NVIC_SystemReset()` | EMU RSTCAUSE |
| a PC (none)                       | the kernel's weak empty hooks: no watchdog, reason `-`; `port_wdt_reset_now()` calls `exit(0)` |||

`reboot()` (`task.h`, as Linux's reboot(2)) resets through `port_wdt_reset_now()`; it syncs
nothing, the `reboot` command syncs first. The stack has three hooks of its own:
`port_stack_init()` (marks it, at `task_initialize()`), `port_stack(&peak, &size)` (`top`) and
`port_stack_left()` (`system()` refuses a command with less than `VFS_SYSTEM_STACK` left);
PIC24 in `port/pic24/wdt.c` (`_SP_init` .. `SPLIM`), elsewhere weak ones that know nothing.

**Every PIC24 / dsPIC33 project links `port/pic24/wdt.c`** (with LETS_WDT 0 too: the kick and
the reset cause are there). The kernel has no weak PIC24 hooks, so a project without the file does
not link rather than run without a watchdog. The host tests (`tests/test_wdt.c`) define the four
hooks as a simulated watchdog on a fake clock.

The kernel keeps the fault, the slot running now and the task table in RAM that start-up does
not clear: a `persistent` variable on XC16, `.noinit` on Cortex-M (the EFR32 linker script has
it; another Cortex-M linker script needs a `.noinit (NOLOAD)` section in RAM). That is how a task
that never returns is named after the reset (`watchdog (hang: TASK)`).

A PIC24F (e.g. PIC24FJ1024GA606) sets the period in its config bits and lets software turn the
watchdog on; `FWDTEN = OFF` also locks `SWDTEN` out:

```c
#pragma config FWDTEN = ON_SWDTEN   // on when the kernel says (SWDTEN), off until then
#pragma config WINDIS = OFF         // not windowed: the kernel kicks whenever a loop ends
#pragma config WDTCMX = LPRC        // always the 31 kHz LPRC, whatever the system clock
#pragma config FWPSA = PR128        // 4.1 ms a count ...
#pragma config WDTPS = PS512        // ... x 512: about 2.1 s (PS1024: 4.2 s)
```

The period must be longer than the slowest step of any task (one flash erase, say: a driver that
erases page after page in one call calls `task_busy()` before each, which kicks), and longer than
`LETS_WDT_DEFAULT_MS` (1000), so a task kept waiting is named before the hardware resets. Other
families name the bits differently: see the device's config-bit list in the XC16 docs.

Traps: MCC's `traps.c` ends every trap in `TRAPS_halt_on_error(code)`, a weak function. With
`LETS_WDT`, `port/pic24/wdt.c` defines it: it records the trap and the running task and resets,
so the board comes back and `top` shows `reset trap 2 (in: shell)` (MCC's codes: 0 oscillator,
1 stack, 2 address, 3 math). A project with its own trap handlers calls `task_trap(code)` from
them, and sets `-DLETS_WDT_NO_TRAPS` if it defines `TRAPS_halt_on_error()` itself.

## Limits

LETS must run on 64 KB of program memory and 8 KB of RAM. A board sets what it needs with
`TASKS_MAX_COUNT`, `MAX_BLOCK_COUNT`, `VFS_NODES_MAX` and `VFS_FDS_MAX`, and a PIC24FJ64GP202
build is the one to check against. `top mem` shows what a board really uses.

## UART

`port/pic24/tty.c` is the UART of a PIC24F or dsPIC33E as a node, up to 6 ports (`-DTTY_PORTS=n`), sent and
received by interrupts, with the direction pin of RS485 driven from the interrupt: see `opt/tty.h`.
It owns its UARTs, so when MCC runs again it must leave them out, or `uartN.c` comes back and defines
the same interrupts. The interrupt priorities stay in MCC's `interrupt_manager.c`; keep the UARTs'
transmit interrupts at one priority when their direction pins share a LATx (see `opt/tty.h`). dsPIC33CK has
another UART: projects on it keep MCC's driver.

Full-duplex RS485 on a straight cable (`-m DPX1` / `DPX2`): the pin picks the pair a port sends on,
so the two ends must be in opposite modes. The driver finds the right one itself, like auto-MDIX:
a DPX port that has received nothing for 2 to 4 s (pseudo-random per port, `opt/tty_dpx.h`, seeded
by the port, its address and the clock) swaps DPX1 <-> DPX2. A port that hears bytes (a DRN link
pings every second) never swaps; `ttyS1 -a 0` keeps the direction; STD and SPX never swap. It
costs ~600 B of code and 10 B of RAM a port (PIC24); a full board builds it out with
`-DTTY_DPX_AUTO=0` (DPX then keeps the mode it is given). Host
tests: `tests/test_tty_dpx.c` (a cable model: 2,000 starts link after ~5 s on average, ~25 s at
worst) and `tests/test_tty.c`.
