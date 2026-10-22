# argscript

A tiny script language on top of the kernel commands. Each line is one statement.
A line that is not a keyword is passed to `system()`, so every existing startup
script is already a valid argscript.

Each script runs as its own task and never blocks. It runs up to 16 lines per
call and then yields; `wait` sleeps through the task return value, so the other
tasks keep running.

## Statements

| line                  | does                                                   |
|-----------------------|--------------------------------------------------------|
| `# text`              | comment, blank lines are skipped too                   |
| `:name`               | label                                                  |
| `set x 5`             | `x = "5"`                                              |
| `set x $a * 2`        | math `+ - * / %`, one operator, float                  |
| `set s any text`      | text, whenever it is not `number op number`            |
| `if a == b`           | `== != < > <= >=`, numbers when both are, else text    |
| `if $x`               | true when not empty and not `0`                        |
| `else` / `end`        |                                                        |
| `while cond` … `end`  |                                                        |
| `repeat n` … `end`    |                                                        |
| `break`               | leave the innermost `while` / `repeat`                 |
| `goto name`           | jump to `:name`, open blocks are dropped               |
| `wait ms`             | sleep, any length                                      |
| `read x node`         | text from a node into `x`, `$?` = bytes, line end cut  |
| `get x node`          | one `float` from a node (an analog node, say)          |
| `put node value`      | one `float` to a node                                  |
| `write node text`     | text to a node, `\n` is a new line                     |
| `run name :label`     | new script task from `:label`, copy of the variables   |
| `exit`                | stop this script                                       |
| anything else         | command line, `$?` = its return value                  |

- `$name` is replaced by the variable's text and `$?` by the last result,
  before the line runs. An unknown variable is empty.
- `"quoted words"` keep spaces in conditions, so `if "$x" == ""` works.
- Indentation is free.
- A failing command does not stop the script; check `$?`. Script errors
  (`divide by zero`, `missing end`, ...) print `-as line N: ...` and stop it.
- Kill a script task with `kill name`, like any task.

## Example

```
# startup
ttyS1 -b 115200
std ttyS1
pot -c -i 0 0.000244

run blink :blink

# show the pot on the LCD every 200 ms, warn above 80 %
:loop
get p pot
set pc $p * 100
if $pc > 80
    echo pot high $pc
end
write lcd $pc
wait 200
goto loop

:blink
while 1
    put led1 1
    wait 100
    put led1 0
    wait 900
end
```

Copy a file:

```
read t in
write out $t\n
```

## Use

```c
#include "../../argscript/argscript.h"

const char script[] = "...";                    // stays in flash

argscript_start("boot", script);                // from C
fsi_register_app("as", argscript_execute);      // shell: as NODE [NAME]
```

`as NODE` copies up to 127 bytes out of a node, for example a `touch` file, and
runs the copy.

## Size

Built for the PIC24 with XC16 at `-O0`, it takes about 3.7 KB of code, 0.5 KB of
strings and 192 B of static RAM. It also takes about 300 B of heap per running
script. Every limit is a `#define` in `argscript.h`:

| define                | default | limit                                    |
|-----------------------|---------|------------------------------------------|
| `ARGSCRIPT_VARS`      | 8       | variables per script                     |
| `ARGSCRIPT_NAME_SIZE` | 8       | variable name, including the `'\0'`      |
| `ARGSCRIPT_TEXT_SIZE` | 24      | variable text, including the `'\0'`      |
| `ARGSCRIPT_DEPTH`     | 6       | how deep `if`/`while`/`repeat` can nest  |
| `ARGSCRIPT_LINE_SIZE` | 96      | a line after `$` expansion               |
| `ARGSCRIPT_STEPS`     | 16      | lines run per task call                  |

Numbers are kept as text with 7 significant digits, so integer counters are
exact up to 9999999.

## Test

The host test runs the interpreter with fake nodes and a fake task loop:

```
cd argscript/test
cc -std=c99 -Wall -include fcntl.h -Dopen=t_open -Dread=t_read -Dwrite=t_write \
   -Dclose=t_close -Dsystem=t_system as_test.c ../argscript.c -o as_test && ./as_test
```
