/*
 *  linux/fs/truncate.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>

#include <sys/stat.h>

//free indexed block
static void free_ind(int dev,int block)
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;

	if (!block)
		return;
	if ((bh=bread(dev,block))) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				free_block(dev,*p);
		brelse(bh);
	}
	free_block(dev,block);
}

//free double indexed block
static void free_dind(int dev,int block)
{
	struct buffer_head * bh;
	unsigned short * p;
	int i;

	if (!block)
		return;
	if ((bh=bread(dev,block))) {
		p = (unsigned short *) bh->b_data;
		for (i=0;i<512;i++,p++)
			if (*p)
				free_ind(dev,*p);
		brelse(bh);
	}
	free_block(dev,block);
}

//销毁一个inode
void truncate(struct m_inode * inode)
{
	int i;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode)))
		return;
	for (i=0;i<7;i++) //inode 有7个直辖区,直接存磁盘块号
		if (inode->i_zone[i]) {
			free_block(inode->i_dev,inode->i_zone[i]);
			inode->i_zone[i]=0;
		}
	free_ind(inode->i_dev,inode->i_zone[7]);//一个index区,该块内容为磁盘块号
	free_dind(inode->i_dev,inode->i_zone[8]);//一个double index区,该块内容为磁盘index块号
	inode->i_zone[7] = inode->i_zone[8] = 0;
	inode->i_size = 0;
	inode->i_dirt = 1;
	inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}

