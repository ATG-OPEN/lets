# argscript

**argscript** is the script language of LETS: a small, line-based language for
bring-up, production tests, demos and simple automation on the target itself,
without reflashing. It adds variables, arithmetic, conditions, loops, labels,
delays and node I/O to the shell commands you already have.

```text
# blink led1 five times, then report the pot
repeat 5
    put led1 1
    wait 100
    put led1 0
    wait 400
end
get v pot
echo pot = $v V
```

Key properties:

- **Every shell command line is a valid statement.** A line that is not an
  argscript keyword goes to `system()`, exactly as if it were typed in the
  shell. A plain list of commands (a startup script, say) is already a valid
  argscript.
- **A script is a task.** It runs side by side with the other tasks and never
  blocks the system: `wait` sleeps through the scheduler, and a long loop
  gives up the CPU every few lines.
- **Small and predictable.** There is no compiler, no parse tree and no
  garbage collector. The interpreter works directly on the source text, and
  each running script uses one fixed block of about 300 bytes of heap.

## Contents

1. [Quick start](#quick-start)
2. [Language reference](#language-reference)
   - [Lines, comments and labels](#lines-comments-and-labels)
   - [Variables and `$` expansion](#variables-and--expansion)
   - [`set`: text and arithmetic](#set-text-and-arithmetic)
   - [Conditions](#conditions)
   - [`if` / `else` / `end`](#if--else--end)
   - [`while`, `repeat` and `break`](#while-repeat-and-break)
   - [`goto`](#goto)
   - [`wait`](#wait)
   - [Node I/O: `read`, `get`, `put` and `write`](#node-io-read-get-put-and-write)
   - [Shell commands and `$?`](#shell-commands-and-)
   - [Tasks: `run`, `exit` and `kill`](#tasks-run-exit-and-kill)
3. [How a script runs](#how-a-script-runs)
4. [Errors](#errors)
5. [Using argscript in a project](#using-argscript-in-a-project)
6. [Examples](#examples)
7. [Limits and pitfalls](#limits-and-pitfalls)
8. [Testing](#testing)
9. [Quick reference](#quick-reference)

---

## Quick start

Before a board can run scripts, its firmware has to do two things:

1. Add `argscript/argscript.c` to the build.
2. Register the `as` command after `shell_initialize()`:

```c
#include "../../lets/argscript/argscript.h"

fsi_register_app("as", argscript_execute);
```

After that, a script can be started in either of two ways:

**From C**, with a script kept in flash:

```c
static const char selftest[] =
    "echo self test\n"
    "repeat 3\n"
    "    put led1 1\n"
    "    wait 200\n"
    "    put led1 0\n"
    "    wait 200\n"
    "end\n"
    "echo done\n";

argscript_start("selftest", selftest);    /* a new task named "selftest" */
```

**From the shell**, with a script stored in a node (a RAM file, an EEPROM file,
...):

```text
> as /tmp/test.as            run it as a task named "as"
> as /tmp/test.as probe      ... or as a task named "probe"
> top                        the script appears in the task list
> kill probe                 stop it at any time
```

`as` copies the script out of the node before it starts, so the node can be
changed or deleted while the script runs. A file (one that `lseek()` can size,
such as a file on a vfs mount) is read whole, up to `AS_FILE_MAX` (1023
bytes); any other node gives what one read returns, up to 127 bytes. The copy
is on the heap while the script runs: about 300 bytes plus the script.

With vfs, `vfs_interpreter_set(argscript_execute)` makes scripts commands: a
file in `/home` runs by its name, any other by its path, as a task of its name:

```text
~$ blink                      runs /home/blink as the task "blink"
~$ kill blink
```

---

## Language reference

### Lines, comments and labels

A script is a sequence of lines separated by `\n` (a `\r\n` ending works too).
Each line holds one statement.

| Line            | Meaning                                          |
|-----------------|--------------------------------------------------|
| *(empty)*       | Skipped.                                         |
| `# text`        | A comment, skipped. `#` must be the first character on the line. |
| `:name`         | A label, a target for `goto` and `run`. Skipped when it is reached. |
| anything else   | A statement: a keyword, or a shell command.       |

Leading and trailing spaces and tabs are ignored, so indent blocks in
whatever way reads best. Keywords are lowercase and are recognized only as a
whole first word: `settings` is a command, not `set`.

### Variables and `$` expansion

Variables hold **text**. Numbers are stored as their text too, so every value
can be printed, compared or passed to a command as it is.

- A variable is created by the first `set`, `read` or `get` that names it.
- A name is 1 to 7 characters long and uses letters, digits and `_`. Names are
  case-sensitive.
- A script has up to 8 variables, and each holds up to 23 characters. Longer
  text is cut to fit.

Before a line runs, every `$name` in it is replaced by the variable's text,
and every `$?` by the result of the last command or I/O statement (see
[Shell commands and `$?`](#shell-commands-and-)):

```text
set port ttyS1
set baud 115200
$port -b $baud           runs: ttyS1 -b 115200
echo result was $?
```

- A name ends at the first character that cannot be part of a name, so
  `$v.txt` and `$v-1` expand `$v` and keep the rest.
- An unknown variable expands to empty text.
- A `$` that is not followed by a name or `?` stays as it is.
- Expansion happens once per line. Text that comes out of a variable is not
  expanded again.

### `set`: text and arithmetic

```text
set NAME TEXT
set NAME A OP B
```

When the rest of the line has the form *number, space, operator, space,
number*, `set` computes the result. In every other case it stores the rest of
the line as text.

| Operator | Result                                         |
|----------|------------------------------------------------|
| `+`      | A + B                                          |
| `-`      | A − B                                          |
| `*`      | A × B                                          |
| `/`      | A ÷ B (a float, so `10 / 4` gives `2.5`)        |
| `%`      | the remainder of the integer parts of A and B (`7 % 3` gives `1`) |

```text
set i 0                  i = "0"
set i $i + 1             i = "1"
set mv $v * 1000         volts to millivolts
set half $n / 2
set neg $a * -1          negative numbers are fine
set msg pump on          msg = "pump on"     (text)
set e 5*2                e = "5*2"           (text: no spaces around the operator)
```

Each `set` performs one operation. Write `a + b + c` as two statements:

```text
set s $a + $b
set s $s + $c
```

Arithmetic uses 32-bit `float`. Results are stored with up to 7 significant
digits, so whole numbers are exact up to 9 999 999. Dividing by zero, or taking
`%` by zero, is an error and stops the script.

### Conditions

`if` and `while` take a condition in one of two forms:

```text
A OP B          a comparison
A               a truth test
```

**A comparison** uses one of `==`, `!=`, `<`, `>`, `<=` or `>=`. When both
sides are numbers, they are compared as numbers (`10 > 9`). Otherwise they are
compared as text, in byte order (`abc < abd`, and also `"10V" < "9V"`).

**A truth test** is true unless the value is empty or a number equal to zero.
So `0`, `0.0` and an empty or unknown variable are false; `1`, `-2` and `abc`
are true.

Wrap a value in double quotes when it might be empty or contain spaces.
Without the quotes, an empty variable leaves nothing behind and the
condition loses a word:

```text
if "$name" == ""
    echo no name yet
end
if "$state" == "not ready"
    echo waiting
end
```

> Quotes have a meaning only in conditions. In `set`, `write` and shell
> commands they are ordinary characters.

### `if` / `else` / `end`

```text
if CONDITION
    ...
else
    ...
end
```

`else` is optional. Blocks nest, and `if`, `while` and `repeat` together can
nest up to 6 deep. There is no `elif`; nest a second `if` inside the `else`
block instead:

```text
if $mode == 1
    echo one
else
    if $mode == 2
        echo two
    else
        echo other
    end
end
```

### `while`, `repeat` and `break`

```text
while CONDITION          repeat COUNT             break
    ...                      ...
end                      end
```

- `while` checks its condition before every round. The condition is expanded
  again each time, so it sees the current values of the variables.
- `repeat` runs its body COUNT times. COUNT is read once, when the loop
  starts. A COUNT of 0 or less skips the body.
- `break` leaves the innermost `while` or `repeat`, even from inside an `if`
  in that loop.

```text
set n 0
while $n < 10
    set n $n + 1
    if $n == 5
        break
    end
end
echo stopped at $n       prints: stopped at 5
```

A `while 1` loop does not freeze the system. The script yields every 16 lines
(see [How a script runs](#how-a-script-runs)), but put a `wait` in such a loop
anyway, so that it does not use every free CPU cycle.

### `goto`

```text
goto NAME                jumps to the line after :NAME
```

The script is searched for `:NAME` from the top, so labels can be placed
before or after the `goto`. A `goto` closes every open `if`, `while` and
`repeat`. Jumping *out of* a loop is therefore safe. Jumping *into* the middle
of a block is not: the block's `end` then has nothing to close, and it stops
the script with `end without block`.

```text
:retry
mount ee0 /mnt/ee
if $? < 0
    wait 1000
    goto retry
end
```

### `wait`

```text
wait MS                  sleep for MS milliseconds
```

The script sleeps and every other task keeps running. MS is a whole number
(`wait $t` works). The sleep can be any length; it is split into steps of at
most 30 s internally. `wait 0` does nothing.

### Node I/O: `read`, `get`, `put` and `write`

These four statements reach any node by name or path (a device, a RAM file,
a GPA point, ...). Each one opens the node, transfers its data and closes the
node again. If the node cannot be opened, `$?` is set to `-1` and the script
continues.

| Statement            | Does                                                          | `$?`                |
|----------------------|---------------------------------------------------------------|---------------------|
| `read VAR NODE`      | Reads text (up to 23 bytes) into VAR, without the line ending. | bytes read           |
| `get VAR NODE`       | Reads one binary `float` and stores it in VAR as text. VAR is unchanged if the read fails. | bytes read (4 on success) |
| `put NODE VALUE`     | Writes VALUE as one binary `float`.                          | bytes written        |
| `write NODE TEXT`    | Writes the rest of the line as text. `\n` writes a new line.   | bytes written        |

Use `get` and `put` for nodes that exchange floats, such as [GPA](../opt/gpa.h)
points (ADC, DAC and GPIO channels in physical units). Use `read` and `write`
for text: files, UARTs, displays.

```text
get t temp              t = "23.75"
put dac0 1.25           set the DAC to 1.25 V
write lcd T=$t\n
read id /mnt/ee/serial
```

### Shell commands and `$?`

Any line that does not start with a keyword is a shell command. It is run with
`system()` after `$` expansion, and its return value becomes `$?`. By
convention 0 means success and a negative value means failure.

A failing command does **not** stop the script. Check `$?` when it matters:

```text
mount ee0 /mnt/ee
if $? < 0
    echo mount failed
    exit
end
```

`$?` is changed by shell commands and by `read`, `get`, `put`, `write` and
`run`. Everything else leaves it unchanged.

### Tasks: `run`, `exit` and `kill`

```text
run NAME :LABEL          start a new script task NAME at :LABEL
exit                     end this script
```

`run` starts a second script from the same source. The new task starts at the
label, with a **copy** of the caller's variables. From then on the two scripts
are independent: a later `set` in one is not seen by the other. To share data
between them, use a node (a RAM file, for example).

Every script is an ordinary task. It is listed by `top` and stopped by
`kill NAME`, from the shell or from another script. A script may even kill
itself. When a script ends (by reaching its last line, by `exit`, by an error
or by `kill`), its memory goes back to the heap.

```text
run blink :blink         background task
wait 5000
kill blink               stop it after 5 s
exit

:blink
while 1
    put led1 1
    wait 100
    put led1 0
    wait 100
end
```

---

## How a script runs

Each script runs in its own cooperative task. On each call from the scheduler,
the interpreter runs up to `ARGSCRIPT_STEPS` lines (16) and then returns, so
that other tasks get their turn:

```text
scheduler ──► script task: run up to 16 lines ──► return
                  │
                  └─ `wait 200`: return 200, and the scheduler calls again in 200 ms
```

Some consequences follow from this design:

- **Nothing blocks.** A script cannot stall the UART, the shell or a control
  loop, even in an endless loop.
- **Timing is cooperative.** `wait 100` means *at least* 100 ms. The delay
  stretches if another task holds the CPU when it runs out.
- **Every line runs to completion.** A shell command started from a script
  runs inside the script's task call. Long commands delay the other tasks
  (the same as typing them at the shell).
- **Variables are private.** Every script has its own variables and block
  stack, so any number of scripts can run at once, limited only by the task
  table and the heap.
- **No code is compiled.** The source is interpreted as it runs, so a script
  started from flash with `argscript_start()` takes no RAM for its text.

---

## Errors

Errors in shell commands are reported through `$?` and never stop a script.
Errors in the script itself stop it and print the line number:

```text
-as line 12: missing end
```

| Message              | Cause                                                       |
|----------------------|-------------------------------------------------------------|
| `bad variable`       | The name is invalid or too long, or all 8 variables are in use. |
| `bad argument`       | An unknown comparison operator, or `run` without `NAME :LABEL`. |
| `divide by zero`     | `/` or `%` by 0 in `set`.                                    |
| `too deep`           | More than 6 nested `if` / `while` / `repeat`.                |
| `missing end`        | A block that is skipped (or left by `break`) has no `end`.    |
| `end without block`  | An `end` with no open block (often after a `goto` into a block). |
| `else without if`    | An `else` that is not inside an `if`.                         |
| `break without loop` | A `break` outside any `while` or `repeat`.                    |
| `not found`          | `goto` or `run` to a label that does not exist.               |
| `line too long`      | A line is longer than 95 characters after `$` expansion.        |
| `out of memory`      | `run` could not get heap memory or a task slot.               |

The `as` command prints `open failed`, `out of memory` or `bad argument` when
it cannot start a script.

---

## Using argscript in a project

### Files

| File                       | Purpose                          |
|----------------------------|----------------------------------|
| `argscript/argscript.h`    | The API and configuration.        |
| `argscript/argscript.c`    | The interpreter (C99; it depends only on the kernel). |
| `argscript/test/as_test.c` | The host test.                    |

### API

```c
int argscript_start(const char *name, const char *source);
```

Starts `source` as a new task called `name`. The source is **not copied**, so
it must stay valid while the script runs. A `const` string in flash is ideal.
The function returns 0, or −1 if the heap or the task table is full. It can be
called before `tasks_start()`, for a boot script, or at any time later.

```c
int argscript_execute(void *args, int argc, char *argv[]);
```

The handler for the `as NODE [NAME]` shell command. Register it under any
name you like with `fsi_register_app()`.

### Boot scripts

The startup script given to `shell_initialize()` is a plain list of commands,
each run once. For a boot sequence that needs logic, keep the startup script
for the basics and start an argscript next to it:

```c
static const char boot[] =
    "get v vbus\n"
    "if $v < 4.5\n"
    "    echo low supply $v V\n"
    "    exit\n"
    "end\n"
    "run heartbeat :hb\n"
    "exit\n"
    ":hb\n"
    "while 1\n"
    "    put led1 1\n"
    "    wait 50\n"
    "    put led1 0\n"
    "    wait 950\n"
    "end\n";

shell_initialize((void *)startup);
fsi_register_app("as", argscript_execute);
argscript_start("boot", boot);
tasks_start();
```

### Configuration

Each limit is a `#define` in `argscript.h`. Override one with `-D` on the
compiler command line, or define it before the header is included.

| Define                | Default | Meaning                                       |
|-----------------------|---------|-----------------------------------------------|
| `ARGSCRIPT_VARS`      | 8       | Variables per script.                          |
| `ARGSCRIPT_NAME_SIZE` | 8       | Variable name length, including the `'\0'`.     |
| `ARGSCRIPT_TEXT_SIZE` | 24      | Variable text length, including the `'\0'`.     |
| `ARGSCRIPT_DEPTH`     | 6       | Deepest nesting of `if` / `while` / `repeat`.   |
| `ARGSCRIPT_LINE_SIZE` | 96      | Longest line, after `$` expansion, including the `'\0'`. |
| `ARGSCRIPT_STEPS`     | 16      | Lines run per task call before yielding.       |

### Footprint

The figures below are for PIC24 with XC16 at `-O0`:

| Item                  | Size                                              |
|-----------------------|---------------------------------------------------|
| Code                  | about 3.7 KB                                      |
| Constant strings      | about 0.5 KB                                      |
| Static RAM            | 192 B (two line buffers, shared by all scripts)    |
| Heap, per script      | about 300 B (+128 B for a script started by `as`) |

Most of the per-script heap is the variable table
(`ARGSCRIPT_VARS × (ARGSCRIPT_NAME_SIZE + ARGSCRIPT_TEXT_SIZE)`). Lower the
defaults on very small parts.

---

## Examples

The node names below (`led1`, `pot`, `red`, `lcd`, `dac0`, ...) are examples;
use the ones your board registers (`ls`, or `ls /dev` with the VFS).

### Board bring-up

```text
# bring-up: console, scales, then a quick check of every output
ttyS1 -b 115200
pot -c -i 0 0.000806            # counts to volts: 3.3 V / 4096
echo bring-up start

repeat 2
    put red 1
    wait 150
    put red 0
    put green 1
    wait 150
    put green 0
    put blue 1
    wait 150
    put blue 0
end

get v pot
echo pot $v V
echo bring-up done
```

### Monitor a value and react to a threshold

```text
:loop
get v pot
set pc $v * 30.3                # 0..3.3 V to 0..100 %
if $pc > 80
    put led1 1
    echo pot high $pc
else
    put led1 0
end
write lcd $pc\n
wait 200
goto loop
```

### Wait for a condition, with a timeout

```text
set t 0
while $t < 50                   # 50 x 100 ms = 5 s
    read s /tmp/status
    if "$s" == "ready"
        break
    end
    wait 100
    set t $t + 1
end
if $t >= 50
    echo timeout
    exit
end
echo ready after $t
```

### Step a DAC through a range

```text
set v 0
while $v <= 3
    put dac0 $v
    wait 500
    get m adc0
    echo set $v read $m
    set v $v + 0.25
end
put dac0 0
```

### Log to a file

```text
touch /tmp/log
repeat 10
    get v pot
    write /tmp/log $v\n
    wait 1000
end
cat /tmp/log
```

### Two scripts at once

```text
run fast :fast
run slow :slow
exit

:fast
while 1
    put led1 1
    wait 100
    put led1 0
    wait 100
end

:slow
while 1
    put led2 1
    wait 1000
    put led2 0
    wait 1000
end
```

Stop them with `kill fast` and `kill slow`.

---

## Limits and pitfalls

- **Spaces matter in arithmetic.** `set x $a + 1` computes; `set x $a+1`
  stores the text `5+1`.
- **One operation per `set`.** There are no parentheses and no precedence.
- **Quote values that may be empty.** Use `if "$x" == ""`, not `if $x == ""`.
- **Variable names are at most 7 characters**, and at most 8 variables exist
  per script. Variable text is cut to 23 characters.
- **Lines are cut at 95 characters** when read, and a line that grows longer
  than that through `$` expansion is an error.
- **`as` reads at most 127 bytes** from a node without a size (a RAM file),
  and up to 1023 bytes (`AS_FILE_MAX`) from a file. Start longer scripts from
  C with `argscript_start()`, or split them and call one from another with
  `as`.
- **Numbers have 7 significant digits.** Above 9 999 999, counters lose
  precision (and print as `1e+07`, for example).
- **`goto` closes every open block.** Do not jump into a loop or an `if`.
- **`wait` takes whole milliseconds.** `wait 1.5` waits 1 ms.
- **Scripts from `argscript_start()` use the source in place.** Do not
  pass a buffer on the stack, or one that is reused.
- **Output goes to stdout.** `echo` and the shell commands print where the
  shell prints (`std ...`). Use `write NODE` to send text to a particular
  device.

---

## Testing

The host test runs the interpreter against fake nodes and a fake scheduler,
and covers every statement, nesting, timing, `run` / `kill` and the error
cases:

```sh
make test                         # all LETS host tests, argscript included
```

To build and run only the argscript test:

```sh
cd argscript/test
cc -std=c99 -Wall -include fcntl.h -Dopen=t_open -Dread=t_read -Dwrite=t_write \
   -Dclose=t_close -Dsystem=t_system as_test.c ../argscript.c -o as_test && ./as_test
```

When you change the interpreter, add a `check()` case to `as_test.c` that
shows the new behavior.

---

## Quick reference

```text
# comment                     :label
set x 5                       set x $a * 2           (+ - * / %)
set s any text
if A OP B ... else ... end    OP: == != < > <= >=    (numbers, else text)
if $x                         true unless empty or 0
while COND ... end            repeat N ... end       break
goto label                    wait MS
read  VAR NODE                get VAR NODE           (text / float in)
write NODE TEXT\n             put NODE VALUE         (text / float out)
run NAME :label               exit                   kill NAME (shell)
any other line                shell command; $? = its result
$name  $?                     expanded before the line runs
```
