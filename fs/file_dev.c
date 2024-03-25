/*
 *  linux/fs/file_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <fcntl.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

int file_read(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	int left,chars,nr;
	struct buffer_head * bh;

	if ((left=count)<=0)
		return 0;
	while (left) { //数据通过不断的循环来得到, 每次最多只读一个BLOCK
		//先拿到文件位置所对应的块号
		if ((nr = get_diskBlock(inode,(filp->f_pos)/BLOCK_SIZE))) {
			if (!(bh=bread(inode->i_dev,nr))) //读取该块号对应的数据,可见是1块1块的读取的
				break;
		} else
			bh = NULL;
		nr = filp->f_pos % BLOCK_SIZE; //文件位置取余得到从块边界开始的大小
//计算该块中有效的字符数,调整只会发生在第一次和最后一次读,中间都是整块
		chars = MIN( BLOCK_SIZE-nr , left ); 
		filp->f_pos += chars; //当修正了chars 后, (left 大时)下一次又会对齐块边界了.
		left -= chars;
		if (bh) {
			char * p = nr + bh->b_data;
			while (chars-->0)
				put_fs_byte(*(p++),buf++); //把磁盘缓冲数据传送到用户区
			brelse(bh);
		} else {
			while (chars-->0)
				put_fs_byte(0,buf++);
		}
	}
	inode->i_atime = CURRENT_TIME;
	return (count-left)?(count-left):-ERROR;
}

int file_write(struct m_inode * inode, struct file * filp, char * buf, int count)
{
	off_t pos;
	int block,c;
	struct buffer_head * bh;
	char * p;
	int i=0;

/*
 * ok, append may not work when many processes are writing at the same time
 * but so what. That way leads to madness anyway.
 */
	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = filp->f_pos;
	while (i<count) { // pos/BLOCK_SIZE为文件偏移块号, block为系统总的块号
		if (!(block = create_diskBlock(inode,pos/BLOCK_SIZE)))
			break;
		if (!(bh=bread(inode->i_dev,block)))
			break;
		c = pos % BLOCK_SIZE;
		p = c + bh->b_data;
		bh->b_dirt = 1;  //buffer_head 已经脏了(因为已经写了数据),需要刷新到磁盘,在适当的时候会刷新.
		c = BLOCK_SIZE-c;
		if (c > count-i) c = count-i;  //计算要写入的个数,count-i是还剩余的字节数
		pos += c; // 修正pos
		if (pos > inode->i_size) {
			inode->i_size = pos;
			inode->i_dirt = 1; // inode dirty
		}
		i += c; //修正计数
		while (c-->0)
			*(p++) = get_fs_byte(buf++); //从参数取数,放入磁盘缓冲
		brelse(bh);
	}
	inode->i_mtime = CURRENT_TIME; //modify time
	if (!(filp->f_flags & O_APPEND)) {
		filp->f_pos = pos;
		inode->i_ctime = CURRENT_TIME; //create time
	}
	return (i?i:-1);
}
