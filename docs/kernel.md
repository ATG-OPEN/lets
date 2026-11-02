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
that falls behind starts over from now. `top` shows each task's last and longest call (`DUR`,
`MAX`): the longest call of any task is what all the others have to wait.

Tasks never block or busy-wait; keep each call short.

## Memory

- `malloc` / `free` use a fixed heap with 32 blocks (`-DMAX_BLOCK_COUNT=n`) and 2-byte alignment
  (8 in a host build).
- New blocks have no owner. `task_create` gives `args` to the new task, and
  `kill` or a negative return frees every block that task owns. `task_alloc()` is `malloc` for
  more memory the task owns; plain `malloc` is for what outlives it.
- Allocate a task's state in its `*_execute` function and hand it over with
  `task_create`. Free it yourself if `task_create` fails.

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
stops for 50 ms, for programs that send a line per packet. The startup script,
`/boot/rc`, is the same syntax, one command per line, run by argscript.

For loops, conditions, variables and background scripts, see
`docs/argscript.md`. Every startup script is also a valid argscript.

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
