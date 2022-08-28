/*
 *  linux/fs/namei.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#include <string.h> 
#include <fcntl.h>
#include <errno.h>
#include <const.h>
#include <sys/stat.h>

#define ACC_MODE(x) ("\004\002\006\377"[(x)&O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4

/*
 *	permission()
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 */
static int permission(struct m_inode * inode,int mask)
{
	int mode = inode->i_mode;

/* special case: not even root can read/write a deleted file */
	if (inode->i_dev && !inode->i_nlinks)
		return 0;
	else if (current->euid==inode->i_uid)
		mode >>= 6;
	else if (current->egid==inode->i_gid)
		mode >>= 3;
	if (((mode & mask & 0007) == mask) || suser())
		return 1;
	return 0;
}

/*
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, match returns 1 for success, 0 for failure.
 */
static int match(int len,const char * name,struct dir_entry * de)
{
	register int same ;

	if (!de || !de->inodenr || len > NAME_LEN)
		return 0;
	if (len < NAME_LEN && de->name[len])
		return 0;
	__asm__("cld\n\t"
		"fs ; repe ; cmpsb\n\t"		// 用户空间比较
		"setz %%al"					// 若为0设置al=1
		:"=a" (same)
		:"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
		);
	return same;
}

/*
 *	find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * This also takes care of the few special cases due to '..'-traversal
 * over a pseudo-root and a mount point.
 */
static struct buffer_head * find_entry(struct m_inode ** dir_i,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int entries;
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;
	struct super_block * sb;

#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	entries = (*dir_i)->i_size / (sizeof (struct dir_entry));
	*res_dir = NULL;
	if (!namelen)
		return NULL;
/* check for '..', as we might have to do some "magic" for it */
	if (namelen==2 && get_fs_byte(name)=='.' && get_fs_byte(name+1)=='.') {
/* '..' in a pseudo-root results in a faked '.' (just change namelen) */
		if ((*dir_i) == current->root)
			namelen=1;
		else if ((*dir_i)->i_num == ROOT_INO) {
/* '..' over a mount-point results in 'dir_i' being exchanged for the mounted
   directory-inode. NOTE! We set mounted, so that we can iput the new dir_i */
			sb=get_super((*dir_i)->i_dev);
			if (sb->s_inode_mount) {
				iput(*dir_i);
				(*dir_i)=sb->s_inode_mount;
				(*dir_i)->i_count++;
			}
		}
	}
	if (!(block = (*dir_i)->i_zone[0]))
		return NULL;
	if (!(bh = bread((*dir_i)->i_dev,block)))
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	while (i < entries) {
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			if (!(block = bmap(*dir_i,i/DIR_ENTRIES_PER_BLOCK)) ||
			    !(bh = bread((*dir_i)->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		if (match(namelen,name,de)) {
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

/*
 *	add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
static struct buffer_head * add_entry(struct m_inode * dir_i,
	const char * name, int namelen, struct dir_entry ** res_dir)
{
	int block,i;
	struct buffer_head * bh;
	struct dir_entry * de;

	*res_dir = NULL;
#ifdef NO_TRUNCATE
	if (namelen > NAME_LEN)
		return NULL;
#else
	if (namelen > NAME_LEN)
		namelen = NAME_LEN;
#endif
	if (!namelen)
		return NULL;
	if (!(block = dir_i->i_zone[0]))
		return NULL;
	if (!(bh = bread(dir_i->i_dev,block)))
		return NULL;
	i = 0;
	de = (struct dir_entry *) bh->b_data;
	while (1) {
		if ((char *)de >= BLOCK_SIZE+bh->b_data) {
			brelse(bh);
			bh = NULL;
			block = create_block(dir_i,i/DIR_ENTRIES_PER_BLOCK);
			if (!block)
				return NULL;
			if (!(bh = bread(dir_i->i_dev,block))) {
				i += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			de = (struct dir_entry *) bh->b_data;
		}
		if (i*sizeof(struct dir_entry) >= dir_i->i_size) {
			de->inodenr=0;
			dir_i->i_size = (i+1)*sizeof(struct dir_entry);
			dir_i->i_dirt = 1;
			dir_i->i_ctime = CURRENT_TIME;
		}
		if (!de->inodenr) {
			dir_i->i_mtime = CURRENT_TIME;
			for (i=0; i < NAME_LEN ; i++)
				de->name[i]=(i<namelen)?get_fs_byte(name+i):0;
			bh->b_dirt = 1;
			*res_dir = de;
			return bh;
		}
		de++;
		i++;
	}
	brelse(bh);
	return NULL;
}

/*
 *	get_dir_inode()
 *
 * Getdir traverses the pathname until it hits the topmost directory.
 * It returns NULL on failure.
 */
static struct m_inode * get_dir_inode(const char * pathname)
{
	char c;
	const char * thisname;
	struct m_inode * inode;
	struct buffer_head * bh;
	int namelen,inr,idev;
	struct dir_entry * de;

	if (!current->root || !current->root->i_count)
		panic("No root inode");
	if (!current->pwd || !current->pwd->i_count)
		panic("No cwd inode");
	if ((c=get_fs_byte(pathname))=='/') {
		inode = current->root;
		pathname++;
	} else if (c)
		inode = current->pwd;
	else
		return NULL;	/* empty name is bad */
	inode->i_count++;
	while (1) {
		thisname = pathname;
		if (!S_ISDIR(inode->i_mode) || !permission(inode,MAY_EXEC)) {
			iput(inode);
			return NULL;
		}
		for(namelen=0;(c=get_fs_byte(pathname++))&&(c!='/');namelen++)
			/* nothing */ ;
		if (!c)
			return inode;
		if (!(bh = find_entry(&inode,thisname,namelen,&de))) {
			iput(inode);
			return NULL;
		}
		inr = de->inodenr;
		idev = inode->i_dev;
		brelse(bh);
		iput(inode);
		if (!(inode = iget(idev,inr)))
			return NULL;
	}
}

/*
 *	dir_namei()
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */
static struct m_inode * dir_namei(const char * pathname,
	int * namelen, const char ** name)
{
	char c;
	const char * basename;
	struct m_inode * node;

	if (!(node = get_dir_inode(pathname)))
		return NULL;
	basename = pathname;
	while ((c=get_fs_byte(pathname++)))
		if (c=='/')
			basename=pathname;
	*namelen = pathname-basename-1;
	*name = basename;
	return node;
}

/*
 *	namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */
struct m_inode * namei(const char * pathname)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir_i;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir_i = dir_namei(pathname,&namelen,&basename)))
		return NULL;
	if (!namelen)			/* special case: '/usr/' etc */
		return dir_i;
	bh = find_entry(&dir_i,basename,namelen,&de);
	if (!bh) {
		iput(dir_i);
		return NULL;
	}
	inr = de->inodenr;
	dev = dir_i->i_dev;
	brelse(bh);
	iput(dir_i);
	dir_i=iget(dev,inr);
	if (dir_i) {
		dir_i->i_atime=CURRENT_TIME;
		dir_i->i_dirt=1;
	}
	return dir_i;
}

/*
 *	open_namei()
 *
 * namei for open - this is in fact almost the whole open-routine.
 */
int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode)
{
	const char * basename;
	int inr,dev,namelen;
	struct m_inode * dir_i, *inode_n;
	struct buffer_head * bh;
	struct dir_entry * de;

	if ((flag & O_TRUNC) && !(flag & O_ACCMODE))
		flag |= O_WRONLY;
	mode &= 0777 & ~current->umask;
	mode |= I_REGULAR;
	if (!(dir_i = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {			/* special case: '/usr/' etc */
		if (!(flag & (O_ACCMODE|O_CREAT|O_TRUNC))) {
			*res_inode=dir_i;
			return 0;
		}
		iput(dir_i);
		return -EISDIR;
	}
	bh = find_entry(&dir_i,basename,namelen,&de);
	if (!bh) {
		if (!(flag & O_CREAT)) {
			iput(dir_i);
			return -ENOENT;
		}
		if (!permission(dir_i,MAY_WRITE)) {
			iput(dir_i);
			return -EACCES;
		}
		inode_n = new_inode(dir_i->i_dev);
		if (!inode_n) {
			iput(dir_i);
			return -ENOSPC;
		}
		inode_n->i_uid = current->euid;
		inode_n->i_mode = mode;
		inode_n->i_dirt = 1;
		bh = add_entry(dir_i,basename,namelen,&de);
		if (!bh) {
			inode_n->i_nlinks--;
			iput(inode_n);
			iput(dir_i);
			return -ENOSPC;
		}
		de->inodenr = inode_n->i_num;
		bh->b_dirt = 1;
		brelse(bh);
		iput(dir_i);
		*res_inode = inode_n;
		return 0;
	}
	inr = de->inodenr;
	dev = dir_i->i_dev;
	brelse(bh);
	iput(dir_i);
	if (flag & O_EXCL)
		return -EEXIST;
	if (!(inode_n=iget(dev,inr)))
		return -EACCES;
	if ((S_ISDIR(inode_n->i_mode) && (flag & O_ACCMODE)) ||
	    !permission(inode_n,ACC_MODE(flag))) {
		iput(inode_n);
		return -EPERM;
	}
	inode_n->i_atime = CURRENT_TIME;
	if (flag & O_TRUNC)
		truncate(inode_n);
	*res_inode = inode_n;
	return 0;
}

int sys_mknod(const char * filename, int mode, int dev)
{
	const char * basename;
	int namelen;
	struct m_inode * dir_i, * inode_n;
	struct buffer_head * bh;
	struct dir_entry * de;
	
	if (!suser())
		return -EPERM;
	if (!(dir_i = dir_namei(filename,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir_i);
		return -ENOENT;
	}
	if (!permission(dir_i,MAY_WRITE)) {
		iput(dir_i);
		return -EPERM;
	}
	bh = find_entry(&dir_i,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir_i);
		return -EEXIST;
	}
	inode_n = new_inode(dir_i->i_dev);
	if (!inode_n) {
		iput(dir_i);
		return -ENOSPC;
	}
	inode_n->i_mode = mode;
	if (S_ISBLK(mode) || S_ISCHR(mode))
		inode_n->i_zone[0] = dev;
	inode_n->i_mtime = inode_n->i_atime = CURRENT_TIME;
	inode_n->i_dirt = 1;
	bh = add_entry(dir_i,basename,namelen,&de);
	if (!bh) {
		iput(dir_i);
		inode_n->i_nlinks=0;
		iput(inode_n);
		return -ENOSPC;
	}
	de->inodenr = inode_n->i_num;
	bh->b_dirt = 1;
	iput(dir_i);
	iput(inode_n);
	brelse(bh);
	return 0;
}

int sys_mkdir(const char * pathname, int mode)
{
	const char * basename;
	int namelen;
	struct m_inode * dir_i, * inode_n;
	struct buffer_head * bh, *dir_block;
	struct dir_entry * de;

	if (!suser())
		return -EPERM;
	if (!(dir_i = dir_namei(pathname,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir_i);
		return -ENOENT;
	}
	if (!permission(dir_i,MAY_WRITE)) {
		iput(dir_i);
		return -EPERM;
	}
	bh = find_entry(&dir_i,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir_i);
		return -EEXIST;
	}
	inode_n = new_inode(dir_i->i_dev);
	if (!inode_n) {
		iput(dir_i);
		return -ENOSPC;
	}
	inode_n->i_size = 32;
	inode_n->i_dirt = 1;
	inode_n->i_mtime = inode_n->i_atime = CURRENT_TIME;
	if (!(inode_n->i_zone[0]=new_block(inode_n->i_dev))) {
		iput(dir_i);
		inode_n->i_nlinks--;
		iput(inode_n);
		return -ENOSPC;
	}
	inode_n->i_dirt = 1;
	if (!(dir_block=bread(inode_n->i_dev,inode_n->i_zone[0]))) {
		iput(dir_i);
		free_block(inode_n->i_dev,inode_n->i_zone[0]);
		inode_n->i_nlinks--;
		iput(inode_n);
		return -ERROR;
	}
	de = (struct dir_entry *) dir_block->b_data;
	de->inodenr=inode_n->i_num;
	strcpy(de->name,".");
	de++;
	de->inodenr = dir_i->i_num;
	strcpy(de->name,"..");
	inode_n->i_nlinks = 2;
	dir_block->b_dirt = 1;
	brelse(dir_block);
	inode_n->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
	inode_n->i_dirt = 1;
	bh = add_entry(dir_i,basename,namelen,&de);
	if (!bh) {
		iput(dir_i);
		free_block(inode_n->i_dev,inode_n->i_zone[0]);
		inode_n->i_nlinks=0;
		iput(inode_n);
		return -ENOSPC;
	}
	de->inodenr = inode_n->i_num;
	bh->b_dirt = 1;
	dir_i->i_nlinks++;
	dir_i->i_dirt = 1;
	iput(dir_i);
	iput(inode_n);
	brelse(bh);
	return 0;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int empty_dir(struct m_inode * inode)
{
	int nr,block;
	int len;
	struct buffer_head * bh;
	struct dir_entry * de;

	len = inode->i_size / sizeof (struct dir_entry);
	if (len<2 || !inode->i_zone[0] ||
	    !(bh=bread(inode->i_dev,inode->i_zone[0]))) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	de = (struct dir_entry *) bh->b_data;
	if (de[0].inodenr != inode->i_num || !de[1].inodenr || 
	    strcmp(".",de[0].name) || strcmp("..",de[1].name)) {
	    	printk("warning - bad directory on dev %04x\n",inode->i_dev);
		return 0;
	}
	nr = 2;
	de += 2;
	while (nr<len) {
		if ((void *) de >= (void *) (bh->b_data+BLOCK_SIZE)) {
			brelse(bh);
			block=bmap(inode,nr/DIR_ENTRIES_PER_BLOCK);
			if (!block) {
				nr += DIR_ENTRIES_PER_BLOCK;
				continue;
			}
			if (!(bh=bread(inode->i_dev,block)))
				return 0;
			de = (struct dir_entry *) bh->b_data;
		}
		if (de->inodenr) {
			brelse(bh);
			return 0;
		}
		de++;
		nr++;
	}
	brelse(bh);
	return 1;
}

int sys_rmdir(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir_i, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!suser())
		return -EPERM;
	if (!(dir_i = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir_i);
		return -ENOENT;
	}
	if (!permission(dir_i,MAY_WRITE)) {
		iput(dir_i);
		return -EPERM;
	}
	bh = find_entry(&dir_i,basename,namelen,&de);
	if (!bh) {
		iput(dir_i);
		return -ENOENT;
	}
	if (!(inode = iget(dir_i->i_dev, de->inodenr))) {
		iput(dir_i);
		brelse(bh);
		return -EPERM;
	}
	if ((dir_i->i_mode & S_ISVTX) && current->euid &&
	    inode->i_uid != current->euid) {
		iput(dir_i);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode->i_dev != dir_i->i_dev || inode->i_count>1) {
		iput(dir_i);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (inode == dir_i) {	/* we may not delete ".", but "../dir" is ok */
		iput(inode);
		iput(dir_i);
		brelse(bh);
		return -EPERM;
	}
	if (!S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir_i);
		brelse(bh);
		return -ENOTDIR;
	}
	if (!empty_dir(inode)) {
		iput(inode);
		iput(dir_i);
		brelse(bh);
		return -ENOTEMPTY;
	}
	if (inode->i_nlinks != 2)
		printk("empty directory has nlink!=2 (%d)",inode->i_nlinks);
	de->inodenr = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks=0;
	inode->i_dirt=1;
	dir_i->i_nlinks--;
	dir_i->i_ctime = dir_i->i_mtime = CURRENT_TIME;
	dir_i->i_dirt=1;
	iput(dir_i);
	iput(inode);
	return 0;
}

int sys_unlink(const char * name)
{
	const char * basename;
	int namelen;
	struct m_inode * dir_i, * inode;
	struct buffer_head * bh;
	struct dir_entry * de;

	if (!(dir_i = dir_namei(name,&namelen,&basename)))
		return -ENOENT;
	if (!namelen) {
		iput(dir_i);
		return -ENOENT;
	}
	if (!permission(dir_i,MAY_WRITE)) {
		iput(dir_i);
		return -EPERM;
	}
	bh = find_entry(&dir_i,basename,namelen,&de);
	if (!bh) {
		iput(dir_i);
		return -ENOENT;
	}
	if (!(inode = iget(dir_i->i_dev, de->inodenr))) {
		iput(dir_i);
		brelse(bh);
		return -ENOENT;
	}
	if ((dir_i->i_mode & S_ISVTX) && !suser() &&
	    current->euid != inode->i_uid &&
	    current->euid != dir_i->i_uid) {
		iput(dir_i);
		iput(inode);
		brelse(bh);
		return -EPERM;
	}
	if (S_ISDIR(inode->i_mode)) {
		iput(inode);
		iput(dir_i);
		brelse(bh);
		return -EPERM;
	}
	if (!inode->i_nlinks) {
		printk("Deleting nonexistent file (%04x:%d), %d\n",
			inode->i_dev,inode->i_num,inode->i_nlinks);
		inode->i_nlinks=1;
	}
	de->inodenr = 0;
	bh->b_dirt = 1;
	brelse(bh);
	inode->i_nlinks--;
	inode->i_dirt = 1;
	inode->i_ctime = CURRENT_TIME;
	iput(inode);
	iput(dir_i);
	return 0;
}

int sys_link(const char * oldname, const char * newname)
{
	struct dir_entry * de;
	struct m_inode * oldinode, * dir_i;
	struct buffer_head * bh;
	const char * basename;
	int namelen;

	oldinode=namei(oldname);
	if (!oldinode)
		return -ENOENT;
	if (S_ISDIR(oldinode->i_mode)) {
		iput(oldinode);
		return -EPERM;
	}
	dir_i = dir_namei(newname,&namelen,&basename);
	if (!dir_i) {
		iput(oldinode);
		return -EACCES;
	}
	if (!namelen) {
		iput(oldinode);
		iput(dir_i);
		return -EPERM;
	}
	if (dir_i->i_dev != oldinode->i_dev) {
		iput(dir_i);
		iput(oldinode);
		return -EXDEV;
	}
	if (!permission(dir_i,MAY_WRITE)) {
		iput(dir_i);
		iput(oldinode);
		return -EACCES;
	}
	bh = find_entry(&dir_i,basename,namelen,&de);
	if (bh) {
		brelse(bh);
		iput(dir_i);
		iput(oldinode);
		return -EEXIST;
	}
	bh = add_entry(dir_i,basename,namelen,&de);
	if (!bh) {
		iput(dir_i);
		iput(oldinode);
		return -ENOSPC;
	}
	de->inodenr = oldinode->i_num;
	bh->b_dirt = 1;
	brelse(bh);
	iput(dir_i);
	oldinode->i_nlinks++;
	oldinode->i_ctime = CURRENT_TIME;
	oldinode->i_dirt = 1;
	iput(oldinode);
	return 0;
}
