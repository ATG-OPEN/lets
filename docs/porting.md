# Porting

The kernel needs three things from a target: a microsecond clock, a heap and somewhere for
stdout to go. Everything else is plain C.

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
(64 blocks, 2-byte alignment; 8 in a host build). `alloc_map()` / `alloc_info()` print it; `top` shows each
task's share.

## stdout and the shell

Handles 0 / 1 / 2 are stdin / stdout / stderr. `std <node>` points them at a node (a UART, a
network port); the shell reads its commands from handle 0. `printf` ends up in `write(1, ...)`,
which is line buffered (128 bytes).

## XC16

The kernel's `open`, `read`, `write`, `close`, `lseek`, `system`, `malloc` and `free` are placed
in `.libc.*` sections, so XC16 links them instead of its own library versions. `reboot` uses
the `reset` instruction.

## A PC (host build)

`port/host/prelude.h` is forced in front of every kernel source. It reads the system headers
first, then renames the kernel's C-library functions to `k_open`, `k_read`, ... so they do not
clash with the system's. `port/host/host_tty.c` (compiled without the prelude) is the terminal
as a node, with non-blocking reads. See `examples/host/main.c` and the `Makefile`.

## Another MCU

Nothing in `kernel/`, `fsi/` or `lib/` touches registers. Provide the clock, make sure your
toolchain links the kernel's C-library functions (or rename them as the host build does), and
write the board's device nodes.
