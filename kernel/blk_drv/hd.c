/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR 3
#include "blk.h"

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

/* Max read/write errors/sector */
#define MAX_ERRORS	7
#define MAX_HD		2

static void recal_intr(void);

static int recalibrate = 0;
static int reset = 0;

/*
 *  This struct defines the HD's and their types.
 */
struct hd_i_struct {
	int head,sect,cyl,wpcom,lzone,ctl;
	};
#ifdef HD_TYPE
struct hd_i_struct hd_info[] = { HD_TYPE };
#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))
#else
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;
#endif

static struct hd_struct {
	long start_sect;
	long nr_sects;
} hd[5*MAX_HD]={{0,0},};

#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr))

#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr))

extern void hd_interrupt(void);
extern void rd_load(void);

/* This may be used only once, enforced by 'static int callable' */
int sys_setup(void * BIOS)
{
	static int callable = 1;
	int i,drive;
	unsigned char cmos_disks;
	struct partition *p;
	struct buffer_head * bh;

	if (!callable)
		return -1;
	callable = 0;
#ifndef HD_TYPE
	for (drive=0 ; drive<2 ; drive++) {
		hd_info[drive].cyl = *(unsigned short *) BIOS;
		hd_info[drive].head = *(unsigned char *) (2+BIOS);
		hd_info[drive].wpcom = *(unsigned short *) (5+BIOS);
		hd_info[drive].ctl = *(unsigned char *) (8+BIOS);
		hd_info[drive].lzone = *(unsigned short *) (12+BIOS);
		hd_info[drive].sect = *(unsigned char *) (14+BIOS);
		BIOS += 16;
	}
	if (hd_info[1].cyl)
		NR_HD=2;
	else
		NR_HD=1;
#endif
	for (i=0 ; i<NR_HD ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = hd_info[i].head*
				hd_info[i].sect*hd_info[i].cyl;
	}

	/*
		We querry CMOS about hard disks : it could be that 
		we have a SCSI/ESDI/etc controller that is BIOS
		compatable with ST-506, and thus showing up in our
		BIOS table, but not register compatable, and therefore
		not present in CMOS.

		Furthurmore, we will assume that our ST-506 drives
		<if any> are the primary drives in the system, and 
		the ones reflected as drive 1 or 2.

		The first drive is stored in the high nibble of CMOS
		byte 0x12, the second in the low nibble.  This will be
		either a 4 bit drive type or 0xf indicating use byte 0x19 
		for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

		Needless to say, a non-zero value means we have 
		an AT controller hard disk for that drive.

		
	*/

	if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
		if (cmos_disks & 0x0f)
			NR_HD = 2;
		else
			NR_HD = 1;
	else
		NR_HD = 0;
	for (i = NR_HD ; i < 2 ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = 0;
	}
	for (drive=0 ; drive<NR_HD ; drive++) {
		//读磁盘主引导扇区,输入参数从设备号是 drive*5 (指待一个主分区,4个分区)
		if (!(bh = bread(0x300 + drive*5,0))) { 
			printk("Unable to read partition table of drive %d\n\r",
				drive);
			panic("");
		}
		if (bh->b_data[510] != 0x55 || (unsigned char)
		    bh->b_data[511] != 0xAA) {
			printk("Bad partition table on drive %d\n\r",drive);
			panic("");
		}
		p = 0x1BE + (void *)bh->b_data;
		for (i=1;i<5;i++,p++) { //存4个分区的信息
			hd[i+5*drive].start_sect = p->start_sect;
			hd[i+5*drive].nr_sects = p->nr_sects;
		}
		brelse(bh);
	}
	if (NR_HD)
		printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
	rd_load();
	mount_root();
	return (0);
}

static int controller_ready(void)
{
	int retries=100000;

	while (--retries && (inb_p(HD_STATUS)&0x80));
	return (retries);
}

static int win_result(void)
{
	int i=inb_p(HD_STATUS);

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);
	return (1);
}
//向硬盘drive(设备号)发出读写命令(cmd),位置在cyl,head,sect,大小nsect,回调程序intr_addr
static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void))
{
	register int port asm("dx");

	if (drive>1 || head>15)
		panic("Trying to write bad sector");
	if (!controller_ready())
		panic("HD controller not ready");
	do_hd = intr_addr; //注册do_hd 函数,硬盘中断程序的回调函数
	outb_p(hd_info[drive].ctl,HD_CMD);
	port=HD_DATA;
	outb_p(hd_info[drive].wpcom>>2,++port);
	outb_p(nsect,++port);
	outb_p(sect,++port);
	outb_p(cyl,++port);
	outb_p(cyl>>8,++port);
	outb_p(0xA0|(drive<<4)|head,++port);
	outb(cmd,++port); //发命令及参数到端口,等其中断响应
}

static int drive_busy(void)
{
	unsigned int i;

	for (i = 0; i < 10000; i++)
		if (READY_STAT == (inb_p(HD_STATUS) & (BUSY_STAT|READY_STAT)))
			break;
	i = inb(HD_STATUS); //读取状态寄存器
	i &= BUSY_STAT | READY_STAT | SEEK_STAT;
	if (i == (READY_STAT | SEEK_STAT))
		return(0);
	printk("HD controller times out\n\r");
	return(1);
}

static void reset_controller(void)
{
	int	i;

	outb(4,HD_CMD);
	for(i = 0; i < 100; i++) nop();
	outb(hd_info[0].ctl & 0x0f ,HD_CMD);
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if ((i = inb(HD_ERROR)) != 1)
		printk("HD-controller reset failed: %02x\n\r",i);
}

static void reset_hd(int nr) //nr 是子设备号
{
	reset_controller(); //发命令到命令端口复位控制器
	hd_out(nr,hd_info[nr].sect,hd_info[nr].sect,hd_info[nr].head-1,
		   hd_info[nr].cyl,WIN_SPECIFY,&recal_intr); //执行命令
}

void unexpected_hd_interrupt(void)
{
	printk("Unexpected HD interrupt\n\r");
}
//出错加1并做相应处理
static void bad_rw_intr(void)
{
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	if (CURRENT->errors > MAX_ERRORS/2)
		reset = 1;
}

static void read_intr(void)
{
	if (win_result()) { //查询状态不成功时
		bad_rw_intr();
		do_hd_request();
		return;
	}
	port_read(HD_DATA,CURRENT->buffer,256); //从端口读取256个word
	CURRENT->errors = 0;
	CURRENT->buffer += 512; //缓冲指针加512
	CURRENT->sector++; // 扇区数加1
	if (--CURRENT->nr_sectors) {
		do_hd = &read_intr; //还有数据需要读,设置read_intr 为回调
		return;
	}
	end_request(1); //数据已读完,正常结束请求
	do_hd_request(); //做硬盘请求? 处理下一请求
}

static void write_intr(void)
{
	if (win_result()) { //结果查询
		bad_rw_intr();
		do_hd_request();
		return;
	}
	if (--CURRENT->nr_sectors) {
		CURRENT->sector++;
		CURRENT->buffer += 512;
		do_hd = &write_intr; //还有数据要写,设回调函数,写一个扇区数据
		port_write(HD_DATA,CURRENT->buffer,256);
		return;
	}
	end_request(1);//正常结束请求
	do_hd_request();//处理下一请求
}

static void recal_intr(void)
{
	if (win_result()) //查当前状态
		bad_rw_intr(); // not-ok,把出错计数加1并做相应处理
	do_hd_request(); //处理下一请求
}
//从链表中拿到下一个请求,处理请求
void do_hd_request(void)
{
	int i,r = 0;
	unsigned int block,dev;
	unsigned int sec,head,cyl;
	unsigned int nsect;

	INIT_REQUEST;
	dev = MINOR(CURRENT->dev); //此处CURRENT 是当前请求 blk_dev[MAJOR_NR].current_request
	block = CURRENT->sector;
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) { //合法性判别
		end_request(0); //善后处理,给出出错提示,唤醒等待进程及更新请求等.
		goto repeat; //repeat 在 INIT_REQUEST 中定义
	}
	block += hd[dev].start_sect; //此处的dev是(0-4) (一个硬盘1个主分区+4个分区) 
	dev /= 5;//此处dev 是子设备号
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
			"r" (hd_info[dev].sect)); //余数在edx,为sector,商在eax,继续做除法
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
			"r" (hd_info[dev].head)); //block/sector/head = cylinder, 余数head
	sec++;
	nsect = CURRENT->nr_sectors; //扇区数
	if (reset) { //静态全局变量
		reset = 0;
		recalibrate = 1;
		reset_hd(CURRENT_DEV); //是设备号,硬盘设备号是(子设备号/5),每个子设备可有4个分区.
		return;
	}
	if (recalibrate) {
		recalibrate = 0;
		hd_out(dev,hd_info[CURRENT_DEV].sect,0,0,0,
			   WIN_RESTORE,&recal_intr); //发校准命令
		return;
	}	
	if (CURRENT->cmd == WRITE) {
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr); //发写命令
		for(i=0 ; i<3000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++) //查询状态
			/* nothing */ ;
		if (!r) {
			bad_rw_intr();
			goto repeat;
		}
		//写一个扇区数据,数据写完成后,会调用write_intr
		port_write(HD_DATA,CURRENT->buffer,256); 
	} else if (CURRENT->cmd == READ) {
		//发读命令,数据读回后,会调用read_intr
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
	} else
		panic("unknown hd-command");
}

void hd_init(void)
{
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST; //此处函数为do_hd_request
	set_intr_gate(0x2E,&hd_interrupt); //设定0x2e中断门,服务程序为hd_interrupt
	outb_p(inb_p(0x21)&0xfb,0x21); // 带pause 输出(值,端口)
	outb(inb_p(0xA1)&0xbf,0xA1); // 输出(值,端口)
}
