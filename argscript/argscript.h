/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

#ifndef ARGSCRIPT_H
#define ARGSCRIPT_H

#include <stdint.h>

/**
 * @file argscript.h
 * @brief argscript: a tiny line-based script language for the kernel.
 *
 * Every line that is not a keyword is passed to system(), so a plain list of
 * shell commands is already a valid script. Each script runs as its own task
 * and never blocks: it runs a few lines per call, and `wait` sleeps through
 * the task's return value while the other tasks keep running.
 *
 * @code{.unparsed}
 *   # comment               blank lines and comments are skipped
 *   :name                   label
 *   set x 5                 x = "5"
 *   set x $a * 2            math: + - * / %
 *   set s any text          text when it is not "number op number"
 *   if a == b               == != < > <= >= compare numbers, else text
 *   if $x                   true when not empty and not 0
 *   else / end
 *   while cond ... end
 *   repeat n ... end
 *   break                   leave the innermost while / repeat
 *   goto name               jump to :name, drops the open blocks
 *   wait ms                 sleep, the other tasks keep running
 *   read x node             text from a node, $? = bytes read
 *   get x node              one float from a node (binary)
 *   put node value          one float to a node (binary)
 *   write node text         text to a node, \n is a new line
 *   run name :label         new script task from :label with a copy of the
 *                           variables
 *   exit                    stop this script
 *   anything else           command line for system(), $? = its result
 * @endcode
 *
 * $name is replaced by the variable's text, $? by the last result.
 * Indentation is free. Variables hold text; numbers are stored as text too.
 *
 * Example: blink an LED node 10 times.
 * @code{.unparsed}
 *   repeat 10
 *       write led 1
 *       wait 200
 *       write led 0
 *       wait 200
 *   end
 * @endcode
 *
 * The limits below can be changed by defining them before this header is
 * included (for example with -D on the compiler command line).
 */

#ifndef ARGSCRIPT_VARS
#define ARGSCRIPT_VARS 8 /**< Variables per script. */
#endif

#ifndef ARGSCRIPT_NAME_SIZE
#define ARGSCRIPT_NAME_SIZE 8 /**< Variable name length, with the 0. */
#endif

#ifndef ARGSCRIPT_TEXT_SIZE
#define ARGSCRIPT_TEXT_SIZE 24 /**< Variable text length, with the 0. */
#endif

#ifndef ARGSCRIPT_DEPTH
#define ARGSCRIPT_DEPTH 6 /**< Deepest nesting of if / while / repeat. */
#endif

#ifndef ARGSCRIPT_LINE_SIZE
#define ARGSCRIPT_LINE_SIZE 96 /**< Longest line after $ expansion. */
#endif

#ifndef ARGSCRIPT_STEPS
#define ARGSCRIPT_STEPS 16 /**< Lines run per task call. */
#endif

/**
 * @brief Starts a script as a new task.
 * @param name   Task name.
 * @param source Script text. It is not copied, so it must stay valid while
 *               the script runs (a string constant in flash is ideal).
 * @return 0, or -1 when the heap or the task table is full.
 */
int argscript_start(const char *name, const char *source);

/**
 * @brief The `as <node> [name]` command: runs the script stored in a node.
 *
 * The script (up to 127 bytes) is copied first, so the node may change while
 * it runs. The task is named @p name, or "as".
 *
 * @return 0, or -1 on error.
 */
int argscript_execute(void *args, int argc, char *argv[]);

#endif /* ARGSCRIPT_H */
