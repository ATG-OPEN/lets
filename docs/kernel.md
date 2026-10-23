# Kernel rules

A small cooperative kernel for PIC24 / dsPIC33. Everything, hardware and programs
alike, is a node in one table (`fsi`) and is reached through `open`, `read`,
`write`, `close` and `system`.

## Boot

```c
SYSTEM_Initialize();                        // MCC
TMRx_SetInterruptHandler(system_time_callback);

task_initialize(system_time_get);           // us clock, must not go backwards
fsi_initialize();
board_initialize();                         // registers board nodes
fsi_register_app("name", name_execute);     // project apps
shell_initialize((void *)startup);          // shell task + startup script

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

A task can kill itself, for example `kill shell` typed in the shell. It must not
touch its args afterwards; whatever it returns, it is not woken again.

Tasks never block or busy-wait; keep each call short.

## Memory

- `malloc` / `free` use a fixed heap with 64 blocks and 2-byte alignment.
- New blocks have no owner. `task_create` gives `args` to the new task, and
  `kill` or a negative return frees every block that task owns.
- Allocate a task's state in its `*_execute` function and hand it over with
  `task_create`. Free it yourself if `task_create` fails.

## Nodes

```c
fsi_node_create(name, hidden, args, &handlers);   // device, file
fsi_register_app(name, execute);                  // command
```

- Handle `0/1/2` = stdin/stdout/stderr, set with `std <node>`.
- `std -t <node>` makes the node a terminal for a person: the shell edits and echoes the line,
  completes names with Tab, recalls lines with Up / Down, and stdout ends lines with `\r\n`.
  Plain `std <node>` is for a program: whole lines in, replies out, no echo.
- stdout is line buffered (128 B); a line is sent on `\n`.
- `fsi_node_delete` does not free `args`. The creator owns them and frees them
  after the delete.
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

The shell runs one line per read; `\r` and `\n` are stripped. The startup script
is the same syntax, one command per line.

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
