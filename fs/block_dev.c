/*
 *  linux/fs/block_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

int block_write(int dev, long * pos, char * buf, int count)
{
	int block = *pos >> BLOCK_SIZE_BITS;
	int offset = *pos & (BLOCK_SIZE-1);
	int chars;
	int written = 0;
	struct buffer_head * bh;
	register char * p;

	while (count>0) {
		chars = BLOCK_SIZE - offset; //从偏移写道尾巴(满足一个数据块)
		if (chars > count)
			chars=count;
		if (chars == BLOCK_SIZE)
			bh = getblk(dev,block);
		else //当chars 不是1024时,读该块及后面的2块
			bh = breada(dev,block,block+1,block+2,-1);
		block++;
		if (!bh)
			return written?written:-EIO;
		p = offset + bh->b_data; // p 是目的地址
		offset = 0;
		*pos += chars;
		written += chars;
		count -= chars;
		while (chars-->0)
			*(p++) = get_fs_byte(buf++); //buf是用户数据,p 是目标地址
		bh->b_dirt = 1;
		brelse(bh);
	}
	return written;
}

int block_read(int dev, unsigned long * pos, char * buf, int count)
{
	int block = *pos >> BLOCK_SIZE_BITS;  // 10bits, 计算block 块号
	int offset = *pos & (BLOCK_SIZE-1); //计算该块中的偏移
	int chars;
	int read = 0;
	struct buffer_head * bh;
	register char * p;

	while (count>0) {
		chars = BLOCK_SIZE-offset;
		if (chars > count)
			chars = count; //计算要读取的字符个数
		if (!(bh = breada(dev,block,block+1,block+2,-1))) //预读三块数据
			return read?read:-EIO;
		block++; //块号加1
		p = offset + bh->b_data;
		offset = 0; //offset 调整为0
		*pos += chars;
		read += chars;
		count -= chars;
		while (chars-->0)
			put_fs_byte(*(p++),buf++);
		brelse(bh);
	}
	return read;
}
