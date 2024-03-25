/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end;
extern void release_super(int);
extern void invalidate_inodes(int);

struct buffer_head * start_buffer = (struct buffer_head *) &end;
struct buffer_head * hash_table[NR_HASH];
static struct buffer_head * free_list;
static struct task_struct * buffer_wait = NULL;
int NR_BUFFERS = 0;

static inline void wait_buffer_unlock(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	sti();
}

int sys_sync(void)
{
	int i;
	struct buffer_head * bh;

	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_buffer_unlock(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_buffer_unlock(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	sync_inodes();
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_buffer_unlock(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

void  invalidate_buffers(int dev) //使设备缓冲块无效
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_buffer_unlock(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(int dev)
{
	int i;

	if (MAJOR(dev) != 2)//不是软盘返回
		return;
	if (!floppy_change(dev & 0x03)) //最多4个软盘，盘无变化返回
		return;
	for (i=0 ; i<NR_SUPER ; i++)  //当软盘已经更换时,该设备信息都要消除.
		if (super_block[i].s_dev == dev) //超级块是该设备
			release_super(super_block[i].s_dev); //释放超级块
	//释放掉该设备的所有inode， inode数最大32个,NR_INODE=32,只能存储32个文件?太小了,玩具系统!
	invalidate_inodes(dev); 
	invalidate_buffers(dev); //释放掉该设备对应的所有buffer
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next; //把自己从链表中摘除，
	if (hash(bh->b_dev,bh->b_blocknr) == bh) //如果自己是hash 头， 更新之
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;
/* remove from free list */
	if (!(bh->b_prev_free) || !(bh->b_next_free)) //这是个环形链表，必需有前后
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free; //把自己从环形链表中摘除
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh) //若自己是free_list, 更新之，
		free_list = bh->b_next_free; // 所以这个free_list 根本不是空闲块链表，而是使用块环形链表，把free_list 改成use_list 更切且。use_list 也不切且，应该叫？？
}

static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */ //是在尾巴插入吗？ 
	bh->b_next_free = free_list; //环形链表，没有尾巴之说，就在指针处断开插入,但从free_list开始数,它是在尾巴上.
	//但这个新找的 bh->b_next_free, bh->b_prev_free 被直接赋值会不会影响到 free_list 这个大的链表？
	//答:不会,因为调用它前已经release该bh了
	bh->b_prev_free = free_list->b_prev_free; 
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

//查找该dev, block 是否在缓冲块中，不在返回空，在返回缓冲块指针
static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
//获取dev,block 的缓冲块,存在则bh->b_count++, 否则返回为NULL
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		bh->b_count++;
		wait_buffer_unlock(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
//返回 dev,block 对应的一个缓冲块
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	if ((bh = get_hash_table(dev,block))) //查看dev,block是否在hash表中,是返回
		return bh;
	tmp = free_list;
	do {
		if (tmp->b_count)//这块被使用了,查看下一块
			continue;
		if (!bh || BADNESS(tmp)<BADNESS(bh)) { //查找可用的bh, 即不dirt,也不lock
			bh = tmp;
			if (!BADNESS(tmp))//非dirt,非lock就选用该块
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list); //从空闲的bh列表中找一个缓冲块
	if (!bh) {
		sleep_on(&buffer_wait);
		goto repeat;
	}
	wait_buffer_unlock(bh); //确认该块没有lock
	if (bh->b_count) //确认该块没有使用
		goto repeat;
	while (bh->b_dirt) { //如果该块已脏,先同步设备
		sync_dev(bh->b_dev);
		wait_buffer_unlock(bh);
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	if (find_buffer(dev,block)) //再确认dev,block 没有对应缓冲块
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	bh->b_count=1;
	bh->b_dirt=0;
	bh->b_uptodate=0;
	remove_from_queues(bh);
	bh->b_dev=dev;
	bh->b_blocknr=block;  // 在这个缓冲块中要存上dev 和 block
	insert_into_queues(bh);//就用找到的这个缓冲块，填好dev,block,插入到队列中
	return bh;
}

//get_hash_table 时 buf->b_count 被++, 需要在何时的时候调用brelse实现b_count平衡
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_buffer_unlock(buf); //解锁该buf
	if (!(buf->b_count--)) //就是把buf->b_count 减1,这是核心.
		panic("Trying to free free buffer");
	wake_up_last(&buffer_wait); //唤醒等待该buf的进程
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */
struct buffer_head * bread(int dev,int block)
{ //此处一次读取1024字节， 2*512bytes, 可认为一个磁盘块是2个扇区块
	struct buffer_head * bh;

	if (!(bh=getblk(dev,block))) //一定会得到一个bh,并且其bh->count>=1, >1表示不只一处使用
		panic("bread: getblk returned NULL\n");
	if (bh->b_uptodate)
		return bh;
	ll_rw_block(READ,bh); //还没有数据,从底层读
	wait_buffer_unlock(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}

#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	)

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

	for (i=0 ; i<4 ; i++) //读取4块为1页(4K)
		if (b[i]) {
			if ((bh[i] = getblk(dev,b[i])))
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		if (bh[i]) {
			wait_buffer_unlock(bh[i]);
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]); // 就是把bh[i]->count-1 表示本次操作不再占用该bh
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args;
	struct buffer_head * bh, *tmp;

	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
//				ll_rw_block(READA,bh);
				ll_rw_block(READA,tmp); //应该是tmp
			//第一次getbld(dev,first)申请时会为1,此处count减1变成了0,表示它没有被占用.
			//因为tmp 这部分是预读.
			tmp->b_count--; 
		}
	}
	va_end(args);
	wait_buffer_unlock(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer;
	void * b;
	int i;

	if (buffer_end == 1<<20)
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {
		h->b_dev = 0;
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b;
		h->b_prev_free = h-1;
		h->b_next_free = h+1;
		h++;
		NR_BUFFERS++;
		if (b == (void *) 0x100000)
			b = (void *) 0xA0000;
	}
	h--;
	free_list = start_buffer; //此时free_list 是一个最大的环形链表
	free_list->b_prev_free = h;
	h->b_next_free = free_list;
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	
