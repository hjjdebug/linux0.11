file tools/system
target remote localhost:1234
b main
set follow-fork-mode child
#b ticks_to_floppy_on
#b mem_init
#b get_free_page
b init/main.c:141
b init/main.c:177
b init/main.c:178
b open_namei
#b copy_process
c
