/** Determine if the image has already been formatted into a1fs. */
static bool a1fs_is_present(void *image)
{
	//TODO: check if the image already contains a valid a1fs superblock
	struct a1fs_superblock *sb = (struct a1fs_superblock *)image;
	if(sb->magic!=A1FS_MAGIC) return false;

	//printf("superblock exists\n");

	return true;
}

void *getpointer(void *image,int i){
	return image+(A1FS_BLOCK_SIZE*i); 
}

//some helper functions that prints stuff
void printmap(const char* bitmap,int size){
	int count=0;
    while(count<size){
        unsigned char x = bitmap[count/8];
        for(int j=0;j<8;j++){
			if(count==size){
				printf("\n");
				return;
			}
            int num = ((x&(1<<j))>0)?1:0;
            printf("%d",num);
			count++;
        }	
        printf(" ");
    }
    printf("\n");
}

//write to the ith place in the bitmap, assume that this place is in the bitmap.
void writemap(char** bitmap,int i){
	int count = i/8;
	int count2 = i%8;
	//printf("writing to the %d location of %d number\n",count2,count);
	(*bitmap)[count]=((*bitmap)[count]|(1<<count2));
}

//print out information about the num-th inode in the inode table
void printnode(struct a1fs_inode *inode_table, int num){
	struct a1fs_inode* node = inode_table+(num*sizeof(struct a1fs_inode));
	char type = (node->mode == 33188)?'f':'d'; 
	//printf("mode:%hu \n",node->i_mode);
	unsigned int size = node->size;
	unsigned short links = node->links;
	unsigned int blocks = node->a1fs_blocks; 
	printf("[%d] type: %c size: %u links: %hu blocks: %u\n",num,type,size,links,blocks);
}

//print out the superblock or the context
void printsb(struct a1fs_superblock* sb){
	void *image = (void*)sb;
	const char *ibitmap = (const char *)getpointer(image,sb->s_inode_bitmap);
	const char *bbitmap = (const char *)getpointer(image,sb->s_blocks_bitmap);
	struct a1fs_inode *inode_table = (struct a1fs_inode *)getpointer(image,sb->s_inode_table);
	printf("\nprintmap1\n");
	printmap(ibitmap,sb->inode_num);
	printf("\nprintmap2\n");
	printmap(bbitmap,24);
	printf("\nprintmap3\n");
	printnode(inode_table,0);
}

//given a pointer, return the block of this pointer. used for debugging.
int getblock(void* pt,void* image){
	int offset = (int)(pt-image);
	return offset/A1FS_BLOCK_SIZE;
}

/**
 * Format the image into a1fs.
 *
 * NOTE: Must update mtime of the root directory.
 *
 * @param image  pointer to the start of the image.
 * @param size   image size in bytes.
 * @param opts   command line options.
 * @return       true on success;
 *               false on error, e.g. options are invalid for given image size.
 */
static bool mkfs(void *image, size_t size, mkfs_opts *opts)
{	
	if(size<4*A1FS_BLOCK_SIZE){
		return false;	
	}
	//TODO: initialize the superblock and create an empty root directory
	//NOTE: the mode of the root directory inode should be set to S_IFDIR | 0777

	//create the superblock
	struct a1fs_superblock* sb = ((struct a1fs_superblock *)image);
	
	printf("get sb:%d\n", getblock((void*)sb,image));

	//calculate the metadata
	int inode_num = opts->n_inodes;
	int blocks_num = size/A1FS_BLOCK_SIZE;
	int ibitmap_blocks = inode_num/A1FS_BLOCK_SIZE;
	if(inode_num%A1FS_BLOCK_SIZE!=0) ibitmap_blocks++;
	int bbitmap_blocks = blocks_num/A1FS_BLOCK_SIZE;
	if(blocks_num%A1FS_BLOCK_SIZE!=0) bbitmap_blocks++;
	int inode_table_blocks = (opts->n_inodes * sizeof(struct a1fs_inode))/A1FS_BLOCK_SIZE;
	if((opts->n_inodes * sizeof(struct a1fs_inode))%A1FS_BLOCK_SIZE!=0) inode_table_blocks++;

	//reset the blocks, so that sb and both bitmaps are protected
	memset(image, 0, (ibitmap_blocks+1+bbitmap_blocks)*A1FS_BLOCK_SIZE);

	//initialize the superblock
	sb->magic = (uint64_t) A1FS_MAGIC;
	sb->size = (uint64_t) size;
	sb->s_inode_bitmap = 1;
	sb->s_blocks_bitmap = sb->s_inode_bitmap+ ibitmap_blocks;
	sb->s_inode_table = sb->s_blocks_bitmap+ bbitmap_blocks;
	sb->s_first_data_block = sb->s_inode_table + inode_table_blocks;

	
	//the place where the metadata ends, used for writing bitmap.
	int end = sb->s_first_data_block;


	//write the bitmaps
	char* ibitmap = (char*) getpointer(image,sb->s_inode_bitmap);
	char* bbitmap = (char*) getpointer(image,sb->s_blocks_bitmap);
	writemap(&ibitmap,0);
	for(int i=0;i<end;i++) writemap(&bbitmap,i);

	//print the bitmaps
	printf("get ibitmap:%d\n", getblock((void*)ibitmap,image));
	printf("printing ibitmap:\n");
	printmap(ibitmap,inode_num);

	printf("get bbitmap:%d\n", getblock((void*)bbitmap,image));
	printf("\n printing bbitmap:\n");
	printmap(bbitmap,16);

	//create root inode
	struct a1fs_inode* rootnode =  (struct a1fs_inode *)getpointer(image,sb->s_inode_table);
	//printf("get rootnode:%d\n", getblock((void*)rootnode,image));
	rootnode->mode = S_IFDIR | 0777; 
	rootnode->links=2;
	rootnode->size = A1FS_BLOCK_SIZE;
	rootnode->a1fs_blocks = 1;
	rootnode->a1fs_extent_table = sb->s_first_data_block;
	rootnode->extent_num = 1;
	clock_gettime(CLOCK_REALTIME, &rootnode->mtime);
	//create the contents in root
	struct a1fs_extent* firstextent = (struct a1fs_extent *)getpointer(image,rootnode->a1fs_extent_table);
	//printf("get firstextent:%d\n", getblock((void*)firstextent,image));
	firstextent->start = end+1;
	//printf("%ld\n",sizeof(struct a1fs_dentry));
	firstextent->count = 1;

	//write the block bitmap for these blocks
	writemap(&bbitmap,rootnode->a1fs_extent_table);
	for(unsigned int i=0;i<firstextent->count;i++){
		writemap(&bbitmap,firstextent->start+i);	
	}

	struct a1fs_dentry* this = (struct a1fs_dentry *)getpointer(image,firstextent->start);
	//printf("get this:%d\n", getblock((void*)this,image));
	this->ino = (a1fs_ino_t)0;
	strncpy(this->name,".",252);

	struct a1fs_dentry* parent = (struct a1fs_dentry*)((void*)this+sizeof(struct a1fs_dentry));
	//printf("get parent:%d\n", getblock((void*)parent,image));
	parent->ino = (a1fs_ino_t)0;
	//printf("%d\n",(int)parent->ino);
	strncpy(parent->name,"..",252);

	//initialize the superblock
	sb-> inode_num = inode_num;
	sb-> free_inum = inode_num-1;
	sb-> block_num = blocks_num;
	sb-> free_bnum = blocks_num - (ibitmap_blocks+bbitmap_blocks+inode_table_blocks+1);
	sb-> block_size = A1FS_BLOCK_SIZE;
	sb-> inode_size = sizeof(struct a1fs_inode);
	//printf("%d\n", sb->inode_size);
	sb-> extent_size = sizeof(struct a1fs_extent);
	sb-> dentry_size = sizeof(struct a1fs_dentry);
	//printf("%d\n", sb-> dentry_size);
	sb-> help = opts->help;
	sb-> force = opts->force;
	sb-> zero = opts->zero;
	sb-> magic = A1FS_MAGIC;

	//print out the root inode
	printnode(rootnode,0);

	//print some messages
	printf("Image size: %ld KB\n",sb->size/1024);
	printf("Inodes: %d, %d reserved\n",inode_num, sb->inode_num-sb-> free_inum);
	printf("Blocks: %d, %d reserved\n\n", blocks_num, blocks_num-sb-> free_bnum);

	//printf("Inode bitmap: [0,%d) (%d blocks)\n",ibitmap_blocks,ibitmap_blocks);
	//printf("Block bitmap: [%d,%d) (%d blocks)\n",ibitmap_blocks,ibitmap_blocks+bbitmap_blocks,bbitmap_blocks);
	//printf("Inode table: [%d,%d) (%d blocks)\n",ibitmap_blocks+bbitmap_blocks,end,inode_table_blocks);

	printsb(sb);
	return true;
}


int main(int argc, char *argv[])
{
	mkfs_opts opts = {0};// defaults are all 0
	if (!parse_args(argc, argv, &opts)) {
		// Invalid arguments, print help to stderr
		print_help(stderr, argv[0]);
		return 1;
	}
	if (opts.help) {
		// Help requested, print it to stdout
		print_help(stdout, argv[0]);
		return 0;
	}

	// Map image file into memory
	size_t size;
	void *image = map_file(opts.img_path, A1FS_BLOCK_SIZE, &size);
	if (image == NULL) return 1;

	// Check if overwriting existing file system
	int ret = 1;
	if (!opts.force && a1fs_is_present(image)) {
		fprintf(stderr, "Image already contains a1fs; use -f to overwrite\n");
		goto end;
	}

	if (opts.zero) memset(image, 0, size);
	if (!mkfs(image, size, &opts)) {
		fprintf(stderr, "Failed to format the image\n");
		goto end;
	}

	ret = 0;
end:
	munmap(image, size);
	return ret;
}
