/*
 * This code is provided solely for the personal and private use of students
 * taking the CSC369H course at the University of Toronto. Copying for purposes
 * other than this use is expressly prohibited. All forms of distribution of
 * this code, including but not limited to public repositories on GitHub,
 * GitLab, Bitbucket, or any other online platform, whether as given or with
 * any changes, are expressly prohibited.
 *
 * Authors: Alexey Khrabrov, Karen Reid
 *
 * All of the files in this directory and all subdirectories are:
 * Copyright (c) 2020 Karen Reid
 */

/**
 * CSC369 Assignment 1 - File system runtime context header file.
 */

#pragma once

#include <stddef.h>

#include "options.h"
#include "a1fs.h"

/**
 * Mounted file system runtime state - "fs context".
 */
typedef struct fs_ctx {
	/** Pointer to the start of the image. */
	void *image;
	/** Image size in bytes. */
	size_t size;

	//TODO: useful runtime state of the mounted file system should be cached
	// here (NOT in global variables in a1fs.c)
	/** the block of the Blocks bitmap. */
	unsigned int bbitmap;
	/** the block of the Inode bitmap. */
	unsigned int ibitmap;
	/** the block of the First Data Block. */
	unsigned int first_data_block;
	/** the block of the Inode table. */
	unsigned int inode_table;
	// inode number
	int inode_num;
	// free inode number
	int free_inum;
	// number of blocks stored
	int block_num;
	// free block number
	int free_bnum;
	// block size (A1FS_BLOCK_SIZE)
	int block_size;
	// inode size
	int inode_size;
	// extent size
	int extent_size;
	// directory entry size
	int dentry_size;
	// magic
	unsigned long sid;
	// command line options from mkfs_opts
	bool help;
	bool force;
	bool zero;
} fs_ctx;

/**
 * Initialize file system context.
 *
 * @param fs     pointer to the context to initialize.
 * @param image  pointer to the start of the image.
 * @param size   image size in bytes.
 * @return       true on success; false on failure (e.g. invalid superblock).
 */
bool fs_ctx_init(fs_ctx *fs, void *image, size_t size);

/**
 * Destroy file system context.
 *
 * Must cleanup all the resources created in fs_ctx_init().
 */
void fs_ctx_destroy(fs_ctx *fs);
