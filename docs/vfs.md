# vfs: the virtual file system

`vfs` is the system's node table and the tree over it: every device, command and file is a node,
reached by its bare name (`ttyS1`) or by a path (`/dev/ttyS1`, `/mnt/ee/config`), with real files
on SPI EEPROM, SPI SRAM, SPI flash and the PIC24FJ's own flash. (It replaces `fsi`, the flat node
table it grew from, which is gone.)

```text
/
├── bin/    the system's commands: ls, cd, mount, top, std, ed, as, ... (vfs_register_app("ls", ...))
├── dev/    devices: every plain node that can be read or written (ttyS1, led1, ain1, lcd, ee0, ...)
├── opt/    every other application: drn-fs, link, trg, pid, ... (vfs_register_app("/opt/trg", ...))
├── tmp/    RAM files (touch), or a file system on SPI SRAM / a RAM disk mounted over it
├── mnt/    mounted file systems: /mnt/ee on the SPI EEPROM (configs, scripts, firmware images)
├── boot/   (a mount) the startup script, rc
└── home/   (a mount) the user's scripts, run by name: `blink` runs /home/blink
```

The shell looks and works like a Linux one: the prompt is the working directory (`/dev$ `, `~$ `
in the home), `ls` lists plain names (`ls -p` marks the directories, `ls -l` shows the type and
`rwx` as Linux does), `cd` alone goes home and `~` is the home in any path.

## Sources and build options

| | |
|-|-|
| always | `vfs/node.c` (the table), `vfs/vfs.c` (the tree), `vfs/posix.c` (`open`, `read`, `write`, `close`, `lseek`, `system`), `bin/tmp.c` (RAM files) |
| always too | `vfs/bin.c`: the built-in commands in flash, `ls cat echo rm mv touch cd pwd mkdir cp mount umount df date uptime std kill top shell` (+ `crc mkfs` with tfs), the prompt, Tab on paths; `bin/date.c` the system time |
| with tfs | `vfs/tfs.c`, `vfs/tfsvfs.c` (tfs as the mount type "tfs"), drivers from `vfs/dev/`, optionally `vfs/ed.c` |
| a tiny `/boot` | `vfs/onefs.c`: the mount type "one", one file (rc) on a memory too small for tfs |
| `-DVFS_NO_TFS` | no tfs: for a small MCU with no memory to keep files on. Mounts of other types (the DRN router) still work; `VFS_DIRS_MAX` is 6 |
| `-DVFS_NODES_MAX=n` | the node table: 24 entries of 22 bytes on a PIC24, for devices, RAM files and commands of a project |
| `-DVFS_FDS_MAX=n` | handles open at the same time, devices and files: 8 of 10 bytes |

- A node created with `vfs_node_create("ttyS1", ...)` (a bare name) is in `/dev` when it can be
  read or written (a device keeps its own commands, such as `ttyS1 -b 115200`), else in `/bin`.
  A command registered with `vfs_register_app("ls", ...)` is in `/bin`, and comes before a
  built-in of that name.
- A bare name that isn't in the working directory is looked up among all plain nodes.
  `open("ttyS1", 0, 0)` works from any directory.
- `vfs_nodes_get()` returns the table. A node in a vfs directory such as `/opt` or `/tmp` has the
  directory in the first byte of its name (`0x80 | id`), so its name can be up to 14 characters
  instead of 15.
- Directories and mount points are in a table of their own (`VFS_DIRS_MAX` = 8, names up to 11
  characters, 16 bytes each), and handles in another (`VFS_FDS_MAX`), so neither takes a node.

## Mount types

A file system is mounted by its type, as on Linux: a `vfs_fs_type_t` (mount, info, dir, open,
read, write, seek, close, remove, rename, usage, source) registered with `vfs_fs_register()`.
`vfs_initialize()` registers tfs; other types register themselves, such as the DRN router file
system (drnfs, in the private drn repository):

```text
mount -t drn router /drn        the DRN router at /drn: a port is a file, /drn/.N its config
mount ee0 /mnt/ee               tfs on a block device (-t tfs is the default)
mount -f sram0 /tmp             tfs, formatted if the device holds none
df                              Device, Type, Size, Used, Avail, Mounted on
```

- A name that starts with `.` is hidden: `ls -a` shows it.
- `std` takes a file of a mounted file system as well as a node (`std /drn/250`: the shell over
  DRN); the file stays open while it is the standard streams.
- A read that returns 0 means nothing (yet); a write that returns 0 means not now, try later.

## How every board boots

The same way on every board: the board code registers the memory that holds `/boot` (its own
program flash, an I2C EEPROM, an SPI flash) as a block device, then `main` does

```c
vfs_boot("tfs", "flash0");                  // /boot; a board with no rc gets the recovery rc
shell_initialize();
vfs_bin_initialize();
...                                         // the project's other inits and apps
vfs_register_app("as", argscript_execute);
vfs_interpreter_set(argscript_execute);
system("/boot/rc");                         // last, before tasks_start()
```

`vfs_boot()` mounts the device at `/boot` (formatting one that holds no file system). A board
with no `/boot/rc` (a new board, or one whose rc was deleted) gets `VFS_BOOT_RC`, the same on
every board and kept in lets, not in the project:

```text
ttyS1 -b 9600
std ttyS1
```

a console on ttyS1 to write the board's own rc from (`ed /boot/rc ...`). An rc that is there
stays as it is; it holds all of the board's setup, the shell's port too (`std /drn/250`). If the
memory does not answer, `vfs_boot()` runs those two lines itself.

`/boot/rc` is run by argscript (`vfs_interpreter_set(argscript_execute)`): the file is read
whole, and it runs as its own task, so a plain list of commands works and so do `wait`, `if`
and loops. Without an interpreter set, a script file does not run.

- `"tfs"` for a memory of 1 KiB and more.
- `"one"` (vfs/onefs.c) for a smaller one: the device holds one file, `rc`, after a 6-byte
  header (magic, size, CRC-16), so a 256-byte EEPROM keeps a 250-byte rc. The file is edited in
  RAM and written back at close, header last; if a write is cut, the CRC fails, rc is missing
  and the board boots its default. Register it first: `vfs_fs_register(&vfs_one_type)`.

The memory's driver belongs to the board (the project's board folder), not to lets: it fills a
`vfs_blk_t` (blk.h), and anything with that API mounts.

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
    vfs_initialize();                           // the node table, /opt, /tmp, /mnt

    board_initialize();                         // device nodes
    ee25_init(&ee0, &ee_spi, 131072, 256);
    sram23_init(&ram1, &ram_spi, 131072);
    vfs_blk_register("ee0", &ee0.blk);          // /dev/ee0
    vfs_blk_register("ram1", &ram1.blk);        // /dev/ram1

    shell_initialize();
    vfs_bin_initialize();                       // after the shell: ls, cat, cd, ...
    vfs_interpreter_set(argscript_execute);
    system("/boot/rc");
    tasks_start();
}
```

```text
/boot/rc:
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
cat [-x] [path]         a file to its end; a node with one read. No path: stdin, with `<`
echo text               the words, one line
cp from to              any node or file to any file; `to` may be a directory
mv from to              rename / move within one file system, or within the node table
rm path                 a file, a RAM file or an empty directory (devices stay)
touch path              an empty file (a RAM file outside a file system)
crc path                size and CRC-32, and "ok" / "BAD" against the CRC stored when written
mount [-f] dev path     -f formats a device without a file system; no arguments: as df
umount path             refused while a file is open on it
mkfs dev [block] [n]    block size and most files; by default chosen by the device size
df, mount               every mount: device, size, used and free bytes, mount point
blink                   a script in /home runs by its name, as a task of that name
/tmp/t   ./x            any other script by its path (a file or a RAM file)
```

Scripts are run by the interpreter the project sets, usually argscript:

```c
vfs_register_app("as", argscript_execute);
vfs_interpreter_set(argscript_execute);     // blink: /home/blink as the task "blink"
```

Any command line can be redirected, with `system()` doing the work, so the command never sees it:

```text
ls -l /home > /tmp/list     stdout to a file or a node: replaces it (atomic on a file system)
echo text >> log            appends
cat < /home/blink           stdin from a file or a node
echo hi>f                   blanks are optional, and `"a > b"` in quotes is text
blink > log                 what a command starts stays on it, until the last task on it stops
shell -t < ttyS1 > ttyS1    a shell of its own: see kernel.md
```

`>` means "not a terminal": lines end in `\n`. `-DVFS_NO_REDIRECT` leaves this and the `shell` command
out, for a full flash. Differences from Linux are in [linux.md](linux.md).

The node `sys` (bin/sys.c, `sys_initialize()`) is a command line as a file: `echo df > sys` runs the
line when the file is closed, as a task, and `cat sys` gives what it printed.

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
Flags `0`, which is how drivers and apps open nodes, behave like a RAM file: reads start at the
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

`home/DEMO.X` (PIC24FJ128GL306) boots this way, and its apps have no C code: they are scripts.
`apps/nvfs.c` installs them on a new board and leaves existing ones as they are:

```text
/boot/rc         the network and the shell on it, the pot scale, a banner, cd
/home/blink      LED1 and LED2 take turns
/home/rgb        red, green, blue in turn, the pot sets the speed
/home/knob       the pot in percent on the LCD
/home/buttons    S1 lights LED1, S2 LED2
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

## On an SPI flash: sst25

An SST25VF flash (SST25VF016B and compatibles) erases in 4 KiB sectors, and a program only
clears bits. The `sst25` driver programs a write in place when it only clears bits. Any other
write rewrites the sector: the driver copies it, changed, to a 4 KiB scratch on another device
(an SPI SRAM), erases it and programs it back, which takes about 50 ms. A power loss during a
rewrite loses that sector.

To keep rewrites rare, a block device can have `discard()` (blk.h). tfs calls it on every block
it allocates, and `mkfs` uses 4 KiB blocks on such a device (64 KiB and more), so each block of
a file is one sector, erased once when it is taken. Only the sector that holds the directory
and the FAT is rewritten, about once per file closed and once per two blocks appended.

```c
static sram23_t ram;
static part_t scratch, tmp, boot, home;
static sst25_t sf0;

sram23_init(&ram, &ram_spi, 32768);                     // 23K256
part_init(&scratch, &ram.blk, 0, SST25_SECTOR);
part_init(&tmp, &ram.blk, SST25_SECTOR, 32768 - SST25_SECTOR);
sst25_init(&sf0, &sf_spi, 2097152, &scratch.blk);      // SST25VF016B: id 0xBF2541
part_init(&boot, &sf0.blk, 0, 65536);                   // partitions pass discard on
part_init(&home, &sf0.blk, 65536, 2097152 - 65536);
```

`drn_mcu_prj/DIRO6.X` (PIC24FJ256GB412) boots this way, with its shell on USB CDC.

## Size (XC16 2.10, -O1, PIC24FJ128GL306)

| file | flash | RAM |
|------|-------|-----|
| `vfs/tfs.c` | 5.3 KB (3.9 KB with `TFS_READONLY`) | - |
| `vfs/vfs.c` | 4.9 KB | 230 B (8 directories, 8 handles) |
| `vfs/node.c` / `vfs/posix.c` | 0.4 / 1.5 KB | 530 B (24 nodes) / 260 B |
| `vfs/bin.c` (21 commands, prompt, Tab) | 4.3 KB | - |
| `vfs/dev/flash24.c` / `part.c` | 1.5 / 0.2 KB | - |
| `vfs/dev/ee25.c` / `sram23.c` / `ramdisk.c` | 0.7 / 0.45 / 0.2 KB | - |
| `vfs/dev/sst25.c` | 2.4 KB (DIRO6.X) | - |

Build with `-DTFS_NO_CHECK` to drop the repair at mount.

## Tests

`make test` runs `tests/test_vfs.c`, which tests tfs on a RAM disk and on emulated SPI chips. The
emulated 25LC1024 answers byte by byte the way the real one does: it needs WREN, wraps writes
inside a page and reports WIP. The emulated 23LC1024 is tested the same way. The test also cuts
power during a replace, an append and a commit, and checks the whole tree through `system()` and
`open` / `read` / `write` / `lseek`. flash24 runs against two emulated flash pages that count
any double word programmed twice (ECC). An emulated SST25VF flash starts write protected,
only clears bits when it programs, and counts erases and any command it would ignore (AAI
misuse, a program without WREN), and the power is cut at every single erase and program
of a file replacement, checking that the old file or the new one is there each time. `make` also builds `build/lets-vfs-demo`, a shell on your
terminal with `/tmp` and `/mnt/ee` on RAM disks.
