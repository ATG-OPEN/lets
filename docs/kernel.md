# Kernel rules

A small cooperative kernel for PIC24 / dsPIC33. Everything, hardware and programs
alike, is a node of the virtual file system (`vfs`, see [vfs.md](vfs.md)) and is
reached through `open`, `read`, `write`, `close` and `system`.

## Boot

```c
SYSTEM_Initialize();                        // MCC
TMRx_SetInterruptHandler(system_time_callback);

task_initialize(system_time_get);           // us clock, must not go backwards
vfs_initialize();                           // the node table, /opt, /tmp, /mnt
board_initialize();                         // registers board nodes
vfs_register_app("name", name_execute);     // project apps
shell_initialize();                         // the shell task
vfs_bin_initialize();                       // ls, cat, cd, ...
vfs_interpreter_set(argscript_execute);     // script files run by argscript
system("/boot/rc");                         // the board's startup, after the inits

tasks_start();                              // never returns
```

`system_time_get()` builds the time from a software overflow counter and the
hardware timer. Read the counter, then the timer, and repeat until the counter
has not changed. Otherwise time jumps back when the overflow interrupt lands
between the two reads.

The heap is declared once per project: `ALLOC_INIT_HEAP(size);`.

## Tasks

```c
int my_task(void * args, uint64_t system_time);
task_create("name", my_task, args);
```

| return | meaning                                       |
|--------|-----------------------------------------------|
| `> 0`  | sleep that many ms (16-bit `int`, max 32767)  |
| `0`    | run again on the next loop                    |
| `< 0`  | stop, the task's memory is freed              |

A task can stop itself, for example `kill shell` typed in the shell. Its memory stays until it
returns; `task_stopping()` says it should, and whatever it returns, it is not woken again.

A sleep counts from the last wake, so `return 5` is a period of 5 ms whatever the task took. One
that falls behind starts over from now. `top` shows each task's last and longest call (`LAST`,
`MAX`), the time between its calls (`PERIOD`) and its share of the time (`CPU`): the longest
call of any task is what all the others have to wait.

A command whose work goes on in a task of its own (as `top` does: one line per call, so a full
packet port only delays a line) calls `shell_wait(task)`: the shell it was typed into shows no
prompt and reads no input until that task ends, and Ctrl-C stops it.

Tasks never block or busy-wait; keep each call short.

## Watchdog

Built with `-DLETS_WDT=1` (off by default), the scheduler starts the hardware watchdog in
`tasks_start()` and kicks it once per loop, but only while no task is late. **Every task is
watched**, from `task_create()`:

- **Ready tasks get their turn.** A task that returned 0, or whose sleep is over, must be called
  within `LETS_WDT_DEFAULT_MS` (1000 by default). A sleeping task waits for nothing: it is never
  late, however long it sleeps (event-driven and I/O-waiting tasks sleep or return 0 and are
  called every loop). So a call that holds the loop too long makes the others late:
  `top`: `reset watchdog (late: mnl, by: shell)` - `by` is the task with the longest call then.
- **A task that never returns** stops the loop and so every kick; the hardware resets the chip.
  The kernel notes the slot it calls before each call, in RAM kept across the reset:
  `reset watchdog (hang: shell)`.
- **A task that runs but makes no progress** (a stuck state machine that still returns) cannot be
  seen by the scheduler; such a task asks for more with `task_watch()` and checks in with
  `task_alive()`:

```c
task_watch(task_create("link", link_task, NULL), 500); /* or task_watch(NULL, 500) from the task */

static int link_task(void *args, uint64_t now)
{
        if (link_made_progress()) {
                task_alive();          /* checked in: good for another 500 ms */
        }
        return 10;
}
```

  The deadline (1 to 32767 ms) counts the task's sleep too: make it longer than its longest
  sleep. Late: `reset watchdog (late: link)`.
- `task_watch(t, 0)` takes a task out altogether: one allowed to wait for its turn.
- **One long call that makes progress** (a flash driver copying page after page: an `ed` rewrite
  of `/boot/rc` on a PIC24 is ~80 page copies, ~4 s, in one call) calls `task_busy()` between
  steps: it kicks the watchdog (`port_wdt_kick()`) and checks the task in, and the tasks it kept
  waiting wait from the end of the call, so none of them is late. The memory drivers do it before
  every erase (`flash24`, `sst25`, the EFR32 and nRF52 flash). A call that keeps calling it for
  longer than `LETS_WDT_BUSY_MS` (30000) is a hang: the kicks stop, `reset watchdog (hang: ed)`.

The kernel stamps every task when `tasks_start()` begins, so a slow boot (`/boot/rc`) is not late.

Once a task is late the kicks stop for good and the watchdog resets the chip (`LETS_WDT_MS`,
2000 by default; a PIC24 takes its period from its config bits); keep `LETS_WDT_DEFAULT_MS`
below it. A trap goes through `task_trap(code)`. After the reset `task_reset_reason()` says why
(power-on, brown-out, MCLR pin, software, watchdog, trap), `task_fault()` which task (`kind`
`TASK_FAULT_LATE`, `_HANG` or `_TRAP`, `task`, `by`, `code`), and `top` shows both:
`reset watchdog (late: link)`, `reset watchdog (hang: shell)`, `reset trap 2 (in: shell)`.
`top mem` lists the tasks that check in (and how long ago) and those not watched.

The cost: 6 bytes of RAM per task slot plus about 50 bytes; a loop over the task table per pass
and two stores per task call. The hardware is the port's: `port_wdt_start(ms)`,
`port_wdt_kick()`, `port_wdt_reset_now()`, `port_wdt_reset_cause(&raw)` (`docs/porting.md`).

## Memory

- `malloc` / `free` use a fixed heap with 32 blocks (`-DMAX_BLOCK_COUNT=n`) and 2-byte alignment
  (8 in a host build).
- New blocks have no owner. `task_create` gives `args` to the new task, and
  `kill` or a negative return frees every block that task owns. `task_alloc()` is `malloc` for
  more memory the task owns; plain `malloc` is for what outlives it.
- Allocate a task's state in its `*_execute` function and hand it over with
  `task_create`. Free it yourself if `task_create` fails.
- All tasks share one stack: on a PIC24 what the data leaves of 8 KB (720 B on a full board),
  and a `printf()` alone takes up to 350 B of it. `system()` refuses a command when less than
  `VFS_SYSTEM_STACK` (384 B, XC16 only) is left, with `too deep`, rather than overflow (a
  reset): a command run from inside a command inside a command. `top mem` shows the most
  stack used since start-up. Keep large buffers off the stack of what commands call.

## Nodes

```c
vfs_node_create(name, hidden, args, &handlers);   // device (/dev), or at a path
vfs_register_app("/opt/name", execute);          // an application: /opt
```

- `/bin` holds the system's commands only (the shell's and vfs's, `ed`, `as`). Every other
  application registers in `/opt` (`/opt/link`, `/opt/trg`); it still runs by its bare name.
- Handle `0/1/2` = stdin/stdout/stderr. Every task has its own, in `task_t.streams`, and a task
  inherits them from whoever created it. `task_streams()` gives the running task's.
- A task with no streams of its own follows the console, which is what `std <node>` sets. So does
  the shell `shell_initialize()` starts, and with it every board that uses `std`.
- `<`, `>` and `>>` on a command line give the command, and any task it starts, its own streams:
  `system()` opens them and closes them when the last task on them stops.
- `shell [-t] < in > out` starts a shell on its own streams, until `kill shell`. The first one on
  a board with no console takes over the memory of the console's shell, others take about 210 B
  of heap, and 256 B more for the history of a terminal.
- `-t`, on `shell` or `std`, is a terminal for a person: the shell edits and echoes the line,
  completes names with Tab, recalls lines with Up / Down, and stdout ends lines with `\r\n`.
  Without it, it is for a program: whole lines in, replies out, no echo.
- stdout is line buffered (128 B); a line is sent on `\n`, or when another task's stdout
  needs the buffer.
- The table has `VFS_NODES_MAX` entries (24, set more with `-D`). The built-in commands are in
  flash and take none.
- A handle is an entry of a table of `VFS_FDS_MAX` (8, set more with `-D`), for devices and files
  alike: close what you open. `O_RDONLY` can't write and `O_WRONLY` can't read, so a device you
  do both on is `O_RDWR`. A handle has its own read position, and its node's close handler runs
  at the last close of it. A task holds its streams open, so they outlive whoever made them.
- Board drivers live in the project's board folder and expose hardware only as
  nodes (a UART as `ttyS1`, an analog input as `ain1`, ...). Apps never call the
  MCU's peripheral drivers directly.

A common convention for analog nodes: each `read` / `write` moves one `float`
(scaled engineering units), so scripts can `get` and `put` values.

## Commands

```c
int name_execute(void * args, int argc, char *argv[]);
```

- Check `argc` before every `argv[i]`.
- Return `0` on success and `-1` on error.
- Errors print one short line: `bad argument`, `out of memory`, `open failed`,
  `not found`. Messages from running tasks start with `-`.
- `-help` prints usage, one option per line.

On a link the shell runs lines, ended by `\n` or `\r`; a line with no end runs when the input
stops for 50 ms, for programs that send a line per packet. After each command it sends the
prompt a terminal shows, the working directory and `$ ` (`~$ `, `/tmp$ `), after what the command
wrote, and also when it wrote nothing (`cd`, `passwd`, `echo`): a program reads up to the prompt
and knows the command is done, without a timeout. A command that leaves a task the shell waits
for (`top`) gets its prompt when the task ends; an empty line gets none.

```
cd /tmp          ->  /tmp$
pwd              ->  /tmp\n/tmp$
nothing          ->  not found\n/tmp$
``` The startup script,
`/boot/rc`, is the same syntax, one command per line, run by argscript.

For loops, conditions, variables and background scripts, see
`docs/argscript.md`. Every startup script is also a valid argscript.

## Programs

A program on LETS - a command in `bin/`, a script, a board's application - uses the C library
and the POSIX calls the kernel and the vfs provide underneath, as on Linux: `open`, `read`,
`write`, `close`, `lseek`, `system`, `malloc`, `free`, and from `lib/posix.h` `chdir`,
`getcwd`, `mkdir`, `remove`, `rename`, `mount`, `umount`, `sync`, `fsync`, `isatty`, with
`reboot()` in `kernel/task.h`. stdout (1) is line buffered and stderr (2) is the same stream
unbuffered: a write to 2 says how much a full stream took (0: nothing now, try later), which is
how `lines.h` and `top` never lose a line on a packet port; `fsync(1)` passes on stdout's line
first. `isatty(0)` is the terminal mode (`std -t`, `shell -t`): a line ends in "\r\n" there.

What only the kernel knows is in `/proc` (`vfs/proc.c`, a file system type mounted at boot):
`/proc/stat` and `/proc/meminfo` are the first two lines of `top`, `/proc/mem` is `top mem`, and
`/proc/<ID>` is the row of the task in slot ID. `top` reads them, `kill NAME` finds the ID in the
rows and calls `kill(ID, SIGTERM)`. A board short of flash builds with `-DVFS_PROC=0`: no mount
(and one directory slot less), `top` takes the same lines from `vfs_proc_line()` directly.

The commands of `bin/` are programs in this sense; the command table `vfs_commands` is theirs
(`bin/bin.c`), and `system()` underneath finds a name in it. The old `vfs_*` names of these calls
(`vfs_chdir`, `vfs_mkdir`, ...) remain as macros for code written before.

## Style

The code follows the [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html),
with the 100-column limit; `make format` applies it (clang-format, see `.clang-format`).

- Tabs for indentation, K&R braces; a function's opening brace on its own line.
- `/* */` comments. Every public function, type and constant is documented in
  its header in Doxygen form (`/** @brief ... @param ... @return ... */`);
  `make docs` builds the HTML reference.
- `module_verb` names (`tty_register_node`).
- Guard clauses first.
- Module-private functions and data are `static`. Sizes are named constants.
- Header guards are `FILE_H` and unique within the project.
- Pass floats to `printf` as `(double)`.
- Cast `size_t` to `unsigned` for `%u`.
