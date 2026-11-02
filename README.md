# LETS - Linux-like Embedded Task Scheduler

A small, Linux-like kernel for 16-bit Microchip MCUs (PIC24, dsPIC33), written in C. Hardware and programs
alike are **nodes** in one table and are reached through the usual `open` / `read` / `write` /
`close` / `system` calls. A built-in **shell** and a tiny **script language** run on top, so a
board can be configured, tested and driven over a serial line without reflashing.

- **Made to fit**: LETS must run on 64 KB of program memory and 8 KB of RAM. `home/UDM.X`
  (PIC24FJ64GP202) is the reference: a change that does not fit there does not go in.
- **Cooperative tasks**: 16 run-to-completion tasks (`-DTASKS_MAX_COUNT`); a task returns how
  long to sleep (ms). No stacks per task, no locks, nothing blocks. The scheduler feeds the
  watchdog, so a task that never returns resets the board.
- **Everything is a node in a tree**: a UART, an ADC channel, a file or a command is a node of
  the virtual file system (vfs): devices in `/dev`, commands in `/bin`, project apps in
  `/opt`; `ls -l /dev` lists them, `std ttyS1` sends stdout there, and
  `ls > log` or `shell -t < ttyS1 > ttyS1` redirect as on Linux.
- **A shell for people and programs**: `std -t ttyS1` gives a terminal with line editing,
  Tab completion and history (use `screen`, PuTTY, ...); `std ttyS1` gives a program whole
  command lines and plain replies.
- **The C library, provided by vfs**: `open`, `read`, `write`, `close`, `lseek`,
  `system`, `malloc`, `free` (XC16 links them from the kernel). A fixed heap: when a task
  ends, what it owns is freed, its argument and what it got from `task_alloc()`.
- **System time**: `date`, `uptime` and the node `date`, so a network can set it. UTC, kept
  from the moment it is set until a reset.
- **File systems (optional)**: paths everywhere, and a tiny power-loss-safe file system for
  SPI EEPROM, SPI SRAM, SPI flash and the PIC24FJ's own flash (`mount ee0 /mnt/ee`) for
  configs, scripts and firmware images; file systems mount by type (`mount -t TYPE SOURCE PATH`).
  A small MCU builds without tfs (`VFS_NO_TFS`).
- **Shell and scripts**: `ls`, `cd`, `cat`, `echo`, `top`, `kill`, `mv`, `touch`, `rm`, and
  *argscript*, which runs the board's startup script `/boot/rc` (variables, `if`/`while`/`repeat`, `wait`, labels) for test and
  bring-up scripts.
- **Libraries**: packet framing with CRC (`byte8`), chunked transfers (`ddata`), PID control,
  ring queues, EMA and median filters, and portable signal / control code (plain C99, no
  kernel needed): 2nd-order IIR filters in float and Q14 fixed point, pulse / return
  detection, an avalanche-photodiode bias controller, a spiral search pattern.
- **Runs on a PC too**: the same sources build as a host program with a shell on your terminal,
  and every module has host tests.

## Try it on a PC

```sh
make            # builds build/lets-demo and build/lets-vfs-demo
./build/lets-demo
```

```text
kernel running on the host - try: ls /bin, top, echo hi > f, cat f
top
2		15824B		16384B		1.40		716
ID		NAME		RAM		DUR		PER
0		shell		0B		0.01ms		10.28ms
2		idle		0B		1.40ms		1.28ms
```

`top mem` shows the heap, the most stack ever used, and why the chip last reset (RCON).

`build/lets-demo` is built with `VFS_NO_TFS`, as a small MCU; `./build/lets-vfs-demo` is the
same with file systems: try `ls /`, `ls -l /dev`, `cd /mnt/ee`, `echo hi > f`, `cat f`, `df`.

`make test` runs all host tests (kernel, vfs, argscript, libraries; with AddressSanitizer).
`make docs` builds the API reference with Doxygen (into `build/docs/html`).

## On the MCU

Add `kernel/`, `bin/`, `vfs/node.c`, `vfs/vfs.c`, `vfs/posix.c`, `vfs/bin.c` and
`lib/args.c` to your MPLAB X project (XC16), plus any `opt/` node your board uses. Without
tfs define `VFS_NO_TFS`; with it add `vfs/tfs.c`, `vfs/tfsvfs.c` and the memory drivers from
`vfs/dev/`. Declare the heap once and boot:

```c
ALLOC_INIT_HEAP(4096);

int main(void)
{
    SYSTEM_Initialize();                            // your MCC / board init
    TMR2_SetInterruptHandler(system_time_callback); // a microsecond clock, see docs/porting.md

    task_initialize(system_time_get);
    vfs_initialize();                               // the node table, /opt, /tmp, /mnt
    board_initialize();                             // registers your device nodes
    shell_initialize();                             // the shell task
    vfs_bin_initialize();                           // ls, cat, cd, cp, mount, df, ...
    vfs_register_app("as", argscript_execute);
    vfs_interpreter_set(argscript_execute);         // script files run by argscript
    system("/boot/rc");                             // the board's startup, after the inits
    tasks_start();                                  // never returns
}
```

A task:

```c
static int blink(void * args, uint64_t system_time)
{
    LED_Toggle();
    return 500;                                     // run again in 500 ms
}

task_create("blink", blink, NULL);
```

A command (typed in the shell, or a line of a script):

```c
static int hello(void * args, int argc, char * argv[])
{
    printf("hello %s\n", argc > 1 ? argv[1] : "world");
    return 0;
}

vfs_register_app("hello", hello);              // /bin/hello; "/opt/hello" for /opt
```

A device: its driver fills a `vfs_handler_t` and calls
`vfs_node_create("ttyS1", false, args, &handlers)`; it is then `/dev/ttyS1`.

### With file systems

Add `vfs/tfs.c` and the memory drivers you need from `vfs/dev/` (25xx SPI EEPROM, 23xx SPI
SRAM, SST25VF SPI flash, PIC24FJ flash, RAM disk):

```c
vfs_initialize();
board_initialize();                             // as before, plus:
ee25_init(&ee0, &ee_spi, 131072, 256);          // 25LC1024
vfs_blk_register("ee0", &ee0.blk);              // /dev/ee0
shell_initialize();
vfs_bin_initialize();
system("mount ee0 /mnt/ee");
```

```c
int fd = open("/mnt/ee/config", O_WRONLY | O_CREAT | O_TRUNC, 0);
write(fd, &config, sizeof(config));
close(fd);                                      // the old config is replaced only now
```

See [docs/kernel.md](docs/kernel.md) (rules: tasks, memory, nodes, commands, style),
[docs/vfs.md](docs/vfs.md) (the virtual file system and tfs),
[docs/argscript.md](docs/argscript.md) (the script language) and
[docs/porting.md](docs/porting.md) (clock, heap, a new MCU or a PC).

## Layout

| Path | |
|------|-|
| `kernel/` | scheduler (`task`), heap (`alloc`) |
| `bin/` | the shell and its kernel commands (`top`, `kill`, RAM files) |
| `opt/` | optional nodes for a board: `gpa` (a GPIO pin, ADC or DAC channel as a node, in physical units), `tty` (a PIC24F UART that never makes a write wait, with an RS485 direction pin) |
| `vfs/` | the node table (`node`), the tree (`vfs`), the C-library calls (`posix`), the file commands (`bin`), the tiny file system (`tfs`), memory drivers (`dev/`: SPI EEPROM, SPI SRAM, SST25VF SPI flash, RAM disk, PIC24FJ flash, partitions) |
| `argscript/` | the script language |
| `lib/` | `args` (command-line parsing) and the portable libraries, see [lib/README.md](lib/README.md) |
| `port/host/` | the PC build |
| `examples/host/` | the PC demos (`main.c`, `vfs_main.c`) |
| `tests/` | kernel and vfs host tests |

## Code style

The [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html)
(100 columns), applied with `make format`. Every public function is documented in its header.

## Licence

Apache License 2.0, see [LICENSE](LICENSE). Contributions are welcome, see
[CONTRIBUTING.md](CONTRIBUTING.md) and the [code of conduct](CODE_OF_CONDUCT.md); security
issues: [SECURITY.md](SECURITY.md).
