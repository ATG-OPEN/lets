# vfs: the virtual file system

`vfs` is `fsi` with a directory tree, paths, and real files on SPI EEPROM and SPI SRAM. It runs on
the same node table as `fsi`. `fsi/fsi.c` and `fsi/fsi.h` are unchanged, and everything written for
fsi keeps working: drivers, apps, startup scripts, argscripts, `std ttyS1`, `echo x > lcd`.

```text
/
├── bin/    commands: every node that only executes (fsi_register_app)
├── dev/    devices: every plain fsi node that can be read or written (ttyS1, led1, ain1, lcd, ee0, ...)
├── opt/    project applications (vfs_register_app("/opt/trgmon", ...))
├── tmp/    RAM files (touch), or a file system on SPI SRAM / a RAM disk mounted over it
├── mnt/    mounted file systems: /mnt/ee on the SPI EEPROM (configs, scripts, firmware images)
├── boot/   (a mount) the startup script, rc
└── home/   (a mount) the user's scripts, run by name: `blink` runs /home/blink
```

The shell looks and works like a Linux one: the prompt is the working directory (`/dev$ `, `~$ `
in the home), `ls` lists plain names (`ls -p` marks the directories, `ls -l` shows the type and
`rwx` as Linux does), `cd` alone goes home and `~` is the home in any path.

## fsi or vfs

A project links one of the two:

| | fsi | vfs |
|-|-----|-----|
| sources | `fsi/fsi.c`, `kernel/posix.c` | `fsi/fsi.c`, `vfs/vfs.c`, `vfs/posix.c`, `vfs/tfs.c`, `vfs/bin.c`, drivers from `vfs/dev/` as needed |
| names | flat: `ttyS1`, `ls` | the same bare names, plus paths: `/dev/ttyS1`, `/mnt/ee/config` |
| files | `touch`: 126-byte RAM files | RAM files as well, plus files of any size on a mounted file system |
| commands | `cat echo kill ls mv rm top touch std reboot` | the same; `ls cat echo rm mv touch` take paths; plus `cd pwd mkdir cp crc mount umount mkfs df`; scripts by name |

Compatibility, both ways:

- A node created with `fsi_node_create()` is in `/dev` (a device keeps its own commands, such as
  `ttyS1 -b 115200`). A command registered with `fsi_register_app()` is in `/bin`. No code
  changes.
- A bare name that isn't in the working directory is looked up among all plain nodes, the same
  way fsi does it. `open("ttyS1", 0, 0)` works from any directory.
- Code that walks `fsi_nodes_get()` still sees every node. A node in a vfs directory such as
  `/opt` or `/tmp` has the directory in the first byte of its name (`0x80 | id`), so its name
  can be up to 14 characters instead of 15.
- vfs itself takes no place in the node table (40 entries, fixed by `fsi.h`): directories and
  mount points are in a table of its own (`VFS_DIRS_MAX` = 8, names up to 11 characters, 16
  bytes each), and an open file's handle comes after the node table (40 to 43 for
  `VFS_FILES_MAX` = 4 files open at once).
- In a vfs project the kernel's `ls`, `cat`, `echo` and `mv` (`bin/ls.c` ...) can be left out of
  the build, since vfs has its own: `shell_initialize()` references them weakly. That saves
  about 1.6 KB of flash at -O1.

## Boot

```c
ALLOC_INIT_HEAP(4096);

static ee25_t ee0;                              // 25LC1024 on SPI1
static sram23_t ram1;                           // 23LC1024 on SPI1

int main(void)
{
    SYSTEM_Initialize();
    TMR2_SetInterruptHandler(system_time_callback);

    task_initialize(system_time_get);
    fsi_initialize();
    vfs_initialize();                           // /opt, /tmp, /mnt

    board_initialize();                         // device nodes, as with fsi
    ee25_init(&ee0, &ee_spi, 131072, 256);
    sram23_init(&ram1, &ram_spi, 131072);
    vfs_blk_register("ee0", &ee0.blk);          // /dev/ee0
    vfs_blk_register("ram1", &ram1.blk);        // /dev/ram1

    shell_initialize((void *)startup);
    vfs_bin_initialize();                       // after the shell: replaces ls, cat, ...
    tasks_start();
}
```

```text
startup script:
    std ttyS1
    mount -f ram1 /tmp          # SRAM is empty after power-up: format it
    mount ee0 /mnt/ee           # the EEPROM keeps its files
    as /mnt/ee/boot.as          # the board's own script, editable without reflashing
```

The SPI bus is two functions from the board, `select(bool)` and `exchange(byte)`. See
`vfs/dev/spi.h`. Chips on one bus share `exchange` and each has its own `select`.

## Shell

```text
ls [-a] [-l] [-p] [path]  -l: type (d directory, - file, c device, b block device), rwx, size
cd [path]   pwd   mkdir path     cd alone: home (VFS_HOME, /home)
cat [-x] path           a file to its end; a node with one read, as before
echo text > path        replace (atomic on a file system); >> appends
cp from to              any node or file to any file; `to` may be a directory
mv from to              rename / move within one file system, or within the node table
rm path                 a file, a RAM file or an empty directory (devices stay)
touch path              an empty file (a RAM file outside a file system)
crc path                size and CRC-32, and "ok" / "BAD" against the CRC stored when written
mount [-f] dev path     -f formats a device without a file system; no arguments: as df
umount path             refused while a file is open on it
mkfs dev [block] [n]    block size and most files; by default chosen by the device size
df                      every mount: size, free bytes
blink                   a script in /home runs by its name, as a task of that name
/tmp/t   ./x            any other script by its path (a file or a RAM file)
```

Scripts are run by the interpreter the project sets, usually argscript:

```c
fsi_register_app("as", argscript_execute);
vfs_interpreter_set(argscript_execute);     // blink: /home/blink as the task "blink"
```

`top` shows a running script under its name and `kill blink` stops it. Words after the name are
not passed to the script.

## In C

```c
int fd = open("/mnt/ee/config", O_WRONLY | O_CREAT | O_TRUNC, 0);
write(fd, &config, sizeof(config));
close(fd);                                  // only now does the new config replace the old

fd = open("/mnt/ee/config", O_RDONLY, 0);
read(fd, &config, sizeof(config));
lseek(fd, 0, SEEK_SET);                     // 32-bit positions: images larger than 32 KiB
close(fd);
```

Flags: `O_RDONLY`, `O_WRONLY` and `O_RDWR` read and write at the position. `O_CREAT`, `O_EXCL` and
`O_APPEND` work as in POSIX. `O_TRUNC` writes a new copy that replaces the old file at `close()`.
Flags `0`, which is how all fsi code opens nodes, behave like an fsi RAM file: reads start at the
beginning and writes append. So `echo` and argscript `write` add to a file instead of overwriting
it.

There is also `vfs_mkdir`, `vfs_remove`, `vfs_rename`, `vfs_chdir`, `vfs_resolve`, `vfs_dir_read`,
`vfs_mount` and `vfs_umount`. See `vfs/vfs.h`. `tfs.h` is the file system on its own, with no
kernel needed.

## tfs, the file system

Flat (one directory per device), linked by a FAT, for byte-addressable memories: SPI EEPROM, SPI
SRAM, a RAM buffer. Names can be up to 15 characters.

```text
0                 superblock (16 bytes)
16                directory: entries x 32 bytes
16 + entries*32   FAT: u16 per block (FFFF free, FFFE last, else next block)
data              blocks
```

- **RAM**: nothing is cached. A mount uses 18 bytes, an open file 22. The repair at mount needs
  a temporary bitmap of blocks/8 bytes.
- **Power loss**: an `O_TRUNC` write goes to a new directory entry. The old file stays readable
  and is dropped only after the new one is committed at close. At mount, a half-written file is
  dropped, the newer of two copies wins, and lost blocks are freed. A config or a firmware image
  is therefore either the old one or the new one, never a mix.
- **EEPROM wear**: blocks are allocated round-robin, so rewrites spread over the chip. A directory
  entry is one 32-byte write that never crosses a page.
- **CRC-32** (zlib / IEEE) is kept while a file is written from start to end in order, which is
  how images are transferred. Any other write clears it. `crc` checks it, and a bootloader can
  too.
- **Speed**: an EEPROM page write takes about 5 ms. The `ee25` driver waits for it, and that is
  the only place where vfs blocks. Write from a task in chunks of one page or less. Writing a
  40 KB image to a 25LC1024 with 256-byte blocks takes about 4,900 page writes (data and FAT),
  roughly 25 s.
- **Block size**: chosen by the device size unless `mkfs` is given one: 32 bytes below 2 KiB,
  64 below 16 KiB, else 128; one directory entry per 256 bytes (4 .. 32). Use a multiple of the
  EEPROM page size for big files, and a smaller one for many small files.

Format parameters are chosen by `mkfs`. The layout is little-endian and byte-by-byte, so an
image built on a PC matches one built on the MCU.

## In the MCU's own flash: flash24

A board without an EEPROM keeps `/boot` and `/home` in two erase pages of its program flash
(PIC24FJ GA7 / GB4 / GL / GU: 1024-instruction pages, double-word programming, ECC). Each
instruction holds 16 bits of data, so the two pages give one 2044-byte device; `part` splits it:

```c
#define NV_ADDRESS 0x14800ul    /* two pages below the configuration page */

static const uint16_t nv_space[2 * 1024] __attribute__((space(prog), address(NV_ADDRESS), noload));
static const uint16_t nv_top[0x380] __attribute__((space(prog), address(0x15800), noload));

static flash24_t nv;
static part_t boot, home;

flash24_init(&nv, NV_ADDRESS);
part_init(&boot, &nv.blk, 0, 512);
part_init(&home, &nv.blk, 512, nv.blk.size - 512);
vfs_blk_register("nv0", &boot.blk);
vfs_blk_register("nv1", &home.blk);
vfs_mount("nv0", "/boot", true);
vfs_mount("nv1", "/home", true);
```

- The second array keeps the linker from putting code above the two pages: code on both sides
  of them would be out of reach of the short `rcall` branches ("PC Relative branch ... out of
  range").
- The pages take turns. A write that only fills erased double words is programmed in place.
  Any other write copies the page, changed, to the other page and programs that page's header
  last, so a power loss leaves the old page, and every write is all or nothing.
- A double word is programmed once between erases, as ECC requires.
- A copy erases one page and programs up to 511 double words, and the CPU stalls meanwhile
  (tens of ms). Saving a small file costs 10 to 15 copies, so about 1500 saves fit in the
  pages' 10 000 erase cycles. That is enough for configs and scripts, but not for logging.
- `noload` keeps the pages out of the hex. Program with `ipecmd ... -OP14800-157FF` (in MPLAB X:
  "Preserve Program Memory", range 14800-157ff) so that a firmware update keeps the files.

`home/DEMO.X` (PIC24FJ128GL306) boots this way, and its demo has no C code: it is scripts.
`apps/nvfs.c` installs them on a new board and leaves existing ones as they are:

```text
/boot/rc         console set up in main.c, then: pot scale, a banner, demo, cd
/home/demo       the launcher: S1 kills the running app and starts the next
/home/blink      LED1 and LED2 take turns
/home/rgb        red, green, blue in turn, the pot sets the speed
/home/knob       the pot in percent on the LCD
/home/buttons    S1 lights LED1, S2 LED2 (not in the launcher: it uses S1)
```

## Firmware images

Store the new program-memory image as a file, for example `/mnt/ee/fw.bin`, written with `O_TRUNC`
so that a half-received image never replaces a good one. At reset, a bootloader that links
`vfs/tfs.c` with `-DTFS_READONLY` (about 3.9 KB with XC16 -O1, no heap needed) mounts the EEPROM,
opens the image, checks its CRC against `tfs_info_t.crc`, and programs it:

```c
tfs_t fs;
tfs_file_t f;
tfs_info_t t;

if (tfs_mount(&fs, &ee0.blk) == 0 && tfs_stat(&fs, "fw.bin", &t) == 0 &&
    (t.attr & TFS_ATTR_CRC) && tfs_open(&fs, &f, "fw.bin", 0) == 0) {
    /* pass 1: tfs_read() in chunks, tfs_crc32() == t.crc ?
       pass 2: tfs_seek(&f, 0, 0), tfs_read(), erase and write flash pages */
}
```

The transport that fills the file is not part of vfs. Any link that can `write()` to a node can
do it, for example `cp` from a port, or a protocol task that writes chunks with `O_TRUNC` and
closes at the end.

## Size (XC16 2.10, -O1, PIC24FJ128GL306)

| file | flash | RAM |
|------|-------|-----|
| `vfs/tfs.c` | 5.3 KB (3.9 KB with `TFS_READONLY`) | - |
| `vfs/vfs.c` | 4.9 KB | 150 B (8 directories, 4 open files) |
| `vfs/posix.c` | 1.5 KB (`kernel/posix.c`: 0.9 KB) | 260 B (the same as kernel/posix.c) |
| `vfs/bin.c` (15 commands, prompt, Tab) | 4.3 KB, minus 1.6 KB for the kernel commands left out | - |
| `vfs/dev/flash24.c` / `part.c` | 1.5 / 0.2 KB | - |
| `vfs/dev/ee25.c` / `sram23.c` / `ramdisk.c` | 0.7 / 0.45 / 0.2 KB | - |

Leave out `bin.c` if a project only needs files from C. Build with `-DTFS_NO_CHECK` to drop the
repair at mount.

## Tests

`make test` runs `tests/test_vfs.c`, which tests tfs on a RAM disk and on emulated SPI chips. The
emulated 25LC1024 answers byte by byte the way the real one does: it needs WREN, wraps writes
inside a page and reports WIP. The emulated 23LC1024 is tested the same way. The test also cuts
power during a replace, an append and a commit, and checks the whole tree through `system()` and
`open` / `read` / `write` / `lseek`. flash24 runs against two emulated flash pages that count
any double word programmed twice (ECC), and the power is cut at every single erase and program
of a file replacement, checking that the old file or the new one is there each time. `make` also builds `build/lets-vfs-demo`, a shell on your
terminal with `/tmp` and `/mnt/ee` on RAM disks.
