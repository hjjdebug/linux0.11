/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* test_bit uses setb, as gas doesn't recognize setc */
#define test_bit(bitnr,addr) ({ \
register int __res ; \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void unlock_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up_last(&(sb->s_wait));
	sti();
}

static void wait_on_super_unlock(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}
//获取super_block *
//如果8个super 中有该dev 的super, 返回指针,否则返回为NULL
struct super_block * check_in_superb(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		if (s->s_dev == dev) {
			wait_on_super_unlock(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void release_super(int dev) //change put_super 为release_super
{
	struct super_block * sb;
	/* struct m_inode * inode;*/
	int i;

	if (dev == ROOT_DEV) {		// 根文件系统设备是不能释放的
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = check_in_superb(dev)))
		return;
	if (sb->s_inode_mount) {		//还有引用就卸载，显示警告
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++) //把8个imap 缓存块释放掉
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);//把8个zmap 缓存块释放掉
	unlock_super(sb);
	return;
}
//从磁盘获取super_block *
static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	if ((s = check_in_superb(dev))) //取到了 super_block* 返回指针
		return s;
	for (s = 0+super_block ;; s++) { //如果无空闲超级块,返回空
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev)
			break; //找到了空闲超级块,就用它
	}
	s->s_dev = dev;
	s->s_inode_super = NULL;
	s->s_inode_mount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s);
	if (!(bh = bread(dev,1))) { //磁盘上第1个块号是超级块, (第0个是引导块)
		s->s_dev=0;
		unlock_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data); //读取的数据存入结构变量s
	brelse(bh);
	if (s->s_magic != SUPER_MAGIC) { //对读取的数据分析
		s->s_dev = 0;
		unlock_super(s);
		return NULL;
	}
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;// 第0块为boot块，第1块为super块,第2块为inode map 块，1块（1024byte)
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if ((s->s_imap[i]=bread(dev,block))) //bread 返回的是buffer_header*
			block++; //把inode_map 全部读入内存,最大8块
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if ((s->s_zmap[i]=bread(dev,block)))
			block++; //把zone_map 全部读入内存,最大8块
		else
			break;
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++) //合法性判别,不合法全部释放
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		unlock_super(s);
		return NULL;
	}
	s->s_imap[0]->b_data[0] |= 1;  // 将地址或上1，另有用途
	s->s_zmap[0]->b_data[0] |= 1;
	unlock_super(s);
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=check_in_superb(dev)) || !(sb->s_inode_mount))
		return -ENOENT;
	if (!sb->s_inode_mount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_inode_mount->i_mount=0; //super 有2个内存inode, 一个inode_mount,一个inode_super, 它们都指向根inode
	iput(sb->s_inode_mount); //主要是对inode->icount--并相应处理
	sb->s_inode_mount = NULL;
	iput(sb->s_inode_super);
	sb->s_inode_super = NULL;
	release_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_inode_mount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	sb->s_inode_mount=dir_i;
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

//需要读取到设备上的super_block 和 ROOT inode,这就叫mount_root,表示根可用了
void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode)) //合法性检查
		panic("bad i-node size");
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0; //初始化file_table(64个)
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL; //初始化超级块结构(8个),实际用一个就够了.
	}
	if (!(p=read_super(ROOT_DEV))) //从设备上要能读到超级块
		panic("Unable to mount root");
	if (!(mi=iget(ROOT_DEV,ROOT_INO))) //从设备上要能读到根INODE
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	p->s_inode_super = p->s_inode_mount = mi;
	current->pwd = mi; //该inode 作为当前工作目录和根目录
	current->root = mi;
	free=0;
	i=p->s_nzones; //得到块个数
	while (-- i >= 0) //计算空block个数, bh为1k字节,8kbits
		if (!test_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1; //得到inode 个数
	while (-- i >= 0) //计算空闲的inode 个数
		if (!test_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
