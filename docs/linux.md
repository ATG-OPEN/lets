# LETS compared with Linux

LETS looks like Linux on purpose: paths, `ls`, `cat`, `mount`, `<` and `>`. This page lists where it
behaves differently, and what is worth adding. It was made from the code, not from memory of it.

Each difference is marked:

- **fix**: looks like Linux but is wrong in a way that will surprise someone. Worth changing.
- **add**: missing, and worth having on an MCU.
- **keep**: different for a good reason.

## Tasks

| Linux | LETS | |
|-|-|-|
| Processes, preempted | Tasks are C functions that return, run in turn. One slow task stalls all of them | keep |
| Each process has its own stack and memory | One stack, one address space, no protection. A bad pointer or a stack overflow takes down the board | keep |
| `fork`, `exec`, `wait` | None. A command is a function call; a long one starts a task and returns | keep |
| A foreground command blocks only its shell | A command runs inside `system()`, and every other task waits for it | keep |
| Each process has its own working directory | The same: per task, inherited. A shell starts in `/home` | - |
| PID | None. `top` shows an ID, but `kill` takes a name and stops the first task with it. Two shells can't be told apart | fix |
| Signals, `SIGTERM` lets a program clean up | `kill` removes the task at once: its streams and the memory it owns are released, but a task can't switch off its hardware first | add |
| Ctrl-C stops the running command | Only clears the line. It can't stop a command, because the shell isn't running while one does | keep |
| Exit status 0 means success | **Many commands return a count**: `ls` the entries, `cat` the bytes, `df` the mounts. So `$?` of a good `cat` is 18 | fix |
| `wait` gets a child's status | `system()` returns when the command returns. A task it started has no status anyone can get | keep |
| Users, groups, `root`, `sudo` | None | keep |
| `init` restarts services | rc runs once. A task that stops stays stopped | keep |

## Shell

| Linux | LETS | |
|-|-|-|
| `\|` pipes | None. There are no blocking reads, so two tasks can't stream to each other. Running the first into a RAM buffer and the second from it would work for short output | add |
| `;`, `&&`, `\|\|` | None; one command per line | add |
| `&` | None. A command that starts a task is in the background anyway | keep |
| `2>`, `2>&1` | None. stderr follows stdout, and in `ls 2> x` the `2` is an argument to `ls` | keep |
| A shell reads to the end of a line | The same. One with no end runs when the input stops for 50 ms, as some programs send a line per packet | keep |
| A shell ends at the end of its input | It never ends: a read that returns nothing can mean "no data yet" | keep |
| `exit` | None; `kill shell` | add |
| Variables, `$?`, `$1` | Only in argscript, not at the prompt | keep |
| A script gets its arguments | **Words after a script's name are dropped.** `blink 500` runs `blink` | fix |
| `*`, `?` wildcards | None | add |
| Single quotes, `\` escapes | Only `"`, and only around a whole word | keep |
| `#` comments at the prompt | Only in argscript | keep |
| `$PATH` | Any node of that name, then `/home` for scripts. Fixed | keep |
| `#!` picks the interpreter | One interpreter per board | keep |
| A script needs `+x` | Any file in `/home` runs | keep |
| Left / Right edit the line | Ignored. Up / Down keep 2 lines | keep |
| Long lines | Cut at 127 characters and 16 words, with no message | fix |

## Files and handles

| Linux | LETS | |
|-|-|-|
| Each process has a table of open files | One table for the board: 8 handles in all, devices and files | keep |
| Each `open` gets its own handle and position | The same | - |
| `errno` says why it failed | Nothing sets `errno`, everything is -1. Messages are `open failed`, `bad argument`, `cannot open`, and on macOS a full disk over FUSE reads as "No such file" | fix |
| `read` blocks, or `O_NONBLOCK` | Never blocks, and 0 means either "no data yet" or "end of file" | keep |
| `select`, `poll` | None | keep |
| `O_RDONLY` can't write | The same | - |
| `O_TRUNC` empties the file at once | Writes a new copy that replaces the old one at `close()`. Better than Linux, but a file can only be up to half of its partition | keep |
| `stat`, `fstat` | `vfs_file_info`, nothing standard. No `stat` command | add |
| `isatty` | `vfs_terminal()`, the running task's and not a handle's | add |
| `dup`, `dup2`, `pipe`, `fcntl`, `ioctl` | None. A device is configured by running it: `ttyS1 -b 9600`, `pot -c` | keep |
| stdout is buffered per process | One 128 B buffer for the board, passed between tasks. A line over 128 B arrives in two writes | keep |

## File system

| Linux | LETS | |
|-|-|-|
| Permissions, owners | None. `ls -l` shows what a node can do | keep |
| Timestamps | None; the board has no clock | keep |
| Links, symlinks | None | keep |
| `/proc`, `/sys` | None; `top`, `top mem`, `df` | keep |
| `/tmp` holds any file | A RAM file is a node of **126 B**, and uses one of 24 table slots. A write past that is refused with no message | fix |
| Reading goes forward to the end | The same, `lseek` too. A write to a RAM file always appends | keep |
| A device is any node | One table of 24 for devices, RAM files and a project's commands, with names of up to 15 characters; the built-in commands are in flash | keep |
| Directories nest freely | 6 directories besides `/`, `/bin`, `/dev`; a path of 64 characters | keep |
| `mv` across file systems | Refused: use `cp` | keep |

## Commands

| Linux | LETS | |
|-|-|-|
| `rm -r`, `cp -r`, `mkdir -p` | None | keep |
| `rm` removes a device node | Refused, on purpose | keep |
| `rm` of an open file works | Refused for a RAM file: its node must not go while a handle is on it | keep |
| `ls a b`, `cat a b` | One path | add |
| `cat` of a device reads until it ends | One read, because a sensor never ends | keep |
| `echo -n` | None | add |
| `touch` updates a time | Makes an empty file, or a RAM file outside a mount | keep |
| `ps`, `free` | `top`, `top mem` | keep |
| `top` is live | One screen | keep |
| `sleep` | Only `wait` in argscript | keep |
| `date`, `uptime` | The same, UTC only. `date` is also a node, to set it over a network. Nothing keeps it across a reset | keep |
| `dmesg`, `syslog` | None: what prints before a console exists is lost | add |
| `grep`, `head`, `tail`, `wc` | None | add |
| `tail -f` | None | add |
| `test`, `[`, `true`, `false` | `if` in argscript | keep |
| `reboot` | Built into `system()`, not a command, so `ls /bin` doesn't show it | keep |
| Terminals have `stty` | `std -t` or `shell -t` | keep |

`std` has no Linux equivalent. It is the console: where a task with no streams of its own reads and
writes. `ed` takes its edits as arguments, as `sed` would, because there is no full-screen editor.

## What to add, in order

1. **Exit status 0.** Commands that return a count print it instead, so `$?` means what it does on
   Linux. Scripts that test `$?` need checking.
2. **Real errors.** Set `errno` in `open` and the file calls, and print `cat: /x: no such file`.

Next, for working on a live system:

3. **`kill` by ID, and a stop callback.** A task can then switch its outputs to a safe state, which
   matters on a machine more than anywhere: today a killed task leaves them as they were.
4. **`;` and `&&`** in `system()`.
5. **Arguments for scripts:** `$1`, `$2` in argscript.
6. **A log.** A ring buffer as a node, `/dev/log`, kept across a reset when RAM survives it.
7. **`tail -f`, `grep`, `wc`, `head`.** Small, each its own command so that a project pays only for
   what it links.
8. **Pipes by a RAM buffer,** for short output: `ls | grep x`.
9. **`stat`, `sleep`.**

Not worth it on these chips: users and permissions, `fork`, preemption, per-task stacks, links,
timestamps without a clock, `/proc`.
