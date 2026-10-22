/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/*
 * The tiny file system. See tfs.h for the layout on the device. Every value is
 * read and written byte by byte in little endian, so an image made on a PC
 * and one made on a PIC24 are the same.
 */

#include "tfs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define TFS_SB_SIZE    16u
#define TFS_ENTRY_SIZE 32u
#define TFS_MAGIC      "TFS1"
#define TFS_CHECK      0x5446u /* superblock check: XOR of its fields and this */

#define TFS_FREE 0xFFFFu /* FAT: free block (as erased memory) */
#define TFS_END	 0xFFFEu /* FAT: last block of a chain */

/* directory entry fields */
#define TFS_E_STATE 16u
#define TFS_E_GEN   17u
#define TFS_E_FIRST 18u
#define TFS_E_SIZE  20u
#define TFS_E_CRC   24u
#define TFS_E_ATTR  28u

#define TFS_STATE_WRITING 1u
#define TFS_STATE_VALID	  2u

/* tfs_file_t.mode, besides enum TFS_MODE */
#define TFS_REPLACE 0x20u /* a new entry that replaces the old file at close */
#define TFS_DIRTY   0x40u /* size or CRC changed: commit at close */
#define TFS_CRCOK   0x80u /* f->crc is the CRC of the whole file */

#define TFS_FILL 32u /* bytes written at a time by tfs_format() */

/* -------------------------------------------------------------------------- */

static uint16_t tfs_get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t tfs_get32(const uint8_t *p)
{
	return tfs_get16(p) | ((uint32_t)tfs_get16(p + 2) << 16);
}

static void tfs_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void tfs_put32(uint8_t *p, uint32_t v)
{
	tfs_put16(p, (uint16_t)v);
	tfs_put16(p + 2, (uint16_t)(v >> 16));
}

static int tfs_rd(tfs_t *fs, uint32_t addr, void *buffer, unsigned int length)
{
	return fs->blk->read(fs->blk, addr, buffer, length);
}

static int tfs_wr(tfs_t *fs, uint32_t addr, const void *buffer, unsigned int length)
{
	return fs->blk->write(fs->blk, addr, buffer, length);
}

uint32_t tfs_crc32(uint32_t crc, const void *buffer, unsigned int length)
{
	const uint8_t *p = buffer;

	crc = ~crc;

	while (length--) {
		crc ^= *p++;

		for (int i = 0; i < 8; i++) {
			crc = (crc >> 1) ^ (0xEDB88320ul & (0ul - (crc & 1u)));
		}
	}

	return ~crc;
}

/* -------------------------------------------------------------------------- */
/* FAT */

static uint16_t tfs_fat_get(tfs_t *fs, uint16_t block)
{
	uint8_t b[2];

	if (tfs_rd(fs, fs->fat + 2ul * block, b, 2) < 0) {
		return TFS_END; /* stops any walk */
	}

	return tfs_get16(b);
}

static int tfs_fat_set(tfs_t *fs, uint16_t block, uint16_t value)
{
	uint8_t b[2];

	tfs_put16(b, value);

	return tfs_wr(fs, fs->fat + 2ul * block, b, 2);
}

/* a free block, now marked as the last of a chain; TFS_NONE when full */
static uint16_t tfs_alloc(tfs_t *fs)
{
	uint16_t b = fs->rover;

	for (uint16_t n = 0; n < fs->blocks; n++, b++) {
		if (b >= fs->blocks) {
			b = 0;
		}

		if (tfs_fat_get(fs, b) != TFS_FREE) {
			continue;
		}

		if (tfs_fat_set(fs, b, TFS_END) < 0) {
			return TFS_NONE;
		}

		fs->rover = b + 1;

		return b;
	}

	return TFS_NONE;
}

static void tfs_free_chain(tfs_t *fs, uint16_t block)
{
	/* at most every block once, even in a damaged chain */
	for (uint16_t n = 0; block < fs->blocks && n < fs->blocks; n++) {
		uint16_t next = tfs_fat_get(fs, block);

		tfs_fat_set(fs, block, TFS_FREE);
		block = next;
	}
}

/* -------------------------------------------------------------------------- */
/* directory */

static uint32_t tfs_entry_addr(uint8_t e)
{
	return TFS_SB_SIZE + (uint32_t)e * TFS_ENTRY_SIZE;
}

static int tfs_entry_get(tfs_t *fs, uint8_t e, tfs_info_t *t)
{
	uint8_t r[TFS_ENTRY_SIZE];

	if (tfs_rd(fs, tfs_entry_addr(e), r, sizeof(r)) < 0) {
		return -1;
	}

	memcpy(t->name, r, TFS_NAME_SIZE);
	t->name[TFS_NAME_SIZE - 1] = '\0';

	t->state = r[TFS_E_STATE];
	t->gen = r[TFS_E_GEN];
	t->first = tfs_get16(r + TFS_E_FIRST);
	t->size = tfs_get32(r + TFS_E_SIZE);
	t->crc = tfs_get32(r + TFS_E_CRC);
	t->attr = r[TFS_E_ATTR];

	if (r[0] == 0x00 || r[0] == 0xFF ||
	    (t->state != TFS_STATE_WRITING && t->state != TFS_STATE_VALID)) {
		t->state = 0;
	}

	return 0;
}

/* one write: an entry never crosses an EEPROM page */
static int tfs_entry_put(tfs_t *fs, uint8_t e, const tfs_info_t *t)
{
	uint8_t r[TFS_ENTRY_SIZE];

	memset(r, 0xFF, sizeof(r));
	memset(r, 0, TFS_NAME_SIZE);
	memcpy(r, t->name, strnlen(t->name, TFS_NAME_SIZE - 1));

	r[TFS_E_STATE] = t->state;
	r[TFS_E_GEN] = t->gen;
	tfs_put16(r + TFS_E_FIRST, t->first);
	tfs_put32(r + TFS_E_SIZE, t->size);
	tfs_put32(r + TFS_E_CRC, t->crc);
	r[TFS_E_ATTR] = t->attr;

	return tfs_wr(fs, tfs_entry_addr(e), r, sizeof(r));
}

/* frees an entry and its blocks: the entry first, so a power loss only leaks */
static void tfs_entry_drop(tfs_t *fs, uint8_t e, uint16_t first)
{
	uint8_t zero = 0;

	tfs_wr(fs, tfs_entry_addr(e), &zero, 1);
	tfs_free_chain(fs, first);
}

/* the valid entry named @name, -1 if none */
static int tfs_find(tfs_t *fs, const char *name, tfs_info_t *t)
{
	for (unsigned int e = 0; e < fs->entries; e++) {
		if (tfs_entry_get(fs, (uint8_t)e, t) < 0) {
			return -1;
		}

		if (t->state == TFS_STATE_VALID && strcmp(t->name, name) == 0) {
			return (int)e;
		}
	}

	return -1;
}

static int tfs_find_free(tfs_t *fs)
{
	tfs_info_t t;

	for (unsigned int e = 0; e < fs->entries; e++) {
		if (tfs_entry_get(fs, (uint8_t)e, &t) == 0 && t.state == 0) {
			return (int)e;
		}
	}

	return -1;
}

static bool tfs_name_ok(const char *name)
{
	size_t n = (name == NULL) ? 0 : strlen(name);

	return n > 0 && n < TFS_NAME_SIZE && (uint8_t)name[0] != 0xFF && strchr(name, '/') == NULL;
}

/* -------------------------------------------------------------------------- */
/* format and mount */

#if !defined(TFS_READONLY)
int tfs_format(vfs_blk_t *blk, uint16_t bsize, uint8_t entries)
{
	if (blk == NULL || bsize < 16u || bsize > 4096u || (bsize & (bsize - 1u)) ||
	    entries == 0) {
		return -1;
	}

	uint32_t fat = TFS_SB_SIZE + (uint32_t)entries * TFS_ENTRY_SIZE;

	if (blk->size <= fat + bsize + 2u) {
		return -1;
	}

	uint32_t blocks = (blk->size - fat) / (bsize + 2u);
	uint32_t data;

	if (blocks > TFS_END - 1u) {
		blocks = TFS_END - 1u;
	}

	/* the FAT and the alignment of the data may cost a block or two */
	for (;;) {
		data = (fat + 2u * blocks + bsize - 1u) & ~(uint32_t)(bsize - 1u);

		if (data + blocks * bsize <= blk->size || blocks == 0) {
			break;
		}

		blocks--;
	}

	if (blocks == 0) {
		return -1;
	}

	/* directory and FAT: all 0xFF, free */
	uint8_t fill[TFS_FILL];

	memset(fill, 0xFF, sizeof(fill));

	for (uint32_t a = TFS_SB_SIZE; a < data; a += TFS_FILL) {
		unsigned int n = (data - a < TFS_FILL) ? (unsigned int)(data - a) : TFS_FILL;

		if (blk->write(blk, a, fill, n) < 0) {
			return -1;
		}
	}

	/* the superblock last: a device is formatted only when it is complete */
	uint8_t *sb = fill;

	memcpy(sb, TFS_MAGIC, 4);
	tfs_put16(sb + 4, bsize);
	tfs_put16(sb + 6, (uint16_t)blocks);
	tfs_put16(sb + 8, entries);
	tfs_put32(sb + 10, data);
	tfs_put16(sb + 14, (uint16_t)(bsize ^ blocks ^ entries ^ data ^ (data >> 16) ^ TFS_CHECK));

	return blk->write(blk, 0, sb, TFS_SB_SIZE);
}
#endif

#if !defined(TFS_NO_CHECK) && !defined(TFS_READONLY)
/*
 * Repairs what a power loss can leave: an entry still being written, two valid
 * entries with one name (the older generation goes), and blocks no file reaches.
 */
static void tfs_check(tfs_t *fs)
{
	tfs_info_t a, b;

	for (unsigned int i = 0; i < fs->entries; i++) {
		if (tfs_entry_get(fs, (uint8_t)i, &a) < 0 || a.state == 0) {
			continue;
		}

		if (a.state == TFS_STATE_WRITING) {
			tfs_entry_drop(fs, (uint8_t)i, a.first);
			continue;
		}

		for (unsigned int j = i + 1; j < fs->entries; j++) {
			if (tfs_entry_get(fs, (uint8_t)j, &b) < 0 || b.state != TFS_STATE_VALID ||
			    strcmp(a.name, b.name) != 0) {
				continue;
			}

			if ((int8_t)(b.gen - a.gen) > 0) {
				tfs_entry_drop(fs, (uint8_t)i, a.first);
				break;
			}

			tfs_entry_drop(fs, (uint8_t)j, b.first);
		}
	}

	/* mark every block a file reaches, free the rest */
	uint8_t *seen = malloc((fs->blocks + 7u) / 8u);

	if (seen == NULL) {
		return;
	}

	memset(seen, 0, (fs->blocks + 7u) / 8u);

	for (unsigned int i = 0; i < fs->entries; i++) {
		if (tfs_entry_get(fs, (uint8_t)i, &a) < 0 || a.state != TFS_STATE_VALID) {
			continue;
		}

		/* the blocks the size needs; a longer chain is cut (writes not committed) */
		uint32_t need = (a.size + fs->bsize - 1u) >> fs->bshift;
		uint16_t blk = a.first;

		if (need == 0 && blk != TFS_NONE) {
			uint8_t v[2] = {0xFF, 0xFF};

			tfs_wr(fs, tfs_entry_addr((uint8_t)i) + TFS_E_FIRST, v, 2);
		}

		for (uint32_t n = 1; n <= need && blk < fs->blocks; n++) {
			if (seen[blk >> 3] & (1u << (blk & 7u))) {
				break; /* cross-linked: keep what is there */
			}

			seen[blk >> 3] |= (uint8_t)(1u << (blk & 7u));

			uint16_t next = tfs_fat_get(fs, blk);

			if (n == need && next != TFS_END) {
				tfs_fat_set(fs, blk, TFS_END);
			}

			blk = next;
		}
	}

	for (uint16_t blk = 0; blk < fs->blocks; blk++) {
		if (!(seen[blk >> 3] & (1u << (blk & 7u))) && tfs_fat_get(fs, blk) != TFS_FREE) {
			tfs_fat_set(fs, blk, TFS_FREE);
		}
	}

	free(seen);
}
#endif

int tfs_mount(tfs_t *fs, vfs_blk_t *blk)
{
	uint8_t sb[TFS_SB_SIZE];

	if (fs == NULL || blk == NULL || blk->read(blk, 0, sb, sizeof(sb)) < 0) {
		return -1;
	}

	if (memcmp(sb, TFS_MAGIC, 4) != 0) {
		return -1;
	}

	uint16_t bsize = tfs_get16(sb + 4);
	uint16_t blocks = tfs_get16(sb + 6);
	uint16_t entries = tfs_get16(sb + 8);
	uint32_t data = tfs_get32(sb + 10);
	uint16_t check = (uint16_t)(bsize ^ blocks ^ entries ^ data ^ (data >> 16) ^ TFS_CHECK);

	if (tfs_get16(sb + 14) != check || bsize < 16u || (bsize & (bsize - 1u)) || entries == 0 ||
	    entries > 255u || data + (uint32_t)blocks * bsize > blk->size) {
		return -1;
	}

	fs->bshift = 0;

	while ((1u << fs->bshift) < bsize) {
		fs->bshift++;
	}

	fs->blk = blk;
	fs->bsize = bsize;
	fs->blocks = blocks;
	fs->entries = (uint8_t)entries;
	fs->fat = TFS_SB_SIZE + (uint32_t)entries * TFS_ENTRY_SIZE;
	fs->data = data;
	fs->rover = 0;

#if !defined(TFS_NO_CHECK) && !defined(TFS_READONLY)
	tfs_check(fs);
#endif

	return 0;
}

/* -------------------------------------------------------------------------- */
/* files */

int tfs_open(tfs_t *fs, tfs_file_t *f, const char *name, uint8_t mode)
{
	if (fs == NULL || f == NULL || !tfs_name_ok(name)) {
		return -1;
	}

	tfs_info_t t;
	int e = tfs_find(fs, name, &t);

	if (e < 0 && !(mode & TFS_CREATE)) {
		return -1;
	}

	if (e >= 0 && (mode & TFS_CREATE) && (mode & TFS_EXCL)) {
		return -1;
	}

	memset(f, 0, sizeof(*f));
	f->fs = fs;
	f->block = TFS_NONE;

	if (e >= 0 && !(mode & TFS_TRUNC)) {
		/* the file as it is */
		f->entry = (uint8_t)e;
		f->mode = mode;
		f->first = t.first;
		f->size = t.size;
		f->crc = t.crc;

		if (t.attr & TFS_ATTR_CRC) {
			f->mode |= TFS_CRCOK;
		}

		return 0;
	}

	/* a new, empty entry; replacing an old file it is committed at close */
	int n = tfs_find_free(fs);

	if (n < 0) {
		return -1;
	}

	t.gen = (e >= 0) ? (uint8_t)(t.gen + 1u) : 0u;
	strncpy(t.name, name, TFS_NAME_SIZE - 1);
	t.name[TFS_NAME_SIZE - 1] = '\0';
	t.state = (e >= 0) ? TFS_STATE_WRITING : TFS_STATE_VALID;
	t.first = TFS_NONE;
	t.size = 0;
	t.crc = 0;
	t.attr = TFS_ATTR_CRC;

	if (tfs_entry_put(fs, (uint8_t)n, &t) < 0) {
		return -1;
	}

	f->entry = (uint8_t)n;
	f->mode = mode | TFS_WRITE | TFS_DIRTY | TFS_CRCOK | ((e >= 0) ? TFS_REPLACE : 0u);
	f->first = TFS_NONE;

	return 0;
}

/* the block at index @idx of the file's chain; @grow allocates the missing ones */
static uint16_t tfs_block_at(tfs_file_t *f, uint16_t idx, bool grow)
{
	tfs_t *fs = f->fs;

	if (f->first == TFS_NONE) {
		if (!grow) {
			return TFS_NONE;
		}

		uint16_t b = tfs_alloc(fs);
		uint8_t v[2];

		if (b == TFS_NONE) {
			return TFS_NONE;
		}

		/* into the entry at once: an allocated block always belongs to a file */
		tfs_put16(v, b);

		if (tfs_wr(fs, tfs_entry_addr(f->entry) + TFS_E_FIRST, v, 2) < 0) {
			return TFS_NONE;
		}

		f->first = b;
		f->block = b;
		f->bidx = 0;
	}

	uint16_t b = f->first;
	uint16_t i = 0;

	if (f->block != TFS_NONE && idx >= f->bidx) {
		b = f->block;
		i = f->bidx;
	}

	while (i < idx) {
		uint16_t next = tfs_fat_get(fs, b);

		if (next >= fs->blocks) {
			if (!grow) {
				return TFS_NONE;
			}

			next = tfs_alloc(fs);

			if (next == TFS_NONE || tfs_fat_set(fs, b, next) < 0) {
				return TFS_NONE;
			}
		}

		b = next;
		i++;
	}

	f->block = b;
	f->bidx = i;

	return b;
}

int tfs_read(tfs_file_t *f, void *buffer, unsigned int length)
{
	if (f == NULL || buffer == NULL) {
		return -1;
	}

	tfs_t *fs = f->fs;
	uint8_t *p = buffer;
	unsigned int done = 0;

	if (f->pos >= f->size) {
		return 0;
	}

	if (length > f->size - f->pos) {
		length = (unsigned int)(f->size - f->pos);
	}

	while (done < length) {
		uint16_t b = tfs_block_at(f, (uint16_t)(f->pos >> fs->bshift), false);
		unsigned int off = (unsigned int)f->pos & (fs->bsize - 1u);
		unsigned int n = fs->bsize - off;

		if (n > length - done) {
			n = length - done;
		}

		uint32_t at = fs->data + ((uint32_t)b << fs->bshift) + off;

		if (b == TFS_NONE || tfs_rd(fs, at, p + done, n) < 0) {
			return done ? (int)done : -1;
		}

		f->pos += n;
		done += n;
	}

	return (int)done;
}

int tfs_write(tfs_file_t *f, const void *buffer, unsigned int length)
{
	if (f == NULL || buffer == NULL || !(f->mode & TFS_WRITE)) {
		return -1;
	}

	tfs_t *fs = f->fs;
	const uint8_t *p = buffer;
	unsigned int done = 0;

	if (f->mode & TFS_APPEND) {
		f->pos = f->size;
	}

	if (f->pos != f->size) {
		f->mode &= (uint8_t)~TFS_CRCOK; /* an overwrite: the CRC is unknown */
	}

	while (done < length) {
		uint16_t b = tfs_block_at(f, (uint16_t)(f->pos >> fs->bshift), true);
		unsigned int off = (unsigned int)f->pos & (fs->bsize - 1u);
		unsigned int n = fs->bsize - off;

		if (n > length - done) {
			n = length - done;
		}

		uint32_t at = fs->data + ((uint32_t)b << fs->bshift) + off;

		if (b == TFS_NONE || tfs_wr(fs, at, p + done, n) < 0) {
			break;
		}

		if ((f->mode & TFS_CRCOK) && f->pos == f->size) {
			f->crc = tfs_crc32(f->crc, p + done, n);
		}

		f->pos += n;
		done += n;

		if (f->pos > f->size) {
			f->size = f->pos;
		}

		f->mode |= TFS_DIRTY;
	}

	return (done || length == 0) ? (int)done : -1;
}

int32_t tfs_seek(tfs_file_t *f, int32_t offset, int whence)
{
	if (f == NULL) {
		return -1;
	}

	int32_t base = (whence == 0) ? 0 : (whence == 1) ? (int32_t)f->pos : (int32_t)f->size;
	int32_t pos = base + offset;

	if (whence < 0 || whence > 2 || pos < 0 || (uint32_t)pos > f->size) {
		return -1;
	}

	f->pos = (uint32_t)pos;

	return pos;
}

int tfs_close(tfs_file_t *f)
{
	if (f == NULL || !(f->mode & TFS_DIRTY)) {
		return 0;
	}

	tfs_t *fs = f->fs;
	tfs_info_t t, o;

	if (tfs_entry_get(fs, f->entry, &t) < 0) {
		return -1;
	}

	t.state = TFS_STATE_VALID;
	t.first = f->first;
	t.size = f->size;
	t.crc = (f->mode & TFS_CRCOK) ? f->crc : 0u;
	t.attr = (f->mode & TFS_CRCOK) ? (t.attr | TFS_ATTR_CRC) : (t.attr & ~TFS_ATTR_CRC);

	if (tfs_entry_put(fs, f->entry, &t) < 0) {
		return -1;
	}

	f->mode &= (uint8_t)~TFS_DIRTY;

	if (!(f->mode & TFS_REPLACE)) {
		return 0;
	}

	/* the new file is in: the old one goes */
	for (unsigned int e = 0; e < fs->entries; e++) {
		if (e == f->entry || tfs_entry_get(fs, (uint8_t)e, &o) < 0) {
			continue;
		}

		if (o.state == TFS_STATE_VALID && strcmp(o.name, t.name) == 0) {
			tfs_entry_drop(fs, (uint8_t)e, o.first);
		}
	}

	f->mode &= (uint8_t)~TFS_REPLACE;

	return 0;
}

int tfs_remove(tfs_t *fs, const char *name)
{
	tfs_info_t t;
	int e = (fs == NULL || name == NULL) ? -1 : tfs_find(fs, name, &t);

	if (e < 0) {
		return -1;
	}

	tfs_entry_drop(fs, (uint8_t)e, t.first);

	return 0;
}

int tfs_rename(tfs_t *fs, const char *from, const char *to)
{
	tfs_info_t t, o;

	if (fs == NULL || from == NULL || !tfs_name_ok(to)) {
		return -1;
	}

	int e = tfs_find(fs, from, &t);

	if (e < 0) {
		return -1;
	}

	int d = tfs_find(fs, to, &o);

	if (d == e) {
		return 0;
	}

	if (d >= 0) {
		tfs_entry_drop(fs, (uint8_t)d, o.first);
	}

	strncpy(t.name, to, TFS_NAME_SIZE - 1);
	t.name[TFS_NAME_SIZE - 1] = '\0';

	return tfs_entry_put(fs, (uint8_t)e, &t);
}

int tfs_stat(tfs_t *fs, const char *name, tfs_info_t *info)
{
	if (fs == NULL || name == NULL || info == NULL) {
		return -1;
	}

	return tfs_find(fs, name, info) < 0 ? -1 : 0;
}

int tfs_dir(tfs_t *fs, uint8_t *index, tfs_info_t *info)
{
	if (fs == NULL || index == NULL || info == NULL) {
		return -1;
	}

	while (*index < fs->entries) {
		uint8_t e = (*index)++;

		if (tfs_entry_get(fs, e, info) == 0 && info->state == TFS_STATE_VALID) {
			return 0;
		}
	}

	return -1;
}

uint32_t tfs_free(tfs_t *fs)
{
	uint32_t n = 0;

	for (uint16_t b = 0; fs != NULL && b < fs->blocks; b++) {
		if (tfs_fat_get(fs, b) == TFS_FREE) {
			n++;
		}
	}

	return (fs == NULL) ? 0 : n << fs->bshift;
}
