file tools/system
target remote localhost:1234
set follow-fork-mode child
set print pretty
#b ticks_to_floppy_on
#b mem_init
#b get_free_page
b main
#b init/main.c:141
#b init/main.c:142
# main.c 中init 下都不能中断下来？不知何故,只有main函数内语句可中断
#b init/main.c:162
#b init/main.c:177
#b init/main.c:178
#b init/main.c:181
#b init/main.c:183
#b init/main.c:184
#b open
#b dup
#b sys_dup
#b sys_read
#b sys_write
#b open_namei
#b sys_open
#b malloc
#b system_call
#b copy_process
#b sys_setup
b mount_root
#b tty_write
b iget
b read_inode
c
