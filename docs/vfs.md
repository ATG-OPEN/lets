# vfs: the virtual file system

`vfs` is `fsi` with a directory tree, paths, and real files on SPI EEPROM and SPI SRAM. It runs on
the same node table as `fsi`. `fsi/fsi.c` and `fsi/fsi.h` are unchanged, and everything written for
fsi keeps working: drivers, apps, startup scripts, argscripts, `std ttyS1`, `echo x > lcd`.

```text
/
├── bin/    commands: every node that only executes (fsi_register_app)
├── dev/    devices: every plain fsi node that can be read or written (ttyS1, led1, ain1, lcd, ee0, ...)
├── app/    project applications (vfs_register_app("/app/trgmon", ...))
├── tmp/    RAM files (touch), or a file system on SPI SRAM / a RAM disk mounted over it
└── mnt/    mounted file systems: /mnt/ee on the SPI EEPROM (configs, scripts, firmware images)
```

## fsi or vfs

A project links one of the two:

| | fsi | vfs |
|-|-----|-----|
| sources | `fsi/fsi.c`, `kernel/posix.c` | `fsi/fsi.c`, `vfs/vfs.c`, `vfs/posix.c`, `vfs/tfs.c`, `vfs/bin.c`, drivers from `vfs/dev/` as needed |
| names | flat: `ttyS1`, `ls` | the same bare names, plus paths: `/dev/ttyS1`, `/mnt/ee/config` |
| files | `touch`: 126-byte RAM files | RAM files as well, plus files of any size on a mounted file system |
| commands | `cat echo kill ls mv rm top touch std reboot` | the same; `ls cat echo rm mv touch` take paths; plus `cd pwd mkdir cp crc mount umount mkfs df` |

Compatibility, both ways:

- A node created with `fsi_node_create()` is in `/dev` (a device keeps its own commands, such as
  `ttyS1 -b 115200`). A command registered with `fsi_register_app()` is in `/bin`. No code
  changes.
- A bare name that isn't in the working directory is looked up among all plain nodes, the same
  way fsi does it. `open("ttyS1", 0, 0)` works from any directory.
- Code that walks `fsi_nodes_get()` still sees every node. A node in a vfs directory such as
  `/app` or `/tmp` has the directory in the first byte of its name (`0x80 | index`), so its name
  can be up to 14 characters instead of 15.
- An open file is a hidden node, and it goes away on `close()`. Its name starts with `0xFF`.

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
    vfs_initialize();                           // /app, /tmp, /mnt

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
ls [-a] [-l] [path]     -l: kind (d dir, m mount, f file, n node) / rwxs / size / name
cd [path]   pwd   mkdir path
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
```

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

## Size (XC16 2.10, -O1, PIC24FJ64GU205)

| file | flash | RAM |
|------|-------|-----|
| `vfs/tfs.c` | 5.2 KB (3.9 KB with `TFS_READONLY`) | - |
| `vfs/vfs.c` | 4.6 KB | 50 B |
| `vfs/posix.c` | 1.2 KB (`kernel/posix.c`: 0.9 KB) | 258 B (the same as kernel/posix.c) |
| `vfs/bin.c` (15 commands) | 3.6 KB | - |
| `vfs/dev/ee25.c` / `sram23.c` / `ramdisk.c` | 0.7 / 0.45 / 0.2 KB | - |

Leave out `bin.c` if a project only needs files from C. Build with `-DTFS_NO_CHECK` to drop the
repair at mount.

## Tests

`make test` runs `tests/test_vfs.c`, which tests tfs on a RAM disk and on emulated SPI chips. The
emulated 25LC1024 answers byte by byte the way the real one does: it needs WREN, wraps writes
inside a page and reports WIP. The emulated 23LC1024 is tested the same way. The test also cuts
power during a replace, an append and a commit, and checks the whole tree through `system()` and
`open` / `read` / `write` / `lseek`. `make` also builds `build/lets-vfs-demo`, a shell on your
terminal with `/tmp` and `/mnt/ee` on RAM disks.
