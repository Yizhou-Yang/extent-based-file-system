# extent-based-file-system
An C written extent based file system. PERSONAL USE AND MAY NOT BE SHARED PUBLICLY
The only reason I'm including this to my github publicly but not privitely is so that interviewers get a rough idea of my OS skills.

my work focuses on a1fs.c, which implements many standard operations of a file system. mkdir,rmdir,touch,ls,stat,rm and truncate/read/write.
mkfs.c and fsctx.c and correponding .h files are also designed and implemented by me. Everything else is our prof's initial starter code.

it is still 1500+ lines of my original code and my own design, though. It is similar to ext2 but it is extent based like ext4.

it is written entirely in C.

I removed all work not done by myself, therefore this code does not compile. But I did included the school given autotest to provide an insight of how this code worked.
