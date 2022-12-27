/*
 *  linux/kernel/keyboard.S
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *	Thanks to Alfred Leung for US keyboard patches
 *		Wolfgang Thiel for German keyboard patches
 *		Marc Corsini for the French keyboard
 */

#include <linux/config.h>

.text
.globl keyboard_interrupt

/*
 * these are for the keyboard read functions
 */
size	= 1024		/* must be a power of two ! And MUST be the same
			   as in tty_io.c !!!! */
head = 4
tail = 8
proc_list = 12
buf = 16

mode:	.byte 0		/* caps, alt, ctrl and shift mode */
leds:	.byte 2		/* num-lock, caps, scroll-lock mode (nom-lock on) */
e0:	.byte 0

/*
 *  con_int is the real interrupt routine that reads the
 *  keyboard scan-code and converts it into the appropriate
 *  ascii character(s).
 */
keyboard_interrupt:
	pushl %eax
	pushl %ebx
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	movl $0x10,%eax  //将ds,es置为内核段
	mov %ax,%ds
	mov %ax,%es
	xor %al,%al		/* %eax is scan code */
	inb $0x60,%al	//从0x60端口读取扫描码
	cmpb $0xe0,%al
	je set_e0
	cmpb $0xe1,%al	// set_e0,set_e1
	je set_e1
	call *key_table(,%eax,4)  //执行扫描码转换
	movb $0,e0
e0_e1:	inb $0x61,%al		//取键盘端口b 状态
	jmp 1f
1:	jmp 1f
1:	orb $0x80,%al		//bit7 置1
	jmp 1f
1:	jmp 1f
1:	outb %al,$0x61		//禁止键盘
	jmp 1f
1:	jmp 1f
1:	andb $0x7F,%al		//bit7 置0
	outb %al,$0x61		//使竜键盘
	movb $0x20,%al		//向8259芯片发送EOI 中断结束信号
	outb %al,$0x20
	pushl $0			//console 编号
	call do_tty_interrupt
	addl $4,%esp
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	iret
set_e0:	movb $1,e0
	jmp e0_e1
set_e1:	movb $2,e0
	jmp e0_e1

/*
 * This routine fills the buffer with max 8 bytes, taken from
 * %ebx:%eax. (%edx is high). The bytes are written in the
 * order %al,%ah,%eal,%eah,%bl,%bh ... until %eax is zero.
 */
put_queue:
	pushl %ecx
	pushl %edx
	movl table_list,%edx		# read-queue for console, 取tty_table[0].read_q送edx
	movl head(%edx),%ecx		//head->4, 取reqd_q的head(头指针）送ecx
1:	movb %al,buf(%edx,%ecx)		//buf->16	将al 存人read_q的buf的头指针处
	incl %ecx					//头指针加1
	andl $size-1,%ecx			//size=1024
	cmpl tail(%edx),%ecx		# buffer full - discard everything tail->8
	je 3f
	shrdl $8,%ebx,%eax			//将ebx右移8bit到eax中，ebx不变
	je 2f						//为0跳2
	shrl $8,%ebx				//ebx右移8bit,回跳1
	jmp 1b
2:	movl %ecx,head(%edx)		//保存头指针
	movl proc_list(%edx),%ecx	//取进程指针
	testl %ecx,%ecx
	je 3f
	movl $0,(%ecx)				//把0赋值给proc_list->state， 就绪
3:	popl %edx
	popl %ecx
	ret

ctrl:	movb $0x04,%al	//bit2
	jmp 1f
alt:	movb $0x10,%al	//bit5
1:	cmpb $0,e0			//e0 为0吗？e0为1表示右边的ctrl,alt
	je 2f				//等于0转走
	addb %al,%al		// al二进制相加,变成相应的mode
2:	orb %al,mode		//设置mode标志中对应bit位
	ret
unctrl:	movb $0x04,%al
	jmp 1f
unalt:	movb $0x10,%al
1:	cmpb $0,e0
	je 2f
	addb %al,%al
2:	notb %al
	andb %al,mode 	//将相应的位清除
	ret

lshift:
	orb $0x01,mode
	ret
unlshift:
	andb $0xfe,mode
	ret
rshift:
	orb $0x02,mode
	ret
unrshift:
	andb $0xfd,mode
	ret

caps:	testb $0x80,mode
	jne 1f
	xorb $4,leds
	xorb $0x40,mode
	orb $0x80,mode
set_leds:
	call kb_wait
	movb $0xed,%al		/* set leds command */ //发送0xed 到键盘控制器0x60端口
	outb %al,$0x60
	call kb_wait
	movb leds,%al		//发送leds 到0x60端口
	outb %al,$0x60
	ret
uncaps:	andb $0x7f,mode
	ret
scroll:
	xorb $1,leds
	jmp set_leds
num:	xorb $2,leds
	jmp set_leds

/* 方向键,数字键盘方向键处理
 *  curosr-key/numeric keypad cursor keys are handled here.
 *  checking for numeric keypad etc.
 */
cursor:
	subb $0x47,%al	//0x47 数字小键盘
	jb 1f
	cmpb $12,%al
	ja 1f
	jne cur2		/* check for ctrl-alt-del */ //想等是del键(0x53)
	testb $0x0c,mode	//ctrl键?
	je cur2
	testb $0x30,mode	//alt键?
	jne reboot
cur2:	cmpb $0x01,e0		/* e0 forces cursor movement */ //e0 置位了吗?
	je cur
	testb $0x02,leds	/* not num-lock forces cursor */ //num-lock键置位了码？
	je cur
	testb $0x03,mode	/* shift forces cursor */ //shift键同时按下吗？
	jne cur
	xorl %ebx,%ebx
	movb num_table(%eax),%al //把数字放入队列
	jmp put_queue
1:	ret

cur:	movb cur_table(%eax),%al  //方向键处理程序
	cmpb $'9,%al
	ja ok_cur
	movb $'~,%ah		/* 比9小(5,6,2,3),把~g给ah */
ok_cur:	shll $16,%eax	//将ax移到高字
	movw $0x5b1b,%ax	//0x5b1b"esc [" 给ax , 1b是esc
	xorl %ebx,%ebx
	jmp put_queue		//放入队列

#if defined(KBD_FR)
num_table:
	.ascii "789 456 1230."
#else
num_table:
	.ascii "789 456 1230,"
#endif
cur_table:
	.ascii "HA5 DGC YB623"

/* 功能键处理程序
 * this routine handles function keys
 */
func:
	pushl %eax
	pushl %ecx
	pushl %edx
	call show_stat	//显示各任务状态
	popl %edx
	popl %ecx
	popl %eax
	subb $0x3B,%al //键F1的扫描码是0x3B, 此处转为索引
	jb end_func
	cmpb $9,%al
	jbe ok_func		//功能键是F1-F10吗？
	subb $18,%al	//F11(0x57),F12(0x58)扫描码
	cmpb $10,%al	//0x57-0x3B = 28
	jb end_func
	cmpb $11,%al
	ja end_func
ok_func:
	cmpl $4,%ecx		/* check that there is enough room */
	jl end_func			// 要放入4个字符，若无空间，返回
	movl func_table(,%eax,4),%eax //取功能键对应的字符序列
	xorl %ebx,%ebx
	jmp put_queue
end_func:
	ret

/*
 * function keys send F1:'esc [ [ A' F2:'esc [ [ B' etc.
 */
func_table:
	.long 0x415b5b1b,0x425b5b1b,0x435b5b1b,0x445b5b1b
	.long 0x455b5b1b,0x465b5b1b,0x475b5b1b,0x485b5b1b
	.long 0x495b5b1b,0x4a5b5b1b,0x4b5b5b1b,0x4c5b5b1b

#if	defined(KBD_FINNISH)
key_map:
	.byte 0,27				//扫描码0x00,0x01对应的ascii码
	.ascii "1234567890+'"	//扫描码0x002-0x0d 对应的ascii码
	.byte 127,9
	.ascii "qwertyuiop}"
	.byte 0,13,0
	.ascii "asdfghjkl|{"
	.byte 0,0
	.ascii "'zxcvbnm,.-"
	.byte 0,'*,0,32		/* 36-39 */ //扫描码 0x36-0x39对应的ascii码
	.fill 16,1,0		/* 3A-49 */ //0x3A-0x49 .fill repeat,size,value copy 16次1byte,0
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '<
	.fill 10,1,0

shift_map:
	.byte 0,27
	.ascii "!\"#$%&/()=?`"
	.byte 127,9
	.ascii "QWERTYUIOP]^"
	.byte 13,0
	.ascii "ASDFGHJKL\\["
	.byte 0,0
	.ascii "*ZXCVBNM;:_"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '>
	.fill 10,1,0

alt_map:
	.byte 0,0
	.ascii "\0@\0$\0\0{[]}\\\0"
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte '~,13,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0,0,0		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte 0,0,0,0,0		/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '|
	.fill 10,1,0

#elif defined(KBD_US)

key_map:
	.byte 0,27
	.ascii "1234567890-="
	.byte 127,9
	.ascii "qwertyuiop[]"
	.byte 13,0
	.ascii "asdfghjkl;'"
	.byte '`,0
	.ascii "\\zxcvbnm,./"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '<
	.fill 10,1,0


shift_map:
	.byte 0,27
	.ascii "!@#$%^&*()_+"
	.byte 127,9
	.ascii "QWERTYUIOP{}"
	.byte 13,0
	.ascii "ASDFGHJKL:\""
	.byte '~,0
	.ascii "|ZXCVBNM<>?"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '>
	.fill 10,1,0

alt_map:
	.byte 0,0
	.ascii "\0@\0$\0\0{[]}\\\0"
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte '~,13,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0,0,0		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte 0,0,0,0,0		/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '|
	.fill 10,1,0

#elif defined(KBD_GR)

key_map:
	.byte 0,27
	.ascii "1234567890\\'"
	.byte 127,9
	.ascii "qwertzuiop@+"
	.byte 13,0
	.ascii "asdfghjkl[]^"
	.byte 0,'#
	.ascii "yxcvbnm,.-"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '<
	.fill 10,1,0


shift_map:
	.byte 0,27
	.ascii "!\"#$%&/()=?`"
	.byte 127,9
	.ascii "QWERTZUIOP\\*"
	.byte 13,0
	.ascii "ASDFGHJKL{}~"
	.byte 0,''
	.ascii "YXCVBNM;:_"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '>
	.fill 10,1,0

alt_map:
	.byte 0,0
	.ascii "\0@\0$\0\0{[]}\\\0"
	.byte 0,0
	.byte '@,0,0,0,0,0,0,0,0,0,0
	.byte '~,13,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0,0,0		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte 0,0,0,0,0		/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '|
	.fill 10,1,0


#elif defined(KBD_FR)

key_map:
	.byte 0,27
	.ascii "&{\"'(-}_/@)="
	.byte 127,9
	.ascii "azertyuiop^$"
	.byte 13,0
	.ascii "qsdfghjklm|"
	.byte '`,0,42		/* coin sup gauche, don't know, [*|mu] */
	.ascii "wxcvbn,;:!"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '<
	.fill 10,1,0

shift_map:
	.byte 0,27
	.ascii "1234567890]+"
	.byte 127,9
	.ascii "AZERTYUIOP<>"
	.byte 13,0
	.ascii "QSDFGHJKLM%"
	.byte '~,0,'#
	.ascii "WXCVBN?./\\"
	.byte 0,'*,0,32		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte '-,0,0,0,'+	/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '>
	.fill 10,1,0

alt_map:
	.byte 0,0
	.ascii "\0~#{[|`\\^@]}"
	.byte 0,0
	.byte '@,0,0,0,0,0,0,0,0,0,0
	.byte '~,13,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0
	.byte 0,0,0,0,0,0,0,0,0,0,0
	.byte 0,0,0,0		/* 36-39 */
	.fill 16,1,0		/* 3A-49 */
	.byte 0,0,0,0,0		/* 4A-4E */
	.byte 0,0,0,0,0,0,0	/* 4F-55 */
	.byte '|
	.fill 10,1,0

#else
#error "KBD-type not defined"
#endif
/*
 * do_self handles "normal" keys, ie keys that don't change meaning
 * and which have just one character returns.
 */
do_self:
	lea alt_map,%ebx		//取alt_map 地址
	testb $0x20,mode		/* alt-gr */ //右alt键是否同时按下?
	jne 1f
	lea shift_map,%ebx		//取shift_map 地址
	testb $0x03,mode		//测试shift建是否同时按下
	jne 1f
	lea key_map,%ebx		//取正常key_map
1:	movb (%ebx,%eax),%al	//将扫描码做索引值，取对应ascii
	orb %al,%al
	je none					// 为0返回
	testb $0x4c,mode		/* ctrl or caps */ //判断ctrl 或caps 是否按下
	je 2f					//无，向前跳转
	cmpb $'a,%al			/*与ascii 'a' 相比 */
	jb 2f
	cmpb $'},%al			/*与ascii '}' 相比 */
	ja 2f
	subb $32,%al			//将al 转换为大写字符
2:	testb $0x0c,mode		/* ctrl */
	je 3f
	cmpb $64,%al		//ctrl 健按下，与$64-96比较，在此范围减64变为0-32之间控制字符
	jb 3f
	cmpb $64+32,%al
	jae 3f
	subb $64,%al
3:	testb $0x10,mode		/* left alt */ //测试left-alt
	je 4f
	orb $0x80,%al			// bit7 置位
4:	andl $0xff,%eax			//清除eax高字及ah寄存器，只留al
	xorl %ebx,%ebx
	call put_queue			//放入队列
none:	ret

/*
 * minus has a routine of it's own, as a 'E0h' before
 * the scan code for minus means that the numeric keypad
 * slash was pushed.
 */
minus:	cmpb $1,e0
	jne do_self
	movl $'/,%eax
	xorl %ebx,%ebx
	jmp put_queue

/*
 * This table decides which routine to call when a scan-code has been
 * gotten. Most routines just call do_self, or none, depending if
 * they are make or break.
 */
key_table:
	.long none,do_self,do_self,do_self	/* 00-03 s0 esc 1 2 */
	.long do_self,do_self,do_self,do_self	/* 04-07 3 4 5 6 */
	.long do_self,do_self,do_self,do_self	/* 08-0B 7 8 9 0 */
	.long do_self,do_self,do_self,do_self	/* 0C-0F + ' bs tab */
	.long do_self,do_self,do_self,do_self	/* 10-13 q w e r */
	.long do_self,do_self,do_self,do_self	/* 14-17 t y u i */
	.long do_self,do_self,do_self,do_self	/* 18-1B o p } ^ */
	.long do_self,ctrl,do_self,do_self	/* 1C-1F enter ctrl a s */
	.long do_self,do_self,do_self,do_self	/* 20-23 d f g h */
	.long do_self,do_self,do_self,do_self	/* 24-27 j k l | */
	.long do_self,do_self,lshift,do_self	/* 28-2B { para lshift , */
	.long do_self,do_self,do_self,do_self	/* 2C-2F z x c v */
	.long do_self,do_self,do_self,do_self	/* 30-33 b n m , */
	.long do_self,minus,rshift,do_self	/* 34-37 . - rshift * */
	.long alt,do_self,caps,func		/* 38-3B alt sp caps f1 */
	.long func,func,func,func		/* 3C-3F f2 f3 f4 f5 */
	.long func,func,func,func		/* 40-43 f6 f7 f8 f9 */
	.long func,num,scroll,cursor		/* 44-47 f10 num scr home */
	.long cursor,cursor,do_self,cursor	/* 48-4B up pgup - left */
	.long cursor,cursor,do_self,cursor	/* 4C-4F n5 right + end */
	.long cursor,cursor,cursor,cursor	/* 50-53 dn pgdn ins del */
	.long none,none,do_self,func		/* 54-57 sysreq ? < f11 */
	.long func,none,none,none		/* 58-5B f12 ? ? ? */
	.long none,none,none,none		/* 5C-5F ? ? ? ? */
	.long none,none,none,none		/* 60-63 ? ? ? ? */
	.long none,none,none,none		/* 64-67 ? ? ? ? */
	.long none,none,none,none		/* 68-6B ? ? ? ? */
	.long none,none,none,none		/* 6C-6F ? ? ? ? */
	.long none,none,none,none		/* 70-73 ? ? ? ? */
	.long none,none,none,none		/* 74-77 ? ? ? ? */
	.long none,none,none,none		/* 78-7B ? ? ? ? */
	.long none,none,none,none		/* 7C-7F ? ? ? ? */
	.long none,none,none,none		/* 80-83 ? br br br */
	.long none,none,none,none		/* 84-87 br br br br */
	.long none,none,none,none		/* 88-8B br br br br */
	.long none,none,none,none		/* 8C-8F br br br br */
	.long none,none,none,none		/* 90-93 br br br br */
	.long none,none,none,none		/* 94-97 br br br br */
	.long none,none,none,none		/* 98-9B br br br br */
	.long none,unctrl,none,none		/* 9C-9F br unctrl br br */
	.long none,none,none,none		/* A0-A3 br br br br */
	.long none,none,none,none		/* A4-A7 br br br br */
	.long none,none,unlshift,none		/* A8-AB br br unlshift br */
	.long none,none,none,none		/* AC-AF br br br br */
	.long none,none,none,none		/* B0-B3 br br br br */
	.long none,none,unrshift,none		/* B4-B7 br br unrshift br */
	.long unalt,none,uncaps,none		/* B8-BB unalt br uncaps br */
	.long none,none,none,none		/* BC-BF br br br br */
	.long none,none,none,none		/* C0-C3 br br br br */
	.long none,none,none,none		/* C4-C7 br br br br */
	.long none,none,none,none		/* C8-CB br br br br */
	.long none,none,none,none		/* CC-CF br br br br */
	.long none,none,none,none		/* D0-D3 br br br br */
	.long none,none,none,none		/* D4-D7 br br br br */
	.long none,none,none,none		/* D8-DB br ? ? ? */
	.long none,none,none,none		/* DC-DF ? ? ? ? */
	.long none,none,none,none		/* E0-E3 e0 e1 ? ? */
	.long none,none,none,none		/* E4-E7 ? ? ? ? */
	.long none,none,none,none		/* E8-EB ? ? ? ? */
	.long none,none,none,none		/* EC-EF ? ? ? ? */
	.long none,none,none,none		/* F0-F3 ? ? ? ? */
	.long none,none,none,none		/* F4-F7 ? ? ? ? */
	.long none,none,none,none		/* F8-FB ? ? ? ? */
	.long none,none,none,none		/* FC-FF ? ? ? ? */

/*
 * kb_wait waits for the keyboard controller buffer to empty.
 * there is no timeout - if the buffer doesn't empty, we hang.
 */
kb_wait:
	pushl %eax
1:	inb $0x64,%al		//读键盘控制器状态
	testb $0x02,%al		//测试是否为空
	jne 1b
	popl %eax
	ret
/*
 * This routine reboots the machine by asking the keyboard
 * controller to pulse the reset-line low.
 */
reboot:
	call kb_wait
	movw $0x1234,0x472	/* don't do memory check */ //bios会查询0x472内存的值，若为0x1234会热启动,跳过内存检测
	movb $0xfc,%al		/* pulse reset and A20 low */
	outb %al,$0x64		// 向系统复位和使A20线置低. 
die:	jmp die
