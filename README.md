# LETS - Linux-like Embedded Task Scheduler

A small, Linux-like kernel for 16-bit Microchip MCUs (PIC24, dsPIC33), written in C. Hardware and programs
alike are **nodes** in one table and are reached through the usual `open` / `read` / `write` /
`close` / `system` calls. A built-in **shell** and a tiny **script language** run on top, so a
board can be configured, tested and driven over a serial line without reflashing.

- **Cooperative tasks**: up to 32 run-to-completion tasks; a task returns how long to sleep
  (ms). No stacks per task, no locks, nothing blocks.
- **Everything is a node**: a UART, an ADC channel, a file in RAM or a command is a name in
  the node table; `ls` lists them, `std ttyS1` sends stdout there.
- **The C library, provided by the kernel**: `open`, `read`, `write`, `close`, `lseek`,
  `system`, `malloc`, `free` (XC16 links them from the kernel). A fixed heap with per-task
  ownership: when a task ends, its memory is freed.
- **Virtual file system (optional)**: `/bin`, `/dev`, `/app`, `/tmp`, `/mnt`, paths everywhere,
  and a tiny power-loss-safe file system for SPI EEPROM / SPI SRAM (`mount ee0 /mnt/ee`) for
  configs, scripts and firmware images. It is built on the unchanged node table, so fsi code
  runs on it as it is.
- **Shell and scripts**: `ls`, `cat`, `echo`, `top`, `kill`, `mv`, `touch`, `rm`, a startup
  script, and *argscript* (variables, `if`/`while`/`repeat`, `wait`, labels) for test and
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
kernel running on the host - try: ls, top, echo hi, touch f, cat f
top
2		15824B		16384B		1.40		716
ID		NAME		RAM		DUR		PER
0		shell		0B		0.01ms		10.28ms
2		idle		0B		1.40ms		1.28ms
```

`./build/lets-vfs-demo` is the same with the virtual file system: try `ls /`, `ls -l /dev`,
`cd /mnt/ee`, `echo hi > f`, `cat f`, `df`.

`make test` runs all host tests (kernel, vfs, argscript, libraries; with AddressSanitizer).
`make docs` builds the API reference with Doxygen (into `build/docs/html`).

## On the MCU

Add `kernel/`, `bin/`, `fsi/` and `lib/args.c` to your MPLAB X project (XC16), plus any
`opt/` node your board uses, declare the heap once and boot:

```c
ALLOC_INIT_HEAP(4096);

int main(void)
{
    SYSTEM_Initialize();                            // your MCC / board init
    TMR2_SetInterruptHandler(system_time_callback); // a microsecond clock, see docs/porting.md

    task_initialize(system_time_get);
    fsi_initialize();
    board_initialize();                             // registers your device nodes
    shell_initialize((void *)startup);              // shell task + startup script
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

fsi_register_app("hello", hello);
```

### With the virtual file system

Use `vfs/posix.c` instead of `kernel/posix.c`, add `vfs/vfs.c`, `vfs/tfs.c`, `vfs/bin.c` and the
memory drivers you need from `vfs/dev/` (25xx SPI EEPROM, 23xx SPI SRAM, RAM disk):

```c
fsi_initialize();
vfs_initialize();                               // /app, /tmp, /mnt
board_initialize();                             // as before, plus:
ee25_init(&ee0, &ee_spi, 131072, 256);          // 25LC1024
vfs_blk_register("ee0", &ee0.blk);              // /dev/ee0
shell_initialize((void *)startup);              // "mount ee0 /mnt/ee\nas /mnt/ee/boot.as\n"
vfs_bin_initialize();                           // ls, cat, cp, cd, mount, df, ... with paths
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
| `kernel/` | scheduler (`task`), heap (`alloc`), the C-library calls (`posix`) |
| `bin/` | shell and its commands |
| `fsi/` | the node table |
| `opt/` | optional nodes for a board: `gpa` (a GPIO pin, ADC or DAC channel as a node, in physical units) |
| `vfs/` | the virtual file system on the node table (`vfs`, `posix`, `bin`), the tiny file system (`tfs`), memory drivers (`dev/`) |
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
