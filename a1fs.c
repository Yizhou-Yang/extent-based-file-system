#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Using 2.9.x FUSE API
#define FUSE_USE_VERSION 29
#include <fuse.h>

#include "a1fs.h"
#include "fs_ctx.h"
#include "options.h"
#include "map.h"

//NOTE: All path arguments are absolute paths within the a1fs file system and
// start with a '/' that corresponds to the a1fs root directory.
//
// For example, if a1fs is mounted at "~/my_csc369_repo/a1b/mnt/", the path to a
// file at "~/my_csc369_repo/a1b/mnt/dir/file" (as seen by the OS) will be
// passed to FUSE callbacks as "/dir/file".
//
// Paths to directories (except for the root directory - "/") do not end in a
// trailing '/'. For example, "~/my_csc369_repo/a1b/mnt/dir/" will be passed to
// FUSE callbacks as "/dir".




/**
 * Initialize the file system.
 *
 * Called when the file system is mounted. NOTE: we are not using the FUSE
 * init() callback since it doesn't support returning errors. This function must
 * be called explicitly before fuse_main().
 *
 * @param fs    file system context to initialize.
 * @param opts  command line options.
 * @return      true on success; false on failure.
 */
static bool a1fs_init(fs_ctx *fs, a1fs_opts *opts)
{
	// Nothing to initialize if only printing help
	if (opts->help) return true;

	size_t size;
	void *image = map_file(opts->img_path, A1FS_BLOCK_SIZE, &size);
	if (!image) return false;

	return fs_ctx_init(fs, image, size);
}

/**
 * Cleanup the file system.
 *
 * Called when the file system is unmounted. Must cleanup all the resources
 * created in a1fs_init().
 */
static void a1fs_destroy(void *ctx)
{
	fs_ctx *fs = (fs_ctx*)ctx;
	if (fs->image) {
		munmap(fs->image, fs->size);
		fs_ctx_destroy(fs);
	}
}


/** Get file system context. */
static fs_ctx *get_fs(void)
{
	return (fs_ctx*)fuse_get_context()->private_data;
}

void *getpointer(void *image,int i){
	return image+(A1FS_BLOCK_SIZE*i); 
}

//some helper functions that prints stuff
void printmap(const char* bitmap,int size){
	int count=0;
    while(count<size){
        unsigned char x = bitmap[count/8];
        for(int j=0;j<8;j++){;
            int num = ((x&(1<<j))>0)?1:0;
            printf("%d",num);
        }	
        printf(" ");
		count+=8;
    }
    printf("\n");
}

//write to the ith place in the bitmap, assume that this place is in the bitmap.
void writemap(char** bitmap,int i){
	int count = i/8;
	int count2 = i%8;
	// printf("writing to the %d location of %d number\n",count2,count);
	(*bitmap)[count]=((*bitmap)[count]|(1<<count2));
}

//read the ith place in bitmap
int readmap(char* bitmap,int i){
	int count = i/8;
	int count2 = i%8;
	return (bitmap[count]&(1<<count2))>0?1:0;
}

//set the bit to 0. not combined with write map because we already used it many times.
void erasemap(char** bitmap,int i){
	int count = i/8;
	int count2 = i%8;
	(*bitmap)[count]=((*bitmap)[count]^(1<<count2));
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
void printcontext(){
	fs_ctx *fs = get_fs();
	const char *ibitmap = (const char *)getpointer(fs->image,fs->ibitmap);
	const char *bbitmap = (const char *)getpointer(fs->image,fs->bbitmap);
	struct a1fs_inode *inode_table = (struct a1fs_inode *)getpointer(fs->image,fs->inode_table);
	printf("\nprintmap1\n");
	printmap(ibitmap,fs->inode_num);
	printf("\nprintmap2\n");
	printmap(bbitmap,24);
	printf("\nprintmap3\n");
	printnode(inode_table,0);
}


//print out the superblock or the context
void printsb(struct a1fs_superblock* sb,void* image){
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


//get first free bit in bitmap, -1 if it can't find one
int get_free_bit(fs_ctx *fs, char **bitmap, int bitmap_size, int ignore_bit) {
	for (int i = 0; i < (int)fs->block_size * 8 * bitmap_size; i++) {
		if (!((*bitmap)[i/8] & (1<<(i%8))) && i != ignore_bit) return i;
	}
	return -1;
}

int get_free_inode_bit(fs_ctx *fs, int ignore_bit) {
	char *bitmap = (char *)getpointer(fs->image, fs->ibitmap);
	int free_bit = get_free_bit(fs, &bitmap, fs->bbitmap - fs->ibitmap, ignore_bit);
	return free_bit;
}

int get_free_block_bit(fs_ctx *fs, int ignore_bit) {
	char *bitmap = (char *)getpointer(fs->image, fs->bbitmap);
	int free_bit = get_free_bit(fs, &bitmap, fs->inode_table - fs->bbitmap, ignore_bit);
	return free_bit;
}

//IMPORTANT:this allocation algorithm keeps fragmentation low because newly allocated blocks are as close as possible to the last extent

//allocate n free data blocks in a way that "keeps fragmentation low"
//return -errno if there is not enough space, return the new number of extents otherwise.
//table is the start of the extent table, extent_num is the old number of extents in an inode
//this function allocates the blocks, memsets them,marks on bbitmap and adds to extent table all in one atomic function
//IMPORTANT!!!!: EXTENT NUM OF THE INODE IS NOT INCREASED HERE, YOU HAVE TO GET THE RETURN VALUE, AND RESET IT YOURSELF!!!
int allocate_blocks(int n, struct a1fs_extent* table, int extent_num){
	fs_ctx *fs = get_fs();
	struct a1fs_extent* curr_extent = table+extent_num; 
	int curr_extnum = extent_num;
	struct a1fs_extent* last_extent = curr_extent-1; 
	int last = (int)(last_extent->start+last_extent->count);
	//extent parameter
	int start = 0;
	int count = 0;
	//start from block last until the end	
	//return as soon as we have the needed blocks
	char *bbitmap = (char*)getpointer(fs->image,fs->bbitmap);
	
	//printf("bbitmap before allocation:\n");
	//printmap(bbitmap,10);
	
	for(int i=last;i<fs->block_num;i++){
		//printf("%d:%d\n",i,n);
		int num = readmap(bbitmap,i);
		//when n==0, start and count does not equal to 0.
		if(curr_extnum>=512){
			fprintf(stderr,"allocate_blocks: Extent out of bound");	
			return -ENOMEM;
		}
		if(n == 0){
			if(count!=0){
				curr_extent->start = start;
				curr_extent->count = count;
				//clear those blocks before allocation;
				memset(getpointer(fs->image,start),0,(count-1)*fs->block_size);
				printf("writen extent:%d:%d\n",start,count);
				curr_extnum++;
			}
			return curr_extnum;
		}
		if(num==0){
			writemap(&bbitmap,i);
			if(start==0){
				start = i;
				count = 1;		
			}
			else if(start!=0){
				count++;		
			}
			n--;
		}
		if((num==1&&start!=0)||i==fs->block_num-1){
			curr_extent->start = start;
			curr_extent->count = count;
			memset(getpointer(fs->image,start),0,(count-1)*fs->block_size);
			// printf("writen extent:%d:%d\n",start,count);
			curr_extent++;
			curr_extnum++;
			start = 0;	
			count = 0;	
		}
	}

	for(int i=fs->first_data_block;i<last;i++){
		// printf("%d:%d\n",i,n);
		int num = readmap(bbitmap,i);
		//when n==0, start and count does not equal to 0.
		if(curr_extnum>=512){
			fprintf(stderr,"allocate_blocks: Extent out of bound");	
			return -ENOMEM;	
		}
		if(n == 0){
			if(count!=0){
				curr_extent->start = start;
				curr_extent->count = count;
				memset(getpointer(fs->image,start),0,(count-1)*fs->block_size);
				printf("writen extent:%d:%d\n",start,count);
				curr_extnum++;
			}
			return curr_extnum;
		}
		if(num==0){
			writemap(&bbitmap,i);
			if(start==0){
				start = i;
				count = 1;		
			}
			else if(start!=0){
				count++;		
			}
			n--;
		}
		if((num==1&&start!=0)||i==last-1){
			curr_extent->start = start;
			curr_extent->count = count;
			memset(getpointer(fs->image,start),0,(count-1)*fs->block_size);
			printf("writen extent:%d:%d\n",start,count);
			curr_extent++;
			curr_extnum++;
			start = 0;	
			count = 0;	
		}
	}
	
	
	if(n == 0) return 0;
	return -1;
}


/**
 * Get file system statistics.
 *
 * Implements the statvfs() system call. See "man 2 statvfs" for details.
 * The f_bfree and f_bavail fields should be set to the same value.
 * The f_ffree and f_favail fields should be set to the same value.
 * The following fields can be ignored: f_fsid, f_flag.
 * All remaining fields are required.
 *
 * Errors: none
 *
 * @param path  path to any file in the file system. Can be ignored.
 * @param st    pointer to the struct statvfs that receives the result.
 * @return      0 on success; -errno on error.
 */
static int a1fs_statfs(const char *path, struct statvfs *st)
{
	fs_ctx *fs = get_fs();

	memset(st, 0, sizeof(*st));
	//TODO: fill in the rest of required fields based on the information stored
	// in the superblock

	//everything here need to be stored in the superblock for data to persist
	
	st->f_bsize   = fs->block_size;
	st->f_frsize  = fs->block_size;
	st->f_blocks = fs->block_num;
	st->f_bfree = fs->free_bnum;
	st->f_bavail = st->f_bfree;

	st->f_files = fs->inode_num;
	st->f_ffree = fs->free_inum;
	st->f_favail = st->f_ffree;

	//store the fsid although can be ignored because I need to check consistency
	st->f_fsid = fs->sid;
	(void) path;
	st->f_namemax = A1FS_NAME_MAX;
	return 0;
}

/**
 * Get file or directory attributes.
 *
 * Implements the lstat() system call. See "man 2 lstat" for details.
 * The following fields can be ignored: st_dev, st_ino, st_uid, st_gid, st_rdev,
 *                                      st_blksize, st_atim, st_ctim.
 * All remaining fields are required.
 *
 * NOTE: the st_blocks field is measured in 512-byte units (disk sectors).
 *
 * Errors:
 *   ENAMETOOLONG  the path or one of its components is too long.
 *   ENOENT        a component of the path does not exist.
 *   ENOTDIR       a component of the path prefix is not a directory.
 *
 * @param path  path to a file or directory.
 * @param st    pointer to the struct stat that receives the result.
 * @return      0 on success; -errno on error;
 */

//a helper that, given a name and an inode, return the child inode that corresponds to the name
struct a1fs_inode* getattr_helper(struct a1fs_inode* curr_inode, char* name, fs_ctx *fs){
	//iterate the extents,find their inode (there are at most 512 extents per file)
	struct a1fs_extent* extent_table = getpointer(fs->image,curr_inode->a1fs_extent_table);
	struct a1fs_inode* itable = getpointer(fs->image,fs->inode_table);
	for(int i=0;i<curr_inode->extent_num;i++){
		//go to the ith extent in the extent table
		struct a1fs_extent* curr_extent = (struct a1fs_extent*)((void*)extent_table+(fs->extent_size * i));
		a1fs_blk_t start = (a1fs_blk_t) curr_extent->start;
		//the size of the file divided by the size of dentry is how many enties there are.
		int iterations = fs->block_size/fs->dentry_size;
		//the starting block of the actual extents
		struct a1fs_dentry* start_entry = (struct a1fs_dentry*) getpointer(fs->image,start);
		//for each extent, read all the dir_entrys in order
		for(int j=0;j<iterations;j++){
			//first find the current entry in the data blocks
			struct a1fs_dentry* curr_entry = start_entry + j;
			// check its name, if same find the inode and return 
			//printf("Getting: Extent %d Dentry: %d (%s|%d)\n", i, j, curr_entry->name, curr_entry->ino);
			if(strcmp(curr_entry->name, name) == 0){
				
				a1fs_ino_t inode = curr_entry->ino;
				return itable+inode;
			}
		}
	}
	//if still not found, then return root(which should never be normally returned), which triggers -ENOENT
	return itable;
}

static int a1fs_getattr(const char *path, struct stat *st)
{
	if (strlen(path) >= A1FS_PATH_MAX) return -ENAMETOOLONG;
	fs_ctx *fs = get_fs();
	memset(st, 0, sizeof(*st));

	struct a1fs_inode* root = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	
	//TODO: lookup the inode for given path and, if it exists, fill in the
	// required fields based on the information stored in the inode
	//find the root inode
	// printf("%d\n",fs->inode_num);
	//printnode(root,0);
	//edge case for the root
	if(strcmp(path,"/")==0){
		//printf("is root\n");
		st->st_mode = root->mode;
		st->st_nlink = root->links;
		st->st_size = root->size;
		st->st_blocks = (root->size)/512;
		st->st_mtim = root->mtime;
		return 0;
	}
	if (strlen(path) >= A1FS_PATH_MAX) return -ENAMETOOLONG;
	//parse the string with strtok
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	char *name = strtok(temp, "/");
	struct a1fs_inode* curr_inode = root;
	while(name!=NULL){
		curr_inode = getattr_helper(curr_inode,name,fs);
		if(curr_inode == root) return -ENOENT;
		name = strtok(NULL,"/");
		if((curr_inode->mode & S_IFREG) && name!=NULL) return -ENOTDIR;
	}
	//now the inode is found, set its stats.
	st->st_mode = curr_inode->mode;
	st->st_nlink = curr_inode->links;
	st->st_size = curr_inode->size;
	st->st_blocks = (curr_inode->size)/512;
	if(curr_inode->size%512!=0) st->st_blocks++; 
	st->st_mtim = curr_inode->mtime;
	return 0;
}


/**
 * Read a directory.
 *
 * Implements the readdir() system call. Should call filler(buf, name, NULL, 0)
 * for each directory entry. See fuse.h in libfuse source code for details.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a filler() call failed).
 *
 * @param path    path to the directory.
 * @param buf     buffer that receives the result.
 * @param filler  function that needs to be called for each directory entry.
 *                Pass 0 as offset (4th argument). 3rd argument can be NULL.
 * @param offset  unused.
 * @param fi      unused.
 * @return        0 on success; -errno on error.
 */


static int a1fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
	(void)offset;// unused
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	//TODO: lookup the directory inode for given path and iterate through its
	// directory entries
	//printf("%s\n",path);
	//find the root inode
	struct a1fs_inode* root = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	//parse the string with strtok
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	char *name = strtok(temp,"/");
	struct a1fs_inode* curr_inode = root;
	while(name!=NULL){
		curr_inode = getattr_helper(curr_inode,name,fs);
		name = strtok(NULL,"/");
	}

	//some duplicate(but with many subtle differences, hard to encapsulate) code
	//now we have the directory inode,iterate all its contents, and call filler.
	//printf("%d\n",curr_inode->a1fs_extent_table);
	struct a1fs_extent* extent_table = getpointer(fs->image,curr_inode->a1fs_extent_table);
	for(int i=0;i<curr_inode->extent_num;i++){
		//go to the ith extent in the extent table
		struct a1fs_extent* curr_extent = extent_table + i;
		a1fs_blk_t start = (a1fs_blk_t) curr_extent-> start;
		//the size of the file divided by the size of dentry is how many enties there are.
		int iterations = fs->block_size/fs->dentry_size;
		//the starting block of the actual extents
		struct a1fs_dentry* start_entry = (struct a1fs_dentry*) getpointer(fs->image,start);
		//for each extent, read all the dir_entrys in order
		for(int j=0;j<iterations;j++){
			//first find the current entry in the data blocks
			struct a1fs_dentry* curr_entry = start_entry+ j;
			//for each entry, if it is . or .., continue, else call filler
			if(strcmp(curr_entry->name,"")!=0&&curr_entry->ino!=0)
			printf("name:%s,inode:%d\n",curr_entry->name,(int)curr_entry->ino);
			if(strcmp(curr_entry->name,".")==0 || strcmp(curr_entry->name,"..")==0){
				//printf("read self or prev\n");
				continue;
			} else if(strcmp(curr_entry->name, "")) {
				if (filler(buf, curr_entry->name , NULL, 0) != 0) {
					return -ENOMEM;	
				}		
			}
		}
	}
	return 0;
}

/** 
 * Traverses a path to find a file/directory's parent directory.
 * 
 * Returns the inode number of the parent of a file/directory at location path.
 */
static a1fs_ino_t get_parent_inode(fs_ctx *fs, const char *path)
{
	fprintf(stderr, "a1fs_mkdir: Looking for parent inode of: %s\n", path);
	
	char fullpath[A1FS_PATH_MAX];
	strncpy(fullpath, path, A1FS_PATH_MAX);
	char *ptr = strtok(fullpath, "/");
	
	char prev_ptr[A1FS_PATH_MAX];
	strcpy(prev_ptr, ptr);
	ptr = strtok(NULL,"/");
	printf("ptr:%s\n\n\n",prev_ptr);
	if (ptr == NULL) {
		return (a1fs_ino_t) 0;
	}
	
	
	struct a1fs_inode *curr_inode = (struct a1fs_inode*) getpointer(fs->image, fs->inode_table);
	int parent_inode_num = 0;
	while(ptr != NULL) {
		struct a1fs_extent *curr_extent = (struct a1fs_extent *) getpointer(fs->image, curr_inode->a1fs_extent_table);
		struct a1fs_dentry *curr_dentry;
		for (a1fs_blk_t i = 0; i < curr_extent->count; i++) {
			curr_dentry = (struct a1fs_dentry *) getpointer(fs->image, curr_extent->start);
			for (int j = 0; j < fs->block_size/fs->dentry_size; j++) {
				if (curr_dentry != NULL && strcmp(curr_dentry->name, "")) {
					if (strcmp(curr_dentry->name, prev_ptr) == 0 && !(curr_inode->mode & S_IFREG)) {
						curr_inode = (struct a1fs_inode*)(getpointer(fs->image, fs->inode_table)) + curr_dentry->ino;
						parent_inode_num = curr_dentry->ino;
					}
				} 
				curr_dentry++;
			}
			curr_extent++; 
		}
		strcpy(prev_ptr, ptr);
		strcat(prev_ptr, "\0");
		ptr = strtok(NULL,"/");
	}
	return parent_inode_num;
}



//given a path, changed size, changed time , update those fields.
//positive for adding, negative for deleting.
//link count and extent_num adding/deleting is local to the parent and will not be done here.
//block count is calculated dynamically so we only update size.
//is_file is 1 for files, 0 for directories.
void update(const char *path, int size_change){
	fs_ctx *fs = get_fs();
	// first get the source inode
	struct a1fs_inode* curr_inode = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	int length = strlen(temp);
	int count = 0;
	//count the number of iterations to go.
	for(int i=0;i<length;i++){
		if(temp[i]=='/')count++;
	}
	//then update the directories knowing file name.
	char *name = strtok(temp,"/");
	while(count>0){
		//update the directory metadata and filename
		curr_inode->size += size_change;
		clock_gettime(CLOCK_REALTIME, &curr_inode->mtime);
		curr_inode = getattr_helper(curr_inode,name,fs);
		name = strtok(NULL,"/");
		count--;
	}
}

//update the superblock and fsctx by checking the bitmaps.
void update_sb(){
	fs_ctx *fs = get_fs();
	struct a1fs_superblock* sb = (struct a1fs_superblock*)fs->image;
	char *bbitmap = (char*)getpointer(fs->image,fs->bbitmap);
	char *ibitmap = (char*)getpointer(fs->image,fs->ibitmap);
	int ifree = 0;
	int bfree = 0;
	for(int i=0;i<sb->inode_num;i++){
		if(readmap(ibitmap,i)==0) ifree++; 
	}
	for(int i=0;i<sb->block_num;i++){
		if(readmap(bbitmap,i)==0) bfree++; 
	}
	sb->free_inum = ifree;
	fs->free_inum = ifree;
	sb->free_bnum = bfree;
	fs->free_bnum = bfree;
}

/**
 * Create a directory.
 *
 * Implements the mkdir() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the directory to create.
 * @param mode  file mode bits.
 * @return      0 on success; -errno on error.
 */

// Writes a dentry to a parent_inode, returns the parent inode
static int write_dentry(fs_ctx *fs, a1fs_ino_t free_inode_num, const char *path)
{	
	a1fs_ino_t parent_inode_num = get_parent_inode(fs, path);
	printf("the parent inode is:%d\n\n",((int)parent_inode_num));
	struct a1fs_inode *parent_inode = (struct a1fs_inode*)getpointer(fs->image, fs->inode_table) + parent_inode_num;
	struct a1fs_extent *parent_extent = (struct a1fs_extent *) getpointer(fs->image, parent_inode->a1fs_extent_table);
	struct a1fs_dentry *curr_dentry;
	// if there is space in the current data block
	for (int extent = 0; extent < parent_inode->extent_num; extent++) {
		curr_dentry = (struct a1fs_dentry *) getpointer(fs->image, parent_extent->start);
		for(a1fs_blk_t i = 0; i < parent_extent->count; i++) {
			for (int j = 0; j < fs->block_size / fs->dentry_size; j++) {
				// find first empty directory entry 
				printf("a1fs_mkdir: Viewing dentry (%s|%d)\n", curr_dentry->name, curr_dentry->ino);
				if (strcmp(curr_dentry->name, "") == 0) {
					printf("a1fs_mkdir: Creating dentry at location %d\n", j);
					curr_dentry->ino = free_inode_num;
					strcpy(curr_dentry->name, strrchr(path, '/') + 1);
					return (int) parent_inode_num;
				}
				curr_dentry++;
			}
		}
		parent_extent++;
	}
	
	// allocate another data block
	struct a1fs_extent *last_extent = parent_extent + parent_inode->extent_num - 1;
	int last_block = last_extent->start + last_extent->count - 1;
	// find a free block, if we can't then return -1 so the outside function throws an error
	int free_bit = get_free_block_bit(fs, -1);
	if (free_bit == -1) return -1;  
	// if we are one after the last block, simply extend the extent, otherwise we need a new extent
	if (free_bit == last_block + 1) {
		last_extent->count++;
	} else {
		// is it even possible to add a new extent?
		printf("a1fs_mkdir: Checking that number of extents isn't greater than or equal to %d\n", fs->block_size / fs->extent_size);
		if (parent_inode->extent_num >= fs->block_size / fs->extent_size) return -1;
		printf("a1fs_mkdir: It isn't!\n");
		// move the extent forward since we know there is space in front of it.
		last_extent++;
		last_extent->start = free_bit;
		last_extent->count = 1;
		// update the parent to make sure it knows a new extent exists
		parent_inode->extent_num++;
	}
	
	
	// initialize the first dentry and rest of its block
	// this is an exception for a1fs_dentries, since we don't want to read garbage data
	curr_dentry->ino = free_inode_num;
	strcpy(curr_dentry->name, strrchr(path, '/') + 1);
	for (int i = 1; i < fs->block_size / fs->dentry_size; i++) {
		curr_dentry++;
		strcpy(curr_dentry->name, "\0");
	}
	// create the new dentry
	struct a1fs_dentry *new_dentry = (struct a1fs_dentry *)getpointer(fs->image, free_bit);
	new_dentry->ino = free_inode_num;
	strcpy(new_dentry->name, strrchr(path, '/') + 1);
	// write the data block that was added into bitmap, and add its size to parent
	char *bbitmap = (char *)getpointer(fs->image, fs->bbitmap);
	writemap(&bbitmap, free_bit);
	parent_inode->size+= fs->block_size;
	update(path,fs->block_size);
	return parent_inode_num;
}

bool check_space(int inode, int block){
	//creating a file/directory requires 1 inode
	//creating a directory requires 1 block
	//for writing,
	fs_ctx *fs = get_fs();
	if(fs->free_inum<inode||fs->free_bnum>block) return false;
	return true;
}

static int a1fs_mkdir(const char *path, mode_t mode)
{
	mode = mode | S_IFDIR;
	fs_ctx *fs = get_fs();
	//TODO: create a directory at given path with given mode
	int free_inode_num = get_free_inode_bit(fs, -1);
	if (free_inode_num == -1) return -ENOSPC;
	fprintf(stderr, "a1fs_mkdir: Creating a new directory at inode number: %d\n", free_inode_num);
	// create a new directory entry in the parent inode
	int parent_inode_num = write_dentry(fs, free_inode_num, path);
	if (parent_inode_num == -1) return -ENOSPC;
	// create the directory, and record a new inode for it
	int free_block_num_1 = get_free_block_bit(fs, -1);
	int free_block_num_2 = get_free_block_bit(fs, free_block_num_1);
	fprintf(stderr, "a1fs_mkdir: Creating an extent table for directory at block number: %d\n", free_block_num_1);
	fprintf(stderr, "a1fs_mkdir: Assigning a data block for directory at block number: %d\n", free_block_num_2);
	if (free_block_num_1 == -1 || free_block_num_2 == -1) return -ENOSPC;
	struct a1fs_inode *free_inode = (struct a1fs_inode *)(((void *) getpointer(fs->image, fs->inode_table)) + (free_inode_num * fs->inode_size));
	fprintf(stderr, "a1fs_mkdir: Creating a free inode: %p\n", free_inode);
	free_inode->mode = mode;
	free_inode->links = 2;
	free_inode->size = fs->block_size;
	free_inode->a1fs_blocks = 1;
	clock_gettime(CLOCK_REALTIME, &free_inode->mtime);

	free_inode->a1fs_extent_table = free_block_num_1;
	free_inode->extent_num = 1;
	struct a1fs_extent *free_extent = (struct a1fs_extent *)getpointer(fs->image, free_inode->a1fs_extent_table);
	free_extent->start = (a1fs_blk_t) free_block_num_2;
	free_extent->count = (a1fs_blk_t) 1;
	
	//add two dentries into the first free extent
	struct a1fs_dentry* this = (struct a1fs_dentry *)getpointer(fs->image,free_extent->start);
	this->ino = (a1fs_ino_t)free_inode_num;
	strncpy(this->name,".",252);

	struct a1fs_dentry* parent = (struct a1fs_dentry*)((void*)this+sizeof(struct a1fs_dentry));
	parent->ino = (a1fs_ino_t)parent_inode_num;
	strncpy(parent->name,"..",252);

	// update parent metadata
	struct a1fs_inode *parent_inode = (struct a1fs_inode*)((void *)getpointer(fs->image, fs->inode_table) + (parent_inode_num * fs->inode_size));
	parent_inode->links++;

	// update inode and block bitmaps
	char *bbitmap = (char *)getpointer(fs->image, fs->bbitmap);
	char *ibitmap = (char *)getpointer(fs->image, fs->ibitmap);
	writemap(&bbitmap, free_block_num_1);
	writemap(&bbitmap, free_block_num_2);
	writemap(&ibitmap, free_inode_num);

	//in the end, update the superblock,size and time.
	update(path, fs->block_size);
	update_sb();
	return 0;
}

/**
 * Remove a directory.
 *
 * Implements the rmdir() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a directory.
 *
 * Errors:
 *   ENOTEMPTY  the directory is not empty.
 *
 * @param path  path to the directory to remove.
 * @return      0 on success; -errno on error.
 */
static int a1fs_rmdir(const char *path)
{
	fs_ctx *fs = get_fs();
	
	//TODO: remove the directory at given path (only if it's empty)
	char fullpath[A1FS_PATH_MAX];
	strncpy(fullpath, path, A1FS_PATH_MAX);
	char *ptr = strtok(fullpath, "/");
	struct a1fs_inode *prev_inode = (struct a1fs_inode *)getpointer(fs->image, fs->inode_table);
	struct a1fs_inode *curr_inode = NULL;
	int curr_inode_num = -1;
	while (ptr != NULL) {
		if (curr_inode != NULL) {
			prev_inode = curr_inode;
		}
		for (int extent = 0; extent < prev_inode->extent_num; extent++) {
			struct a1fs_extent *prev_extent = (struct a1fs_extent *) getpointer(fs->image, prev_inode->a1fs_extent_table) + extent;
			struct a1fs_dentry *prev_dentry = (struct a1fs_dentry *) getpointer(fs->image, prev_extent->start);
			for (a1fs_blk_t i = 0; i < prev_extent->count; i++) {
				for (int j = 0; j < fs->block_size/fs->dentry_size; j++) {
					if (prev_dentry != NULL && !strcmp(prev_dentry->name, ptr)) {
						curr_inode = (struct a1fs_inode *)(getpointer(fs->image, fs->inode_table)) + prev_dentry->ino;
						curr_inode_num = (int)prev_dentry->ino;
					} 
					prev_dentry++;
				}
				prev_extent++; 
			}
		}
		ptr = strtok(NULL, "/");
	}

	if (curr_inode == NULL) return -1;
	if (curr_inode->extent_num > 1){
		printf("current extent number:%d\n\n\n",curr_inode->extent_num);
		return -ENOTEMPTY;
	}
	int extent_table = curr_inode->a1fs_extent_table;
	char *bbitmap = (char *)getpointer(fs->image, fs->bbitmap);
	char *ibitmap = (char *)getpointer(fs->image, fs->ibitmap);
	// get rid of dentry in parent
	struct a1fs_extent *prev_extent = (struct a1fs_extent *) getpointer(fs->image, prev_inode->a1fs_extent_table);
	struct a1fs_dentry *prev_dentry;
	for (int i = 0; i < prev_inode->extent_num; i++) {
		prev_dentry = (struct a1fs_dentry *) getpointer(fs->image, prev_extent->start);
		for(int j = 0; j < fs->block_size / fs->dentry_size; j++) {
			if (prev_dentry != NULL && (int)prev_dentry->ino == curr_inode_num) {
				printf("a1fs_rmdir: Set dentry of %s in parent to empty string.\n", prev_dentry->name);
				strcpy(prev_dentry->name, "\0");
				prev_dentry->ino = 0;
			}
			prev_dentry++;
		}
		prev_extent++; 
	}
	

	// clear parent link and decrease its size by one directory entry
	prev_inode->links--;

	// clear data blocks
	struct a1fs_extent *curr_extent = (struct a1fs_extent *) getpointer(fs->image, extent_table);
	for (int i = 0; i < curr_inode->extent_num; i++) {
		for (a1fs_blk_t j = curr_extent->start; j < curr_extent->start + curr_extent->count; j++) {
			printf("a1fs_rmdir: Unwriting data block number: %d\n", j);
			erasemap(&bbitmap, j);
		}
		curr_extent++;
	}

	// clear extent table
	printf("a1fs_rmdir: Removed extent table at block number: %d\n", extent_table);
	erasemap(&bbitmap, extent_table);
	printf("a1fs_rmdir: Removed directory at inode number: %d\n", curr_inode_num);
	erasemap(&ibitmap, curr_inode_num);

	//update the superblock
	update(path,-(fs->block_size));
	update_sb();
	return 0;
}

/**
 * Create a file.
 *
 * Implements the open()/creat() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" doesn't exist.
 *   The parent directory of "path" exists and is a directory.
 *   "path" and its components are not too long.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to create.
 * @param mode  file mode bits.
 * @param fi    unused.
 * @return      0 on success; -errno on error.
 */
static int a1fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	//things to modify: inode bitmap(file); inode(parent),extent table(parent)(only if a new block is allocated),
	//data block(parent) to indicate that there is a new dentry
	//if the data from the parent block is full, allocate a new block
	//do not modify block bitmap nor random data blocks
	(void)fi;// unused
	assert(S_ISREG(mode));
	fs_ctx *fs = get_fs();
	//TODO: create a file at given path with given mode
	//first use the getattr_helper to find the directory
	struct a1fs_inode* root = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	char *name = strtok(temp,"/");
	struct a1fs_inode* curr_inode = root;
	char filename[A1FS_NAME_MAX];
	while(name!=NULL){
		//first get the next inode
		struct a1fs_inode* next_inode = getattr_helper(curr_inode,name,fs);
		//if it exists, then continue, if it does not exist, then we reached the end
		// store the name as file name, and curr_inode as the parent directory
		if(next_inode == root){
			strcpy(filename,name);
			break;
		}
		name = strtok(NULL,"/");
	}
	
	//printf("%s%d\n",filename,curr_inode->a1fs_extent_table);

	char *ibitmap =(char *)getpointer(fs->image,fs->ibitmap);
	int bit = get_free_inode_bit(fs,-1);
	if (bit==-1) return -ENOSPC;
	writemap(&ibitmap,bit);
	//printmap(ibitmap,4);
	//write the inode table to acutally create the new inode
	struct a1fs_inode* new_inode = root + bit;	
	new_inode->mode = S_IFREG | 0777; 
	new_inode->links= 1;
	new_inode->size = 0;

	//when a new file is created, there is no blocks nor extent table.
	new_inode->a1fs_blocks = 0;
	new_inode->a1fs_extent_table = 0;
	new_inode->extent_num = 0;
	clock_gettime(CLOCK_REALTIME, &new_inode->mtime);
	
	//if we need to,allocate a new block,else the new entry is at the end of the old dentrys.
	write_dentry(fs,(a1fs_ino_t)bit,path);

	//parent's link count need to increase
	curr_inode->links++;

	//iterate back up to change the size and mtime of all ancestors. use a helper.
	update(path,0);
	update_sb();
	return 0;
}

//free the blocks on bitmap, and clear the extent
void clear_extent(struct a1fs_extent* table, int num){
	fs_ctx *fs = get_fs();
	char *bbitmap = (char *)getpointer(fs->image,fs->bbitmap);
	struct a1fs_extent* target = table+num;
	for(unsigned int i=target->start;i<(target->start+target->count);i++){
		erasemap(&bbitmap,i);
	}
	memset(target,0,fs->extent_size);
}


/**
 * Remove a file.
 *
 * Implements the unlink() system call.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors: none
 *
 * @param path  path to the file to remove.
 * @return      0 on success; -errno on error.
 */


static int a1fs_unlink(const char *path)
{
	fs_ctx *fs = get_fs();

	//TODO: remove the file at given path

	//what to do: get the file inode, delete each of its extents(clear bbitmap), delete the dentry, clear ibitmap, clear inode table, 		
	//change parent inode, and change all ancestors
	char *bbitmap = (char *)getpointer(fs->image, fs->bbitmap);
	char *ibitmap = (char *)getpointer(fs->image, fs->ibitmap);
	void *inode_table = getpointer(fs->image, fs->inode_table);
	//first get the inode	
	struct a1fs_inode* root = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	char *name = strtok(temp,"/");
	
	int count = 0;
	struct a1fs_inode* curr_inode = root;
	while(name!=NULL){
		curr_inode = getattr_helper(curr_inode,name,fs);
		count++;
		name = strtok(NULL,"/");
	}
	
	//get parent inode
	strcpy(temp,path);
	name = strtok(temp,"/");
	
	struct a1fs_inode* prev_inode = root;
	while(name!=NULL){
		count--;
		if(count==0) break;
		prev_inode = getattr_helper(prev_inode,name,fs);
		name = strtok(NULL,"/");
	}
	
	int curr_inode_num = ((void*)curr_inode - inode_table)/fs->inode_size;
	struct a1fs_extent* table = getpointer(fs->image,curr_inode->a1fs_extent_table);

	//clear the extent table and free the bbitmap
	for(int i=0;i<curr_inode->extent_num;i++){
		clear_extent(table,i);	
	}
	
	//remove the dentry from parent
	struct a1fs_extent *parent_table = (struct a1fs_extent *) getpointer(fs->image, prev_inode->a1fs_extent_table);
	struct a1fs_dentry *curr_dentry = (struct a1fs_dentry *) getpointer(fs->image, parent_table->start);
	int removed = 0;
	while(removed ==0){
		if((int)(curr_dentry->ino)==curr_inode_num){
			strcpy(curr_dentry->name,"");
			curr_dentry->ino = (a1fs_ino_t)0;
			removed = 1;
			printf("a1fs_rm:file dentry removal successful!\n");	
		}
		curr_dentry++;
	}
	

	// clear extent table, that is if the file has one
	if(curr_inode->a1fs_extent_table!=0){	
		printf("a1fs_rm: Removed extent table at block number: %d\n", curr_inode->a1fs_extent_table);
		erasemap(&bbitmap, curr_inode->a1fs_extent_table);
	}
	printf("a1fs_rm: Removed file at inode number: %d\n", curr_inode_num);
	erasemap(&ibitmap, curr_inode_num);
	//iterate back up to change the size and mtime of all ancestors. use a helper.
	update(path,-curr_inode->size);
	update_sb();
	return 0;
}


/**
 * Change the modification time of a file or directory.
 *
 * Implements the utimensat() system call. See "man 2 utimensat" for details.
 *
 * NOTE: You only need to implement the setting of modification time (mtime).
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists.
 *
 * Errors: none
 *
 * @param path   path to the file or directory.
 * @param times  timestamps array. See "man 2 utimensat" for details.
 * @return       0 on success; -errno on failure.
 */
static int a1fs_utimens(const char *path, const struct timespec times[2])
{
	fs_ctx *fs = get_fs();

	//TODO: update the modification timestamp (mtime) in the inode for given
	// path with either the time passed as argument or the current time,
	// according to the utimensat man page

	//first get the inode	
	struct a1fs_inode* root = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	char *name = strtok(temp,"/");
	struct a1fs_inode* curr_inode = root;
	while(name!=NULL){
		curr_inode = getattr_helper(curr_inode,name,fs);
		name = strtok(NULL,"/");
	}
	
	//now we have the inode, update its time.
	if(times == NULL){
		clock_gettime(CLOCK_REALTIME, &curr_inode->mtime);
	}
	
	else{
		curr_inode->mtime = times[1];
	}
	return 0;
}

/**
 * Change the size of a file.
 *
 * Implements the truncate() system call. Supports both extending and shrinking.
 * If the file is extended, the new uninitialized range at the end must be
 * filled with zeros.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path  path to the file to set the size.
 * @param size  new file size in bytes.
 * @return      0 on success; -errno on error.
 */
 

//ENOMEM AND ENOSPC not yet implemented
static int a1fs_truncate(const char *path, off_t size)
{
	fs_ctx *fs = get_fs();

	//TODO: set new file size, possibly "zeroing out" the uninitialized range
	//first get the inode	
	struct a1fs_inode* root = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	char*bbitmap = (char *)getpointer(fs->image,fs->bbitmap);
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	char *name = strtok(temp,"/");	

	struct a1fs_inode* curr_inode = root;
	while(name!=NULL){
		curr_inode = getattr_helper(curr_inode,name,fs);
		name = strtok(NULL,"/");
	}
	
	int blocks_needed = (int)size/fs->block_size;
	if(size%fs->block_size!=0) blocks_needed++;
	int blocks_actual = curr_inode->size/fs->block_size;
	if(curr_inode->size%fs->block_size!=0) blocks_actual++;
	
	
	//if the current file size is 0, then there is no extent table.. initialize one
	if(curr_inode->size==0){
		curr_inode->a1fs_extent_table = get_free_block_bit(fs,-1);
		writemap(&bbitmap,curr_inode->a1fs_extent_table);
	}
	
	//if there is one already, get it.
	struct a1fs_extent* table = getpointer(fs->image,curr_inode->a1fs_extent_table);
	
	//find the last extent
	struct a1fs_extent* last = table+curr_inode->extent_num-1;
	
	//find last block
	void *last_block = getpointer(fs->image,last->start+last->count-1);
	
	//if size is 0, delete the file and create the same file..
	if(size==0){
		int mode = curr_inode->mode;
		a1fs_unlink(path);
		a1fs_create(path,mode,NULL);
	}
	
	
	if(blocks_needed == blocks_actual){
		//extend the current block
		if((unsigned int)size>curr_inode->size){
			void *data_start = last_block+curr_inode->size%fs->block_size;
			memset(data_start,0,(int)size-curr_inode->size);
		}
		//if smaller then do nothing but change size
	}
	
	//deallocate some blocks
	else if(blocks_needed < blocks_actual){
		//starting from last extent, going back, deallocate entire extents, and remove the extents from table and change bbitmap
		//extent_num-- for each deleted
		//if it becomes 0, then the extent block itself can be removed
		int deallocate_num = blocks_actual-blocks_needed;
		for(int i = curr_inode->extent_num-1;i>=0;i--){
			struct a1fs_extent* extent = table+i;
			int count = extent->count;
			if(count>=deallocate_num){
				extent->count = deallocate_num;
				//erase the bitmaps for these blocks
				for(int i=deallocate_num;i>0;i--){
					erasemap(&bbitmap,extent->start+i-1);
				}
				deallocate_num = 0;
				break;
			}
			else{
				clear_extent(table,i);
				deallocate_num-=extent->count;
				curr_inode->extent_num--;
			}
		}
	}
	
	//allocate more blocks, reset them
	else if(blocks_needed > blocks_actual){
		int status = allocate_blocks(blocks_needed - blocks_actual,table,curr_inode->extent_num);
		
		//printf("bbitmap after allocation:\n");
		//printmap(bbitmap,10);
		
		
		if(status<0) return status;
		else curr_inode->extent_num = status;
	}
	
	//update parent directories for the size change, and update size of file.
	update(path,size-curr_inode->size);
	curr_inode->size=size;
	update_sb();
	return 0;
}


/**
 * Read data from a file.
 *
 * Implements the pread() system call. Must return exactly the number of bytes
 * requested except on EOF (end of file). Reads from file ranges that have not
 * been written to must return ranges filled with zeros. You can assume that the
 * byte range from offset to offset + size is contained within a single block.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors: none
 *
 * @param path    path to the file to read from.
 * @param buf     pointer to the buffer that receives the data.
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to read from.
 * @param fi      unused.
 * @return        number of bytes read on success; 0 if offset is beyond EOF;
 *                -errno on error.
 */
 
//some helpers for debugging

//given an inode, print out the first 10 entries of its extent table
void print_etable(struct a1fs_extent* table){
	printf("printing extent table:\n");
	for(int i=0;i<10;i++){
		if(table->count!=0)printf("start:%d, count:%d\n", table->start,table->count);
		table++;
	}
}
 
//given extent table and the n-th block within, return the block number of the n-th block
//if it is past the last extent return -1.
int get_block(struct a1fs_extent *table,unsigned int n, int extent_num){
	for(int i=0;i<extent_num;i++){
		struct a1fs_extent *curr_extent = table+i;
		if(curr_extent->count>=n){
			printf("block found,returning %d \n",curr_extent->start+n-1);
			return curr_extent->start+n-1;
		}
		else n -= curr_extent->count; 
	}
	return -1;
}
 
static int a1fs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi)
{
	(void)fi;// unused
	fs_ctx *fs = get_fs();
	
	//before everything, first see what buf,size,offset is for this particular read
	printf("read start: buf = %s, size = %ld, offset = %ld\n", buf,size,offset);

	//TODO: read data from the file at given offset into the buffer
	//first get the inode	
	struct a1fs_inode* root = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	char *name = strtok(temp,"/");	

	struct a1fs_inode* curr_inode = root;
	while(name!=NULL){
		curr_inode = getattr_helper(curr_inode,name,fs);
		name = strtok(NULL,"/");
	}
	
	struct a1fs_extent* table = (struct a1fs_extent*)getpointer(fs->image,curr_inode->a1fs_extent_table);
	
	int bytes = size;
	int total_size = offset+size;
	int blocks_needed = total_size/fs->block_size;
	if(total_size%fs->block_size!=0) blocks_needed++;
	if(offset%fs->block_size!=0) blocks_needed++;	
	if(offset>(int)curr_inode->size) return 0;
	printf("blocks_needed:%d,extent_num:%d\n",blocks_needed,curr_inode->extent_num);
	int data_start = get_block(table,blocks_needed,curr_inode->extent_num);
	print_etable(table);
	if(data_start == -1) return 0;
	
	//now we have the start of the data block,get the pointer pointing to data we need.
	void* data = getpointer(fs->image,data_start) + (offset%fs->block_size);
	//if size+offset is bigger than the remainder of the block
	if((size+offset)%fs->block_size>curr_inode->size%fs->block_size){
		void* toclear = getpointer(fs->image,data_start) + (curr_inode->size%fs->block_size);
		//reset the remainder of the block
		memset(toclear,0,fs->block_size-(curr_inode->size%fs->block_size));
		//how many bits we have actually read
		bytes = (curr_inode->size%fs->block_size)- (offset%fs->block_size);
	}
	
	//copy the data
	memcpy(buf,data,size);
	return bytes;
}

/**
 * Write data to a file.
 *
 * Implements the pwrite() system call. Must return exactly the number of bytes
 * requested except on error. If the offset is beyond EOF (end of file), the
 * file must be extended. If the write creates a "hole" of uninitialized data,
 * the new uninitialized range must filled with zeros. You can assume that the
 * byte range from offset to offset + size is contained within a single block.
 *
 * Assumptions (already verified by FUSE using getattr() calls):
 *   "path" exists and is a file.
 *
 * Errors:
 *   ENOMEM  not enough memory (e.g. a malloc() call failed).
 *   ENOSPC  not enough free space in the file system.
 *
 * @param path    path to the file to write to.
 * @param buf     pointer to the buffer containing the data.
 * @param size    buffer size (number of bytes requested).
 * @param offset  offset from the beginning of the file to write to.
 * @param fi      unused.
 * @return        number of bytes written on success; -errno on error.
 */
static int a1fs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi)
{
	(void)fi;// unused
	fs_ctx *fs = get_fs();

	//before everything, first see what buf,size,offset is for this particular write
	printf("write start: buf = %s, size = %ld, offset = %ld\n", buf,size,offset);

	//TODO: write data from the buffer into the file at given offset, possibly
	// "zeroing out" the uninitialized range
	//first get the inode	
	struct a1fs_inode* root = (struct a1fs_inode*) getpointer(fs->image,fs->inode_table);
	char temp[A1FS_NAME_MAX];
	strcpy(temp,path);
	char *name = strtok(temp,"/");	

	struct a1fs_inode* curr_inode = root;
	while(name!=NULL){
		curr_inode = getattr_helper(curr_inode,name,fs);
		name = strtok(NULL,"/");
	}
	
	//note that it is 1 even if we can fit in 0 and dont need to allocate a new block.
	int total_size = offset+size;
	
	int blocks_needed = total_size/fs->block_size;
	if(total_size%fs->block_size!=0) blocks_needed++;
	int blocks_actual = curr_inode->size/fs->block_size;
	if(curr_inode->size%fs->block_size!=0) blocks_actual++;
	
	//extend the file, fill in in-between values with 0.
	//truncate the thing so that its size is exactly what we need
	int status = a1fs_truncate(path,offset+size);
	//this covers both ENOMEM and ENOSPC if truncate is written properly.
	if(status<0) return -status;
	
	struct a1fs_extent* table = (struct a1fs_extent*)getpointer(fs->image,curr_inode->a1fs_extent_table);
	//now that the file is exactly the size we need, start writing to it.
	//get the last block
	//the block will not pass the extent,else there is fatal error
	printf("blocks_needed:%d,extent_num:%d\n",blocks_needed,curr_inode->extent_num);
	int data_start = get_block(table,blocks_needed,curr_inode->extent_num);
	print_etable(table);
	if(data_start==-1) printf("a1fs_write:(fatal error) truncate/write failed\n\n");
	void *last_block = getpointer(fs->image,data_start);
	
	//now add offset%blocksize to it, so that we can start writing
	void *write_start = last_block+(offset%fs->block_size);
	//write!
	memcpy(write_start,buf,size);	
	return size;
}


static struct fuse_operations a1fs_ops = {
	.destroy  = a1fs_destroy,
	.statfs   = a1fs_statfs,
	.getattr  = a1fs_getattr,
	.readdir  = a1fs_readdir,
	.mkdir    = a1fs_mkdir,
	.rmdir    = a1fs_rmdir,
	.create   = a1fs_create,
	.unlink   = a1fs_unlink,
	.utimens  = a1fs_utimens,
	.truncate = a1fs_truncate,
	.read     = a1fs_read,
	.write    = a1fs_write,
};

int main(int argc, char *argv[])
{
	a1fs_opts opts = {0};// defaults are all 0
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	if (!a1fs_opt_parse(&args, &opts)) return 1;

	fs_ctx fs = {0};
	if (!a1fs_init(&fs, &opts)) {
		fprintf(stderr, "Failed to mount the file system\n");
		return 1;
	}

	return fuse_main(args.argc, args.argv, &a1fs_ops, &fs);
}
