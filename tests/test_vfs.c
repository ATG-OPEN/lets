/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * vfs host test: the tiny file system on a RAM disk and on emulated SPI chips (25LC1024 EEPROM,
 * 23LC1024 SRAM, byte by byte as the real ones answer), power loss, and the tree over the fsi
 * table: paths, bare fsi names, /bin, /dev, /app, /tmp, mounts, open / read / write / lseek and
 * the shell commands.
 */
#include "../kernel/task.h"
#include "../kernel/alloc.h"
#include "../bin/shell.h"
#include "../fsi/fsi.h"
#include "../vfs/vfs.h"
#include "../vfs/dev/ramdisk.h"
#include "../vfs/dev/ee25.h"
#include "../vfs/dev/sram23.h"
#include "../vfs/dev/flash24.h"
#include "../vfs/dev/part.h"

ALLOC_INIT_HEAP(8192);

static int failures;
#define CHECK(c)                                                            \
	do {                                                                \
		if (!(c)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			failures++;                                         \
		}                                                           \
	} while (0)

/* -------------------------------------------------------------------------- */
/* an SPI memory chip, emulated: 25xx EEPROM (WREN, page wrap, WIP) or 23xx SRAM */

#define CHIP_SIZE 131072u

typedef struct {
	uint8_t mem[CHIP_SIZE];
	bool eeprom;
	uint16_t page;
	bool selected, wel;
	int busy;  /* status reads until a page write is done */
	int count; /* bytes since select */
	uint8_t cmd;
	uint32_t addr, base;
	unsigned writes; /* page writes */
} chip_t;

static chip_t ee_chip = {.eeprom = true, .page = 256};
static chip_t ram_chip = {.eeprom = false};
static chip_t *bus_chip;

static uint8_t chip_exchange(chip_t *c, uint8_t b)
{
	int n = c->count++;

	if (n == 0) {
		c->cmd = b;
		c->addr = 0;
		if (c->eeprom && b == 0x06) {
			c->wel = true;
		}
		return 0xFF;
	}

	if (c->cmd == 0x05) { /* RDSR */
		if (c->busy > 0) {
			c->busy--;
			return 0x03;
		}
		return c->wel ? 0x02 : 0x00;
	}

	if (c->cmd == 0x01) { /* WRMR */
		return 0xFF;
	}

	if (n <= 3) { /* 3 address bytes */
		c->addr = (c->addr << 8) | b;
		c->base = c->addr;
		return 0xFF;
	}

	uint32_t a = c->addr % CHIP_SIZE;

	if (c->cmd == 0x03) {
		c->addr++;
		return c->mem[a];
	}

	if (c->cmd == 0x02 && (!c->eeprom || c->wel)) {
		c->mem[a] = b;
		if (c->eeprom) { /* the address wraps inside the page, as on the chip */
			uint32_t page = c->base & ~(uint32_t)(c->page - 1u);
			c->addr = page + ((c->addr + 1 - page) % c->page);
		} else {
			c->addr++;
		}
	}

	return 0xFF;
}

static void chip_select(chip_t *c, bool on)
{
	if (!on && c->selected && c->eeprom && c->cmd == 0x02 && c->count > 4 && c->wel) {
		c->wel = false;
		c->busy = 3;
		c->writes++;
	}
	c->selected = on;
	c->count = 0;
}

static void ee_select(bool on)
{
	bus_chip = &ee_chip;
	chip_select(&ee_chip, on);
}

static void ram_select(bool on)
{
	bus_chip = &ram_chip;
	chip_select(&ram_chip, on);
}

static uint8_t bus_exchange(uint8_t b)
{
	return chip_exchange(bus_chip, b);
}

static const vfs_spi_t ee_spi = {ee_select, bus_exchange};
static const vfs_spi_t ram_spi = {ram_select, bus_exchange};

/* -------------------------------------------------------------------------- */

static uint8_t pattern(uint32_t i)
{
	return (uint8_t)(i * 7u + (i >> 8));
}

static int fill_file(tfs_t *fs, const char *name, uint32_t size, uint8_t mode)
{
	tfs_file_t f;
	uint8_t buf[50];

	if (tfs_open(fs, &f, name, mode) < 0) {
		return -1;
	}
	for (uint32_t i = 0; i < size;) {
		unsigned n = (size - i < sizeof(buf)) ? (unsigned)(size - i) : sizeof(buf);
		for (unsigned k = 0; k < n; k++)
			buf[k] = pattern(i + k);
		if (tfs_write(&f, buf, n) != (int)n)
			return -1;
		i += n;
	}
	return tfs_close(&f);
}

static bool check_file(tfs_t *fs, const char *name, uint32_t size)
{
	tfs_file_t f;
	uint8_t buf[37];
	uint32_t i = 0;
	int n;

	if (tfs_open(fs, &f, name, 0) < 0 || f.size != size)
		return false;
	while ((n = tfs_read(&f, buf, sizeof(buf))) > 0) {
		for (int k = 0; k < n; k++, i++)
			if (buf[k] != pattern(i))
				return false;
	}
	return i == size;
}

/* the file system itself, on any device */
static void test_tfs(vfs_blk_t *blk, uint16_t bsize)
{
	tfs_t fs;
	tfs_info_t t;
	tfs_file_t f;

	CHECK(tfs_format(blk, bsize, 16) == 0);
	CHECK(tfs_mount(&fs, blk) == 0);
	uint32_t empty = tfs_free(&fs);
	CHECK(empty > 0 && empty <= blk->size);

	/* several blocks, read back, CRC as zlib's */
	CHECK(fill_file(&fs, "image", 5000, TFS_WRITE | TFS_CREATE | TFS_TRUNC) == 0);
	CHECK(check_file(&fs, "image", 5000));
	CHECK(tfs_stat(&fs, "image", &t) == 0 && (t.attr & TFS_ATTR_CRC));
	uint8_t all[5000];
	for (int i = 0; i < 5000; i++)
		all[i] = pattern(i);
	CHECK(t.crc == tfs_crc32(0, all, 5000));
	CHECK(tfs_crc32(0, "123456789", 9) == 0xCBF43926ul);
	CHECK(tfs_free(&fs) == empty - ((5000 + bsize - 1) / bsize) * bsize);

	/* seek and read in the middle */
	uint8_t b[10];
	CHECK(tfs_open(&fs, &f, "image", 0) == 0);
	CHECK(tfs_seek(&f, 4321, 0) == 4321);
	CHECK(tfs_read(&f, b, 10) == 10 && b[0] == pattern(4321) && b[9] == pattern(4330));
	CHECK(tfs_seek(&f, -10, 2) == 4990);
	CHECK(tfs_read(&f, b, 10) == 10 && tfs_read(&f, b, 10) == 0);
	CHECK(tfs_seek(&f, 1, 2) == -1);
	CHECK(tfs_close(&f) == 0);

	/* append keeps the CRC going; an overwrite clears it */
	CHECK(tfs_open(&fs, &f, "image", TFS_WRITE | TFS_APPEND) == 0);
	CHECK(tfs_write(&f, "xyz", 3) == 3 && tfs_close(&f) == 0);
	CHECK(tfs_stat(&fs, "image", &t) == 0 && t.size == 5003 && (t.attr & TFS_ATTR_CRC));
	uint8_t tail[3] = {'x', 'y', 'z'};
	CHECK(t.crc == tfs_crc32(tfs_crc32(0, all, 5000), tail, 3));
	CHECK(tfs_open(&fs, &f, "image", TFS_WRITE) == 0);
	CHECK(tfs_write(&f, "A", 1) == 1 && tfs_close(&f) == 0);
	CHECK(tfs_stat(&fs, "image", &t) == 0 && t.size == 5003 && !(t.attr & TFS_ATTR_CRC));

	/* names, rename, remove, listing */
	CHECK(tfs_open(&fs, &f, "image", TFS_CREATE | TFS_EXCL) == -1);
	CHECK(tfs_open(&fs, &f, "nothing", 0) == -1);
	CHECK(tfs_open(&fs, &f, "a/b", TFS_CREATE) == -1);
	CHECK(tfs_open(&fs, &f, "sixteen-chars-xx", TFS_CREATE) == -1);
	CHECK(fill_file(&fs, "cfg", 10, TFS_WRITE | TFS_CREATE) == 0);
	CHECK(tfs_rename(&fs, "cfg", "config") == 0 && tfs_stat(&fs, "cfg", &t) == -1);
	uint8_t idx = 0;
	int count = 0;
	while (tfs_dir(&fs, &idx, &t) == 0)
		count++;
	CHECK(count == 2);
	CHECK(tfs_remove(&fs, "image") == 0 && tfs_remove(&fs, "image") == -1);
	CHECK(tfs_remove(&fs, "config") == 0);
	CHECK(tfs_free(&fs) == empty);

	/* full: a short write, then nothing; the space comes back */
	CHECK(tfs_open(&fs, &f, "big", TFS_WRITE | TFS_CREATE) == 0);
	uint8_t chunk[64];
	memset(chunk, 0x5A, sizeof(chunk));
	uint32_t total = 0;
	int n;
	while ((n = tfs_write(&f, chunk, sizeof(chunk))) > 0)
		total += n;
	CHECK(total == empty && tfs_free(&fs) == 0);
	CHECK(tfs_close(&f) == 0 && tfs_remove(&fs, "big") == 0 && tfs_free(&fs) == empty);

	/* the directory is full */
	char name[8];
	for (int i = 0; i < 16; i++) {
		snprintf(name, sizeof(name), "f%d", i);
		CHECK(fill_file(&fs, name, 1, TFS_WRITE | TFS_CREATE) == 0);
	}
	CHECK(fill_file(&fs, "one-more", 1, TFS_WRITE | TFS_CREATE) == -1);
	for (int i = 0; i < 16; i++) {
		snprintf(name, sizeof(name), "f%d", i);
		CHECK(tfs_remove(&fs, name) == 0);
	}

	/* a mount sees the same files */
	CHECK(fill_file(&fs, "keep", 777, TFS_WRITE | TFS_CREATE) == 0);
	tfs_t again;
	CHECK(tfs_mount(&again, blk) == 0 && check_file(&again, "keep", 777));
}

/* power loss: nothing written after the last close counts, nothing leaks */
static void test_power_loss(vfs_blk_t *blk)
{
	tfs_t fs;
	tfs_file_t f;
	tfs_info_t t;

	CHECK(tfs_format(blk, 64, 8) == 0 && tfs_mount(&fs, blk) == 0);
	uint32_t empty = tfs_free(&fs);
	CHECK(fill_file(&fs, "config", 300, TFS_WRITE | TFS_CREATE | TFS_TRUNC) == 0);
	uint32_t used = tfs_free(&fs);

	/* a replacement cut off before close: the old file is still there */
	CHECK(tfs_open(&fs, &f, "config", TFS_WRITE | TFS_CREATE | TFS_TRUNC) == 0);
	uint8_t junk[200];
	memset(junk, 0xEE, sizeof(junk));
	CHECK(tfs_write(&f, junk, sizeof(junk)) == 200);
	CHECK(check_file(&fs, "config", 300)); /* readers see the old one meanwhile */
	CHECK(tfs_mount(&fs, blk) == 0);       /* "power loss" */
	CHECK(check_file(&fs, "config", 300));
	CHECK(tfs_free(&fs) == used);

	/* the replacement closed: the new one, the old blocks free */
	CHECK(fill_file(&fs, "config", 100, TFS_WRITE | TFS_CREATE | TFS_TRUNC) == 0);
	CHECK(check_file(&fs, "config", 100));
	CHECK(tfs_free(&fs) == empty - 2 * 64);

	/* an append cut off before close: the size committed before, no leaked blocks */
	CHECK(tfs_open(&fs, &f, "config", TFS_WRITE | TFS_APPEND) == 0);
	CHECK(tfs_write(&f, junk, sizeof(junk)) == 200);
	CHECK(tfs_mount(&fs, blk) == 0);
	CHECK(check_file(&fs, "config", 100) && tfs_free(&fs) == empty - 2 * 64);

	/* both old and new committed (cut between the two steps of close): the newer wins */
	CHECK(tfs_open(&fs, &f, "config", TFS_WRITE | TFS_CREATE | TFS_TRUNC) == 0);
	CHECK(tfs_write(&f, "new", 3) == 3);
	f.mode &= (uint8_t)~0x20u; /* close without dropping the old entry */
	CHECK(tfs_close(&f) == 0);
	CHECK(tfs_mount(&fs, blk) == 0);
	CHECK(tfs_stat(&fs, "config", &t) == 0 && t.size == 3);
	uint8_t idx = 0;
	int count = 0;
	while (tfs_dir(&fs, &idx, &t) == 0)
		count++;
	CHECK(count == 1 && tfs_free(&fs) == empty - 64);

	/* not a file system */
	memset(junk, 0, sizeof(junk));
	CHECK(blk->write(blk, 0, junk, 16) == 0 && tfs_mount(&fs, blk) == -1);
}

/* -------------------------------------------------------------------------- */
/* the tree */

static char sink[512];
static unsigned sink_len;

static int sink_write(void *args, void *buffer, unsigned int length)
{
	if (sink_len + length < sizeof(sink)) {
		memcpy(sink + sink_len, buffer, length);
		sink_len += length;
		sink[sink_len] = '\0';
	}
	return (int)length;
}

static fsi_handler_t sink_node = {.write_handler = sink_write};

/* a terminal for the shell: read() returns the keys given to type(), the echo goes to screen */
static const char *keys = "";
static char screen[512];
static unsigned screen_len;

static int term_read(void *args, void *buffer, unsigned int length)
{
	(void)args;
	unsigned n = (unsigned)strlen(keys);
	if (n > length)
		n = length;
	memcpy(buffer, keys, n);
	keys += n;
	return (int)n;
}

static int term_write(void *args, void *buffer, unsigned int length)
{
	(void)args;
	if (screen_len + length < sizeof(screen)) {
		memcpy(screen + screen_len, buffer, length);
		screen_len += length;
		screen[screen_len] = '\0';
	}
	return (int)length;
}

static fsi_handler_t term_node = {.read_handler = term_read, .write_handler = term_write};

int shell_loop(void *args, uint64_t system_time); /* bin/shell.c: the shell task */

/* types @p text (then Ctrl-U, so the line is empty again) and returns what was shown */
static const char *type(const char *text)
{
	screen_len = 0;
	screen[0] = '\0';
	keys = text;
	while (*keys != '\0')
		shell_loop(NULL, 0);

	static char shown[sizeof(screen)];
	strcpy(shown, screen);
	keys = "\x15";
	shell_loop(NULL, 0);
	return shown;
}

static int last_argc;
static int cmd_probe(void *args, int argc, char *argv[]);

static char script_path[64], script_name[16];

static int cmd_script(void *args, int argc, char *argv[])
{
	if (argc != 3)
		return -1;
	strcpy(script_path, argv[1]);
	strcpy(script_name, argv[2]);
	return 5;
}

static int gpio_value;

static int gpio_read(void *args, void *buffer, unsigned int length)
{
	(void)args;
	memcpy(buffer, &gpio_value, sizeof(gpio_value));
	return (int)length;
}

static fsi_handler_t gpio_node = {
	.read_handler = gpio_read, .write_handler = sink_write, .execute_handler = cmd_probe};

/* true when directory @p path lists @p name */
static bool dir_has(const char *path, const char *name)
{
	vfs_path_t d;
	vfs_dirent_t e;
	uint16_t index = 0;

	if (vfs_resolve(path, &d) != 0 || d.kind != VFS_DIR)
		return false;
	while (vfs_dir_read(d.dir, &index, &e) == 0)
		if (strcmp(e.name, name) == 0)
			return true;
	return false;
}
static int cmd_probe(void *args, int argc, char *argv[])
{
	last_argc = argc;
	return 7;
}

/* what a command prints with printf (the PC's stdout here) */
static const char *capture(const char *command)
{
	static char text[512];
	static int saved = -1; /* the real stdout, kept (close() is the kernel's here) */
	FILE *f = tmpfile();

	if (saved < 0)
		saved = dup(1);

	fflush(stdout);
	dup2(fileno(f), 1);
	system(command);
	fflush(stdout);
	dup2(saved, 1);
	rewind(f);
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	text[n] = '\0';
	fclose(f);
	return text;
}

static int read_all(const char *path, char *buf, unsigned size)
{
	int fd = open(path, 0, 0);
	if (fd < 0)
		return -1;
	int n = read(fd, buf, size - 1);
	close(fd);
	buf[n < 0 ? 0 : n] = '\0';
	return n;
}

static void test_tree(void)
{
	static uint8_t ram0_mem[4096];
	static ramdisk_t ram0;
	static ee25_t ee0;
	static sram23_t ram1;
	vfs_path_t p;
	char buf[128];

	fsi_initialize();
	CHECK(vfs_initialize() == 0);
	shell_initialize(NULL);
	CHECK(vfs_bin_initialize() == 0);

	/* fsi as before: a plain node is in /dev and found by its bare name from anywhere */
	CHECK(fsi_node_create("ttyS1", false, NULL, &sink_node) == 0);
	CHECK(vfs_resolve("/dev/ttyS1", &p) == 0 && p.kind == VFS_NODE);
	int tty = p.node;
	CHECK(open("ttyS1", 0, 0) == tty && open("/dev/ttyS1", 0, 0) == tty);
	CHECK(open("/bin/ttyS1", 0, 0) == -1);
	CHECK(system("std ttyS1") == 0);

	/* commands are in /bin: fsi_register_app, the kernel's and the vfs ones */
	CHECK(fsi_register_app("probe", cmd_probe) == 0);
	CHECK(vfs_resolve("/bin/probe", &p) == 0 && p.kind == VFS_NODE);
	CHECK(vfs_resolve("/bin/ls", &p) == 0 && p.kind == VFS_NODE);
	CHECK(system("probe a b") == 7 && last_argc == 3);
	CHECK(system("/bin/probe a") == 7 && last_argc == 2);
	CHECK(system("ttyS1") == -1 && system("nothing") == -1);

	/* /app */
	CHECK(vfs_register_app("/app/tool", cmd_probe) == 0);
	CHECK(vfs_resolve("/app/tool", &p) == 0 && p.kind == VFS_NODE);
	CHECK(vfs_resolve("/bin/tool", &p) == 0 && p.kind == VFS_NONE);
	CHECK(system("/app/tool x y z") == 7 && last_argc == 4);
	CHECK(system("tool 1") == 7 && last_argc == 2);

	/* directories */
	CHECK(system("cd /tmp") == 0 && vfs_dir_path(vfs_cwd(), buf, sizeof(buf)) == 0);
	CHECK(strcmp(buf, "/tmp") == 0);
	CHECK(system("mkdir logs") == 0 && system("cd logs") == 0);
	CHECK(vfs_dir_path(vfs_cwd(), buf, sizeof(buf)) == 0 && strcmp(buf, "/tmp/logs") == 0);
	CHECK(system("cd ../..") == 0 && vfs_cwd() == VFS_ROOT);
	CHECK(system("cd /nothing") == -1);
	CHECK(vfs_mkdir("/tmp/logs") == -1 && vfs_mkdir("/bin/x") == -1);
	CHECK(vfs_resolve("/tmp/../dev/./ttyS1", &p) == 0 && p.node == tty);

	/* RAM files outside a mount: the kernel's touch, now with paths */
	CHECK(system("touch /tmp/logs/a") == 0);
	CHECK(system("echo hello > /tmp/logs/a") == 0);
	CHECK(read_all("/tmp/logs/a", buf, sizeof(buf)) == 6 && strcmp(buf, "hello\n") == 0);
	{
		/* ls -l shows the bytes stored in a RAM file */
		vfs_path_t d;
		vfs_dirent_t e;
		uint16_t index = 0;
		uint32_t size = 0;

		CHECK(vfs_resolve("/tmp/logs", &d) == 0 && d.kind == VFS_DIR);
		while (vfs_dir_read(d.dir, &index, &e) == 0) {
			if (strcmp(e.name, "a") == 0) {
				size = e.size;
			}
		}
		CHECK(size == 6);
	}
	CHECK(system("rm /tmp/logs") == -1); /* not empty */
	CHECK(system("mv /tmp/logs/a /tmp/b") == 0 && read_all("/tmp/b", buf, sizeof(buf)) == 6);
	CHECK(system("rm /tmp/b") == 0 && system("rm /tmp/logs") == 0);
	CHECK(system("rm ttyS1") == -1); /* devices stay */

	/* block devices in /dev, mounted */
	CHECK(ramdisk_init(&ram0, ram0_mem, sizeof(ram0_mem)) == 0);
	CHECK(ee25_init(&ee0, &ee_spi, CHIP_SIZE, 256) == 0);
	memset(ram_chip.mem, 0x55, sizeof(ram_chip.mem));
	CHECK(sram23_init(&ram1, &ram_spi, CHIP_SIZE) == 0);
	CHECK(vfs_blk_register("ram0", &ram0.blk) == 0);
	CHECK(vfs_blk_register("ee0", &ee0.blk) == 0);
	CHECK(vfs_blk_register("ram1", &ram1.blk) == 0);
	CHECK(vfs_blk_get("/dev/ee0") == &ee0.blk);

	CHECK(system("mount ram1 /tmp") == -1); /* no file system yet */
	CHECK(system("mount -f ram1 /tmp") == 0);
	CHECK(system("mkfs ee0 256 32") == 0);
	CHECK(system("mount ee0 /mnt/ee") == 0);
	CHECK(system("mount ee0 /mnt/again") == -1);
	CHECK(system("mkfs ee0") == -1); /* mounted */
	CHECK(vfs_resolve("/mnt/ee", &p) == 0 && p.kind == VFS_DIR && vfs_mount_get(p.dir));

	/* files by path: echo, cat, cp, the CRC */
	CHECK(system("echo ram file > /tmp/x") == 0);
	CHECK(read_all("/tmp/x", buf, sizeof(buf)) == 9 && strcmp(buf, "ram file\n") == 0);
	CHECK(system("echo line1 > /mnt/ee/boot.as") == 0);
	CHECK(system("echo line2 >> /mnt/ee/boot.as") == 0);
	CHECK(read_all("/mnt/ee/boot.as", buf, sizeof(buf)) == 12);
	CHECK(strcmp(buf, "line1\nline2\n") == 0);
	CHECK(system("echo replaced > /mnt/ee/boot.as") == 0);
	CHECK(read_all("/mnt/ee/boot.as", buf, sizeof(buf)) == 9);
	CHECK(system("cd /mnt/ee") == 0 && read_all("boot.as", buf, sizeof(buf)) == 9);
	CHECK(system("cp boot.as /tmp") == 0 && read_all("/tmp/boot.as", buf, sizeof(buf)) == 9);
	CHECK(system("crc boot.as") == 0);
	CHECK(system("mv boot.as rc") == 0 && vfs_resolve("rc", &p) == 0 && p.kind == VFS_FILE);
	CHECK(system("mv rc /tmp/rc") == -1); /* another file system: cp */
	CHECK(system("cat rc") == 9);
	CHECK(system("ls -l") == 1);
	CHECK(system("ls /") == 5); /* bin dev app tmp mnt */
	CHECK(system("cd /") == 0);

	/* a device that also takes commands (a GPIO, ttyS1 -b) is in /dev, not /bin */
	CHECK(fsi_node_create("led1", false, NULL, &gpio_node) == 0);
	CHECK(dir_has("/dev", "led1") && dir_has("/dev", "ttyS1") && dir_has("/dev", "ee0"));
	CHECK(!dir_has("/bin", "led1") && !dir_has("/bin", "ttyS1"));
	CHECK(dir_has("/bin", "probe") && dir_has("/bin", "ls") && !dir_has("/dev", "probe"));
	CHECK(system("led1 on") == 7 && last_argc == 2);
	CHECK(system("/dev/led1 a b") == 7 && last_argc == 3);
	CHECK(system("/bin/led1 on") == -1);

	/* Tab on a terminal follows the tree */
	CHECK(fsi_node_create("term", false, NULL, &term_node) == 0);
	CHECK(system("std -t term") == 0);
	CHECK(strcmp(type("pro\t"), "/$ probe ") == 0);	     /* a command; the prompt is the directory */
	CHECK(strcmp(type("/app/to\t"), "/app/tool ") == 0); /* a command by path */
	CHECK(strcmp(type("ls /tm\t"), "ls /tmp/") == 0);    /* a directory gets a '/' */
	CHECK(strcmp(type("ls /dev/tt\t"), "ls /dev/ttyS1 ") == 0);
	CHECK(strcmp(type("ls /dev/le\t"), "ls /dev/led1 ") == 0);
	CHECK(strcmp(type("ls /mnt/ee/r\t"), "ls /mnt/ee/rc ") == 0); /* a file system */
	CHECK(strcmp(type("ls /nothing/x\t"), "ls /nothing/x") == 0);
	CHECK(strcmp(type("cat /tmp/bo\t"), "cat /tmp/boot.as ") == 0);
	type("cd /tmp/\r"); /* the '/' Tab leaves is fine for cd */
	CHECK(vfs_dir_path(vfs_cwd(), buf, sizeof(buf)) == 0 && strcmp(buf, "/tmp") == 0);
	CHECK(strcmp(type("cat bo\t"), "cat boot.as ") == 0); /* the working directory */
	CHECK(strcmp(type("std tt\t"), "std ttyS1 ") == 0);   /* not here: a bare fsi name */
	const char *s = type("ls ../\t");		      /* the directory one up only */
	CHECK(strstr(s, "\r\nbin/  dev/  app/  tmp/  mnt/  \r\n") != NULL);
	CHECK(strstr(s, "probe") == NULL && strstr(s, "ttyS1") == NULL);
	CHECK(strcmp(type("ls ../m\t"), "ls ../mnt/") == 0);
	CHECK(strcmp(type("ls ../mnt/\t"), "ls ../mnt/ee/") == 0); /* the only entry */
	s = type("ls /dev/\t");
	CHECK(strstr(s, "ttyS1  ") != NULL && strstr(s, "led1  ") != NULL &&
	      strstr(s, "ee0  ") != NULL && strstr(s, "probe") == NULL);
	s = type("ls /bin/l\t"); /* ls only: led1 is in /dev */
	CHECK(strcmp(s, "ls /bin/ls ") == 0);
	type("cd ..\r");
	CHECK(vfs_cwd() == VFS_ROOT);
	CHECK(strcmp(type("ls de\t"), "ls dev/") == 0); /* /bin and /dev are in / */
	CHECK(system("std ttyS1") == 0);

	/* a firmware image through open / write / lseek, bigger than a 16-bit int */
	int fd = open("/mnt/ee/fw.bin", O_WRONLY | O_CREAT | O_TRUNC, 0);
	CHECK(fd > 2 && vfs_file_get(fd) != NULL);
	uint8_t chunk[128];
	for (uint32_t i = 0; i < 40000; i += sizeof(chunk)) {
		for (unsigned k = 0; k < sizeof(chunk); k++)
			chunk[k] = pattern(i + k);
		CHECK(write(fd, chunk, sizeof(chunk)) == sizeof(chunk));
	}
	CHECK(system("umount /mnt/ee") == -1); /* a file is open */
	CHECK(close(fd) == 0);
	fd = open("/mnt/ee/fw.bin", 0, 0);
	CHECK(lseek(fd, 0, 2) == 40064 && lseek(fd, 33000, 0) == 33000);
	CHECK(read(fd, chunk, 4) == 4 && chunk[0] == pattern(33000) && chunk[3] == pattern(33003));
	CHECK(close(fd) == 0);
	CHECK(system("crc /mnt/ee/fw.bin") == 0);
	printf("vfs: 25LC1024 page writes for a 40 KB image and the rest: %u\n", ee_chip.writes);

	/* survives an unmount and a mount (a reboot) */
	CHECK(system("umount /mnt/ee") == 0 && vfs_resolve("/mnt/ee/rc", &p) == 0);
	CHECK(p.kind == VFS_NONE);
	CHECK(system("mount ee0 /mnt/ee") == 0);
	CHECK(read_all("/mnt/ee/rc", buf, sizeof(buf)) == 9 && strcmp(buf, "replaced\n") == 0);
	CHECK(system("df") == 2);

	/* raw device */
	fd = open("/dev/ee0", 0, 0);
	CHECK(fd > 2 && read(fd, chunk, 4) == 4 && memcmp(chunk, "TFS1", 4) == 0);
	CHECK(lseek(fd, 0, 0) == 0);
	close(fd);

	/* open files are not nodes: handles after the table, all closed now */
	for (int i = 0; i < FSI_MAX_NODES_COUNT + (int)VFS_FILES_MAX; i++)
		CHECK(vfs_file_get(i) == NULL);
	int h[VFS_FILES_MAX + 1];
	for (unsigned i = 0; i <= VFS_FILES_MAX; i++)
		h[i] = open("/mnt/ee/rc", 0, 0);
	CHECK(h[0] == FSI_MAX_NODES_COUNT && h[VFS_FILES_MAX] == -1); /* all in use */
	for (unsigned i = 0; i < VFS_FILES_MAX; i++)
		CHECK(close(h[i]) == 0);

	/* stdout reached the node it was sent to */
	CHECK(strstr(sink, "replaced") != NULL);

	/* ls as on Linux: names only, -p marks the directories, -l the type and rwx */
	CHECK(strcmp(capture("ls /"), "bin  dev  app  tmp  mnt\n") == 0);
	CHECK(strcmp(capture("ls -p /"), "bin/  dev/  app/  tmp/  mnt/\n") == 0);
	CHECK(strncmp(capture("ls -l /dev/ee0"), "brw-", 4) == 0);
	CHECK(strncmp(capture("ls -l /dev/ttyS1"), "c-w-", 4) == 0);
	CHECK(strncmp(capture("ls -l /mnt/ee/rc"), "-rw-", 4) == 0);
	CHECK(strncmp(capture("ls -l /mnt"), "drwx", 4) == 0);
	CHECK(system("mkdir /app/empty") == 0 && strcmp(capture("ls /app/empty"), "") == 0);
	CHECK(system("rm /app/empty") == 0);

	/* scripts run by name from /home, by path from anywhere; the prompt shows ~ there */
	CHECK(system("mount -f ram0 /home") == 0);
	CHECK(system("echo echo hi > /home/hello") == 0);
	CHECK(system("hello") == -1); /* no interpreter yet */
	vfs_interpreter_set(cmd_script);
	CHECK(system("hello") == 5 && strcmp(script_path, "/home/hello") == 0);
	CHECK(strcmp(script_name, "hello") == 0);
	CHECK(system("/mnt/ee/rc") == 5 && strcmp(script_name, "rc") == 0);
	CHECK(system("nothing") == -1);
	CHECK(vfs_resolve("~/hello", &p) == 0 && p.kind == VFS_FILE);
	CHECK(vfs_resolve("~", &p) == 0 && p.kind == VFS_DIR && vfs_mount_get(p.dir) != NULL);

	/* directories, mounts and open files take no node: the table holds only fsi nodes */
	int nodes = 0;
	for (int i = 0; i < FSI_MAX_NODES_COUNT; i++)
		nodes += fsi_nodes_get()[i].used == FSI_NODE_USED;
	int fd2 = open("~/hello", 0, 0);
	CHECK(fd2 >= FSI_MAX_NODES_COUNT && vfs_mkdir("/app/d1") == 0 && vfs_mkdir("/app/d1/d2") == 0);
	int now = 0;
	for (int i = 0; i < FSI_MAX_NODES_COUNT; i++)
		now += fsi_nodes_get()[i].used == FSI_NODE_USED;
	CHECK(now == nodes && close(fd2) == 0);
	CHECK(vfs_rename("/app/d1/d2", "/app/d3") == 0 && vfs_rename("/app/d1", "/app/d1/x") == -1);
	CHECK(vfs_remove("/app/d1") == 0 && vfs_remove("/app/d3") == 0);

	CHECK(system("cd") == 0 && vfs_dir_path(vfs_cwd(), buf, sizeof(buf)) == 0);
	CHECK(strcmp(buf, "/home") == 0);
	CHECK(system("std -t term") == 0);
	CHECK(strstr(type("\r"), "~$ ") != NULL);
	CHECK(system("std ttyS1") == 0);
	vfs_interpreter_set(NULL);
}

/* -------------------------------------------------------------------------- */
/* two pages of PIC24FJ flash, emulated: ECC (a double word programmed once per erase), power cuts */

#define NV_BASE 0x14800ul

static uint32_t nv_mem[2048]; /* 2 x 1024 instructions, 24 bits each */
static int nv_budget = -1;    /* flash operations until the power goes, -1: never */
static int nv_ecc_errors, nv_erases, nv_programs;

static bool nv_cut(void)
{
	if (nv_budget == 0)
		return true;
	if (nv_budget > 0)
		nv_budget--;
	return false;
}

uint32_t flash24_hw_read(uint32_t addr)
{
	return nv_mem[(addr - NV_BASE) / 2];
}

int flash24_hw_erase(uint32_t addr)
{
	if (nv_cut())
		return -1;
	uint32_t i = (addr - NV_BASE) / 2;
	if (i % 1024 != 0)
		nv_ecc_errors++;
	for (int k = 0; k < 1024; k++)
		nv_mem[i + k] = 0xFFFFFF;
	nv_erases++;
	return 0;
}

int flash24_hw_program(uint32_t addr, uint32_t w0, uint32_t w1)
{
	if (nv_cut())
		return -1;
	uint32_t i = (addr - NV_BASE) / 2;
	if (i % 2 != 0 || nv_mem[i] != 0xFFFFFF || nv_mem[i + 1] != 0xFFFFFF)
		nv_ecc_errors++;
	nv_mem[i] = w0 & 0xFFFFFF;
	nv_mem[i + 1] = w1 & 0xFFFFFF;
	nv_programs++;
	return 0;
}

static void test_flash24(void)
{
	static flash24_t nv;
	static part_t boot, home;
	tfs_t fs;
	char buf[300];

	memset(nv_mem, 0xFF, sizeof(nv_mem)); /* erased: 0xFFFFFF in 32 bits */
	for (int i = 0; i < 2048; i++)
		nv_mem[i] = 0xFFFFFF;

	CHECK(flash24_init(&nv, NV_BASE + 2) == -1); /* not page aligned */
	CHECK(flash24_init(&nv, NV_BASE) == 0 && nv.blk.size == 2044 && nv.live == 0);
	CHECK(part_init(&boot, &nv.blk, 0, 512) == 0);
	CHECK(part_init(&home, &nv.blk, 512, nv.blk.size - 512) == 0);
	CHECK(part_init(&home, &nv.blk, 512, nv.blk.size) == -1); /* past the end */
	CHECK(part_init(&home, &nv.blk, 512, nv.blk.size - 512) == 0);

	/* both file systems on the one flash */
	CHECK(tfs_format(&boot.blk, 0, 0) == 0 && tfs_format(&home.blk, 32, 8) == 0);
	CHECK(tfs_mount(&fs, &boot.blk) == 0 && fill_file(&fs, "rc", 200, TFS_WRITE | TFS_CREATE) == 0);
	CHECK(tfs_mount(&fs, &home.blk) == 0 && fill_file(&fs, "blink", 300, TFS_WRITE | TFS_CREATE) == 0);
	CHECK(fill_file(&fs, "rgb", 250, TFS_WRITE | TFS_CREATE | TFS_TRUNC) == 0);
	CHECK(nv_ecc_errors == 0);
	printf("vfs: flash24 for 2 file systems and 3 files: %d erases, %d double words\n",
	       nv_erases, nv_programs);

	/* after a reset: the newer page */
	CHECK(flash24_init(&nv, NV_BASE) == 0 && nv.gen > 0);
	CHECK(tfs_mount(&fs, &boot.blk) == 0 && check_file(&fs, "rc", 200));
	CHECK(tfs_mount(&fs, &home.blk) == 0 && check_file(&fs, "blink", 300));
	CHECK(check_file(&fs, "rgb", 250));

	/* power cuts at every step of replacing a file: the old file or the new one, never less */
	uint32_t saved[2048];
	CHECK(tfs_mount(&fs, &home.blk) == 0 && fill_file(&fs, "cfg", 100, TFS_WRITE | TFS_CREATE) == 0);
	memcpy(saved, nv_mem, sizeof(saved));
	int olds = 0, news = 0;
	for (int budget = 0; budget < 5000; budget++) {
		memcpy(nv_mem, saved, sizeof(saved));
		CHECK(flash24_init(&nv, NV_BASE) == 0 && tfs_mount(&fs, &home.blk) == 0);
		nv_budget = budget;
		int r = fill_file(&fs, "cfg", 160, TFS_WRITE | TFS_CREATE | TFS_TRUNC);
		nv_budget = -1;
		CHECK(flash24_init(&nv, NV_BASE) == 0 && tfs_mount(&fs, &home.blk) == 0);
		bool old = check_file(&fs, "cfg", 100), new = check_file(&fs, "cfg", 160);
		CHECK(old || new);
		CHECK(check_file(&fs, "blink", 300));
		olds += old;
		news += new;
		if (r == 0)
			break;
	}
	CHECK(olds > 0 && news == 1 && nv_ecc_errors == 0);

	/* the raw device: bytes as written, at odd addresses too (it overwrites both file systems) */
	CHECK(nv.blk.write(&nv.blk, 509, "abcdef", 6) == 0 && nv.blk.read(&nv.blk, 509, buf, 6) == 0);
	CHECK(memcmp(buf, "abcdef", 6) == 0 && nv_ecc_errors == 0);
	CHECK(nv.blk.write(&nv.blk, 2040, "wxyz", 4) == 0 && nv.blk.write(&nv.blk, 2043, "12", 2) == -1);
}

int main(void)
{
	static uint8_t mem[8192];
	ramdisk_t disk;
	ee25_t ee;
	sram23_t ram;

	ramdisk_init(&disk, mem, sizeof(mem));
	test_tfs(&disk.blk, 64);
	test_tfs(&disk.blk, 128);
	test_power_loss(&disk.blk);

	/* format without parameters: sized to the device, even a tiny one */
	ramdisk_t tiny;
	tfs_t tfs;
	ramdisk_init(&tiny, mem, 512);
	CHECK(tfs_format(&tiny.blk, 0, 0) == 0 && tfs_mount(&tfs, &tiny.blk) == 0);
	CHECK(tfs.bsize == 32 && tfs.entries == 4 && tfs_free(&tfs) == 320);
	ramdisk_init(&disk, mem, sizeof(mem));
	CHECK(tfs_format(&disk.blk, 0, 0) == 0 && tfs_mount(&tfs, &disk.blk) == 0);
	CHECK(tfs.bsize == 64 && tfs.entries == 32);

	ee25_init(&ee, &ee_spi, CHIP_SIZE, 256);
	test_tfs(&ee.blk, 256);
	test_power_loss(&ee.blk);

	sram23_init(&ram, &ram_spi, CHIP_SIZE);
	test_tfs(&ram.blk, 128);

	test_flash24();
	test_tree();

	fprintf(stderr, "vfs: %s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
