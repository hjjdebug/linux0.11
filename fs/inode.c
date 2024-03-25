/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h> 
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);

static inline void wait_on_inode_unlock(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up_last(&inode->i_wait);
}

void invalidate_inodes(int dev) //释放掉所有该设备的inode, inode数最大32个NR_INODE=32
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode_unlock(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;
		}
	}
}

void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode_unlock(inode);
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);
	}
}
// input: inode(i节点指针）, block_seq(数据块顺序号）,create(bool)
// return:  盘块号（磁盘上的逻辑块号）
// descpription: block map, 数据块映射到磁盘块处理程序
// _bmap 是块映射的意思
static int _bmap(struct m_inode * inode,int block_seq,int create)
{
	struct buffer_head * bh;
	int i;

	if (block_seq<0)
		panic("_bmap: block<0");
	if (block_seq >= 7+512+512*512)
		panic("_bmap: block>big");
	if (block_seq<7) { //数据块号<7为直接寻址块
		if (create && !inode->i_zone[block_seq])
			if ((inode->i_zone[block_seq]=new_block(inode->i_dev))) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block_seq];
	}
	block_seq -= 7;
	if (block_seq<512) { // 块号在7-512+7之间为一级间接寻址
		if (create && !inode->i_zone[7])
			if ((inode->i_zone[7]=new_block(inode->i_dev))) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7]) //i_zone[7]存放了一页,上面是顺序号到块号的映射
			return 0;
		if (!(bh = bread(inode->i_dev,inode->i_zone[7]))) //先读这页内容
			return 0;
		i = ((unsigned short *) (bh->b_data))[block_seq];//1个块号占2bytes,可存512个块号
		if (create && !i)
			if ((i=new_block(inode->i_dev))) {
				((unsigned short *) (bh->b_data))[block_seq]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	block_seq -= 512; //块号>512+7为2级寻址
	if (create && !inode->i_zone[8])
		if ((inode->i_zone[8]=new_block(inode->i_dev))) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])//该页是2级索引
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8]))) //先把该页内容读出来
		return 0;
	i = ((unsigned short *)bh->b_data)[block_seq>>9]; //数序号>>9先找到页地址
	if (create && !i)
		if ((i=new_block(inode->i_dev))) {
			((unsigned short *) (bh->b_data))[block_seq>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i))) //把该页内容再取出来
		return 0;
	i = ((unsigned short *)bh->b_data)[block_seq&511]; //页号与511的余数找到对应的块号
	if (create && !i)
		if ((i=new_block(inode->i_dev))) {
			((unsigned short *) (bh->b_data))[block_seq&511]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

int get_diskBlock(struct m_inode * inode,int block_seq) //修改bmap为 ->get_diskBlock
{
	return _bmap(inode,block_seq,0);
}

int create_diskBlock(struct m_inode * inode, int block_seq) //修改create_block为 ->create_diskBlock
{
	return _bmap(inode,block_seq,1);
}
//主要功能时inode->i_count--, 并做关联处理		
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode_unlock(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	if (inode->i_pipe) { //当inode 是管道时
		wake_up_last(&inode->i_wait);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	if (!inode->i_dev) {
		inode->i_count--;
		return;
	}
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode_unlock(inode);
	}
repeat:
	if (inode->i_count>1) { 
		inode->i_count--;//还有引用,则返回
		return;
	}
	if (!inode->i_nlinks) { //没有任何连接,则销毁.
		truncate(inode);
		free_inode(inode);
		return;
	}
	if (inode->i_dirt) { // dirt 要同步
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode_unlock(inode);
		goto repeat;
	}
	inode->i_count--; //有可能减到0
	return;
}
//inode_table表一共才有32个INODE, 要拿到一个空的inode
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table; //环形操作
			if (!last_inode->i_count) { //如果该inode 未被使用,找到,退出.
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		if (!inode) { //出错提示,死机
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		wait_on_inode_unlock(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode_unlock(inode);
		}
	} while (inode->i_count);
	memset(inode,0,sizeof(*inode));
	inode->i_count = 1;
	return inode;
}

struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}
//输入dev 和 node number,节点号从1开始, 返回i_node
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode(); //获取一个空inode
	inode = inode_table;
	// 查询inode_table表,看看inode是否被超级块mount,32个inode
	while (inode < NR_INODE+inode_table) {	
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode_unlock(inode);
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		inode->i_count++; //表明该inode在使用
		if (inode->i_mount) { //如果该inode被mount过
			int i;

			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_inode_mount==inode) //查找是那个super_block
					break;
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode); //把该inode 放回
			dev = super_block[i].s_dev; //该dev 未该super_block dev
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode); //把这个inode 从磁盘中读进来(根据inore->i_num)
	return inode;
}
//从磁盘读取inode 数据.
//根据inode的dev和inr, 计算出blocknr,读取该blocknr,从而更新了inode磁盘部分数据
static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!(sb=check_in_superb(inode->i_dev)))
		panic("trying to read inode without dev");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +		//2 为一个启动块，一个超级块,后面是imap+zmap,再后面是inode 块!
		(inode->i_num-1)/INODES_PER_BLOCK;  //由inode_nr号就能计算出磁盘块号
	if (!(bh=bread(inode->i_dev,block)))			//找到磁盘块,读磁盘
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data) //由于一个磁盘块包含32个inode,所以再计算出准确位置,取出磁盘inode
			[(inode->i_num-1)%INODES_PER_BLOCK];	//找到对应的项
	brelse(bh);
	unlock_inode(inode);
}
//只是设立了缓冲区与磁盘不一致标志
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=check_in_superb(inode->i_dev)))
		panic("trying to write inode without device");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +		//启动块+超级块占了2块,i节点位图块，逻辑位图块, i节点号-1
		(inode->i_num-1)/INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =				//更新对应的inode
			*(struct d_inode *)inode;
	bh->b_dirt=1;											// 缓冲区与磁盘不一致，故置1, 该缓冲块会被写回磁盘
	inode->i_dirt=0;										// i节点已与缓冲区一致，故清0
	//为什么要release?,只是把bh-count-1, bh->b_dirt=1 怎么办? 做请求时或读该node或刷新时会同步到磁盘
	brelse(bh); 
	unlock_inode(inode);
}
