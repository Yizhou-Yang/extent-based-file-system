bool fs_ctx_init(fs_ctx *fs, void *image, size_t size)
{	
	fs->image = image;
	fs->size = size;
	//TODO: check if the file system image can be mounted and initialize its
	// runtime state 
	struct a1fs_superblock *sb = (struct a1fs_superblock *)image;
	//read all the important data in the superblock and cache them
	fs->ibitmap = sb->s_inode_bitmap;
	fs->bbitmap = sb->s_blocks_bitmap;
	fs->inode_table = sb->s_inode_table;
	fs->first_data_block = sb->s_first_data_block;
	fs->inode_num = sb->inode_num;
	fs->free_inum = sb->free_inum;
	fs->block_num = sb->block_num;
	fs->free_bnum = sb->free_bnum;
	fs->block_size = sb->block_size;
	fs->inode_size = sb->inode_size;
	fs->extent_size = sb->extent_size;
	fs->dentry_size = sb->dentry_size;
	fs->sid = sb->magic;
	fs->help = sb->help;		
	fs->force = sb->force;
	fs->zero = sb->zero;
	if(fs->sid!= A1FS_MAGIC){
	 	printf("magic not match\n");
		return false;
	}
	return true;
}

void fs_ctx_destroy(fs_ctx *fs)
{
	(void)fs;
}
