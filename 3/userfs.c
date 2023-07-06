#include "userfs.h"
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
	BLOCKS_PER_FILE = MAX_FILE_SIZE / BLOCK_SIZE
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	int idx;
	/** Block memory. */
	char *memory;
	/** How many bytes are occupied. */
	size_t occupied;
	/** Previous block in the file. */
	struct block *prev;
	/** Next block in the file. */
	struct block *next;
};

struct file {
	/** Double-linked list of file blocks. */
	struct block *block_list;
	/**
	 * Last block in the list above for fast access to the end
	 * of file.
	 */
	struct block *last_block;
	/** How many file descriptors are opened on the file. */
	size_t refs;
	/** File name. */
	char *name;
	/** Files are stored in a double-linked list. */
	struct file *prev;
	struct file *next;

	/** `true` if the file should be deleted as soon as the last file descriptor is closed. */
	bool ghost;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
	struct file *file;
	bool open;

	struct block *block;
	size_t offset;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc *file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

static void *mustmalloc(size_t sz) {
	void *ptr = malloc(sz);
	if (ptr == NULL) {
		perror("Malloc failed");
		exit(EXIT_FAILURE);
	}
	return ptr;
}

static void mustrealloc(void **ptr, size_t sz) {
	*ptr = realloc(*ptr, sz);
	if (ptr == NULL) {
		perror("Relloc failed");
		exit(EXIT_FAILURE);
	}
}

static struct block *new_block(struct block *prev, int idx) {
	struct block *b = mustmalloc(sizeof (struct block) + BLOCK_SIZE);
	b->idx = idx;
	b->memory = (char *)(b + 1);
	b->occupied = 0;
	b->prev = prev;
	b->next = NULL;
	return b;
}

static struct file *ins_new_file(char *name) {
	struct file *f = mustmalloc(sizeof (struct file));
	f->block_list = new_block(NULL, 0);
	f->last_block = f->block_list;
	f->refs = 0;
	f->name = name;
	f->prev = NULL;
	f->next = file_list;
	f->ghost = false;
	if (file_list)
		file_list->prev = f;
	file_list = f;
	return f;
}

static struct file *find_file(const char *name) {
	struct file *f = file_list;
	while (f && strcmp(f->name, name))
		f = f->next;
	return f;
}

int ins_new_fd(struct file *f) {
	f->refs++;
	struct filedesc *fd;
	int i;
	for (i = 0, fd = file_descriptors; i < file_descriptor_count; ++i, ++fd) {
		if (!fd->open)
			break;
	}
	if (i == file_descriptor_count) {
		if (file_descriptor_count == file_descriptor_capacity) {
			file_descriptor_capacity = 1 + file_descriptor_capacity * 2;
			mustrealloc((void *)&file_descriptors, file_descriptor_capacity * sizeof (struct filedesc));

			fd = &file_descriptors[i];
		}

		++file_descriptor_count;
	}

	fd->file = f;
	fd->open = true;
	fd->block = f->block_list;
	fd->offset = 0;
	return i;
}

int
ufs_open(const char *filename, int flags)
{
	struct file *f = find_file(filename);
	if (f == NULL) {
		if (flags & UFS_CREATE) {
			f = ins_new_file(strdup(filename));
		} else {
			ufs_error_code = UFS_ERR_NO_FILE;
			return -1;
		}
	}
	return ins_new_fd(f);
}

static void seq_write(struct filedesc *fd, const char *buf, size_t *remaining) {
	size_t cur = MIN(*remaining, BLOCK_SIZE - fd->offset);
	memcpy(fd->block->memory + fd->offset, buf, cur);
	if (fd->block->occupied < fd->offset + cur)
		fd->block->occupied = fd->offset + cur;
	*remaining -= cur;
	if (*remaining && fd->block->idx + 1 < BLOCKS_PER_FILE) {
		if (!fd->block->next)
			fd->block->next = new_block(fd->block, fd->block->idx + 1);
		fd->block = fd->block->next;
		fd->offset = 0;
		seq_write(fd, buf + cur, remaining);
	} else {
		fd->offset += cur;
	}
}

static void seq_read(struct filedesc *fd, char *buf, size_t *remaining) {
	size_t cur = MIN(fd->block->occupied - fd->offset, *remaining);
	memcpy(buf, fd->block->memory + fd->offset, cur);
	*remaining -= cur;
	if (*remaining && fd->block->next) {
		fd->block = fd->block->next;
		fd->offset = 0;
		seq_read(fd, buf + cur, remaining);
	} else {
		fd->offset += cur;
	}
}

ssize_t
ufs_write(int fdi, const char *buf, const size_t size)
{
	struct filedesc *fd = &file_descriptors[fdi];
	if (fdi < 0 || fdi >= file_descriptor_count || !fd->open) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	size_t remaining = size;

	seq_write(fd, buf, &remaining);

	if (size == remaining) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}
	return size - remaining;
}

ssize_t
ufs_read(int fdi, char *buf, const size_t size)
{
	struct filedesc *fd = &file_descriptors[fdi];
	if (fdi < 0 || fdi >= file_descriptor_count || !fd->open) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	size_t remaining = size;
	seq_read(fd, buf, &remaining);
	return size - remaining;
}

static void destroy_file(struct file *f) {
	struct block *b = f->block_list;
	while (b) {
		struct block *n = b->next;
		free(b);
		b = n;
	}
	free(f->name);
	free(f);
}

int
ufs_close(int fdi)
{
	struct filedesc *fd = &file_descriptors[fdi];
	if (fdi < 0 || fdi >= file_descriptor_count || !fd->open) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	fd->open = false;
	fd->file->refs--;

	if (!fd->file->refs && fd->file->ghost)
		destroy_file(fd->file);

	return 0;
}

int
ufs_delete(const char *filename)
{
	struct file *f = find_file(filename);
	if (!f) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	struct file *prev = f->prev, *next = f->next;
	if (prev)
		prev->next = next;
	if (next)
		next->prev = prev;
	f->prev = f->next = NULL;

	if (f == file_list)
		file_list = next;

	if (!f->refs)
		destroy_file(f);
	else
		f->ghost = true;

	return 0;
}

void
ufs_destroy(void)
{
	free(file_descriptors);
	struct file *f = file_list;
	while (f) {
		struct file *n = f->next;
		free(f);
		f = n;
	}
}
