.. Kenneth Lee 版权所有 2022

:Authors: Kenneth Lee
:Version: 1.0

Kenneth的开发日志2
*******************

这里记录linx-block方案的qemu开发日志。从2022年5月以后开始

20220505
========

前置工作记录
------------

五一假期在家做了三个功能：

1. -d mmu的时候写Page Walk日志（未来需要可以再做一个过滤的功能）

2. -symfile可以人工指定符号文件，这个功能是从qemu-user移植过来的，直接解释elf文
   件，提取符号表然后提供查找功能就好了。比我想象中简单。只做了一个上午。

3. 把tcg breakpoint功能实现了，这个功能本来是等着给石晓强做的，但我放假无聊，在
   家也只能做这种通用功能，所以，就做了。

前两个功能王州已经合入主线了，第三个功能等石晓强完整测试一下由他合入吧。

做以上功能的一些经验记录一下：

1. 以前看到有两个tcg.h，一个是tcg/tcg.h，一个是sysemu/tcg.h。没有注意它们的区别，
   这次要用了，才分辨了一下：前者是所有tcg都用的，后者仅仅是系统模拟器用的。

2. type_init定义不是一个函数，而是一组函数，用于决定启动顺序，所以，可以通过这
   个初始化函数的定义，决定你自己的模块如何初始化。

3. qemu-user中实现符号表其实比较直接，因为运行的程序和guest平台类型必然是一致的，
   通过-symfile指定可不一定，Guest是模拟的平台指令，elf的平台类型是文件决定的，
   两者不需要一致。

4. elf符号表的符号value，是内置的字符串表的偏移，这种为C写的格式，还是用C处理比
   较容易，想想如果用python处理，就要自己处理零结束符了。

5. hmp是人工命令，qmp是机器命令，qemu的建议是你先实现qmp命令，然后再基于这个适
   配hmp命令。qmp是一个基于json的命令接口，我觉得挺烦的，所以没有实现它，直接用
   了hmp。这个如果要做上传，人家应该不答应。

6. hmp-commands.hx定义所有的hmp命令，其中.args_type和.params参数定义格式，没有
   文档，我一开始仿其他命令写，晕得一逼，最终还是把代码看了一遍。它的原理是这样的：

   1. params根本没有用，只是一个字符串，用来提示参数用的
   2. args_type才是解码格式定义，原理也很简单，就是**名字:类型**这样的定义，一
      路解释过去，如果某个参数是可选的，就加个问号。比如做一条命令：tcgbp list
      3 attr1。args_type的定义就可以是，action:s,cpu:i,attr:?s。这样你可以直接
      得到action=list，cpu=3,attr=attr1，如此而已。

7. Qemu为什么一直没有做我们这个tcg bp的功能呢？我认为Qemu认为不需要，如果我们有
   gdb，正确的用法应该是用gdb来连Qemu，然后在Qemu中直接设置断点，而不是靠TCG来
   设置断点。但我们因为使能进程的原因，没有gdb，所以才需要现在的手段。而且qemu
   的gdbstub考虑了chained问题，比我们现在是考虑得更加全面的。不过，我觉得我们的
   功能还是在很多时候有价值的，因为毕竟gdb使用的是二进制程序视角，而tcg bp是tcg
   视角，反馈更加直接。初始化阶段tcg bp比较有用，运行起来后，主要还是依靠
   gdbstub，比较好。（现在binutils已经完成linx平台的切换了，我开始试试能不能使
   能gdb）

期间OS组已经进展到有打印输出了，现在的碰到的问题大部分都是汇编没有实现的问题。
编译器现在的主要问题是会生成非法的set_mask块头，qemu现在主要问题是进入head/body
解码状态没有设计完。

所以我今天的工作主要聚焦在这两个：

1. 分析qemu gdbstub的工作原理
2. 开始启动gdb-linx的移植

qemu gdbstub原理
----------------

qemu gdbstub的原理和tcg bp的原理几乎是一样的，都是比较cpu_exec执行时的bp，碰上
就做vm_stop()，然后做动作。gdbstub还在jmp上插桩，保证chain了也能跳出来。另外，它
的数据断点是靠平台模拟代码生成的，而不是靠监控tlb_fill来获得的。

我试着启动了一个多核的vm，然后用gdb vmlinux去跟，发现线程的数量和核的数量一致，
而调用栈是vcpu在内核态就对内核态做unwind，在用户态就在用户态做unwind。

gdbserver整个工作在iothread的事件队列上，有两个调用栈。一个是client刚连上的时候：::

  #0  gdb_get_process (pid=1) at ../gdbstub.c:731
  #1  gdb_get_cpu_process (cpu=0x555556a29c70, cpu=<optimized out>) at ../gdbstub.c:751
  #2  0x0000555555b2c205 in gdb_first_attached_cpu () at ../gdbstub.c:816
  #3  gdb_chr_event (event=<optimized out>, opaque=<optimized out>) at ../gdbstub.c:3381
  #4  gdb_chr_event (opaque=0x555556682f40 <gdbserver_state>, event=<optimized out>) at ../gdbstub.c:3369
  #5  0x0000555555cdd5e4 in tcp_chr_new_client (chr=0x555556750680, sioc=0x7fff58047090) at ../chardev/char-socket.c:931
  #6  0x0000555555c1d68e in qio_net_listener_channel_func (ioc=<optimized out>, condition=<optimized out>, opaque=<optimized out>) at ../io/net-listener.c:54
  #7  0x00007ffff7482c24 in g_main_context_dispatch () at /lib/x86_64-linux-gnu/libglib-2.0.so.0
  #8  0x0000555555df8af0 in glib_pollfds_poll () at ../util/main-loop.c:232
  #9  os_host_main_loop_wait (timeout=511189054) at ../util/main-loop.c:255
  #10 main_loop_wait (nonblocking=nonblocking@entry=0) at ../util/main-loop.c:531
  #11 0x0000555555b1335b in qemu_main_loop () at ../softmmu/runstate.c:726
  #12 0x0000555555866832 in main (argc=<optimized out>, argv=<optimized out>, envp=<optimized out>) at ../softmmu/main.c:50

另一个是在socket上收字符串的一个状态机管理，调用栈是这样的：::

  #0  handle_query_threads (params=0x55555695ad30, user_ctx=0x0) at ../gdbstub.c:2059
  #1  0x0000555555b2926e in process_string_cmd (data=<optimized out>, cmds=cmds@entry=0x555556522240 <gdb_gen_query_table>, num_cmds=num_cmds@entry=12, user_ctx=0x0) at ../gdbstub.c:1499
  #2  0x0000555555b29896 in handle_gen_query (params=<optimized out>, params=<optimized out>, user_ctx=<optimized out>) at ../gdbstub.c:2448
  #3  handle_gen_query (params=0x55555695ae60, user_ctx=<optimized out>) at ../gdbstub.c:2436
  #4  0x0000555555b2926e in process_string_cmd
      (data=data@entry=0x555556682f64 <gdbserver_state+36> "qfThreadInfo", cmds=cmds@entry=0x555556521f20 <gen_query_cmd_desc>, num_cmds=num_cmds@entry=1, user_ctx=0x0) at ../gdbstub.c:1499
  #5  0x0000555555b2d720 in run_cmd_parser (data=0x555556682f64 <gdbserver_state+36> "qfThreadInfo", cmd=0x555556521f20 <gen_query_cmd_desc>) at ../gdbstub.c:1517
  #6  gdb_handle_packet (line_buf=0x555556682f64 <gdbserver_state+36> "qfThreadInfo") at ../gdbstub.c:2728
  #7  gdb_read_byte (ch=98 'b') at ../gdbstub.c:3064
  #8  gdb_chr_receive (opaque=<optimized out>, buf=<optimized out>, size=<optimized out>) at ../gdbstub.c:3365
  #9  0x0000555555cdd1aa in tcp_chr_read (chan=<optimized out>, cond=<optimized out>, opaque=<optimized out>) at ../chardev/char-socket.c:564
  #10 0x00007ffff7482c24 in g_main_context_dispatch () at /lib/x86_64-linux-gnu/libglib-2.0.so.0
  #11 0x0000555555df8af0 in glib_pollfds_poll () at ../util/main-loop.c:232
  #12 os_host_main_loop_wait (timeout=1000000000) at ../util/main-loop.c:255
  #13 main_loop_wait (nonblocking=nonblocking@entry=0) at ../util/main-loop.c:531
  #14 0x0000555555b1335b in qemu_main_loop () at ../softmmu/runstate.c:726
  #15 0x0000555555866832 in main (argc=<optimized out>, argv=<optimized out>, envp=<optimized out>) at ../softmmu/main.c:50

整个收gdbserver消息的状态机管理在gdb_read_byte()中，状态机捕获一个完整的命令，
就转到gdb_handle_packet，后面就是命令相关的。上面的调用栈跟踪的是线程信息的读取，
在attach的时候就发生的。

这个模块里有trace跟踪，我用::

  -d trace:gdbstub_*

跟踪了一下，结果如下：::

  gdbstub_op_start Starting gdbstub using device tcp::1234
  gdbstub_hit_paused RUN_STATE_PAUSED
  gdbstub_io_reply Sent: T02thread:01;
  gdbstub_io_got_ack Got ACK
  gdbstub_io_command Received: qSupported:multiprocess+;swbreak+;hwbreak+;qRelocInsn+;
                fork-events+;vfork-events+;exec-events+;vContSupported+;QThreadEvents+;
                no-resumed+;memory-tagging+;xmlRegisters=i386 <---------------- 能力对齐
  gdbstub_io_reply Sent: PacketSize=1000;qXfer:features:read+;vContSupported+;multiprocess+
  gdbstub_io_got_ack Got ACK
  gdbstub_err_garbage received garbage between packets: 0x2b
  gdbstub_io_command Received: vMustReplyEmpty
  gdbstub_io_reply Sent:
  gdbstub_io_got_ack Got ACK
  gdbstub_io_command Received: Hgp0.0
  gdbstub_io_reply Sent: OK
  gdbstub_io_got_ack Got ACK
  gdbstub_io_command Received: qXfer:features:read:target.xml:0,ffb
  gdbstub_io_binaryreply 0x0000: 6c 3c 3f 78  6d 6c 20 76  65 72 73 69  6f 6e 3d 22  l<?xml version="
  gdbstub_io_binaryreply 0x0010: 31 2e 30 22  3f 3e 3c 21  44 4f 43 54  59 50 45 20  1.0"?><!DOCTYPE
  gdbstub_io_binaryreply 0x0020: 74 61 72 67  65 74 20 53  59 53 54 45  4d 20 22 67  target SYSTEM "g
  gdbstub_io_binaryreply 0x0030: 64 62 2d 74  61 72 67 65  74 2e 64 74  64 22 3e 3c  db-target.dtd"><
  gdbstub_io_binaryreply 0x0040: 74 61 72 67  65 74 3e 3c  61 72 63 68  69 74 65 63  target><architec
  gdbstub_io_binaryreply 0x0050: 74 75 72 65  3e 72 69 73  63 76 3a 72  76 36 34 3c  ture>riscv:rv64<
  gdbstub_io_binaryreply 0x0060: 2f 61 72 63  68 69 74 65  63 74 75 72  65 3e 3c 78  /architecture><x
  ...                                                          <--------- 发送平台定义的xml文件
  gdbstub_io_got_ack Got ACK
  （后面删除ACK响应，节省空间）
  gdbstub_io_command Received: qXfer:features:read:riscv-64bit-cpu.xml:0,ffb
  gdbstub_io_reply Sent: E00
  gdbstub_io_command Received: qTStatus
  gdbstub_io_reply Sent:
  gdbstub_io_command Received: ?                               <--------- 请求状态
  gdbstub_io_reply Sent: T05thread:p01.01;                     <--------- 第一个线程名称
  gdbstub_io_command Received: qfThreadInfo                    <--------- 对方请求首线程信息（f表示首线程）
  gdbstub_io_reply Sent: mp01.01                               <--------- 返回线程标识
  gdbstub_io_command Received: qsThreadInfo                    <--------- 请求首线程后续信息（s表示后续）
  gdbstub_io_reply Sent: mp01.02
  ...
  gdbstub_io_command Received: qsThreadInfo
  gdbstub_io_reply Sent: l                                     <--------- 1表示qsThreadInfo已经查不到更多的信息了
  gdbstub_io_command Received: qAttached:1                     <--------- attach
  gdbstub_io_reply Sent: 1
  gdbstub_io_command Received: Hc-1                            <--------- tHread Continue, -1表示所有线程
  gdbstub_io_reply Sent: OK
  gdbstub_io_command Received: qOffsets                        <--------- 请求所有段的偏移地址
  gdbstub_io_reply Sent:
  gdbstub_io_command Received: g                               <--------- 对端请求寄存器
  gdbstub_io_reply Sent: 0000000000000000acff0380ffffffff...   <--------- 返回寄存器
  ...
  gdbstub_io_command Received: mffffffff800032f0,4             <--------- 对端请求读内存
  gdbstub_io_reply Sent: 73600110
  gdbstub_io_command Received: mffffffff800032ec,4
  gdbstub_io_reply Sent: 73005010
  gdbstub_io_command Received: qSymbol::
  gdbstub_io_reply Sent:
  gdbstub_io_command Received: D;1
  gdbstub_op_continue Continuing all CPUs
  gdbstub_io_reply Sent: OK
  gdbstub_op_exiting notifying exit with code=0x00
  gdbstub_io_reply Sent: W00

我再看看info threads的协议（也删除ACK了）：::

  gdbstub_io_command Received: qThreadExtraInfo,p1.1
  gdbstub_op_extra_info Thread extra info: CPU#0 [halted ]
  gdbstub_io_reply Sent: 4350552330205b68616c746564205d
  gdbstub_io_command Received: qThreadExtraInfo,p1.2
  gdbstub_op_extra_info Thread extra info: CPU#1 [halted ]
  gdbstub_io_reply Sent: 4350552331205b68616c746564205d
  gdbstub_io_command Received: qThreadExtraInfo,p1.3
  gdbstub_op_extra_info Thread extra info: CPU#2 [halted ]
  gdbstub_io_reply Sent: 4350552332205b68616c746564205d
  gdbstub_io_command Received: qThreadExtraInfo,p1.4
  gdbstub_op_extra_info Thread extra info: CPU#3 [halted ]
  gdbstub_io_reply Sent: 4350552333205b68616c746564205d

总结起来，qemu gdbstub是把每个vcpu看做一个target的线程，然后对每个关联线程对gdb
client做基于栈顶的线程管理。栈顶当时在哪里就在哪里做unwind。断点在cpu_exec()循
环里面捕获，如果断点在tb中间，就把tb修改成只执行一条指令，这样下一条指令必然又
在一个tb的最前面了。

从这个角度来说，我们的TCG很大程度上可以复用这个方案，而不需要自己做一个。命令行
只要调用qemu_insert_breakpoint/watchpoint()就可以了。断出来以后，系统模拟在
cpu_handle_guest_debug()中处理，我们只要发现断点是我们加的，就用我们的方案来断，
否则就用GDB来处理就可以了。如果是user模拟，在对应平台中cpu_loop()中处理，默认是
是让gdbstub切换状态，我们也可以进行调整。唯一的区别是，我们那种多少条指令以后才
断的策略不能实施而已。

无论如何，我们现在这个方案不长远，支持完当前的调试就行了，重点放在gdb支持上。

gdb-linx移植
------------

下最新的block-binutils，在这里：

ssh://git@codehub-dg-y.huawei.com:2222/linx/ISA-Codesign/BlockISA/linx-BLK-binutils.git

master分支。

试试初步的编译：::

  ./configure --enable-targets=linx64-unknow-gnu
  make -j30

链接失败（有符号找不到，不记录了，反正可以重现），下面这样可以通过：::

  ./configure --target=linx64-unknow-gnu
  make -j30

gdb也生成了，只是启动不了：::

  arch-utils.c:693: internal-error: initialize_current_architecture: Selection of initial architecture failed
  A problem internal to GDB has been detected,
  further debugging may prove unreliable.
  Quit this debugging session? (y or n) Segmentation fault

应该是个assert错误，大概看了一下，是gdbarch_registry里面还没有任何注册，这整个
平台的使能还没有，明天开始写代码吧。

20220506
========

本日过程日志
------------

memset时间特别长问题今天还没有定位，我帮忙写个补丁跟踪qemu执行时间。今天内还没
有收到反馈。可能因为大部分人出差在途。

朱熠琛hack了编译器对弱符号链接错误问题，现在空的main函数可以正常退出了。我通过
-strace跟踪看，这个功能现在是正常的。

修复这个问题后，之前调用wscan函数的问题还是segfault，我看了一下现场错误，系统报
si_addr=0x10，访问错误地址。而出错的body如下：::

  get a1
  get a2
  sb t#1, [t#2, 0]
  ...

这个块执行前，a1的值就是0x10，所以，这个部分qemu是按要求执行的，出错要不就是前
面的执行行为不对，要不就是代码本身的行为不对。这个只能基于语义加点一点点逼近错
误点了。

然后继续写GDB的使能。按如下顺序执行：

1. 从riscv-dep.[ch]衍生linx-dep.[ch]，保证gdb能起来，能设置arch为linx。

   这完成了，我删掉了和xlen等变体有关的初始化，这个以后也不用的。这个补丁我发给
   刘盈盈，先合一个桩。

2. 打开一个block的vmlinux，看到符号，看看运气好不好，能够看见源代码？

   这个操作本身也成功了，只是所有发现的符号最终都没有认，定位是bfd库的
   bfd_elf64_bfd_is_target_special_symbol没有实现，被默认返回失败。我对这个地方
   的原理没有认识，需要慢慢看。问题先报给编译器，看看他们能不能快速搞定。

3. target remote连qemu

   这一步是我们这个项目周期内可以做的最后一步，搞定就可以用来调试内核了。今天没
   有进展。


调试数据
--------

地址不被承认的位置：::

  #0  _bfd_bool_bfd_asymbol_false (abfd=0x555555ca5dd0, sym=0x7fffdc32e020) at libbfd.c:52
  #1  0x00005555557195c5 in elf_symtab_read (reader=..., objfile=objfile@entry=0x555555dd2980, type=type@entry=0, number_of_symbols=number_of_symbols@entry=52312,
      symbol_table=symbol_table@entry=0x7ffff4608020, copy_names=copy_names@entry=false) at elfread.c:275
  #2  0x000055555571a2d4 in elf_read_minimal_symbols (symfile_flags=14, ei=0x7fffffffdc90, objfile=0x555555dd2980) at elfread.c:1100
  #3  elf_symfile_read (objfile=0x555555dd2980, symfile_flags=...) at elfread.c:1216
  #4  0x0000555555868583 in read_symbols (objfile=0x555555dd2980, add_flags=...) at symfile.c:781
  #5  0x0000555555867e21 in syms_from_objfile_1 (add_flags=..., addrs=0x7fffffffdda0, objfile=0x555555dd2980) at symfile.c:978
  #6  syms_from_objfile (add_flags=..., addrs=<optimized out>, objfile=0x555555dd2980) at symfile.c:995
  #7  symbol_file_add_with_addrs (abfd=<optimized out>, name=0x555555d8c390 "/home/kenny/work/linx-qemu-dev/LinuxKernel/vmlinux", add_flags=..., addrs=<optimized out>, flags=...,
      parent=<optimized out>) at symfile.c:1098
  #8  0x000055555586925c in symbol_file_add_from_bfd (parent=0x0, flags=..., addrs=0x0, add_flags=..., name=0x555555d8c390 "/home/kenny/work/linx-qemu-dev/LinuxKernel/vmlinux",
      abfd=<optimized out>) at symfile.c:1179
  #9  symbol_file_add (name=0x555555d8c390 "/home/kenny/work/linx-qemu-dev/LinuxKernel/vmlinux", add_flags=..., addrs=0x0, flags=...) at symfile.c:1192
  #10 0x00005555558692d2 in symbol_file_add_main_1 ...
  #11 0x00005555558694e5 in symbol_file_command (args=args@entry=0x555555d8c355 "/home/kenny/work/linx-qemu-dev/LinuxKernel/vmlinux", from_tty=from_tty@entry=1) at symfile.c:1660
  #12 0x0000555555725131 in file_command (arg=0x555555d8c355 "/home/kenny/work/linx-qemu-dev/LinuxKernel/vmlinux", from_tty=1) at exec.c:584
  #13 0x000055555566deba in cmd_func (cmd=<optimized out>, args=<optimized out>, from_tty=<optimized out>) at cli/cli-decode.c:2181
  #14 0x00005555558b18c2 in execute_command (p=<optimized out>, p@entry=0x555555d8c350 "", from_tty=1) at top.c:668
  #15 0x00005555557231f5 in command_handler (command=0x555555d8c350 "") at event-top.c:588
  #16 0x0000555555723581 in command_line_handler (rl=...) at event-top.c:773
  #17 0x0000555555723b7d in gdb_rl_callback_handler (rl=0x555555d8c430 "file /home/kenny/work/linx-qemu-dev/LinuxKernel/vmlinux") at event-top.c:219
  #18 0x00005555559268a8 in rl_callback_read_char () at callback.c:281
  #19 0x0000555555722716 in gdb_rl_callback_read_char_wrapper_noexcept () at event-top.c:177
  #20 0x0000555555723a34 in gdb_rl_callback_read_char_wrapper (client_data=<optimized out>) at event-top.c:194
  #21 0x00005555557224f8 in stdin_event_handler (error=<optimized out>, client_data=0x555555ca46f0) at event-top.c:516
  #22 0x00005555559c4096 in gdb_wait_for_event (block=block@entry=1) at event-loop.cc:673
  #23 0x00005555559c452b in gdb_wait_for_event (block=1) at event-loop.cc:569
  #24 gdb_do_one_event () at event-loop.cc:215
  #25 0x00005555557a5a85 in start_event_loop () at main.c:356
  #26 captured_command_loop () at main.c:416
  #27 0x00005555557a7ec5 in captured_main (During symbol reading: const value length mismatch for 'sigall_set', got 128, expected 0
  data=0x7fffffffe250) at main.c:1253
  #28 gdb_main (args=args@entry=0x7fffffffe280) at main.c:1268
  #29 0x00005555555da4c0 in main (argc=<optimized out>, argv=<optimized out>) at gdb.c:32

通过开gdb/gdbarch.c:GDBARCH_DEBUG可以跟踪到gdbarch_addr_bits_remove的大量调用，
这是读入符号时的回调。跟踪这个流程有如下认识：::

  objfile->compunit_symtabs==0，objfile->partial_symtabs->psymtabs==0。
  objfile->sf->sym_new_init==elf_new_init
  objfile->sf->qf->has_symbols==psym_has_symbols
  number_of_symbols = 52321

明天接着来。

20220506
========

今天接着跟踪昨天的符号不认的问题，先看看位置：::

  symbol_file_add()
    +->symbol_file_add_from_bfd()
         +->symbol_file_add_with_addrs()
              +->syms_from_objfile(objfile, addrs, add_flags) ->syms_from_objfile_1()
              |    +->read_symbols(objfile, add_flags)
              |         +->elf_symfile_read(objfile, symfile_flags)
              |              +->elf_read_minimal_symbols(objfile, symfile_flags, &elf_info)
              |                   +->elf_symtab_read() 到这里为止都能读到
              |                        +->bfd_is_target_special_symbol() 这里返回了false
              +->objfile_has_symbols(objfile) <------ 这里说objfile没有符号

但其实这里返回false是对的，这并没有错，实际上，除了开始跳过几个section_name，后
面也确实调用elfread.c:record_minimal_symbol()了，里面实际调用的是
minimal_symbol_reader::record_full()，也看见其中的m_msym_count在增加。

整个elf_symtab_read()退出后，能看到reader.m_msym_count=50009，
read.m_msym_bunch_index=98。跟下去synthcount=0，没有复合符号。

但objfile_has_symbols()说没有符号。它的判断条件是objfile_has_full_symbols()和
objfile_has_partila_symbols()都返回没有，这两个函数判断的是：objfile->
compunit_symtabs != NULL，和objfile->partial_symtabs->psymtabs != NULL。

好了，到这里为止基本上明白了，这个有没有符号是从drawf中获得的，而
info_address_command()里面用的lookup_symbol()，不但查找这个域，同时还查找
lookup_bound_minimal_symbol()，那些对gdb来说，只能算是"最小"符号，只有个名字和
地址的数字，没有其他信息。这个东西不在objfile->compunit_symtabs和
partial_symtabs里，而是在objfile->per_bfd->msymbol_hash里面。

基于这个认识，忽略那个打印，再补上set_gdbarch_addr_bit(gdbarch, 64)这个初始化，
现在可以正常用info address start_kernel看到符号，甚至可以用disass start_kernel
来进行反汇编。

我再记录一下日志全开的时候的打印，以后关上不看了：::

  (gdb) file vmlinux
  ...
  gdbarch_ptr_bit called
  gdbarch_ptr_bit called
  gdbarch_ptr_bit called
  gdbarch_ptr_bit called
  gdbarch_byte_order called
  gdbarch_byte_order called
  gdbarch_ptr_bit called
  gdbarch_type_align called
  gdbarch_addressable_memory_unit_size called
  gdbarch_int_bit called
  gdbarch_long_bit called
  gdbarch_short_bit called
  gdbarch_float_format called
  gdbarch_float_bit called
  gdbarch_byte_order called
  gdbarch_double_format called
  gdbarch_double_bit called
  gdbarch_byte_order called
  gdbarch_long_long_bit called
  gdbarch_long_double_format called
  gdbarch_long_double_bit called
  gdbarch_byte_order called
  gdbarch_int_bit called
  gdbarch_int_bit called
  gdbarch_ptr_bit called
  language_lookup_primitive_type_as_symbol (c, 0x561865c66bc0, main) = NULL
  Global block symbol cache miss for main, VAR_DOMAIN
  gdbarch_iterate_over_objfiles_in_search_order called
  lookup_symbol_in_objfile (vmlinux, GLOBAL_BLOCK, main, VAR_DOMAIN)
  lookup_symbol_in_objfile (...) = NULL
  Static block symbol cache miss for main, VAR_DOMAIN
  gdbarch_iterate_over_objfiles_in_search_order called
  lookup_symbol_in_objfile (vmlinux, STATIC_BLOCK, main, VAR_DOMAIN)
  lookup_symbol_in_objfile (...) = NULL
  lookup_symbol_aux (...) = NULL
  lookup_minimal_symbol (__jit_debug_register_code, NULL, vmlinux)
  lookup_minimal_symbol (...) = NULL
  lookup_minimal_symbol (std::terminate(), NULL, vmlinux)
  lookup_minimal_symbol (...) = NULL
  lookup_minimal_symbol (_Unwind_DebugHook, NULL, vmlinux)
  lookup_minimal_symbol (...) = NULL
  gdbarch_has_global_breakpoints called

  (gdb) info address start_kernel
  lookup_symbol_aux (start_kernel, 0x0 (objfile NULL), VAR_DOMAIN, c)
  language_lookup_primitive_type_as_symbol (c, 0x561865c66bc0, start_kernel) = NULL
  Global block symbol cache miss for start_kernel, VAR_DOMAIN
  gdbarch_iterate_over_objfiles_in_search_order called
  lookup_symbol_in_objfile (vmlinux, GLOBAL_BLOCK, start_kernel, VAR_DOMAIN)
  lookup_symbol_in_objfile (...) = NULL
  Static block symbol cache miss for start_kernel, VAR_DOMAIN
  gdbarch_iterate_over_objfiles_in_search_order called
  lookup_symbol_in_objfile (vmlinux, STATIC_BLOCK, start_kernel, VAR_DOMAIN)
  lookup_symbol_in_objfile (...) = NULL
  lookup_symbol_aux (...) = NULL
  lookup_minimal_symbol (start_kernel, NULL, vmlinux)
  lookup_minimal_symbol (...) = 0x7fa2bc3e7480 (external)
  gdbarch_addr_bit called
  Symbol "start_kernel" is at 0xffff820011a0 in a file compiled without debugging.

  (gdb) disassemble start_kernel
  lookup_symbol_aux (start_kernel, 0x0 (objfile NULL), VAR_DOMAIN, c)
  language_lookup_primitive_type_as_symbol (c, 0x561865c66bc0, start_kernel) = NULL
  Global block symbol cache hit (not found) for start_kernel, VAR_DOMAIN
  Static block symbol cache hit (not found) for start_kernel, VAR_DOMAIN
  lookup_symbol_aux (...) = NULL
  lookup_minimal_symbol (start_kernel, NULL, vmlinux)
  lookup_minimal_symbol (...) = 0x7fa2bc3e7480 (external)
  gdbarch_char_signed called
  gdbarch_short_bit called
  gdbarch_short_bit called
  gdbarch_int_bit called
  gdbarch_int_bit called
  gdbarch_long_bit called
  gdbarch_long_bit called
  gdbarch_long_long_bit called
  gdbarch_long_long_bit called
  gdbarch_float_format called
  gdbarch_float_bit called
  gdbarch_byte_order called
  gdbarch_double_format called
  gdbarch_double_bit called
  gdbarch_byte_order called
  gdbarch_long_double_format called
  gdbarch_long_double_bit called
  gdbarch_byte_order called
  gdbarch_addr_bit called
  gdbarch_addr_bit called
  gdbarch_deprecated_function_start_offset called
  Dump of assembler code for function start_kernel:
  gdbarch_bfd_arch_info called
  gdbarch_bfd_arch_info called
  gdbarch_byte_order called
  gdbarch_byte_order_for_code called
  gdbarch_disassembler_options_implicit called
  gdbarch_disassembler_options called
  gdbarch_addr_bit called
  gdbarch_addr_bits_remove called
  gdbarch_print_insn called
  gdbarch_addressable_memory_unit_size called
  gdbarch_significant_addr_bit called
  gdbarch_addressable_memory_unit_size called
  gdbarch_significant_addr_bit called
  gdbarch_addressable_memory_unit_size called
  gdbarch_significant_addr_bit called
  gdbarch_addressable_memory_unit_size called
  gdbarch_significant_addr_bit called
  gdbarch_addressable_memory_unit_size called
  gdbarch_significant_addr_bit called
  gdbarch_addressable_memory_unit_size called
  gdbarch_significant_addr_bit called
  gdbarch_addressable_memory_unit_size called
  gdbarch_significant_addr_bit called
  gdbarch_addressable_memory_unit_size called
  gdbarch_significant_addr_bit called
     0x0000ffff820011a0 <+0>:     bstart  b.aux, bnext.concat, battr:none, bget:0x01fc0306, bset:0x00108706, ptr:------ size:0x3b, bnext:------,

现在还有这个报错：::

  Have got a target description
  warning: A handler for the OS ABI "GNU/Hurd" is not built into this configuration
  of GDB.  Attempting to continue with the default linx:lx64 settings.

这应该是target_description没有处理osabi参数引起的，这个晚点再修。反汇编现在只有
头，没有body，这个也可以以后再修。

现在开始做target remote，结果如下：::

  Remote debugging using :1234
  warning: while parsing target description (at line 1): Could not load XML document "riscv-64bit-cpu.xml"
  warning: Could not load XML target description; ignoring
  PC register is not available

这个流程从qemu这边看是这样的：::

  gdbstub_op_start Starting gdbstub using device tcp::1234
  gdbstub_err_garbage received garbage between packets: 0x2b
  gdbstub_io_command Received: qSupported:multiprocess+;swbreak+;hwbreak+;qRelocInsn+;fork-events+;vfork-events+;exec-events+;vContSupported+;QThreadEvents+;no-resumed+
  gdbstub_io_reply Sent: PacketSize=1000;qXfer:features:read+;vContSupported+;multiprocess+
  gdbstub_io_got_ack Got ACK
  gdbstub_io_command Received: vMustReplyEmpty
  gdbstub_io_reply Sent:
  gdbstub_io_got_ack Got ACK
  gdbstub_io_command Received: Hgp0.0
  gdbstub_io_reply Sent: OK
  gdbstub_io_got_ack Got ACK
  gdbstub_io_command Received: qXfer:features:read:target.xml:0,ffb
  gdbstub_io_binaryreply 0x0000: 6c 3c 3f 78  6d 6c 20 76  65 72 73 69  6f 6e 3d 22  l<?xml version="
  gdbstub_io_binaryreply 0x0010: 31 2e 30 22  3f 3e 3c 21  44 4f 43 54  59 50 45 20  1.0"?><!DOCTYPE
  gdbstub_io_binaryreply 0x0020: 74 61 72 67  65 74 20 53  59 53 54 45  4d 20 22 67  target SYSTEM "g
  gdbstub_io_binaryreply 0x0030: 64 62 2d 74  61 72 67 65  74 2e 64 74  64 22 3e 3c  db-target.dtd"><
  gdbstub_io_binaryreply 0x0040: 74 61 72 67  65 74 3e 3c  61 72 63 68  69 74 65 63  target><architec
  gdbstub_io_binaryreply 0x0050: 74 75 72 65  3e 72 69 73  63 76 3a 72  76 36 34 3c  ture>riscv:rv64<
  gdbstub_io_binaryreply 0x0060: 2f 61 72 63  68 69 74 65  63 74 75 72  65 3e 3c 78  /architecture><x
  gdbstub_io_binaryreply 0x0070: 69 3a 69 6e  63 6c 75 64  65 20 68 72  65 66 3d 22  i:include href="
  gdbstub_io_binaryreply 0x0080: 72 69 73 63  76 2d 36 34  62 69 74 2d  63 70 75 2e  riscv-64bit-cpu.
  gdbstub_io_binaryreply 0x0090: 78 6d 6c 22  2f 3e 3c 78  69 3a 69 6e  63 6c 75 64  xml"/><xi:includ
  gdbstub_io_binaryreply 0x00a0: 65 20 68 72  65 66 3d 22  72 69 73 63  76 2d 36 34  e href="riscv-64
  gdbstub_io_binaryreply 0x00b0: 62 69 74 2d  66 70 75 2e  78 6d 6c 22  2f 3e 3c 78  bit-fpu.xml"/><x
  gdbstub_io_binaryreply 0x00c0: 69 3a 69 6e  63 6c 75 64  65 20 68 72  65 66 3d 22  i:include href="
  gdbstub_io_binaryreply 0x00d0: 72 69 73 63  76 2d 36 34  62 69 74 2d  76 69 72 74  riscv-64bit-virt
  gdbstub_io_binaryreply 0x00e0: 75 61 6c 2e  78 6d 6c 22  2f 3e 3c 78  69 3a 69 6e  ual.xml"/><xi:in
  gdbstub_io_binaryreply 0x00f0: 63 6c 75 64  65 20 68 72  65 66 3d 22  72 69 73 63  clude href="risc
  gdbstub_io_binaryreply 0x0100: 76 2d 63 73  72 2e 78 6d  6c 22 2f 3e  3c 2f 74 61  v-csr.xml"/></ta
  gdbstub_io_binaryreply 0x0110: 72 67 65 74  3e                                     rget>
  gdbstub_io_got_ack Got ACK
  gdbstub_io_command Received: qXfer:features:read:riscv-64bit-cpu.xml:0,ffb
  gdbstub_io_reply Sent: E00
  ...

gdb先发了一个qXfer请求target.xml，qemu回了一个riscv的格式的xml，然后gdb再请求里
面描述的riscv-64bit-cpu.xml，然后qemu说我没有（E00），然后就没有然后了。

target.xml的响应在get_feature_xml()函数中，优先处理标准的target.xml，里面提供
architecutre的名字（cpu->gdb_arch_name()返回），然后提供gdb_core_xml_file，这个
由cpu->gdb_core_xml_file提供，下次。下次再来请求下一个xml文件，就会是这个
gdb_core_xml_file，然后再通过xi提供多组的cpu_regs的定义，这回要的是寄存器的定义
文件，从riscv_cpu_register_gdb_regs_for_features()里面设置，然后在里面通过
gdb_register_coprocessor()来定义这些定义文件。我把其他fpu，向量这些删除，就只剩
下linx-csr.xml。

这样，get_feature_xml()收到其他的xml请求，访问的是cpu->gdb_get_dynamic_xml()
（linx/cpu.c），这里你就可以根据需要提供需要提供的信息了。

这样修改以后，整个信息交换过程就好了，现在问题出在第二次调用linx_gdbarch_init()
的时候，没有找到Cache里面的对象，这个需要修改代码。但我今天弄不下去了，腰痛得不
行，先上趟医院，回来再说。

先commit一次吧。

20220509
========

今天继续跟踪昨天的问题，昨天放假在家里先把整个gdbarch的实例设计逻辑分析了一遍，
原理写在这里：\ :ref:gdb_instance\ 。

按这个逻辑重新组织了一下gdbarch_init的初始化。还是匹配不上，打印匹配看到：::

  tdesc_find_feature: compare feature org.gnu.gdb.riscv.cpu and org.gnu.gdb.linx.cpu
  tdesc_find_feature: compare feature org.gnu.gdb.linx.csr and org.gnu.gdb.linx.cpu

.. note::

  这里还有个小插曲，我查了很久发现这两个字符串不相等的问题。一开始发现tdesc中的
  字符串是随机的，查了很久才发现，我printf的时候给进去的参数不是char*，而是std::
  string，需要用std::string:: c_str()转一道。这种C/C++混合程序这种麻烦事多好多，
  很多地方还因为各种封装导致打印不了信息的情况也很多。

我找了半天都没有看出qemu什么时候输出过这个鬼东西，最后全文搜索才发现，这东西居
然是硬写在gdb-xml目录下，然后动态生成的，生成的代码看起来这样：::

  static const char xml_feature_linx_64bit_cpu_xml[] = {
    '<', '?', 'x', 'm', 'l', ' ', 'v', 'e', 'r', 's',
     'i', 'o', 'n', '=', '"', '1', '.', '0', '"', '?',
     '>', '\n',
    '<', '!', '-', '-', ' ', 'C', 'o', 'p', 'y', 'r',
     'i', 'g', 'h', 't', ' ', '(', 'C', ')', ' ', '2',
     '0', '1', '8', '-', '2', '0', '1', '9', ' ', 'F',

WTF，真是没有什么脚本弄不了的。

实际处理在get_feature_xml()函数中，逻辑是对方要target.xml，就动态生成所有xml的
列表。如果不是，用你的gdb_get_dynamic_xml回调来查，如果还是查不到，就用这个
xml_builtin列表来匹配，这种默认的定义，都用的这个列表。

那就没有什么可说的了，调整了这个文件后，终于对上了。现在有两个问题：

1. "PC register is not available"
2. 加断点会出：::

     linx-tdep.c:1331: internal-error: static ULONGEST riscv_insn::fetch_instruction(gdbarch*, CORE_ADDR, int*): Assertion `instlen <= sizeof (buf)' failed.

先处理第一个问题，照理说我设置pc_regnum了，也等于32了，应该没有问题啊。看调用栈：::

  #0  regcache_read_pc (regcache=0x555555d5e2c0) at regcache.c:1214
  #1  0x0000555555778c88 in handle_signal_stop (ecs=0x7fffffffdc90) at infrun.c:5916
  #2  0x000055555577b3cc in handle_inferior_event (ecs=0x7fffffffdc90) at infrun.c:5626
  #3  0x000055555577c5cf in wait_for_inferior (inf=0x555555d7ada0) at infrun.c:3818
  #4  start_remote (from_tty=1) at infrun.c:3228
  #5  0x000055555582d45e in remote_target::start_remote (this=this@entry=0x555555d4b360, from_tty=from_tty@entry=1, extended_p=extended_p@entry=0) at remote.c:4792
  #6  0x000055555582d8d0 in remote_target::open_1 (name=0x555555ca49ae ":1234", from_tty=1, extended_p=0) at remote.c:5668
  #7  0x00005555558a7f1a in open_target (args=0x555555ca49ae ":1234", from_tty=1, command=<optimized out>) at target.c:242

这不是说没有这个寄存器，而是说这个寄存器从target端没有给出来。查日志根本就没有
请求，明天该开debug_infrun看看行为了。

今天腰还是不行，效率奇低，集中不了精神，希望明天好点。

20220510
========

昨晚回去分析了一次RV的整个gdbserver调试过程，有如下总结：

1. 整个执行流程是这样的：

   1. 对齐能力（gdb发一套自己支持什么的列表，server回自己支持其中的哪些能力）
   2. 询问feature（读一堆的xml描述文件，生成tdesc）
   3. 询问现在目标状态，为什么停止
   4. 枚举所有进程和线程
   5. Attach进程
   6. 设置断点
   7. Cont（这里用了一个vCont命令，这个命令也要做能力对齐的）
   8. 等待信号，继续运行，或者发送break到目标，主动要求停下
   9. 下一轮

2. 断点不是靠写内存实现的，而是直接发断点命令Zz。

3. 单步是有命令的sS，所以，Server要实现单步，这个让我想到我们的一个问题，我们还
   没有好好建模块的单步到底指什么。我觉得我们需要把它的语义调整到一次执行整个块，
   单步的时候我们不能允许停在body的中间，否则不少东西没有语义，比如没有提交的某
   个中间过程算什么。不过这也不一定，这个后面还要想想，先使能吧。

今天继续昨天的调试，先看现场，gdb开了debug_infrun后的信息如下：::

  infrun: clear_proceed_status_thread (Thread 1.1)
  infrun: wait_for_inferior ()
  infrun: target_wait (-1.0.0, status) =
  infrun:   1.1.0 [Thread 1.1],
  infrun:   status->kind = stopped, signal = GDB_SIGNAL_TRAP
  infrun: handle_inferior_event status->kind = stopped, signal = GDB_SIGNAL_TRAP
  PC register is not available

qemu的gdbstub日志显示feature信息都送过去了，bstate的一堆寄存器也送过去了，然后
直接就是Attach和Continue了，所以这个过程整个都是正常的，就是寄存器送过来以后本
地合并的过程有问题。

首先修正了寄存器分配的问题，tdesc_use_registers(gdbarch, tdesc, early_data,
unknown_cb)的整体目的是生成一个gdbarch专有的tdesc_data，整个逻辑如下：

1. 先用early_data初始化目标tdesc_data，然后删掉earyly_data这个对象

2. 遍历整个tdesc的所有定义：

   1. 如果定义了reggroup，加入对应reggroup
   2. 和early_data一样的忽略
   3. 通过unknown_cb问每个寄存器的建议id，如果你能给，也加入tdesc_data，提议的
      id和从gdbarch->num_regs开始累加。
   4. 剩下的东西再给一个编号，加入到tdesc_data中

总结起来，就是，你默认一定有的寄存器，就全部定义为early_data，最大值定义为
gdbarch->num_regs，其他东西就靠tdesc来给你说，如果部分你也有所闻，就在unknown_cb中
给它一个你喜欢的id，如果你不关心，就让它全部自由编号就好了。

所以，如果我不给earyly_data初值，PC会从gdbarch->numregs开始算起，这样就没有PC了。

修正这个错误后，第二个问题是根本没有发起g命令向目标要寄存器，调用栈如下：::

  #0  remote_target::fetch_registers (this=0x555555d4b360, regcache=0x555555dfcc20, regnum=32) at remote.c:8255
  #1  0x00005555558a41d1 in target_fetch_registers (regcache=regcache@entry=0x555555dfcc20, regno=regno@entry=32) at target.c:3386
  #2  0x000055555580a9da in regcache::raw_update (regnum=32, this=0x555555dfcc20) at regcache.c:500
  #3  regcache::raw_update (this=0x555555dfcc20, regnum=<optimized out>) at regcache.c:489
  #4  0x000055555580b1be in readable_regcache::raw_read (this=0x555555dfcc20, regnum=32, buf=0x7fffffffd8e0 "") at regcache.c:514
  #5  0x000055555580df4d in readable_regcache::cooked_read<unsigned long, void> (this=this@entry=0x555555dfcc20, regnum=32, val=val@entry=0x7fffffffd940) at regcache.c:693
  #6  0x000055555580d6d9 in regcache_cooked_read_unsigned (val=0x7fffffffd940, regnum=<optimized out>, regcache=0x555555dfcc20) at regcache.c:707
  #7  regcache_read_pc (regcache=0x555555dfcc20) at regcache.c:1211
  #8  0x0000555555778c88 in handle_signal_stop (ecs=0x7fffffffdc90) at infrun.c:5916

跟踪发现这里有一个remote_arch_state->regs[]又做了一次寄存器映射，而这个映射里面
PC（32）的对应值是-1。不知道是哪里没有初始化。

初始化调用栈是这个：::

  #0  map_regcache_remote_table (gdbarch=gdbarch@entry=0x555555dd1b40, regs=0x555555e29bc0) at remote.c:1284
  #1  0x00005555558147fd in remote_arch_state::remote_arch_state (this=0x555555dce990, gdbarch=0x555555dd1b40) at /usr/include/c++/11/bits/unique_ptr.h:173
  #2  0x0000555555816b57 in std::pair<gdbarch* const, remote_arch_state>::pair<gdbarch*&, 0ul, gdbarch*&, 0ul> (__tuple2=..., __tuple1=..., this=0x555555dce988)
  ... C++模板那堆怪东西
  #11 remote_state::get_remote_arch_state (this=0x555555d4b378, gdbarch=0x555555dd1b40) at remote.c:1206
  #12 0x000055555581b802 in remote_target::get_remote_state (this=0x555555d4b360) at remote.c:1232
  #13 remote_target::get_trace_status (this=this@entry=0x555555d4b360, ts=0x555555bfcfc0 <trace_status>) at remote.c:13234
  #14 0x000055555582ca78 in remote_target::start_remote (this=this@entry=0x555555d4b360, from_tty=from_tty@entry=1, extended_p=extended_p@entry=0) at remote.c:4689
  #15 0x000055555582d310 in remote_target::open_1 (name=0x555555ca498e ":1234", from_tty=1, extended_p=0) at remote.c:5668
  #16 0x00005555558a797a in open_target (args=0x555555ca498e ":1234", from_tty=1, command=<optimized out>) at target.c:242

这个get_remote_state调用m_remote_state.get_remote_arch_state(target_gdbarch())
初始化这个对象，从而创建remote_arch_state这个对象。

get_remote_arch_state用了一个C++11的语法，我没有见过，这里记录一下。它是这样
的：::

      auto p = this->m_arch_states.emplace (std::piecewise_construct,
					    std::forward_as_tuple (gdbarch),
					    std::forward_as_tuple (gdbarch));

m_arch_states是一个map，map是一种特殊的vector，所以原来我们从vector说起。在vector
中插入对象，可以用v.insert(Foo(1))，这里创建了一个Foo(1)，insert的时候必须拷贝进去，
这产生一次拷贝构造，所以不值得。所以v还有一个语法，v.emplace(1)，编译器用v的模板
直接在里面构造这个对象，不用产生这个马上就删除的Foo(1)。

Map呢，需要一次送进入一对对象（std::tuple），这个语法第一个参数表明后面产生tuple，
后面两个参数表示直接产生这个对象的构造参数。cppreference.com有一个例子是这样的：::

    std::map<int, std::string> m;
    m.emplace(std::piecewise_construct,
              std::forward_as_tuple(10),
              std::forward_as_tuple(20, 'a'));

无论如何吧，这里最终就触发remote_arch_state的构造，然后这里，它会调用
gdbarch_remote_register_number()来映射寄存器。这东西在tdesc_use_registers()里面
设置了一个公共的，用的就是gdbarch->tdesc_data里面的内容。再跟踪过去，这里用了一
个gdbarch_register_type()来获得寄存器的类型，这东西是gdbarch里面设置的回调之一，
本来直接用tdesc_register_type()默认算就行了，但原生的riscv在里面做了一些变化。

但这个变化也不引起什么问题，奇怪的是，在init_regcache_descr检查的时候，所有类型
定义为xxx_ptr的变量，全部被当成非法类型了。而这个类型是gdbarch_init里面创建默认
tdesc的时候创建的，用的tdesc_create_reg这个函数，里面构造了tdesc_reg，里面用
tdesc_named_type确定你传进去的字符串和对应type的匹配关系。

现在的问题是，这东西在遇到xxx_ptr的时候认为feature里面有定义，所以，不使用系统
默认的定义。再跟过去呢，发现tdesc_reg的构造函数tdesc_named_type其实都找到
xxx_ptr了，反而没有找到int，我都没有搞明白它怎么反过来的。晚上还有开会，只好查
到这里了。

我真的很想给比雅尼几巴掌，再踏上一只脚。这个东西在缺乏标准定义和控制的代码中，
简直能让你死。总的来说，写的人觉得自己是神，什么都可以封装，读的是觉得自己是魔
鬼，只想杀了写的人：）

20220511
========

qemu项目状态跟踪
----------------

我看了一下贾文杰修改的trap的设计，感觉有些要素没有考虑到，导致没法根据这个设计
判断设计有没有漏洞，我尝试在这里做一个建模，提供一些帮助，我重点推演用户态trap
的逻辑，其他部分文杰可以根据我的例子类推。

下面这个是qemu的执行模型：::

  while not_exit:
    ret = cpu_loop();
    handle(ret);

无论是user还是system模式，都是一样的，区别仅仅是两者的cpu_loop()的退出条件不一
定相同。比如说，遇到一条非法指令，两者都在cpu_loop()的翻译中发现了，就会主动退
出循环，这时处理异常的流程就是一样的。但如果访问了非法地址，user是JIT代码（翻译
好的本地代码）真的触发了进程的异常，而system是tlbfill逻辑自行判断发生了异常。所
以，前者是通过监控sigaction列表产生了事件，而后者是读写指令的helper函数中主动发
起了异常退出操作。

虽然这些行为不同，但最终都会走到handle(ret)上，根据当时退出的那个原因，更新cpu的
状态，然后重新回到执行循环中。

现在我们要面对这样一个问题：如果在body执行的中间，我们发生了一次异常，我们也退
出到这个handle(ret)上了，handle(ret)要怎么做才能保证cpu_loop()可以从原来的地方
恢复执行呢？

按我们最初对块指令逻辑的建模，body内部发生中断，中间执行状态进入shadow，然后跳
转到异常向量，通过eret返回cpu原位的时候，cpu恢复一个块的执行，发现shadow有效，
就要用shadow先恢复body上下文，然后从body上下文的tpc的位置继续执行。

具体落实到用户态异常上，我们在主动和被动产生的跳出逻辑上，保存当时的shadow状态。
这是其一。其二，user和system不同，user的异常和ecall的实际发生都不发生的qemu中，
我们需要主动做事后的信号处理或者do_syscall，在两者都结束后，我们需要主动认为
eret已经发生了，这时我们需要主动把shadow有效标志设置上，然后返回cpu_loop()，这
时cpu_loop()会发现现在的pc在块头上，但shadow有效，翻译程序需要根据这个状态把pc
重新设置到body内部，生成从内部开始的tb，继续原来的执行流程。

这里还有一个备选的方案：我们可以handle(ret)里面，回到cpu_loop()前，就直接把PC设
置到body内部，同时设置好bpc1/bpc2，这样回到cpu_loop()的时候，就和原来的逻辑是一
样的。哪个方案更合适，要看system的方案如何设计，看哪种选择更容易让两个逻辑合并。

gdb调试工作
-----------

昨晚回去看了一下最新的gdb代码，对昨天的问题有如下观察：

1. init_regcache_desc()负责初始化和target相关的寄存器描述，其中寄存器类型的基本
   内容是从gdbarch_register_type里面取的。这里int和long都是给了名字的，但
   code/data_ptr不会给，但至少长度是给的，所以，如果gdbarch_register_type()返回
   正确的结果，这就不会有什么问题。

2. 如果用了tdesc_use_register这个封装，gdbarch_register_type用的就是tdesc提供的版本，
   这个版本原理如下：

   1. 有三个概念：
      1. tdesc_reg：这是tdesc描述的reg
      2. arch_reg：这是实际使用的reg
      3. regcache_descr：这是和target通讯用的reg
   2. 先从regcache_descr找gdbarch->tdesc_data里面的arch_reg，没有换pseudo，还是
      没有，类型就是int0_t
   3. 如果找到arch_reg了，但里面没有type，就用里面的tdesc_type或者tdesc_reg里面
      的字符串去实例化这个type，优先试前者，后者只支持int和float两个基本类型。

我们现在遇到的情况就是：我们不是int和float类型，但tdesc_type又支持不了创建type。
实际是，其他正常的寄存器都没有tdesc，而我们这个32号pc，偏偏就是有。再跟踪过去，
这是tdesc_named_type给它设置的内置类型。这个类型判断其实也是对的，就是
tdesc_predefined_types[11], code_ptr。

所以现在的问题在于make_gdb_type凭什么处理这个内置类型会不对？看函数的实现，这个
东西居然是从gdbarch的平台数据gdbtypes_data里面拿的，这个东西用gdbarch_xxxx_bit()
这套回调来初始化，其中指针依赖gdbarch_ptr_bit()（中间还有一套转换）。

那这个东西定位了，rv这些长度都是依赖xlen的，而我把这些东西都删除了。我要做一次
整改，把和xlen有关的东西都干掉，否则不知道还有多少这类基础错误。

修改后看日志，g命令终于发出来了，跟踪一个内核，可以正常连上去，然后用cond命令让
target继续，然后设置断点，说内存不能访问。运行info registers，结果如下：::

  (gdb) info registers
  ra             0xffffffff82208560       0xffffffff82208560 <create_pgd_mapping+160>
  sp             0xffffffff82c03e40       0xffffffff82c03e40
  gp             0xffffffff82ce93a0       0xffffffff82ce93a0 <__compound_literal.81>
  tp             0xffffffff82c09480       0xffffffff82c09480 <init_task>
  t0             0xffffffcefeffef00       -210470179072
  t1             0xbf001352       3204453202
  t2             0xffffffff80000000       -2147483648
  fp             0xffffffff82c03e80       0xffffffff82c03e80
  s1             0x80200000       2149580800
  a0             0xffffffcefeffe000       -210470182912
  a1             0x0      0
  a2             0x1000   4096
  a3             0xffffffcefefff000       -210470178816
  a4             0x0      0
  a5             0xffffffff82ceeff0       -2100367376
  a6             0x1352   4946
  a7             0x48     72
  s2             0xffffffe000000000       -137438953472
  s3             0xffffffcefeffe000       -210470182912
  s4             0xe3     227
  s5             0x200000 2097152
  s6             0xffffffff82401000       -2109730816
  s7             0xffffffff8240a640       -2109692352
  s8             0xffffffff82cf0000       -2100363264
  s9             0x80200000       2149580800
  s10s11t3       0xffffffe000000000       -137438953472
  t4             0xffffffe000000000       -137438953472
  t5             0xc0000000       3221225472
  t6             0x0      0
  pc             0x9      9
  "end of file"  0x80000520       0x80000520
  Aborted

看状态页表没有设置完，这个问题我先不管，Abort这个问题先查，发现是实现
gdbarch_register_name的时候名字数组长度的判断有误，而且这个地方根本没有必要有这
个数组，直接用tdesc_register_name就行了，修正以后显示就正常了，bstate的寄存器的
值也都对了。留个记录：::

  (gdb) info register
  ra             0xffffffff82208560       0xffffffff82208560 <create_pgd_mapping+160>
  sp             0xffffffff82c03e40       0xffffffff82c03e40
  gp             0xffffffff82ce93a0       0xffffffff82ce93a0 <__compound_literal.81>
  tp             0xffffffff82c09480       0xffffffff82c09480 <init_task>
  t0             0xffffffcefeffef00       -210470179072
  t1             0xbf001352       3204453202
  t2             0xffffffff80000000       -2147483648
  fp             0xffffffff82c03e80       0xffffffff82c03e80
  s1             0x80200000       2149580800
  a0             0xffffffcefeffe000       -210470182912
  a1             0x0      0
  a2             0x1000   4096
  a3             0xffffffcefefff000       -210470178816
  a4             0x0      0
  a5             0xffffffff82ceeff0       -2100367376
  a6             0x1352   4946
  a7             0x48     72
  s2             0xffffffe000000000       -137438953472
  s3             0xffffffcefeffe000       -210470182912
  s4             0xe3     227
  s5             0x200000 2097152
  s6             0xffffffff82401000       -2109730816
  s7             0xffffffff8240a640       -2109692352
  s8             0xffffffff82cf0000       -2100363264
  s9             0x80200000       2149580800
  s10            0xffffffe000000000       -137438953472
  s11            0xffffffe000000000       -137438953472
  t3             0xc0000000       3221225472
  t4             0x0      0
  t5             0x9      9
  t6             0x2400248        37749320
  pc             0x80000520       0x80000520
  sstatus        0x8000000000006100       -9223372036854750976
  sie            0x0      0
  stvec          0xffffffff80003180       -2147470976
  scounteren     0xffffffffffffffff       -1
  sscratch       0x0      0
  sepc           0xffffffff80003180       -2147470976
  scause         0x2      2
  stval          0x4f1e1f8b       1327374219
  sip            0x0      0
  satp           0x8000000000082604       -9223372036854241788
  mstatus        0x8000000a00006900       -9223371993905075968
  misa           0x800000000014112d       -9223372036853460691
  medeleg        0xb109   45321
  mideleg        0x222    546
  mie            0x8      8
  mtvec          0x80000520       2147484960
  mcounteren     0xffffffffffffffff       -1
  mhpmevent3     0x0      0
  ... 后面全零删除
  mscratch       0x80018000       2147581952
  mepc           0xffffffff80003180       -2147470976
  mcause         0x2      2
  mtval          0x0      0
  mip            0x0      0
  pmpcfg0        0x1f18   7960
  pmpcfg1        0x0      0
  pmpcfg2        0x0      0
  pmpcfg3        0x0      0
  pmpaddr0       0x20003fff       536887295
  pmpaddr1       0xffffffffffffffff       -1
  pmpaddr2       0x0      0
  ... 后面全零删除
  bstate_tpc     0xffffffff8218295c       -2112345764
  bstate_ext_tr1 0xffffffcefeffefd8       -210470178856
  bstate_ext_tr2 0xffffffcefeffefe8       -210470178840
  bstate_ext_tr3 0xffffffcefeffeff8       -210470178824
  bstate_ext_tr4 0x0      0
  bstate_ext_tr5 0xffffffcefeffef98       -210470178920
  bstate_ext_tr6 0xffffffcefeffefa8       -210470178904
  bstate_ext_tr7 0xffffffcefeffefb8       -210470178888
  bstate_ext_tr8 0xffffffcefeffefc8       -210470178872
  bstate_ext_sbpc0x0      0
  ... 后面全零删除
  bstate_en      0x0      0
  bstate_vld     0x1      1
  mcycle         0x31a4088fc73f76 13972630537715574
  minstret       0x31a4088fcb5f9a 13972630537985946
  mhpmcounter3   0x0      0
  ... 后面全零删除
  cycle          0x31a4089027c0ce 13972630544040142
  time           0x1c5ce3da       475849690
  instret        0x31a408902f9096 13972630544552086
  hpmcounter3    0x0      0
  ... 后面全零删除
  mvendorid      0x0      0
  marchid        0x0      0
  mimpid         0x0      0
  mhartid        0x0      0

然后用x看物理内存（现在没有虚拟内存），功能正常，用物理地址设置断点，看起来也成
功了。先合一个版本吧。

更新qemu和内核，再做一次测试，这次能过页表设置了，用Ctrl-C可以打断，基于符号设置一个
断点，出这个错：::

  linx-tdep.c:1101: internal-error: static ULONGEST riscv_insn::fetch_instruction(gdbarch*, CORE_ADDR, int*): Assertion `instlen <= sizeof (buf)' failed.

跟踪一下：这里instlen是16，用于判断的头是8b, 9c, 地址在ffffffff814108c0还有一次
是8b，3c。看了一下我们的编码，8b恰好就是我们的head的编码（最后7位是b即可，8b是
第八位为1）。所以，内容是没有错的，问题是这个代码在RV看来应该是32位的才对啊。
再看了一下，发现是编译器的人已经做了这个判断了。那就调整一下吧。

搞定这个以后，似乎所有功能都正常了，我让编译器看能不能明天发一个版本，这样我们
就又多一个调试手段了。


20220512
=========

现在又到了一个阶段点，我有几个方向可以投入：

1. 在gdb中用灵犀的断点指令换掉RV的断点指令，但这是个简单的体力活。我等要休息的
   时候干都不迟，或者以后让新人练手就可以了。

2. 使能gdb中的调试信息功能，昨晚分析过Dwarfs的格式和对块指令的适应性，感觉要改
   变的东西不多。但这是个无依赖工作，没有后续工作依赖它，有空的时候做没有问题，
   如果有其他更值得做的，就可以靠后一点。

3. 加入帮忙把用户态进程的全套工作突破了，迎接后面的用户态使能工作跟上

4. 加入帮忙帮助使能内核的使能，比如从昨天不同qemu选项下内核启动到不同位置的问题
   的定位。

5. 做块内断点和single-step的设计，这个也不是关键路径，可以在2之后做。

6. 几个评审和设计问题：

   1. 评审刘盈盈的平台标识设计
   2. 写调用者指令长度判断的问题建模
   3. 写norelax的文件建模

我打算先把2最容易做的部分先做了，摸个底，看现在gdb对块编译器的dwarf信息解释到什
么程度了，然后看3，最后看4。

首先带调试信息重新编译内核，gdb认vmliux是好的，而且很多信息看来很正常：::

  (gdb) info symbol 0xffffffff80000000
  _start in section .head.text
  (gdb) info address start_kernel
  Symbol "start_kernel" is a function at address 0xffffffff84602060.

但有趣的是，这样编译以后，qemu没法解释生成的Image文件了：::

  qemu-system-linx: could not load kernel 'arch/linx/boot/Image'

想想，是我昨天调试的时候用的内存太小了，调整了一下，好了。然后，看来全部都正常了，
可以设置断点，可以看到反汇编，对应的行号看来也是正确的。要测试更多的问题，就只能
等内核稳定一点再说了。

第3点朱熠琛说他今天出版本解决，所以我暂时不管，1，2,5可以慢慢来，所以重点看第4
点，6是文字工作，先做。

然后，今天大部分时间都在6上了。4我开个头。

首先有一个观察，开了-g，链接速度慢成狗（在服务器上，链接一次比不开-g全部重新编
译一次都久）。下次编译我就把这东西删了。这个问题需要让编译器团队知道。

然后跟踪日志，注意到这几个时间点：::

  Trace-Thu May 12 11:16:49 <----------- 机器启动，BIOS进入
  ...
  Trace-Thu May 12 11:16:49 <----------- 80200000，kernel进入
  ...
  Trace-Thu May 12 11:16:50 <----------- ffffffff80000100 satp设置成功
  ...                       <----------- 中间看到很多mem,string的连续chain，但都不是时间大头
  Trace-Thu May 12 11:16:51 <----------- 我一秒秒往下走看路径：do_early_param
                         52 fdt_getprop_by_offset
                         53 memmap_init_range
                         59 format_decode
                      17:00 reserve_bootmem_region
                         01 __free_pages_core/__free_pages_ok
                         32 _prb_read_valid
                         33 vsnprintf
                         34 number
                         35 ___ratelimit
                         36 ___printk   <--- 都有这个了，不是应该看见控制台了吗？为什么没有？
                         37 defer_console_output
                         38 vprintk_store
                         39 wake_up_klogd_work_func
                         ... <-- 后面隔10秒看
                      19:25 wake_up_klogd_work_func... <--- 尼玛，这是日志程序在空转啊

我感觉我们现在是没有调度所以现在切换不到rest_init()中吧。但为什么printk没有输出
到控制台？现在输出到ttyS0，莫非我控制台参数没有用对？

问了一下，内核的兄弟说这里用console=ttyAMA0，开这个控制台以后就好了。那看来内核
没有其他阻碍了，我明天看看各个部分的进展再考虑如何投入吧。

20220513
========

昨天晚上睡觉突然灵机一动，今天回来试试，发现果然，导致打印没有输出的，不是因为
console=ttyS0，而是因为没有earlycon。没有进入rest_init怎么会使用conole呢，都是
直接用着BIOS提供过来的控制台啊，我删掉console这个参数它都是好的。大意了。那这个
问题也不存在了。

换了编译器以后，用户态模拟的系统调用还是不正常，我发现这个问题还是ecall的地址问
题。我升级了LinxTenchAnalyse/cn/块指令软件使能过程独立逻辑建模/调用者指令长度判
断问题建模.rst对这个问题建模。总结起来，就是head.ecall自己设置返回地址，
rv.ecall按它原来的原则处理。

我看了一下代码，RV现在的ecall是直接产生异常RISCV_EXCP_U_ECALL，跳出执行循环后，
做了一个env->pc+=4强行跳过ecall指令。而Linx自己定义了一个LINX_EXCP_U_ECALL，但
还没有用。我可以考虑用这个来区分不同情况引起的调用。

理论上我们可以在执行blk.ecall的时候自己跳过整个头就可以了，但现在的问题是，qemu
还支持TARGET_ERESTARTSYS，遇到这种情形要求重启系统调用。我和王州讨论预计这个东
西是为了截获发给qemu自己的信号而做的，避免因为只这种信号而返回了多余的EAGAIN。
RV的做法是这种情况pc再回退4个字节，重启整个ecall指令，我们这里不行，我们这里会
导致整个head被重新执行，这样会产生重复逻辑的。但直接重新调用do_syscall也是不行的，
因为，这是为了留个机会让qemu自己去处理那些pending的signal，所以，我取个巧，直接
重新设置一下cpu的exception状态，让它一会去就重新出来就好了。

调试完成，用户态都正常，内核没有调试，但原功能正常，发给王州和陈娟娟合并合入。

20220514
========

今天主要修改LARM。

20220517
========

连续几天都在修改LARM，今天终于差不多了。回来这边跟上调试进展。13号修改的代码有
个问题，里面在do_syscall前移动了PC，我看do_syscall没有输入这个参数，就直接等完
成调用再去移动它。今天王州告诉我clone内部用了这个状态。当时做这个修改的时候还是
冒险了，在代码逻辑规整和完全保持原来逻辑之间选择了前者，其实我现在都不知道这个
选择哪个好。再选一次，可能我还是会选择前者吧，否则代码留下的坑太多了。

先处理一个陈立福遇到的问题：代码随机异常。

跟踪记录显示最后在一堆的_delay和panic里面跳，往回找，找到一个die，然后找到一个
oop_exit. add_taint, do_unblank_screen, walk_stackframe... 这看来已经在打印
coredump了嘛，所以再要了一下日志，发现是这样的：::

[    0.000000] SLUB: HWalign=64, Order=0-3, MinObjects=0, CPUs=1, Nodes=1
[    0.000000] NR_IRQS: 64, nr_irqs: 64, preallocated irqs: 0
[    0.000000] riscv-intc: 64 local interrupts mapped
[    0.000000] plic: plic@c000000: mapped 53 interrupts with 1 handlers for 2 contexts.
[    0.000000] random: get_random_bytes called from .L169+0x1e0/0x2a0 with crng_init=0
[    0.000000] riscv_timer_init_dt: Registering clocksource cpuid [0] hartid [0]
[    0.000000] clocksource: riscv_clocksource: mask: 0xffffffffffffffff max_cycles: 0x24e6a1710, max_idle_ns: 440795202120 ns
[    0.001384] sched_clock: 64 bits at 10MHz, resolution 100ns, wraps every 4398046511100ns
[    0.369876] Console: colour dummy device 80x25
[    0.468095] Calibrating delay loop (skipped), value calculated using timer frequency.. 20.00 BogoMIPS (lpj=40000)
[    0.533988] pid_max: default: 32768 minimum: 301
[    0.860072] Mount-cache hash table entries: 512 (order: 0, 4096 bytes, linear)
[    0.912120] Mountpoint-cache hash table entries: 512 (order: 0, 4096 bytes, linear)
[    3.582501] ASID allocator using 16 bits (65536 entries)
[    4.270357] devtmpfs: initialized
[    7.994562] clocksource: jiffies: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: 7645041785100000 ns
[    8.078152] futex hash table entries: 256 (order: 1, 12288 bytes, linear)
[    9.065414] NET: Registered PF_NETLINK/PF_ROUTE protocol family
[    9.801311] BUG: spinlock cpu recursion on CPU#0, swapper/1
[    9.830604]  lock: 0xffffffff82eea500, .magic: 00000000, .owner: <none>/-1, .owner_cpu: 0
[    9.873654] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g5bf3ac9cd4e3 #291
[    9.913380] Hardware name: riscv-virtio,qemu (DT)
[    9.937085] Call Trace:
[    9.954572] [<ffffffff80006680>] dump_backtrace+0x20/0x40
[    9.990063] [<ffffffff8145b000>] dump_stack+0x40/0x60
[   10.025228] [<ffffffff81441380>] .L3+0x40/0x60
[   10.055439] [<ffffffff80091ba0>] .L35+0x60/0x80
[   10.089673] [<ffffffff800e3100>] .L5+0x20/0xa0
[   10.119892] [<ffffffff800e31a0>] tick_handle_periodic+0x20/0x40
[   10.156489] [<ffffffff8106cda0>] riscv_timer_interrupt+0x60/0x80
[   10.196911] [<ffffffff800aa2a0>] .L436+0x0/0x40
[   10.227493] [<ffffffff8009e0e0>] .L100+0x60/0x80
[   10.260630] [<ffffffff807376c0>] riscv_intc_irq+0x40/0x60
[   10.294591] [<ffffffff8145b260>] generic_handle_arch_irq+0x40/0x80
[   10.335202] [<ffffffff80003520>] ret_from_exception+0x0/0x40
[   10.372258] [<ffffffff81466b80>] _raw_spin_lock+0x20/0x30
[   10.407360] [<ffffffff803ff880>] __kernfs_new_node.constprop.0+0xc0/0x140
[   10.450715] [<ffffffff80402600>] .L484+0x20/0x80
[   10.482933] [<ffffffff80407160>] __kernfs_create_file+0x20/0x80
[   10.520645] [<ffffffff804085e0>] .L122+0x20/0x40
[   10.554035] [<ffffffff80408a80>] .L158+0x0/0x40
[   10.587215] [<ffffffff80d7ba80>] .L378+0x20/0x100
[   10.620225] [<ffffffff8222a020>] .L56+0x0/0x20
[   10.653314] [<ffffffff822024e0>] .L214+0x20/0x80
[   10.687770] [<ffffffff82202c00>] .L245+0x40/0x60
[   10.722579] [<ffffffff8145b3f0>] kernel_init+0x30/0xc0
[   10.758788] [<ffffffff80003520>] ret_from_exception+0x0/0x40
[   10.800506] Oops - unknown exception [#1]
[   10.823208] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g5bf3ac9cd4e3 #291
[   10.861699] Hardware name: riscv-virtio,qemu (DT)
[   10.884986] epc : 0xffffffff82ed6880
[   10.908129]  ra : 0xffffffff82ed6880
[   10.931337] epc : ffffffff82ed6880 ra : ffffffff82ed6880 sp : ffffffe00322b6e0
[   10.966317]  gp : ffffffff82ee93a0 tp : ffffffe003238000 t0 : ffffffff82e14638
[   11.002046]  t1 : ffffffffffffffff t2 : ffffffff82edca38 s0 : ffffffe003211000
[   11.036950]  s1 : ffffffff800e3100 a0 : 0000000000000001 a1 : ffffffff82e82538
[   11.073011]  a2 : ffffffe00322b368 a3 : fffffffffffffffe a4 : 0000000000000000
[   11.107641]  a5 : ffffffe00322c000 a6 : 0000000000000000 a7 : 0000000000000000
[   11.142048]  s2 : 0000000122a1db00 s3 : ffffffff82e93008 s4 : 0000000000000005
[   11.176386]  s5 : 8000000000000005 s6 : 0000000000000000 s7 : 0000000000000002
[   11.210714]  s8 : 00000000000081a4 s9 : ffffffff82eea0a8 s10: ffffffff82eeab80
[   11.245468]  s11: 0000000000000000 t3 : 000000000000000f t4 : ffffffff82971130
[   11.279827]  t5 : ffffffff82e825a0 t6 : ffffffff82e825c0
[   11.307691] status: 0000000000000100 badaddr: ffffffff82ed6880 cause: 000000000000000c

这里在_raw_spin_lock()里面发生了一个时钟中断，然后在tick_handle_periodic的时候
又遇到同一个spinlock，就有这个错误了。tick_handle_periodic()里面的spinlock，看
起来是这个：jiffies_lock。我觉得可能可以通过用数据断点跟踪它的更新日志，看看是
不是这样。

还有陈娟娟的sclr日志没有来得及看，也明天再看吧。

20220518
========

先修改了几个LARM的bug。然后继续查昨天的问题。

我先用gdb来跟踪oop_die()想看看调用栈，但结果是这样的：::

  #0  0xffffffff80035440 in oops_exit () at kernel/panic.c:532
  #1  0xffffffff8000a400 in die (regs=<optimized out>, str=0xffffffff86af3f48 "Oops - unknown exception") at arch/linx/kernel/traps.c:49

前面一截没有了，估计是换了堆栈，还是要在原地看。所以我设置这两个断点：

1. spin_bug
2. handle_exception

这回进了这里：::

  #0  handle_exception () at arch/linx/kernel/entry.S:200
  #1  0xffffffff81649620 in vc_init (vc=0xffffffe00720ac00, rows=<optimized out>, cols=<optimized out>, do_clear=<optimized out>) at drivers/tty/vt/vt.c:3460
  scause         0x8000000000000005

这个cause是supervisor timer interrupt，这样过滤不行，换一个把handle_exception换
成handle_exceptions，这是异常的入口。

这回把gdb弄死了：::

  /home3/zhuyichen/git/linx-dataflow/LinxToolchain/build/linx64-unknown-elf/../../src/linx64_elf_build_src/linx-BLK-binutils/gdb/inline-frame.c:172: internal-error: void inline_frame_this_id(frame_info*, void**, frame_id*): Assertion `frame_id_p (*this_id)' failed.

这个绕不过去，因为整个堆栈unwind一直都要调用，我先试试看内核，回头再来查gdb自己
的问题。在gdb出某个assert失败后，错误如下：::

  Breakpoint 1, spin_bug (msg=0xffffffff86af8890 "bad magic", Prologue scan for function starting at 0xffffffff801b32a0 (limit 0xffffffff801b32c0)
  inline-frame.c:176: internal-error: void inline_frame_this_id(frame_info*, void**, frame_id*): Assertion `!frame_id_eq (*this_id, outer_frame_id)' failed.

看qemu这边的栈：::

  x2/sp: ffffffe00726b610
  x3/gp: ffffffff86ee93a0
  (qemu) x/100xg 0xffffffe00726b610
  ffffffe00726b610: 0x0000000000000001 0x0000000000000120
  ffffffe00726b620: 0xffffffe00726b650 0xffffffff8365dd30 X                             _raw_spin_lock_irqsave + 272
  ffffffe00726b630: 0x0000000000000002 0xffffffff86e85280 X                             hrtimer_bases(data)
  ffffffe00726b640: 0xffffffe00726b6b0 0xffffffff80249ac0 X                             hrtimer_interrupt + 128
  ffffffe00726b650: 0xffffffff86eeab80 0xffffffff86eea0a8 kernfs_node_cache(sbss)       __stack_chk_guard(sbss)
  ffffffe00726b660: 0x00000000000041ed 0x0000000000000001
  ffffffe00726b670: 0x0000000000000000 0x8000000000000005
  ffffffe00726b680: 0x0000000000000005 0xffffffff86e93008 X                             riscv_intc_chip(data)
  ffffffe00726b690: 0xffffffe007211000 0xffffffe007210000
  ffffffe00726b6a0: 0xffffffe00726b6c0 0xffffffff82ae2360 X                             riscv_timer_interrupt + 256
  ffffffe00726b6b0: 0xffffffe00726b6f0 0xffffffff801f49c0 X                             handle_percpu_devid_irq + 1376
  ffffffe00726b6c0: 0x0000000000000000 0x0000000000000000
  ffffffe00726b6d0: 0x0000000000000005 0xffffffe00720a400 X                             X
  ffffffe00726b6e0: 0xffffffe00726b710 0xffffffff801d5140 X                             generic_handle_domain_irq + 1088
  ffffffe00726b6f0: 0xffffffff86ee7470 0xffffffe00726b750 __irq_regs(data)              X
  ffffffe00726b700: 0xffffffe00726b720 0xffffffff8141efc0 X                             riscv_intc_irq + 224
  ffffffe00726b710: 0xffffffe00726b750 0xffffffff83635af0 X                             generic_handle_arch_irq + 736
  ffffffe00726b720: 0xffffffffffffffff 0x8000000000006120
  ffffffe00726b730: 0xffffffe0072d8000 0xffffffff801b3300 X                             do_raw_spin_lock + 96
  ffffffe00726b740: 0xffffffe00726b9f0 0xffffffff80004f60 X                             handle_exception + 928
  ffffffe00726b750: 0xffffffff801b3300 0xffffffff8365da10 do_raw_spin_lock + 96         _raw_spin_lock + 160
  ffffffe00726b760: 0xffffffe00726b9d0 0xffffffff86ee93a0 X                             __compound_literal.81(sdata)
  ffffffe00726b770: 0xffffffe0072d8000 0xffffffe0074eb080 X                             X
  ffffffe00726b780: 0xfffffffffffffffe 0x0000000000000000
  ffffffe00726b790: 0xffffffe00726b9f0 0xffffffe0074eb000
  ffffffe00726b7a0: 0xffffffff86e8d150 0x000000000000000b kernfs_idr_lock(data)         X
  ffffffe00726b7b0: 0x0000000000000000 0xffffffe0074eb080 X
  ffffffe00726b7c0: 0xffffffffdead4ead 0x0000000000000002 SPINLOCK_MAGIC                X
  ffffffe00726b7d0: 0x00000000000876eb 0xffffffe03f199268
  ffffffe00726b7e0: 0xffffffe007214000 0x0000000000001000
  ffffffe00726b7f0: 0xffffffff86b067c0 0x0000000000000000 .LC62(rodata)
  ffffffe00726b800: 0x0000000000000000 0x0000000000000001
  ffffffe00726b810: 0x00000000000041ed 0xffffffff86eea0a8 X                             __stack_chk_guard(sbss)
  ffffffe00726b82n: 0xffffffff86eeab80 0x0000000000000000 kernfs_node_cache(sbss)       X
  ffffffe00726b830: 0x0000000000200020 0xffffffff86b09ecc X                             .LC16 + 4(rodata)
  ffffffe00726b840: 0xffffffff86e0fee8 0xffffffff86e0ff08 init_pid_ns(data)
  ffffffe00726b850: 0x8000000000006120 0x0000000000000000
  ffffffe00726b860: 0x8000000000000005 0xffffffff80785000
  ffffffe00726b870: 0x0000000000000001 0x0000000000000000
  ffffffe00726b880: 0xffffffff836d154a 0xffffffff86e8d150 __do_return__.bstop(body)     kernfs_idr_lock(data)
  ffffffe00726b890: 0xffffffffdead4ead 0xffffffe00726b9d0 SPINLOCK_MAGIC                X
  ffffffe00726b8a0: 0x0000000000000020 0xffffffe00726b9f0
  ffffffe00726b8b0: 0xffffffe00726b9e0 0xffffffe0074eb000
  ffffffe00726b8c0: 0xffffffe00726b9d8 0x0000000000000000
  ffffffe00726b8d0: 0x0000000000000000 0x0000000000000000
  ffffffe00726b8e0: 0x0000000000000000 0x0000000000000000
  ffffffe00726b8f0: 0x0000000000000000 0x0000000000000000
  ffffffe00726b900: 0x0000000000000000 0x0000000000000000
  ffffffe00726b910: 0x0000000000000000 0x0000000000000000
  ffffffe00726b920: 0x0000000000000000 0x0000000000000000
  ffffffe00726b930: 0x0000000000000000 0x0000000000000000
  ffffffe00726b940: 0x0000000000000000 0x0000000000000000
  ffffffe00726b950: 0x0000000000000000 0x0000000000000000
  ffffffe00726b960: 0x0000000000000000 0x0000000000000000
  ffffffe00726b970: 0x0000000000000000 0x0000000000000000
  ffffffe00726b980: 0x0000000000000000 0x0000000000000000
  ffffffe00726b990: 0x0000000000000000 0x0000000000000000
  ffffffe00726b9a0: 0x0000000000000000 0x0000000000000000
  ffffffe00726b9b0: 0x0000000000000000 0x0000000000000000
  ffffffe00726b9c0: 0x0000000000000000 0x0000000000001000
  ffffffe00726b9d0: 0xffffffe007214000 0xffffffe0074eb000
  ffffffe00726b9e0: 0xffffffe00726ba00 0xffffffff8365da10 X                             _raw_spin_lock + 160
  ffffffe00726b9f0: 0xffffffe00726bae0 0xffffffff80b67b20 X                             __kernfs_new_node.constprop.0
  ffffffe00726ba00: 0x0000000000000000 0x0000000000000000
  ffffffe00726ba10: 0x00000000000005f2 0xffffffff86af8440 X                             .LC2(rodata)
  ffffffe00726ba20: 0xffffffe00726ba50 0xffffffff801444e0 X                             __might_sleep + 544
  ffffffe00726ba30: 0x00000000000005f2 0xffffffff86af8440 X                             .LC2(rodata)
  ffffffe00726ba40: 0xffffffe00726ba70 0xffffffff801444e0 X                             __might_sleep + 544
  ffffffe00726ba50: 0x0000000000000000 0xffffffff86e8d110 X                             kernfs_rwsem(data)
  ffffffe00726ba60: 0xffffffe00726ba90 0x5b0a5f3ea0431000
  ffffffe00726ba70: 0x0000000000000001 0x0000000000000000
  ffffffe00726ba80: 0xffffffff84600300 0x0000000000000008 ignore_unknown_bootoption     X
  ffffffe00726ba90: 0xffffffff86b9bcd0 0xffffffff84800480 __param_initcall_debug(__param) initcall_level_names + 48(init.data)
  ffffffe00726baa0: 0xffffffff86e8b8a8 0xffffffe007201a38 slab_ktype(data)              X
  ffffffe00726bab0: 0x0000000000000000 0xffffffe007201a30
  ffffffe00726bac0: 0xffffffe0074cc780 0xffffffe007201a30
  ffffffe00726bad0: 0xffffffe00726bb10 0xffffffff80b71ac0 X                             kernfs_create_dir_ns + 288
  ffffffe00726bae0: 0xffffffe0074cc780 0xffffffff86eea0a8 X                             __stack_chk_guard(sbss)
  ffffffe00726baf0: 0x0000000000000000 0xffffffe007201a30
  ffffffe00726bb00: 0xffffffe00726bb60 0xffffffff80b81620 X                             sysfs_create_dir_ns + 480
  ffffffe00726bb10: 0x0000000000000000 0xffffffff00000000
  ffffffe00726bb20: 0xffffffff00000000 0x5b0a5f3ea0431000
  ffffffe00726bb30: 0xffffffe007387da8 0xffffffe007201af8
  ffffffe00726bb40: 0xffffffe007387d80 0xffffffe007201a30
  ffffffe00726bb50: 0xffffffe00726bbb0 0xffffffff83562260 X                              kobject_add_internal + 1248
  ffffffe00726bb60: 0xffffffe007201a30 0xffffffff84800480 X                              initcall_level_names + 48
  ffffffe00726bb70: 0xffffffff8480ef78 0xffffffff86eea008 __initcall__kmod_fcntl__251_1059_fcntl_init6(data) initcall_debug(sbss)
  ffffffe00726bb80: 0xffffffff86eea0a8 0x0000000000000000 __stack_chk_guard(sbss)        X
  ffffffe00726bb90: 0xffffffe007201a30 0xffffffff86b1e940 X                              .LC91(rodata)
  ffffffe00726bba0: 0xffffffe00726bbf0 0xffffffff835635a0 X                              kobject_init_and_add + 288
  ffffffe00726bbb0: 0xffffffe00726bbf0 0x5b0a5f3ea0431000
  ffffffe00726bbc0: 0xffffffff86eea0a8 0xffffffe007201a30 __stack_chk_guard(sbss)
  ffffffe00726bbd0: 0xffffffe007387d80 0xffffffe0072019c0
  ...

（看这个列表要注意：同一行上，后一个值在前一个值前面，因为这是小端，很明显后期
出现多次的kernfs_node_cache其实是__stack_chk_guard的数据。）

所以这个问题仍是spinlock里面发生时钟中断的问题，还原场景应该是initcall中，某个
模块调用了kernfs_create_dir_ns，这里面可能通过__kernfs_new_node调用了spin_lock
（有可能是kernfs_idr_lock），这个时候发生了时钟中断，而时钟中断中使用另一个
spinlock的时候发现重复了。如果仅仅看hrtimer_interrupt的代码，我会猜这是
raw_spin_lock_irqsave(&cpu_base->lock, flags)，但这两个锁不是同一个，不会出错。
所以我更怀疑是不是某个定时器里面访问了kernfs_idr_lock了，但这有没有调用栈，有点
奇怪。

所以关键我还是要定位一下，这里展示为叠加的两个锁，是不是同一个？我在堆栈中制造
了一个局部变量放这个值，到时看堆栈能不能看到两个锁是否相同。再写个脚本自动处理
堆栈分析（在devlog/stack_trace.py里），看看能看到什么。

下面是第一次的结果：::

  ffffffe00726b640: ffffffff86e12740 (d)_printk_rb_static_infos+520
  ffffffe00726b648: ffffffff86e12740 (d)_printk_rb_static_infos+520
  ffffffe00726b650: ffffffff86ee93a0 (D)__global_pointer$
  ffffffe00726b658: ffffffe00726b8c0
  ffffffe00726b660: 8000000000000120
  ffffffe00726b668: ffffffe0072d8000
  ffffffe00726b670: 0000000000000056
  ffffffe00726b678: 0000000000000001
  ffffffe00726b680: ffffffe00726b8d0
  ffffffe00726b688: ffffffe0072d8000
  ffffffe00726b690: ffffffffffffffff
  ffffffe00726b698: 000000000d09a51c
  ffffffe00726b6a0: 0000000001e2c574
  ffffffe00726b6a8: 0000000000000015
  ffffffe00726b6b0: 000000000d09a51c
  ffffffe00726b6b8: 0000000000000000
  ffffffe00726b6c0: ffffffff86e85ab8 (d)clocksource_jiffies
  ffffffe00726b6c8: 0000000001f03cd3
  ffffffe00726b6d0: ffffffff86e93008 (d)riscv_intc_chip
  ffffffe00726b6d8: ffffffe00726b8d0
  ffffffe00726b6e0: 8000000000000005
  ffffffe00726b6e8: 0000000000000005
  ffffffe00726b6f0: 一堆0，省略
  ffffffe00726b720: 0000000000000402
  ffffffe00726b728: 0000000000000002
  ffffffe00726b730: ffffffff86e0ff08 (D)init_pid_ns+32
  ffffffe00726b738: ffffffff86e0fee8 (D)init_pid_ns
  ffffffe00726b740: ffffffff86e12740 (d)_printk_rb_static_infos+520
  ffffffe00726b748: 0000000000000100
  ffffffe00726b750: 0000000000000000
  ffffffe00726b758: 000000000000000c
  ffffffe00726b760: 0000000000000000
  ffffffe00726b768: 0000000000000001
  ffffffe00726b770: ffffffe00726bc40
  ffffffe00726b778: 一堆0，省略
  ffffffe00726b790: ffffffe00726bc90
  ffffffe00726b798: 0000000000000050
  ffffffe00726b7a0: ffffffff80334d00 (t)watchdog_enable+640  <-------------
  ffffffe00726b7a8: 0，省略
  ffffffe00726b7b8: 0000000000000020
  ffffffe00726b7c0: 一堆0，省略
  ffffffe00726b8b0: ffffffff86e12740 (d)_printk_rb_static_infos+520
  ffffffe00726b8b8: 0000000000000000
  ffffffe00726b8c0: ffffffff80135a00 (T)scheduler_tick+864  <-------------
  ffffffe00726b8c8: ffffffe00726b900
  ffffffe00726b8d0: 0000000000000000
  ffffffe00726b8d8: ffffffff86ee8d48 (D)jiffies_64
  ffffffe00726b8e0: 0000000000000000
  ffffffe00726b8e8: ffffffff86ee8d48 (D)jiffies_64
  ffffffe00726b8f0: ffffffff80241da0 (T)update_process_times+1760  <-------------
  ffffffe00726b8f8: ffffffe00726b920
  ffffffe00726b900: ffffffff86eea508 (B)jiffies_seq
  ffffffe00726b908: 000000000aba9500
  ffffffe00726b910: ffffffff80293e20 (t)tick_periodic.constprop.0+352  <-------------
  ffffffe00726b918: ffffffe00726b940
  ffffffe00726b920: ffffffff86ed6880 (d)riscv_clock_event
  ffffffe00726b928: ffffffe00726b9a0
  ffffffe00726b930: ffffffff802940e0 (T)tick_handle_periodic+96  <-------------
  ffffffe00726b938: ffffffe00726b970
  ffffffe00726b940: ffffffff86e93008 (d)riscv_intc_chip
  ffffffe00726b948: 0000000000000004
  ffffffe00726b950: ffffffe007210000
  ffffffe00726b958: ffffffe007211000
  ffffffe00726b960: ffffffff82ae23c0 (t)riscv_timer_interrupt+256  <-------------
  ffffffe00726b968: ffffffe00726b980
  ffffffe00726b970: ffffffff801f4a20 (T)handle_percpu_devid_irq+1376  <-------------
  ffffffe00726b978: ffffffe00726b9b0
  ffffffe00726b980: 0000000000000000
  ffffffe00726b988: 0000000000000000
  ffffffe00726b990: ffffffe00720a400
  ffffffe00726b998: 0000000000000005
  ffffffe00726b9a0: ffffffff801d51a0 (T)generic_handle_domain_irq+1088  <-------------
  ffffffe00726b9a8: ffffffe00726b9d0
  ffffffe00726b9b0: ffffffe00726ba10
  ffffffe00726b9b8: ffffffff86ee7470 (D)__irq_regs
  ffffffe00726b9c0: ffffffff8141f020 (t)riscv_intc_irq+224  <-------------
  ffffffe00726b9c8: ffffffe00726b9e0
  ffffffe00726b9d0: ffffffff83635b50 (T)generic_handle_arch_irq+736  <-------------
  ffffffe00726b9d8: ffffffe00726ba10
  ffffffe00726b9e0: 8000000000006120
  ffffffe00726b9e8: ffffffe00726ba00
  ffffffe00726b9f0: ffffffff80148c20 (W)running_clock+64
  ffffffe00726b9f8: ffffffe0072d8000
  ffffffe00726ba00: ffffffff80004f60 (t)ret_from_exception  <-------------
  ffffffe00726ba08: ffffffe00726bcb0
  ffffffe00726ba10: ffffffff80334e00 (t)watchdog_enable+896  <-------------
  ffffffe00726ba18: ffffffff80148c20 (W)running_clock+64
  ffffffe00726ba20: ffffffff86ee93a0 (D)__global_pointer$
  ffffffe00726ba28: ffffffe00726bc90
  ffffffe00726ba30: ffffffff86e89020 (d)hrtimer_interrupts_saved
  ffffffe00726ba38: ffffffe0072d8000
  ffffffe00726ba40: 0000000000000056
  ffffffe00726ba48: ffffffffffffffff
  ffffffe00726ba50: 0000000000000000
  ffffffe00726ba58: ffffffe00726bcb0
  ffffffe00726ba60: 8000000000006022
  ffffffe00726ba68: ffffffff86e85280 (D)hrtimer_bases
  ffffffe00726ba70: 0000000000000000
  ffffffe00726ba78: 00000000f8e8b400
  ffffffe00726ba80: 0000000000000000
  ffffffe00726ba88: ffffffffdead4ead
  ffffffe00726ba90: 0000000000000025
  ffffffe00726ba98: ffffffe007208400
  ffffffe00726baa0: 0000000000000000
  ffffffe00726baa8: ffffffff86e88f60 (d)hrtimer_interrupts
  ffffffe00726bab0: 0省略
  ffffffe00726baf0: 0000000000000402
  ffffffe00726baf8: 0000000000000002
  ffffffe00726bb00: ffffffff86e0ff08 (D)init_pid_ns+32
  ffffffe00726bb08: ffffffff86e0fee8 (D)init_pid_ns
  ffffffe00726bb10: 0000000000000000
  ffffffe00726bb18: 8000000000006120
  ffffffe00726bb20: ffffffe0072dc6c8
  ffffffe00726bb28: 8000000000000005
  ffffffe00726bb30: 0000000000000000
  ffffffe00726bb38: 0000000000000001
  ffffffe00726bb40: ffffffe00726bc40
  ffffffe00726bb48: ffffffff836b5086 (t)__do_return__.bstop+307640  <-------------
  ffffffe00726bb50: 0000000000000000
  ffffffe00726bb58: 0000000000000000
  ffffffe00726bb60: ffffffe00726bc90
  ffffffe00726bb68: 0000000000000050
  ffffffe00726bb70: ffffffff80334d00 (t)watchdog_enable+640  <-------------

这个错误的出现的时机让我怀疑调度是不是有什么状态没有保存啊？或者有没有可能调度
发生在块中间，qemu的恢复算法有问题？这两个场景经过单元测试吗？

而且，qemu在后一种场景上最好可以加一个统计项，看看我们实际的运行数据是啥样的。

再看一个：::

 scause   000000000000000c 缺页
 x2/sp    ffffffe00726b240
  ffffffe00726b240: ffffffff86e85280 (D)hrtimer_bases
  ffffffe00726b248: ffffffff86e85280 (D)hrtimer_bases
  ffffffe00726b250: ffffffff86ee93a0 (D)__global_pointer$
  ffffffe00726b258: ffffffe00726b4c0
  ffffffe00726b260: 8000000000000120
  ffffffe00726b268: ffffffe0072d8000
  ffffffe00726b270: 0000000000000000
  ffffffe00726b278: 0000000000000001
  ffffffe00726b280: ffffffff86e860d8 (d)tick_cpu_sched
  ffffffe00726b288: ffffffff86e85320 (D)hrtimer_bases+160
  ffffffe00726b290: 00311d3e00000000
  ffffffe00726b298: ffffffff86e85b50 (D)jiffies_lock
  ffffffe00726b2a0: 000000000000095e
  ffffffe00726b2a8: 0000000000000018
  ffffffe00726b2b0: ffffffffffffffff
  ffffffe00726b2b8: ffffffffdead4ead
  ffffffe00726b2c0: ffffffe03f1ae850
  ffffffe00726b2c8: ffffffff86e860d8 (d)tick_cpu_sched
  ffffffe00726b2d0: ffffffe00726b660
  ffffffe00726b2d8: 000000011fc84e28
  ffffffe00726b2e0: 000000011fc742f8
  ffffffe00726b2e8: ffffffff86eea528 (B)tick_next_period
  ffffffe00726b2f0: 0000000000000000
  ffffffe00726b2f8: 0000000000000120
  ffffffe00726b300: 0000000000000001
  ffffffe00726b308: 0000000000000001
  ffffffe00726b310: ffffffff80298f40 (t)tick_sched_timer  <-------------
  ffffffe00726b318: ffffffff86e85300 (D)hrtimer_bases+128
  ffffffe00726b320: 0000000000000001
  ffffffe00726b328: 0000000000200020
  ffffffe00726b330: 0000000000000240
  ffffffe00726b338: ffffffff86e83ff8 (d)timer_bases+4792
  ffffffe00726b340: ffffffff86e85280 (D)hrtimer_bases
  ffffffe00726b348: 0000000000000100
  ffffffe00726b350: ffffffe00726b370
  ffffffe00726b358: 000000000000000c
  ffffffe00726b360: 0000000000000000
  ffffffe00726b368: 0000000000000001
  ffffffe00726b370: ffffffe00726b8f0
  ffffffe00726b378: 0000000000000000
  ffffffe00726b380: ffffffe00726b8e0
  ffffffe00726b388: ffffffe00726b8e0
  ffffffe00726b390: ffffffe00726b8e0
  ffffffe00726b398: ffffffe00726b9d0
  ffffffe00726b3a0: ffffffe00726b8e8
  ffffffe00726b3a8: ffffffff80b67b80 (t)__kernfs_new_node.constprop.0+608  <-------------
  ffffffe00726b3b0: 0000000000000000
  ffffffe00726b3b8: ffffffe00726b8f0
  ffffffe00726b3c0: 全0
  ffffffe00726b4b0: ffffffff86e85280 (D)hrtimer_bases
  ffffffe00726b4b8: 0000000000000000
  ffffffe00726b4c0: ffffffff86e860d8 (d)tick_cpu_sched
  ffffffe00726b4c8: ffffffff86e85300 (D)hrtimer_bases+128
  ffffffe00726b4d0: ffffffff80243ce0 (t)__hrtimer_run_queues.constprop.0+2816  <-------------
  ffffffe00726b4d8: ffffffe00726b560
  ffffffe00726b4e0: 000000011fc742f8
  ffffffe00726b4e8: 0000000000000003
  ffffffe00726b4f0: 0000000000000000
  ffffffe00726b4f8: ffffffe00726b510
  ffffffe00726b500: ffffffff86e853b8 (D)hrtimer_bases+312
  ffffffe00726b508: ffffffff86e853f8 (D)hrtimer_bases+376
  ffffffe00726b510: ffffffff86e852a0 (D)hrtimer_bases+32
  ffffffe00726b518: ffffffff86e85378 (D)hrtimer_bases+248
  ffffffe00726b520: 7fffffffffffffff
  ffffffe00726b528: 000000011fc742f8
  ffffffe00726b530: 000000011fc742f8
  ffffffe00726b538: 0000000000000003
  ffffffe00726b540: ffffffff86e85280 (D)hrtimer_bases
  ffffffe00726b548: 0000000000000120
  ffffffe00726b550: ffffffff80249d80 (T)hrtimer_interrupt+736  <-------------
  ffffffe00726b558: ffffffe00726b5c0
  ffffffe00726b560: ffffffff86eea0a8 (B)__stack_chk_guard
  ffffffe00726b568: ffffffff86eeab88 (B)kernfs_node_cache
  ffffffe00726b570: 0000000000000004
  ffffffe00726b578: 000000000000a1ff
  ffffffe00726b580: 8000000000000005
  ffffffe00726b588: 0000000000000000
  ffffffe00726b590: ffffffff86e93008 (d)riscv_intc_chip
  ffffffe00726b598: 0000000000000005
  ffffffe00726b5a0: ffffffe007210000
  ffffffe00726b5a8: ffffffe007211000
  ffffffe00726b5b0: ffffffff82ae23c0 (t)riscv_timer_interrupt+256  <-------------
  ffffffe00726b5b8: ffffffe00726b5d0
  ffffffe00726b5c0: ffffffff801f4a20 (T)handle_percpu_devid_irq+1376  <-------------
  ffffffe00726b5c8: ffffffe00726b600
  ffffffe00726b5d0: 0000000000000000
  ffffffe00726b5d8: 0000000000000000
  ffffffe00726b5e0: ffffffe00720a400
  ffffffe00726b5e8: 0000000000000005
  ffffffe00726b5f0: ffffffff801d51a0 (T)generic_handle_domain_irq+1088  <-------------
  ffffffe00726b5f8: ffffffe00726b620
  ffffffe00726b600: ffffffe00726b660
  ffffffe00726b608: ffffffff86ee7470 (D)__irq_regs
  ffffffe00726b610: ffffffff8141f020 (t)riscv_intc_irq+224  <-------------
  ffffffe00726b618: ffffffe00726b630
  ffffffe00726b620: ffffffff83635b50 (T)generic_handle_arch_irq+736  <-------------
  ffffffe00726b628: ffffffe00726b660
  ffffffe00726b630: 8000000000006120
  ffffffe00726b638: 0000000000012cc0
  ffffffe00726b640: ffffffff8365da30 (T)_raw_spin_lock+32  <-------------
  ffffffe00726b648: ffffffe0072d8000
  ffffffe00726b650: ffffffff80004f60 (t)ret_from_exception  <-------------
  ffffffe00726b658: ffffffe00726b8f0
  ffffffe00726b660: ffffffff80b67b80 (t)__kernfs_new_node.constprop.0+608  <-------------
  ffffffe00726b668: ffffffff8365da30 (T)_raw_spin_lock+32  <-------------
  ffffffe00726b670: ffffffff86ee93a0 (D)__global_pointer$
  ffffffe00726b678: ffffffe00726b8e0
  ffffffe00726b680: ffffffe007b06080
  ffffffe00726b688: ffffffe0072d8000
  ffffffe00726b690: 0000000000000000
  ffffffe00726b698: fffffffffffffffe
  ffffffe00726b6a0: ffffffe007b06000
  ffffffe00726b6a8: ffffffe00726b8f0
  ffffffe00726b6b0: 000000000000000b
  ffffffe00726b6b8: ffffffff86e8d150 (d)kernfs_idr_lock
  ffffffe00726b6c0: ffffffe007b06080
  ffffffe00726b6c8: 0000000000000000
  ffffffe00726b6d0: 000000000000000b
  ffffffe00726b6d8: ffffffffffbfffff
  ffffffe00726b6e0: ffffffe03f1ae850
  ffffffe00726b6e8: 0000000000087d06
  ffffffe00726b6f0: 0000000000001000
  ffffffe00726b6f8: ffffffe007214000
  ffffffe00726b700: 0000000000000000
  ffffffe00726b708: ffffffe007a75a08
  ffffffe00726b710: 0000000000000004
  ffffffe00726b718: 0000000000000000
  ffffffe00726b720: ffffffff86eea0a8 (B)__stack_chk_guard
  ffffffe00726b728: 000000000000a1ff
  ffffffe00726b730: 0000000000000000
  ffffffe00726b738: ffffffff86eeab88 (B)kernfs_node_cache
  ffffffe00726b740: 0000000000000001
  ffffffe00726b748: 0000000000200020
  ffffffe00726b750: 0000000000000240
  ffffffe00726b758: ffffffff86e83ff8 (d)timer_bases+4792
  ffffffe00726b760: 0000000000000000
  ffffffe00726b768: 8000000000006120
  ffffffe00726b770: ffffffe007212300
  ffffffe00726b778: 8000000000000005
  ffffffe00726b780: 0000000000000000
  ffffffe00726b788: 0000000000000001
  ffffffe00726b790: ffffffe00726b8f0
  ffffffe00726b798: ffffffff836d0c94 (t)__do_return__.bstop+421318  <-------------
  ffffffe00726b7a0: ffffffe00726b8e0
  ffffffe00726b7a8: ffffffe00726b8e0
  ffffffe00726b7b0: ffffffe00726b8e0
  ffffffe00726b7b8: ffffffe00726b9d0
  ffffffe00726b7c0: ffffffe00726b8e8
  ffffffe00726b7c8: ffffffff80b67b80 (t)__kernfs_new_node.constprop.0+608  <-------------
  ffffffe00726b7d0: 0000000000000000
  ffffffe00726b7d8: ffffffe00726b8f0

这像极了spin_bug那个错误，但这是个执行缺页。这更增强了我的之前的怀疑了。不过这里
看不见我在堆栈里面加的标记，所以按编译器同事的建议，增加volatile。同时我删掉-g，
再看一次：::

  scause   000000000000000d 这是读缺页
  x2/sp    ffffffe0032932c0
  ffffffe0032932c0: ffffffff81465900 (T)_raw_spin_unlock_irqrestore+32  <-------------
  ffffffe0032932c8: ffffffff80091cc0 (T)do_raw_spin_unlock  <-------------
  ffffffe0032932d0: ffffffff82ee93a0 (D)__global_pointer$
  ffffffe0032932d8: ffffffe003293540
  ffffffe0032932e0: 8000000000000120
  ffffffe0032932e8: ffffffe0032dca00
  ffffffe0032932f0: 0000000000000000
  ffffffe0032932f8: 0000000000000001
  ffffffe003293300: 0000000000000000
  ffffffe003293308: ffffffe003293560
  ffffffe003293310: 0000000000000120
  ffffffe003293318: ffffffff82e85280 (D)hrtimer_bases
  ffffffe003293320: ffffffff82e88fe0 (d)watchdog_hrtimer
  ffffffe003293328: 0000000000000001
  ffffffe003293330: 0000000000000000
  ffffffe003293338: 0000000000000001
  ffffffe003293340: 0000000000000001
  ffffffe003293348: ffffffff82e860d8 (d)tick_cpu_sched
  ffffffe003293350: ffffffff82e85280 (D)hrtimer_bases
  ffffffe003293358: ffffffff82e85300 (D)hrtimer_bases+128
  ffffffe003293360: 0000000024b874cc
  ffffffe003293368: ffffffff82e85320 (D)hrtimer_bases+160
  ffffffe003293370: 0000000000000000
  ffffffe003293378: 0000000000000120
  ffffffe003293380: 0000000000000001
  ffffffe003293388: 0000000000000001
  ffffffe003293390: ffffffff800e4ce0 (t)tick_sched_timer  <-------------
  ffffffe003293398: ffffffff82e85300 (D)hrtimer_bases+128
  ffffffe0032933a0: 0000000000000000
  ffffffe0032933a8: 0000000000008000
  ffffffe0032933b0: 0000000000000240
  ffffffe0032933b8: ffffffff82e82d78 (d)timer_bases+56
  ffffffe0032933c0: 0000000000000000
  ffffffe0032933c8: 0000000000000100
  ffffffe0032933d0: 0000000021dafe00
  ffffffe0032933d8: 000000000000000d
  ffffffe0032933e0: 0000000000000000
  ffffffe0032933e8: 0000000000000001
  ffffffe0032933f0: ffffffff81465830 (T)_raw_spin_unlock+32  <-------------
  ffffffe0032933f8: 0000000000000000
  ffffffe003293400: fffffffffffffead
  ffffffe003293408: ffffffffdead5000
  ffffffe003293410: ffffffe003293980
  ffffffe003293418: ffffffffdead4ead
  ffffffe003293420: ffffffe0036023b8
  ffffffe003293428: ffffffe0036023b8
  ffffffe003293430: 0000000000000000
  ffffffe003293438: ffffffffdead4ead
  ffffffe003293440: 全0
  ffffffe003293530: ffffffe003293560
  ffffffe003293538: 0000000000000000
  ffffffe003293540: ffffffff82e860d8 (d)tick_cpu_sched
  ffffffe003293548: ffffffff82e85300 (D)hrtimer_bases+128
  ffffffe003293550: ffffffff800c84c0 (t)__hrtimer_run_queues.constprop.0+608  <-------------
  ffffffe003293558: ffffffe0032935e0
  ffffffe003293560: 0000000024b874cc
  ffffffe003293568: 00000000243e9bd4
  ffffffe003293570: 0000000000000000
  ffffffe003293578: ffffffe0032935b0
  ffffffe003293580: ffffffff82e853b8 (D)hrtimer_bases+312
  ffffffe003293588: ffffffff82e853f8 (D)hrtimer_bases+376
  ffffffe003293590: ffffffff82e852a0 (D)hrtimer_bases+32
  ffffffe003293598: ffffffff82e85378 (D)hrtimer_bases+248
  ffffffe0032935a0: 7fffffffffffffff
  ffffffe0032935a8: 0000000024b874cc
  ffffffe0032935b0: 0000000024b874cc
  ffffffe0032935b8: 0000000000000003
  ffffffe0032935c0: ffffffff82e85280 (D)hrtimer_bases
  ffffffe0032935c8: 0000000000000120
  ffffffe0032935d0: ffffffff800ca360 (T)hrtimer_interrupt+224  <-------------
  ffffffe0032935d8: ffffffe003293640
  ffffffe0032935e0: 0000000000000000
  ffffffe0032935e8: 0000000000000000
  ffffffe0032935f0: ffffffe003293a48
  ffffffe0032935f8: ffffffe0032d8db0
  ffffffe003293600: 8000000000000005
  ffffffe003293608: ffffffe003293b84
  ffffffe003293610: ffffffff82e93008 (d)riscv_intc_chip
  ffffffe003293618: 0000000000000005
  ffffffe003293620: ffffffe003210000
  ffffffe003293628: ffffffe003211000
  ffffffe003293630: ffffffff8106b8a0 (t)riscv_timer_interrupt+96  <-------------
  ffffffe003293638: ffffffe003293650
  ffffffe003293640: ffffffff800aa2c0 (T)handle_percpu_devid_irq+160  <-------------
  ffffffe003293648: ffffffe003293680
  ffffffe003293650: 0000000000000000
  ffffffe003293658: 0000000000000000
  ffffffe003293660: ffffffe00320a400
  ffffffe003293668: 0000000000000005
  ffffffe003293670: ffffffff8009e100 (T)generic_handle_domain_irq+128  <-------------
  ffffffe003293678: ffffffe0032936a0
  ffffffe003293680: ffffffe0032936e0
  ffffffe003293688: ffffffff82ee7470 (D)__irq_regs
  ffffffe003293690: ffffffff807361c0 (t)riscv_intc_irq+64  <-------------
  ffffffe003293698: ffffffe0032936b0
  ffffffe0032936a0: ffffffff81459d60 (T)generic_handle_arch_irq+64  <-------------
  ffffffe0032936a8: ffffffe0032936e0
  ffffffe0032936b0: 8000000000006120
  ffffffe0032936b8: 0000000000000005
  ffffffe0032936c0: ffffffff80091ce0 (T)do_raw_spin_unlock+32  <-------------
  ffffffe0032936c8: ffffffe0032dca00
  ffffffe0032936d0: ffffffff80003520 (t)ret_from_exception  <-------------
  ffffffe0032936d8: ffffffe003293980
  ffffffe0032936e0: ffffffff81465830 (T)_raw_spin_unlock+32  <-------------
  ffffffe0032936e8: ffffffff80091ce0 (T)do_raw_spin_unlock+32  <-------------
  ffffffe0032936f0: ffffffff82ee93a0 (D)__global_pointer$
  ffffffe0032936f8: ffffffe003293960
  ffffffe003293700: ffffffe0032f1000
  ffffffe003293708: ffffffe0032dca00
  ffffffe003293710: 0000000000000000
  ffffffe003293718: 0000000000000001
  ffffffe003293720: ffffffe0036023b8
  ffffffe003293728: ffffffe003293980
  ffffffe003293730: 0000000000000020
  ffffffe003293738: ffffffe0036023b8
  ffffffe003293740: ffffffe0036023b8
  ffffffe003293748: 0000000000000020
  ffffffe003293750: ffffffffdead4ead
  ffffffe003293758: ffffffffdead4ead
  ffffffe003293760: 0000000000000001
  ffffffe003293768: 0000000000000000
  ffffffe003293770: ffffffe003293c28
  ffffffe003293778: 0000000000000001
  ffffffe003293780: 0000000000000000
  ffffffe003293788: ffffffe003293c28
  ffffffe003293790: ffffffe003293a48
  ffffffe003293798: ffffffe003293b84
  ffffffe0032937a0: 0000000000000000
  ffffffe0032937a8: ffffffe0032d8db0
  ffffffe0032937b0: 0000000000000000
  ffffffe0032937b8: 0000000000000000
  ffffffe0032937c0: 0000000000000000
  ffffffe0032937c8: 0000000000008000
  ffffffe0032937d0: 0000000000000240
  ffffffe0032937d8: ffffffff82e82d78 (d)timer_bases+56
  ffffffe0032937e0: 0000000000000000
  ffffffe0032937e8: 8000000000006120
  ffffffe0032937f0: ffffffff82e860d8 (d)tick_cpu_sched
  ffffffe0032937f8: 8000000000000005
  ffffffe003293800: 0000000000000000
  ffffffe003293808: 0000000000000001
  ffffffe003293810: ffffffff81465830 (T)_raw_spin_unlock+32  <-------------
  ffffffe003293818: ffffffff814d2168 (t)__do_return__.bstop+403790  <-------------
  ffffffe003293820: fffffffffffffead
  ffffffe003293828: ffffffffdead5000
  ffffffe003293830: ffffffe003293980
  ffffffe003293838: ffffffffdead4ead
  ffffffe003293840: ffffffe0036023b8
  ffffffe003293848: ffffffe0036023b8
  ffffffe003293850: 0000000000000000
  ffffffe003293858: ffffffffdead4ead

这回跑到unlock上了，而且换了一个缺页错误。这再增强我之前的怀疑，但我没有看到我
压在堆栈中的东西，我给unlock也加一个看看：::

  void __lockfunc _raw_spin_unlock(raw_spinlock_t *lock)
  {
	  //add by kenny
	  volatile uint64_t tmp = 0xddddddddcccccccc;
	  volatile uint64_t tmp1 = (uint64_t)lock;

	  do_global_tmp(&tmp);
	  do_global_tmp(&tmp1);
	  __raw_spin_unlock(lock);
  }

这回的结果是没有进入断点，而进了立童用linx_debug指令做的bug_on了。也看看堆栈吧：::

  ffffffe00343b710: ffffffff82e11740 (D)runqueues
  ffffffe00343b718: 0000000000000001
  ffffffe00343b720: ffffffff8007c580 (t)sched_rt_period_timer+512  <-------------
  ffffffe00343b728: ffffffe00343b790
  ffffffe00343b730: 0000000000000001
  ffffffe00343b738: ffffffff82e85300 (D)hrtimer_bases+128
  ffffffe00343b740: 0000000000000000
  ffffffe00343b748: 0000000000000001
  ffffffe00343b750: 000000006be36348
  ffffffe00343b758: 0000000000000120
  ffffffe00343b760: ffffffff82e85280 (D)hrtimer_bases
  ffffffe00343b768: ffffffff82e85320 (D)hrtimer_bases+160
  ffffffe00343b770: ffffffff82ef5708 (B)def_rt_bandwidth+40
  ffffffe00343b778: ffffffff82e85300 (D)hrtimer_bases+128
  ffffffe00343b780: ffffffff800c84e0 (t)__hrtimer_run_queues.constprop.0+640  <-------------
  ffffffe00343b788: ffffffe00343b810
  ffffffe00343b790: 000000006be36348
  ffffffe00343b798: ffffffff82e11770 (D)runqueues+48
  ffffffe00343b7a0: 000000000000000e
  ffffffe00343b7a8: ffffffe00343b7f0
  ffffffe00343b7b0: ffffffff82e853b8 (D)hrtimer_bases+312
  ffffffe00343b7b8: ffffffff82e853f8 (D)hrtimer_bases+376
  ffffffe00343b7c0: ffffffff82e852a0 (D)hrtimer_bases+32
  ffffffe00343b7c8: ffffffff82e85378 (D)hrtimer_bases+248
  ffffffe00343b7d0: 7fffffffffffffff
  ffffffe00343b7d8: 000000006be36348
  ffffffe00343b7e0: 000000006be36348
  ffffffe00343b7e8: 0000000000000003
  ffffffe00343b7f0: ffffffff82e85280 (D)hrtimer_bases
  ffffffe00343b7f8: 0000000000000120
  ffffffe00343b800: ffffffff800ca360 (T)hrtimer_interrupt+224  <-------------
  ffffffe00343b808: ffffffe00343b870
  ffffffe00343b810: 0000000000000001
  ffffffe00343b818: ffffffff82a16528 (D)fair_sched_class
  ffffffe00343b820: 0000000000000078
  ffffffe00343b828: ffffffe0032fdc80
  ffffffe00343b830: 8000000000000005
  ffffffe00343b838: ffffffff82eea0a8 (B)__stack_chk_guard
  ffffffe00343b840: ffffffff82e93008 (d)riscv_intc_chip
  ffffffe00343b848: 0000000000000005
  ffffffe00343b850: ffffffe003210000
  ffffffe00343b858: ffffffe003211000
  ffffffe00343b860: ffffffff8106b8a0 (t)riscv_timer_interrupt+96  <-------------
  ffffffe00343b868: ffffffe00343b880
  ffffffe00343b870: ffffffff800aa2c0 (T)handle_percpu_devid_irq+160  <-------------
  ffffffe00343b878: ffffffe00343b8b0
  ffffffe00343b880: 0000000000000000
  ffffffe00343b888: 0000000000000000
  ffffffe00343b890: ffffffe00320a400
  ffffffe00343b898: 0000000000000005
  ffffffe00343b8a0: ffffffff8009e100 (T)generic_handle_domain_irq+128  <-------------
  ffffffe00343b8a8: ffffffe00343b8d0
  ffffffe00343b8b0: ffffffe00343b910
  ffffffe00343b8b8: ffffffff82ee7470 (D)__irq_regs
  ffffffe00343b8c0: ffffffff807361c0 (t)riscv_intc_irq+64  <-------------
  ffffffe00343b8c8: ffffffe00343b8e0
  ffffffe00343b8d0: ffffffff81459d60 (T)generic_handle_arch_irq+64  <-------------
  ffffffe00343b8d8: ffffffe00343b910
  ffffffe00343b8e0: 8000000000006120
  ffffffe00343b8e8: ffffffe00343b950
  ffffffe00343b8f0: ffffffff81465910 (T)_raw_spin_unlock_irqrestore+48  <-------------
  ffffffe00343b8f8: ffffffe0032fdc80
  ffffffe00343b900: ffffffff80003520 (t)ret_from_exception  <-------------
  ffffffe00343b908: ffffffe00343bbb0
  ffffffe00343b910: ffffffff81465900 (T)_raw_spin_unlock_irqrestore+32  <-------------
  ffffffe00343b918: ffffffff81465910 (T)_raw_spin_unlock_irqrestore+48  <-------------
  ffffffe00343b920: ffffffff82ee93a0 (D)__global_pointer$
  ffffffe00343b928: ffffffe00343bb90
  ffffffe00343b930: 0000000000000040
  ffffffe00343b938: ffffffe0032fdc80
  ffffffe00343b940: 0000000000000317
  ffffffe00343b948: 0000000000000000
  ffffffe00343b950: 0000000000000002
  ffffffe00343b958: ffffffe00343bbb0
  ffffffe00343b960: 8000000000006022
  ffffffe00343b968: ffffffe0032fe288
  ffffffe00343b970: 0000000000000001
  ffffffe00343b978: 0000000000000000
  ffffffe00343b980: 0000000000000001
  ffffffe00343b988: ffffffffdead4ead
  ffffffe00343b990: 0000000054494d45
  ffffffe00343b998: 0000000000000000
  ffffffe00343b9a0: ffffffe00343bc40
  ffffffe00343b9a8: 0000000000000000
  ffffffe00343b9b0: ffffffff82e11740 (D)runqueues
  ffffffe00343b9b8: 0000000000000031
  ffffffe00343b9c0: 0000000000000078
  ffffffe00343b9c8: ffffffff82eea0a8 (B)__stack_chk_guard
  ffffffe00343b9d0: 0000000000000001
  ffffffe00343b9d8: ffffffe0032fdc80
  ffffffe00343b9e0: 000000000000000e
  ffffffe00343b9e8: ffffffff82a16528 (D)fair_sched_class
  ffffffe00343b9f0: 0000000000000402
  ffffffe00343b9f8: 0000000000000002
  ffffffe00343ba00: ffffffff82e825c0 (d)irq_desc_tree+32
  ffffffe00343ba08: ffffffff82e825a0 (d)irq_desc_tree
  ffffffe00343ba10: 0000000000000000
  ffffffe00343ba18: 8000000000006120
  ffffffe00343ba20: ffffffe00343ba70
  ffffffe00343ba28: 8000000000000005
  ffffffe00343ba30: 0000000000000000
  ffffffe00343ba38: 0000000000000001
  ffffffe00343ba40: 0000000000000259
  ffffffe00343ba48: 0000000000000000
  ffffffe00343ba50: 0000000000000180
  ffffffe00343ba58: 0000000000000080
  ffffffe00343ba60: ffffffe003c68880
  ffffffe00343ba68: ffffffe003c68700
  ffffffe00343ba70: 00000000000002a8
  ffffffe00343ba78: 0000000000000258
  ffffffe00343ba80: 0000000000000000
  ffffffe00343ba88: 0000025900000000
  ffffffe00343ba90: 全0
  ffffffe00343bb80: ffffffff81465900 (T)_raw_spin_unlock_irqrestore+32  <-------------
  ffffffe00343bb88: 0000000000000000
  ffffffe00343bb90: ffffffe0032fdc80 <=================== 那么这里就是unlock的时候用的lock指针了
  ffffffe00343bb98: ddddddddcccccccc <=================== 我加的标记
  ffffffe00343bba0: ffffffff80068820 (t)__sched_setscheduler.constprop.0+1280  <-------------
  ffffffe00343bba8: ffffffe00343bc40
  ffffffe00343bbb0: 8000000000006022
  ffffffe00343bbb8: 0000000000000000
  ffffffe00343bbc0: c6da306aa9a73a00
  ffffffe00343bbc8: 0000000000000004
  ffffffe00343bbd0: 全0
  ffffffe00343bbf0: ffffffff800a0640 (t)irq_thread_fn  <-------------
  ffffffe00343bbf8: ffffffe0032d8db0
  ffffffe00343bc00: ffffffe003232400
  ffffffe00343bc08: ffffffe003b18b40
  ffffffe00343bc10: ffffffff800a0740 (t)irq_thread  <-------------
  ffffffe00343bc18: ffffffe003b18b40
  ffffffe00343bc20: ffffffff82eea0a8 (B)__stack_chk_guard
  ffffffe00343bc28: ffffffe00350ef80
  ffffffe00343bc30: ffffffff8006d2e0 (T)sched_set_fifo+32  <-------------
  ffffffe00343bc38: ffffffe00343bca0
  ffffffe00343bc40: 0000000000000000
  ffffffe00343bc48: 0000000100000000
  ffffffe00343bc50: 0000000000000000
  ffffffe00343bc58: 0000003200000000
  ffffffe00343bc60: 0000000000000000
  ffffffe00343bc68: 0000000000000000
  ffffffe00343bc70: c6da306aa9a73a00
  ffffffe00343bc78: 0000000000000000
  ffffffe00343bc80: ffffffe0035d8d80
  ffffffe00343bc88: ffffffe00343bca0
  ffffffe00343bc90: ffffffff800a0780 (t)irq_thread+64  <-------------
  ffffffe00343bc98: ffffffe00343bd40
  ffffffe00343bca0: ffffffe00326b778
  ffffffe00343bca8: 0000000000000001
  ffffffe00343bcb0: c6da306aa9a73a00
  ffffffe00343bcb8: ffffffe000000004
  ffffffe00343bcc0: c6da306aa9a73a00
  ffffffe00343bcc8: ffffffe0032d8db0
  ffffffe00343bcd0: 0000000000000000
  ffffffe00343bcd8: ffffffe00343bd10
  ffffffe00343bce0: 0000000000000000
  ffffffe00343bce8: 0000000000000000
  ffffffe00343bcf0: ffffffff82eea0a8 (B)__stack_chk_guard
  ffffffe00343bcf8: ffffffe0032d8db0
  ffffffe00343bd00: ffffffe00326b778
  ffffffe00343bd08: 0000000000000001
  ffffffe00343bd10: ffffffff800a0740 (t)irq_thread  <-------------
  ffffffe00343bd18: ffffffe003b18b40
  ffffffe00343bd20: ffffffe0035d8d80
  ffffffe00343bd28: ffffffe00350ef80
  ffffffe00343bd30: ffffffff80056760 (t)kthread+480  <-------------
  ffffffe00343bd38: ffffffe00343bd80
  ffffffe00343bd40: ffffffff80056580 (t)kthread  <-------------
  ffffffe00343bd48: ffffffe00343bd60

这回看到我的标记了，但我暂时也不觉得应该查这个递归的问题了，看这一组错误，我觉
得调度一次，引起部分寄存器的结果不符合预期的可能性反而是最高的。需要组织一次针对
调度的集成测试才是正事。

初步的想法是，分成两部分工作，部分直接去review倒换的代码。另一部分做一个方案，
一旦完成中断的设置，后面的就不跑了，进入一个我们的函数，我们就做一个死循环，每
次把自己的寄存器一个个检查预期值以后加1，第一轮测试进入中断以后随便干点什么，然后
回去原来的循环，看中断本身保存上下文有没有问题。第二轮做N个循环，在N个循环上切，
看看是否引起问题。Qemu这边要检查是否覆盖在块中间切走的流程和频度。

我们做完这件事在弄其他的比较安全。

20220519
========

调度问题定位
------------

昨天简单看了一下内核的代码，王州也验证了一下切出和切入的过程，暂时没有发现明显
的错误。我晚上在家里构思了一下，觉得可以增加一个切换跟踪的功能（放在-d里面），
把切换的整个过程跟踪出来，这样就算以后也可以持续用这种方法跟踪切换过程。今天先
做这件事。

长远来说，这个功能以后还可以增加参数，限定一组pasid来跟踪，但大部分时候我们也不
知道pasid怎么分配的，这种事情，等以后数据多了再想办法吧。

1. 切出的实际就是中断处理，就是riscv_cpu_do_interrupt（更好是放在通用调用
   do_interrupt的地方做，但这样就没有平台相关信息了，所以先这样吧）

2. 切入应该在sret和mret的helper里面做。

做完马上就发现问题了：我们进场在body里发生中断，而中断包括m_timer，而SBI不会给
我们恢复上下文，剩下的事情不用说了。这最简单的修改方法是：不要在body里面处理中
断。

长远来说，我们还要解决一个问题：未来SBI是我们自己写的，我们当然可以处理从块中间
中断的情况，但我们并不能避免发生一个S-mode的中断后，马上发生一个M-mode中断，这样
块的中间状态就可以在没有保存的情况下被清除了，所以，要不我们不让M-mode抢占，要不
我们还要增加一套寄存器，

那这个问题算是定位出一个主要原因了，我留给其他人处理剩下的问题。

文档评审
--------

接着做两个评审：

1. 完成刘盈盈的norelax的文档评审，主要没有定义具体编译和链接态的行为，反馈回去了。
2. 然后就是陈娟娟的lrsc设计评审，我觉得有这么一些问题：

   1. 这个设计对lr/sc的语义总结太简单了，都没有把它的语义表达清楚，我们的设计怎
      么向设计目标对齐？

      下面是RV的描述：

          | LR.W loads a word from the address in rs1, places the sign-extended
          | value in rd, and registers a reservation set -- a set of bytes that
          | subsumes the bytes in the addressed word. SC.W conditionally writes a
          | word in rs2 to the address in rs1: the SC.W succeeds only if the
          | reservation is still valid and the reservation set contains the bytes
          | being written. If the SC.W succeeds, the instruction writes the word
          | in rs2 to memory, and it writes zero to rd. If the SC.W fails, the
          | instruction does not write to memory, and it writes a nonzero value
          | to rd. Regardless of success or failure, executing an SC.W
          | instruction invalidates any reservation held by this hart. LR.D and
          | SC.D act analogously on doublewords and are only available on RV64.
          | For RV64, LR.W and SC.W sign-extend the value placed in rd.

       这段描述最大的问题是没有解释什么叫servervation is still valid and the
       reservation set contains the bytes being written.

       但后面它有长的一段说明了几个要点，太长我只总结一下：

       1. 记住的地址的reserved的地址长度实现相关（qemu实际上实现成无限长），比
          如你reserver一个地址100，其他人碰了一下200，照理说和你无关。但你用
          1000作为保护长度，那么碰200也会取消你的reserved。qemu是只要有人碰过内
          存，reserved就取消。
       2. 其他hart写过这个地址（区域），保留取消（奇怪我在qemu实现中没有看到这
          个操作）
       3. 其他hart/device SC过这个区域，保留取消
       4. 按qemu的实现，切换模式也会取消保留，但手册没有看到（ARM的手册我是看过
          这个描述的）

       这个逻辑要全部打通，我们才敢说这个分析完成了。

   2. 对我们来说，上面这个语义和块的类型是无关的，因为行为在内存上，把微指令看
      作是普通指令那样处理就行了。但这个东西不能放在原子块中，因为我们原子块的
      语义是微指令写在cache上，最后提交，你现在这个reserve到什么时候提交？所以
      最简单的方法是：原子块中不得使用lr/sc指令。

GDB调试
-------

好了，终于可以轮到gdb问题的定位了。让我把之前的错误拷贝下来：::

  gdb/inline-frame.c:172: internal-error: void inline_frame_this_id(frame_info*, void**, frame_id*): Assertion `frame_id_p (*this_id)' failed.

用叠加的gdb重现一下调用栈，发现用没有-g的内核，问题根本不出来。一路单步过去，都
没有任何问题。这过程我记录一下关于frame一些基本信息：::

  (gdb) i s
  #0  0xffffffff814658e0 in _raw_spin_unlock_irqrestore ()
  #1  0xffffffff8108d720 in of_match_node ()
  (gdb) frame 1
  #1  0xffffffff8108d720 in of_match_node ()
  (gdb) info frame
  Stack level 1, frame at 0x0:
  pc = 0xffffffff8108d720 in of_match_node; saved pc = <not saved>
  Outermost frame: outermost
  caller of frame at 0xffffffe00326baf0
  Arglist at unknown address.
  Locals at unknown address, Previous frame's sp in sp
  (gdb) frame 0
  #0  0xffffffff814658e0 in _raw_spin_unlock_irqrestore ()
  (gdb) info frame
  Stack level 0, frame at 0xffffffe00326baf0:
  pc = 0xffffffff814658e0 in _raw_spin_unlock_irqrestore; saved pc = 0xffffffff8108d720
  called by frame at 0x0
  Arglist at 0xffffffe00326baf0, args:
  Locals at 0xffffffe00326baf0, Previous frame's sp is 0xffffffe00326baf0
  (gdb) x /30xg $sp
  0xffffffe00326baf0:     0xffffffe00326bb50      0xffffffff82eea008
  0xffffffe00326bb00:     0xffffffff82eea0a8      0xffffffff82e96070
  0xffffffe00326bb10:     0xffffffe0032ac410      0xffffffe0032ac410
  0xffffffe00326bb20:     0xffffffe00326bb40      0xffffffff81091960
  0xffffffe00326bb30:     0xffffffe00326bb70      0xffffffff80d82800
  0xffffffe00326bb40:     0xffffffe00326bba0      0xffffffff80d7e240
  0xffffffe00326bb50:     0xffffffff82e96070      0xffffffe0032ac410
  0xffffffe00326bb60:     0xffffffe00326bba0      0xffffffff80d7e280
  0xffffffe00326bb70:     0xffffffff82eea0a8      0xffffffff80d7e240
  0xffffffe00326bb80:     0xffffffff82e96070      0x0000000000000000
  0xffffffe00326bb90:     0xffffffe00326bbf0      0xffffffff80d781c0
  0xffffffe00326bba0:     0xddddddddcccccccc      0xffffffe00320b4c8
  0xffffffe00326bbb0:     0xffffffe00323a178      0xc623d14e150a6f00
  0xffffffe00326bbc0:     0xffffffff82ec3d10      0x0000000000000000
  0xffffffe00326bbd0:     0xffffffe003511a80      0xffffffff82e96070

可以看到这个语义空间中，outer是上级函数调用，inner是内层。level和显示的数字是直
接对应的。saved pc就是outer的入口，所以到了outmost就没有saved_pc了。Arglist和
Local是协议中说的堆栈中的位置，在这个上下文中，它们都在栈的最顶端。而且因为
canshu都在寄存器中（没有溢出），所以两者地址相等。

专门看一下bt的过程（我加了注释）：::

  Breakpoint 4, 0xffffffff81440100 in _printk ()
  (gdb) bt
  #0  0xffffffff81440100 in _printk ()          <---------- #0是不需要访问堆栈的，它从PC上直接查地址就可以拿到
  Prologue scan for function starting at 0xffffffff81440100 (limit 0xffffffff81440100) <-- 然后直接看代码跳过前言
  End of prologue at 0xffffffff81440100                                                <-- 我们的情况是没有，还在原来位置上
  Frame base is 0xffffffe00326b620 ($sp + 0x0)                                         <-- 这里才开始看堆栈
  Prologue scan for function starting at 0xffffffff80838040 (limit 0xffffffff808380a4) <-- 然后开始找下个函数，但我不知道怎么找到这个值的
  #1  0xffffffff80838700 in uart_add_one_port ()
  (gdb) x /50xg $sp                                                                    <-- 下面这个堆栈中并没有uart_add_one_port
  0xffffffe00326b620:     0xffffffe00326b730      0x317830204f494d4d
  0xffffffe00326b630:     0x0030303030303030      0x00000000000002e8
  0xffffffe00326b640:     0xffffffff82f24f38      0xffffffff82f24f38 serial8250_ports, serial8250_ports
  0xffffffe00326b650:     0x0000000000000000      0xffffffff82f24f38 , serial8250_ports
  0xffffffe00326b660:     0xffffffe00326b6e0      0xc623d14e150a6f00
  0xffffffe00326b670:     0xffffffe0032a9010      0x0000000000000000
  0xffffffe00326b680:     0xffffffff822000e0      0x0000000000000002 ignore_unknown_bootoption,
  0xffffffe00326b690:     0xffffffe00326b730      0xffffffff82eea0a8 , __stack_chk_guard
  0xffffffe00326b6a0:     0xffffffe00326b770      0x00000000000002e8
  0xffffffe00326b6b0:     0xffffffff82ee908c      0xffffffff82f24f38 nr_uarts, serial8250_ports
  0xffffffe00326b6c0:     0xffffffe00350e7a8      0xffffffff82f24f38 , serial8250_ports
  0xffffffe00326b6d0:     0xffffffe00326b720      0xffffffff8083ba00 , serial8250_register_8250_port + 1792
  0xffffffe00326b6e0:     0xffffffe0032a9010      0x0000000000000004
  0xffffffe00326b6f0:     0xffffffe003243580      0xffffffe03fdfb1a0
  0xffffffe00326b700:     0xffffffe0032a9000      0x0000000000000000
  0xffffffe00326b710:     0xffffffe00326bac0      0xffffffff808509c0 , of_platform_serial_probe
  0xffffffe00326b720:     0x00384000dead4ead      0x0000000000000000 SPINLOCK_MAGIC,
  0xffffffe00326b730:     0x0000000010000000      0x00000000100000ff
  0xffffffe00326b740:     0xffffffe03fdfb270      0x0000000000000200
  0xffffffe00326b750:     0x0000000000000000      0x0000000000000000
  0xffffffe00326b760:     0x0000000000000000      0x0000000000000000
  0xffffffe00326b770:     0xdead4ead00000001      0x00000000ffffffff SPINLOCK_MAGIC,

.. note::

   函数和它的limit是一个综合计算的结果，如果有其他调试信息，就从那些调试信息里
   面取，如果没有也可以从minisym里面直接去函数大小。

我先对应一下RV的调用栈结构：::

  +---+ <------------  当前sp
  |   | <-- 这一截应该是下一级的参数
  |   | <----- 局部变量
  |   | <-- 这一截是自己的局部变量
  |   | <-- 最前面应该是callee-save的空间了
  +---+
  |   | 前fp
  +---+
  |   | 前ra
  +---+ <------------ 函数进入前sp  <---- 运行时fp
  |   | 多出来的第一个参数
  +---+
  |   | 多出来的第二个参数
  +---+

换言之，当我们在一个frame里面的时候，跳过前言以后，sp就是不动的，局部变量用fp来
寻址，前言后记用sp寻址。多余的入参用fp寻址，sp和fp定义了整个堆栈的大小。（fp在
RV中又叫s0）

如果使用优化编译，那么就会变成这样：::

  +---+ <------------  当前sp
  |   |
  |   | <----- 局部变量
  |   |
  +---+
  |   | 前ra <--- 这个也不一定存在，取决于本函数还有没有调用其他函数。
  +---+ <------------ 函数进入前sp
  |   | 多出来的第一个参数
  +---+ ...

这种场景没有fp，fp就在上一层的位置上，只有sp是可信的，所有寻址都用sp来完成，包
括函数入口参数。返回地址也不一定在栈底，如果不需要调用其他函数，ra干脆就不保存，
保持在ra寄存器里面。

按这种理解再看一个分析：::

  (gdb) bt
  #0  0xffffffff81440100 in _printk ()
  #1  0xffffffff80099c60 in register_console ()
  (gdb) p $sp
  $11 = (void *) 0xffffffe00326b5f0
  (gdb) p $fp
  $12 = (void *) 0xffffffe00326b620
  (gdb) p $fp - $sp
  $14 = 48
  (gdb) x /8xg $sp
  0xffffffe00326b5f0:     0xffffffff82eea0a8      0x8000000000006022
  0xffffffe00326b600:     0xffffffe00321f000      0xffffffff82f24f38
  0xffffffe00326b610:     0xffffffe00326b6e0      0xffffffff80838b60
  0xffffffe00326b620:     0xffffffe00326b730      0x317830204f494d4d

fp看来是存在的，这个堆栈结构，按理说fp-8就是下一个函数的返回地址，这里我们得到
0xffffffff80838b60：uart_add_one_port + 2848，但gdb的分析结果是register_console，
我弄不清楚这是拿来的，然后看ra的值，发现是<not saved>，这就怪了，怎么就偏偏这个
寄存器没有呢？这个要先查一下：

从info_registers_commnad()->registers_info()看过去，首先发现，这个东西是基于当
前所在的frame来决定的取值的，所以并没有说直接去目标端拿某个寄存器的当前值的说法。
这个靠target直接同步过来，然后根据里面的value决定显示什么，如果显示<not saved>，
那可能是目标端根本没有送过来。

那就跟踪一下gdbserver协议了。有如下观察：

1. 并不是每次都没有ra，只是切换到特定的frame才没有ra
2. 跟踪到的协议上：33个通用寄存器是用g一次取的，其他是用p指令一个个取的。ra属于前者。
3. 做了一次frame然后要求寄存器的时候，server上的行为是先用m取了2个字节，然后再取14个
   字节。这个行为太熟悉了，应该是个指令解码：::

      gdbstub_io_command Received: mffffffff82208b40,2 <-- 读2个字节
      gdbstub_io_reply Sent: 8b1c
      gdbstub_io_got_ack Got ACK
      gdbstub_io_command Received: mffffffff82208b42,e <- 再读14个字节 （读了一个头）
      gdbstub_io_reply Sent: 056c006c60000481000006030c00

这里返回8b1c，1c很明显就是块头的标志，之后就没有了，所以这个错误很可能是分析了
某条指令以后，认为没有什么可分析的，放弃了。这里读的位置和这句话对得上：::

  #0  0xffffffff81440100 in _printk ()
  Prologue scan for function starting at 0xffffffff81440100 (limit 0xffffffff81440100)
  End of prologue at 0xffffffff81440100
  Frame base is 0xffffffff82e03f60 ($sp + 0x0)
  Prologue scan for function starting at 0xffffffff82208b40 (limit 0xffffffff82208ba4)
  #1  0xffffffff82208c80 in mem_init ()

它去访问的地址恰好就是它发现的frame认为的的地址，看完这个地址它就没有再找target
要数据了，所以要查这个问题，关键要看看gdb这段流程是怎么工作的。晚上有Midgard的
讲座，明天再来分析这个吧。

fence.i的问题
-------------

还有个问题补充记录一下，现在的fence.i定义是有问题的，这个东西本身在块中间，功能
却是flush code cache，flush完了，块是不是存在都有问题，语义没法自恰，我修改成终止
块以后生效了，明天没人反对就commit。

20220519
========

昨晚回去分析了一下RV的unwind代码，有如下认识：

1. RV在没有Dwarfs的格式的时候，是靠Prologue进行堆栈分析的，这个核心函数是
   riscv_scan_prologue。它用于两种契机，一种是设断点的时候用来跳过堆栈准备
   代码，另一种是unwind的时候用来获得prologue阶段设置的堆栈数据。

2. riscv_scan_prologue的核心原理有两个：

   1. 一个是一种称为pv, prologue value，的数据结构，呈现为pv_t和pv_area，前者跟
      踪一个寄存器的数据，后者跟踪一个寄存器的堆栈列表。pv_t主要就是寄存器的id，
      类型和内存地址，pv_area是一个pv_t的列表，控制整个堆栈中连续多帧的数据变化
      过程。

   2. 另一个是从pc开始一条条指令分析过去，分析堆栈的状态的一个循环。昨天看到的
      读header的那个过程，其实就是这个循环的第一步，但因为这条指令不属于任何堆栈
      设置代码，所以分析过程马上就停了，这就是为什么堆栈分析总是不对。

而之前的assert，初步看代码也是用dwarf分析这个结构不对，所以整个假设就错了，我先
解决这个没有dwarf的过程。

详细看RV的分析策略：

 +---------------------------------------+------------------------+
 | 指令                                  | 处理方案               |
 +=======================================+========================+
 | * add fp, fp, 0                       | 不处理（也不退出）     |
 +---------------------------------------+------------------------+
 | * addi/addiw sp, sp, imm              | 更新对应寄存器pv数值   |
 | * addi fp, sp, size                   |                        |
 | * add/addw fp, sp, 0                  |                        |
 | * auipc imm：pc加imm                  |                        |
 | * lui reg, imm：reg加imm              |                        |
 | * addi reg1, reg2                     |                        |
 +---------------------------------------+------------------------+
 | * sw/sd reg, reg, offset(sp)          | offset(sp)->reg入栈    |
 +---------------------------------------+------------------------+

还在这个集合内，就是prologue，否则就不是。分析完以后，入过栈的寄存器就从堆栈指
针读，否则就从实体寄存器读，然后根据更新过的值进行还原。

这个本质是拿一个有可能成为prologue的指令子集，然后部分执行它们的行为，把执行的
结果更新到寄存器集中。之后我们就可以用这个分析结果来还原寄存器的原始值。

为了分析我们的head，那就要看看我们准备堆栈的结构是什么样的，下面是一个分析：

1. 链接前，块头几乎都是concat的，而且我们现在的工具定位不到body，非常难看，其实
   这个是有必要尽快改进的。
2. 大部分简单的函数，只有一个块，所有堆栈准备都在块内

下面是一个例子：::

  int abc(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
	  volatile int x;
	  x = a+b+c+d+e+f+g+h+i+j;
	  x++;
	  return x;
  }

  这是-O0的结果：
  0000000000010970 <abc>:
     10970:	00000504 20000104 00003000 ff89200b 	bstart	b.std, bnext.fall, battr:none, bget:0x00000504, bset:0x20000104, ptr:0x10202 size:0xc, bnext:0x10980,
        10202:	f809                	const	0xffffffffffffffc0 # -64
        10204:	0212                	get	sp
        10206:	0400                	add	t#1,t#2
        10208:	0292                	set	sp,t#1
        1020a:	0812                	get	s0
        1020c:	40ef                	sd	t#1,[t#3,56]
        1020e:	0809                	const	0x40 # 64
        10210:	8000                	add	t#5,t#1
        10212:	0892                	set	s0,t#1
        10214:	0a12                	get	a0
        10216:	0001                	addi	t#1,0
        10218:	1d92                	set	t4,t#1
     10980:	00003800 10000440 00002400 ff89a00b 	bstart	b.std, bnext.fall, battr:none, bget:0x00003800, bset:0x10000440, ptr:0x1021a size:0x9, bnext:0x10990,
        1021a:	0b12                	get	a1
        1021c:	0001                	addi	t#1,0
        1021e:	1c92                	set	t3,t#1
        10220:	0c12                	get	a2
        10222:	0001                	addi	t#1,0
        10224:	0692                	set	t1,t#1
        10226:	0d12                	get	a3
        10228:	0001                	addi	t#1,0
        1022a:	0a92                	set	a0,t#1
     10990:	0001c000 00003800 00002400 ff89c00b 	bstart	b.std, bnext.fall, battr:none, bget:0x0001c000, bset:0x00003800, ptr:0x1022c size:0x9, bnext:0x109a0,
        1021a:	0b12                	get	a1
        1021c:	0001                	addi	t#1,0
        1021e:	1c92                	set	t3,t#1
        10220:	0c12                	get	a2
        10222:	0001                	addi	t#1,0
        10224:	0692                	set	t1,t#1
        10226:	0d12                	get	a3
        10228:	0001                	addi	t#1,0
        1022a:	0a92                	set	a0,t#1
     109a0:	30023d46 0000c504 00020400 ff89f80b 	bstart	b.std, bnext.ret, battr:none, bget:0x30023d46, bset:0x0000c504, ptr:0x1023e size:0x81, bnext:------,
        1023e:	1112                	get	a7
        10240:	0001                	addi	t#1,0
        10242:	1d12                	get	t4
        10244:	0001                	addi	t#1,0
        10246:	0812                	get	s0
        10248:	06ee                	sw	t#2,[t#1,-36]
        1024a:	1c12                	get	t3
        1024c:	0001                	addi	t#1,0
        1024e:	62ce                	sw	t#1,[t#4,-40]
        10250:	e001                	addi	t#8,0
        10252:	0612                	get	t1
        10254:	0001                	addi	t#1,0
        10256:	e2ae                	sw	t#1,[t#8,-44]
        10258:	0a12                	get	a0
        1025a:	0001                	addi	t#1,0
        1025c:	0812                	get	s0
        1025e:	068e                	sw	t#2,[t#1,-48]
        10260:	e001                	addi	t#8,0
        10262:	0b12                	get	a1
        10264:	0001                	addi	t#1,0
        10266:	826e                	sw	t#1,[t#5,-52]
        10268:	0c12                	get	a2
        1026a:	0001                	addi	t#1,0
        1026c:	e24e                	sw	t#1,[t#8,-56]
        1026e:	0d12                	get	a3
        10270:	e001                	addi	t#8,0
        10272:	2001                	addi	t#2,0
        10274:	0812                	get	s0
        10276:	062e                	sw	t#2,[t#1,-60]
        10278:	6001                	addi	t#4,0
        1027a:	420e                	sw	t#1,[t#3,-64]
        1027c:	7744                	lw	[t#4,-36]
        1027e:	9644                	lw	[t#5,-40]
        10280:	2000                	add	t#2,t#1
        10282:	1fe1                	slli	t#1,32
        10284:	1fc1                	srai	t#1,32
        10286:	0001                	addi	t#1,0
        10288:	1fe1                	slli	t#1,32
        1028a:	1fc1                	srai	t#1,32
        1028c:	0812                	get	s0
        1028e:	1544                	lw	[t#1,-44]
        10290:	0800                	add	t#1,t#3
        10292:	1fe1                	slli	t#1,32
        10294:	1fc1                	srai	t#1,32
        10296:	0001                	addi	t#1,0
        10298:	1fe1                	slli	t#1,32
        1029a:	1fc1                	srai	t#1,32
        1029c:	f444                	lw	[t#8,-48]
        1029e:	0400                	add	t#1,t#2
        102a0:	1fe1                	slli	t#1,32
        102a2:	1fc1                	srai	t#1,32
        102a4:	0001                	addi	t#1,0
        102a6:	1fe1                	slli	t#1,32
        102a8:	1fc1                	srai	t#1,32
        102aa:	0812                	get	s0
        102ac:	1344                	lw	[t#1,-52]
        102ae:	0800                	add	t#1,t#3
        102b0:	1fe1                	slli	t#1,32
        102b2:	1fc1                	srai	t#1,32
        102b4:	0001                	addi	t#1,0
        102b6:	1fe1                	slli	t#1,32
        102b8:	1fc1                	srai	t#1,32
        102ba:	f244                	lw	[t#8,-56]
        102bc:	0400                	add	t#1,t#2
        102be:	1fe1                	slli	t#1,32
        102c0:	1fc1                	srai	t#1,32
        102c2:	0001                	addi	t#1,0
        102c4:	1fe1                	slli	t#1,32
        102c6:	1fc1                	srai	t#1,32
        102c8:	0812                	get	s0
        102ca:	1144                	lw	[t#1,-60]
        102cc:	0800                	add	t#1,t#3
        102ce:	1fe1                	slli	t#1,32
        102d0:	1fc1                	srai	t#1,32
        102d2:	0001                	addi	t#1,0
        102d4:	1fe1                	slli	t#1,32
        102d6:	1fc1                	srai	t#1,32
        102d8:	f044                	lw	[t#8,-64]
        102da:	0400                	add	t#1,t#2
        102dc:	1fe1                	slli	t#1,32
        102de:	1fc1                	srai	t#1,32
        102e0:	0001                	addi	t#1,0
        102e2:	1fe1                	slli	t#1,32
        102e4:	1fc1                	srai	t#1,32
        102e6:	0812                	get	s0
        102e8:	0044                	lw	[t#1,0]
        102ea:	0800                	add	t#1,t#3
        102ec:	1fe1                	slli	t#1,32
        102ee:	1fc1                	srai	t#1,32
        102f0:	0001                	addi	t#1,0
        102f2:	1fe1                	slli	t#1,32
        102f4:	1fc1                	srai	t#1,32
        102f6:	e244                	lw	[t#8,8]
        102f8:	0e92                	set	a4,t#1
        102fa:	2800                	add	t#2,t#3
        102fc:	1fe1                	slli	t#1,32
        102fe:	1fc1                	srai	t#1,32
        10300:	0001                	addi	t#1,0
        10302:	1fe1                	slli	t#1,32
        10304:	1fc1                	srai	t#1,32
        10306:	0812                	get	s0
        10308:	076e                	sw	t#2,[t#1,-20]
        1030a:	3b44                	lw	[t#2,-20]
        1030c:	0001                	addi	t#1,0
        1030e:	1fe1                	slli	t#1,32
        10310:	1fc1                	srai	t#1,32
        10312:	0101                	addi	t#1,1
        10314:	1fe1                	slli	t#1,32
        10316:	1fc1                	srai	t#1,32
        10318:	0001                	addi	t#1,0
        1031a:	1fe1                	slli	t#1,32
        1031c:	1fc1                	srai	t#1,32
        1031e:	0812                	get	s0
        10320:	076e                	sw	t#2,[t#1,-20]
        10322:	3b44                	lw	[t#2,-20]
        10324:	0001                	addi	t#1,0
        10326:	1fe1                	slli	t#1,32
        10328:	1fc1                	srai	t#1,32
        1032a:	0f92                	set	a5,t#1
        1032c:	2001                	addi	t#2,0
        1032e:	0a92                	set	a0,t#1
        10330:	0212                	get	sp
        10332:	0764                	ld	[t#1,56]
        10334:	0892                	set	s0,t#1
        10336:	0809                	const	0x40 # 64
        10338:	6000                	add	t#4,t#1
        1033a:	0292                	set	sp,t#1
        1033c:	0112                	get	ra
        1033e:	00b2                	setbpc	t#1

  这是-O2的结果：
  0000000000010880 <abc>:
     10880:	0003fc06 00008404 0000f000 ff98380b 	bstart	b.std, bnext.ret, battr:none, bget:0x0003fc06, bset:0x00008404, ptr:0x10202 size:0x3c, bnext:------,
        10202:	0b12                	get	a1
        10204:	0a12                	get	a0
        10206:	0400                	add	t#1,t#2
        10208:	1fe1                	slli	t#1,32
        1020a:	1fc1                	srai	t#1,32
        1020c:	0c12                	get	a2
        1020e:	2000                	add	t#2,t#1
        10210:	1fe1                	slli	t#1,32
        10212:	1fc1                	srai	t#1,32
        10214:	0d12                	get	a3
        10216:	2000                	add	t#2,t#1
        10218:	1fe1                	slli	t#1,32
        1021a:	1fc1                	srai	t#1,32
        1021c:	0212                	get	sp
        1021e:	1021                	subi	t#1,16
        10220:	0e12                	get	a4
        10222:	6000                	add	t#4,t#1
        10224:	1fe1                	slli	t#1,32
        10226:	1fc1                	srai	t#1,32
        10228:	0f12                	get	a5
        1022a:	2000                	add	t#2,t#1
        1022c:	1fe1                	slli	t#1,32
        1022e:	e001                	addi	t#8,0
        10230:	3fc1                	srai	t#2,32
        10232:	2444                	lw	[t#2,16]
        10234:	1012                	get	a6
        10236:	4000                	add	t#3,t#1
        10238:	1fe1                	slli	t#1,32
        1023a:	1fc1                	srai	t#1,32
        1023c:	1112                	get	a7
        1023e:	e001                	addi	t#8,0
        10240:	4400                	add	t#3,t#2
        10242:	e001                	addi	t#8,0
        10244:	3fe1                	slli	t#2,32
        10246:	1fc1                	srai	t#1,32
        10248:	0800                	add	t#1,t#3
        1024a:	1fe1                	slli	t#1,32
        1024c:	1fc1                	srai	t#1,32
        1024e:	e001                	addi	t#8,0
        10250:	0644                	lw	[t#1,24]
        10252:	4000                	add	t#3,t#1
        10254:	1fe1                	slli	t#1,32
        10256:	1fc1                	srai	t#1,32
        10258:	806e                	sw	t#1,[t#5,12]
        1025a:	a344                	lw	[t#6,12]
        1025c:	0101                	addi	t#1,1
        1025e:	e001                	addi	t#8,0
        10260:	3fe1                	slli	t#2,32
        10262:	1fc1                	srai	t#1,32
        10264:	0f92                	set	a5,t#1
        10266:	646e                	sw	t#2,[t#4,12]
        10268:	8344                	lw	[t#5,12]
        1026a:	b001                	addi	t#6,16
        1026c:	0292                	set	sp,t#1
        1026e:	4001                	addi	t#3,0
        10270:	1fe1                	slli	t#1,32
        10272:	1fc1                	srai	t#1,32
        10274:	0a92                	set	a0,t#1
        10276:	0112                	get	ra
        10278:	00b2                	setbpc	t#1

这和一般RV的代码类似，没有优化就是正经准备堆栈，我们基本上可以假设第一个块就是
Prologue，但有优化就很难说了，我们甚至不知道哪里开始处理堆栈。

所以这里我们可以分两种情况，一种是不考虑在body里面可以插断点，这样我们可以用这个规律：

1. 如果函数只有一个块，那我们认为没有prologue，因为这相当于函数没有执行体了。就
   算这个函数有准备堆栈，我们也切不开它。
2. 如果函数第一个块带跳转，我们也认为没有prologue，因为这根本有可能是直接返回。
3. 此外的情况，我们认为第一个块就是prologue，就算它包含额外的内容，反正我也切不开。
   而还有些prologue的内容在第二个块中，我赌这种情况不存在。
   这种情况下，我也不用考虑终止点，我直接把整个body全部执行一遍更新寄存器就可以了。这
   中间如果用到T寄存器，放一组额外的pv来保存好了（为此我还需要用指针跟踪这T寄存
   器的索引）。
4. 对于3，也可以维持一个prologue指令列表，不在范围内的当做看不见好了。先用这个列表。

如果我们考虑在body里面也可以插断点，我们就需要解决这几个问题：

1. 断点来了，我们还要知道这个body是哪个header的，如果这个body和别人共享header，
   我们还要排除这个header。header可以通过读bpc获得，然后再查表判断这个header在
   不在我们的断点列表中。反正无论如何，我们这个平台判断一个指令的位置，同时需要
   两个指针才行。

2. 前一个要求，用于跳过prologue的时候同样成立。我们不能让pc停在body上就算了，我
   们其实是把断点设在一个(bpc, tpc)的组合上。这样gdb很多逻辑都没法成立，因为它
   的概念中"执行到哪里"就是一个字节流的一个位置，用一个"地址"表示，而不是用"两
   个地址表示"。

3. 如果是原子块，断点不能设进去

所以我先不弄这个块内的行为，搞定整个块再说，这样整个逻辑是动态执行一个块的行为
怎么做。这部分工作也是DWARFS不能取代的，就算都靠DWARFS描述，你其实也要决定一个
prologue的位置，否则断点不知道应该设置在哪里，也不知道怎么决定一个设置好堆栈还
是没有设置好堆栈的代码分界线。

按这个策略写完代码。下面是调试记录：

1. gdb很多地方写了exception保护，你明明代码踩内存了，它都可以毫无错误地终止命令，
   这个在调试的时候要消息，你程序跳过了一部分调用你都不知道。

2. 发现一个错误如下：::

     0xffffffff8150ae9a in __do_return__.bstop ()
     (gdb) bt
     #0  0xffffffff8150ae9a in __do_return__.bstop ()
     #1  0xffffffff80072aa0 in do_idle ()

   注意堆栈的第一个函数的入口，是qemu反馈回来的当前pc，这个值还停留在tpc上，我们
   有必要调整它为bpc。

3. 跟踪到的堆栈，发现编译器现在都没有在堆栈中保存ra，至少准备阶段没有做这个，下来
   要写文档并和相关同事对齐相关的堆栈准备策略。

暂时这样吧，现在调试看到的效果是这样的：::

  (gdb) bt
  #0  0xffffffff8150a432 in __do_return__.bstop ()
  Prologue scan for function starting at 0xffffffff8146f81a (limit 0xffffffff8146f87e)
  End of prologue at 0xffffffff8146f81a
  Frame base is 0xffffffff82e03eb0 ($sp + 0x0)
  Prologue scan for function starting at 0xffffffff800e47c0 (limit 0xffffffff800e4824)
  decode head: br_type=7, b_offset=12d84, b_size=2087
  decode concat head: br_type=1, b_offset=10562948
  unwind body: ffffffff8150a2c8: 1, len=2 CONST
  unwind body: ffffffff8150a2ca: 2, len=2 GET
  unwind body: ffffffff8150a2cc: 4, len=2 ADD
  unwind body: ffffffff8150a2ce: 3, len=2 SET
  unwind body: ffffffff8150a2d0: 2, len=2 GET
  unwind body: ffffffff8150a2d2: 5, len=2 unknown minsn 5, end prologue scanning
  End of prologue at 0xffffffff800e47d0
  Frame base is 0xffffffff82e03eb0 ($sp + 0x0)
  #1  0xffffffff800e4a00 in tick_nohz_next_event.constprop ()
  Backtrace stopped: frame did not save the PC

没有保存PC还是没法一层层走上去。


今天未完成的任务
----------------

1. LARM中补充lr/sc不能用于原子块，重新检查lr/sc的语义。
2. LARM中补充说明TPC可能因为重排导致和实际的执行流对不上。
3. LARM中对store指令的偏移的使用方法描述不对
4. LARM中store指令两个T寄存器的用途反了

20220521
========

昨天的跟踪，回去想了想，发现我中止得太早了，可能有我没有想到的情形，所以今天补
一下译码，放大跟踪范围再看看。然后找到了不少译码的错误，包括LARM的描述错误，我
补充到昨天的未完成任务中了，下周统一修改。

这样以后，在做unwind就正常了，我这里看来占时不需要编译器的同事修改什么东西的。

跟踪信息像下面这样的：::

  (gdb) bt
  #0  0xffffffff814d21e6 in __do_return__.bstop ()
  Prologue scan for function starting at 0xffffffff8146f81a (limit 0xffffffff8146f87e)
  End of prologue at 0xffffffff8146f81a
  Frame base is 0xffffffff82e03e40 ($sp + 0x0)
  Prologue scan for function starting at 0xffffffff81465810 (limit 0xffffffff81465830)
  decode head: br_type=7, b_offset=3600f, b_size=3104
  decode concat head: br_type=2, b_offset=221199
  unwind minsn ffffffff814d182e: CONST -32 => tregs[0]=pv(-32)
  unwind minsn ffffffff814d1830: GET r2 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff814d1832: ADD t#1, t#2 => tregs[2]=pv(r2+(-32))
  unwind minsn ffffffff814d1834: SET t#1, r2 => regs[2]=pv(r2+(-32))
  unwind minsn ffffffff814d1836: GET r8 => tregs[4]=pv(r8+(0))
  unwind minsn ffffffff814d1838: SD t#1, 16(t#3) => SD r8, 16(r2) tregs[5]=pv(r2+(-16)) SAVED
  unwind minsn ffffffff814d183a: GET r1 => tregs[6]=pv(r1+(0))
  unwind minsn ffffffff814d183c: SD t#1, 24(t#5) => SD r1, 24(r2) tregs[7]=pv(r2+(-8)) SAVED
  unwind minsn ffffffff814d183e: CONST 32 => tregs[0]=pv(32)
  unwind minsn ffffffff814d1840: ADD t#7, t#1 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff814d1842: SET t#1, r8 => regs[8]=pv(r2+(0))
  unwind minsn ffffffff814d1844: unknown(a) => STOP SCAN
  End of prologue at 0xffffffff81465820
  Register $ra at stack offset -8
  Register $fp at stack offset -16
  Frame base is 0xffffffff82e03e60 ($fp + 0x0)
  #1  0xffffffff81465830 in _raw_spin_unlock ()
  Prologue scan for function starting at 0xffffffff81465810 (limit 0xffffffff81465830)
  decode head: br_type=7, b_offset=3600f, b_size=3104
  decode concat head: br_type=2, b_offset=221199
  unwind minsn ffffffff814d182e: CONST -32 => tregs[0]=pv(-32)
  unwind minsn ffffffff814d1830: GET r2 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff814d1832: ADD t#1, t#2 => tregs[2]=pv(r2+(-32))
  unwind minsn ffffffff814d1834: SET t#1, r2 => regs[2]=pv(r2+(-32))
  unwind minsn ffffffff814d1836: GET r8 => tregs[4]=pv(r8+(0))
  unwind minsn ffffffff814d1838: SD t#1, 16(t#3) => SD r8, 16(r2) tregs[5]=pv(r2+(-16)) SAVED
  unwind minsn ffffffff814d183a: GET r1 => tregs[6]=pv(r1+(0))
  unwind minsn ffffffff814d183c: SD t#1, 24(t#5) => SD r1, 24(r2) tregs[7]=pv(r2+(-8)) SAVED
  unwind minsn ffffffff814d183e: CONST 32 => tregs[0]=pv(32)
  unwind minsn ffffffff814d1840: ADD t#7, t#1 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff814d1842: SET t#1, r8 => regs[8]=pv(r2+(0))
  unwind minsn ffffffff814d1844: unknown(a) => STOP SCAN
  End of prologue at 0xffffffff81465820
  Register $ra at stack offset -8
  Register $fp at stack offset -16
  Frame base is 0xffffffff82e03e80 ($fp + 0x0)
  #2  0xffffffff81465830 in _raw_spin_unlock ()
  Prologue scan for function starting at 0xffffffff800c76c0 (limit 0xffffffff800c7724)
  decode head: br_type=7, b_offset=169bd, b_size=2081
  decode concat head: br_type=2, b_offset=10578365
  unwind minsn ffffffff814f4a3a: CONST -48 => tregs[0]=pv(-48)
  unwind minsn ffffffff814f4a3c: GET r2 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff814f4a3e: ADD t#1, t#2 => tregs[2]=pv(r2+(-48))
  unwind minsn ffffffff814f4a40: SET t#1, r2 => regs[2]=pv(r2+(-48))
  unwind minsn ffffffff814f4a42: GET r8 => tregs[4]=pv(r8+(0))
  unwind minsn ffffffff814f4a44: SD t#1, 32(t#3) => SD r8, 32(r2) tregs[5]=pv(r2+(-16)) SAVED
  unwind minsn ffffffff814f4a46: GET r9 => tregs[6]=pv(r9+(0))
  unwind minsn ffffffff814f4a48: SD t#1, 24(t#5) => SD r9, 24(r2) tregs[7]=pv(r2+(-24)) SAVED
  unwind minsn ffffffff814f4a4a: GET r18 => tregs[0]=pv(r18+(0))
  unwind minsn ffffffff814f4a4c: SD t#1, 16(t#7) => SD r18, 16(r2) tregs[1]=pv(r2+(-32)) SAVED
  unwind minsn ffffffff814f4a4e: ADDI t#8, 0 => tregs[2]=pv(r2+(-48))
  unwind minsn ffffffff814f4a50: GET r19 => tregs[3]=pv(r19+(0))
  unwind minsn ffffffff814f4a52: SD t#1, 8(t#2) => SD r19, 8(r2) tregs[4]=pv(r2+(-40)) SAVED
  unwind minsn ffffffff814f4a54: GET r1 => tregs[5]=pv(r1+(0))
  unwind minsn ffffffff814f4a56: SD t#1, 40(t#4) => SD r1, 40(r2) tregs[6]=pv(r2+(-8)) SAVED
  unwind minsn ffffffff814f4a58: CONST 48 => tregs[7]=pv(48)
  unwind minsn ffffffff814f4a5a: ADD t#6, t#1 => tregs[0]=pv(r2+(0))
  unwind minsn ffffffff814f4a5c: SET t#1, r8 => regs[8]=pv(r2+(0))
  unwind minsn ffffffff814f4a5e: GET r10 => tregs[2]=pv(r10+(0))
  unwind minsn ffffffff814f4a60: ADDI t#1, 0 => tregs[3]=pv(r10+(0))
  unwind minsn ffffffff814f4a62: SET t#1, r9 => regs[9]=pv(r10+(0))
  unwind minsn ffffffff814f4a64: unknown(a) => STOP SCAN
  End of prologue at 0xffffffff800c76d0
  Register $ra at stack offset -8
  Register $fp at stack offset -16
  Register $s1 at stack offset -24
  Register $s2 at stack offset -32
  Register $s3 at stack offset -40
  Frame base is 0xffffffff82e03eb0 ($fp + 0x0)
  #3  0xffffffff800c77a0 in get_next_timer_interrupt ()
  Prologue scan for function starting at 0xffffffff800e47c0 (limit 0xffffffff800e4824)
  decode head: br_type=7, b_offset=12d84, b_size=2087
  decode concat head: br_type=1, b_offset=10562948
  unwind minsn ffffffff8150a2c8: CONST -48 => tregs[0]=pv(-48)
  unwind minsn ffffffff8150a2ca: GET r2 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff8150a2cc: ADD t#1, t#2 => tregs[2]=pv(r2+(-48))
  unwind minsn ffffffff8150a2ce: SET t#1, r2 => regs[2]=pv(r2+(-48))
  unwind minsn ffffffff8150a2d0: GET r8 => tregs[4]=pv(r8+(0))
  unwind minsn ffffffff8150a2d2: SD t#1, 32(t#3) => SD r8, 32(r2) tregs[5]=pv(r2+(-16)) SAVED
  unwind minsn ffffffff8150a2d4: GET r1 => tregs[6]=pv(r1+(0))
  unwind minsn ffffffff8150a2d6: SD t#1, 40(t#5) => SD r1, 40(r2) tregs[7]=pv(r2+(-8)) SAVED
  unwind minsn ffffffff8150a2d8: GET r9 => tregs[0]=pv(r9+(0))
  unwind minsn ffffffff8150a2da: SD t#1, 24(t#7) => SD r9, 24(r2) tregs[1]=pv(r2+(-24)) SAVED
  unwind minsn ffffffff8150a2dc: ADDI t#8, 0 => tregs[2]=pv(r2+(-48))
  unwind minsn ffffffff8150a2de: GET r18 => tregs[3]=pv(r18+(0))
  unwind minsn ffffffff8150a2e0: SD t#1, 16(t#2) => SD r18, 16(r2) tregs[4]=pv(r2+(-32)) SAVED
  unwind minsn ffffffff8150a2e2: GET r19 => tregs[5]=pv(r19+(0))
  unwind minsn ffffffff8150a2e4: SD t#1, 8(t#4) => SD r19, 8(r2) tregs[6]=pv(r2+(-40)) SAVED
  unwind minsn ffffffff8150a2e6: CONST 48 => tregs[7]=pv(48)
  unwind minsn ffffffff8150a2e8: ADD t#6, t#1 => tregs[0]=pv(r2+(0))
  unwind minsn ffffffff8150a2ea: SET t#1, r8 => regs[8]=pv(r2+(0))
  unwind minsn ffffffff8150a2ec: unknown(a) => STOP SCAN
  End of prologue at 0xffffffff800e47d0
  Register $ra at stack offset -8
  Register $fp at stack offset -16
  Register $s1 at stack offset -24
  Register $s2 at stack offset -32
  Register $s3 at stack offset -40
  Frame base is 0xffffffff82e03ee0 ($fp + 0x0)
  #4  0xffffffff800e4a00 in tick_nohz_next_event.constprop ()
  Prologue scan for function starting at 0xffffffff800e5780 (limit 0xffffffff800e57e4)
  decode head: br_type=7, b_offset=12ab7, b_size=2087
  decode concat head: br_type=3, b_offset=10562231
  unwind minsn ffffffff8150acee: CONST -80 => tregs[0]=pv(-80)
  unwind minsn ffffffff8150acf0: GET r2 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff8150acf2: ADD t#1, t#2 => tregs[2]=pv(r2+(-80))
  unwind minsn ffffffff8150acf4: SET t#1, r2 => regs[2]=pv(r2+(-80))
  unwind minsn ffffffff8150acf6: GET r8 => tregs[4]=pv(r8+(0))
  unwind minsn ffffffff8150acf8: SD t#1, 64(t#3) => SD r8, 64(r2) tregs[5]=pv(r2+(-16)) SAVED
  unwind minsn ffffffff8150acfa: GET r9 => tregs[6]=pv(r9+(0))
  unwind minsn ffffffff8150acfc: SD t#1, 56(t#5) => SD r9, 56(r2) tregs[7]=pv(r2+(-24)) SAVED
  unwind minsn ffffffff8150acfe: GET r1 => tregs[0]=pv(r1+(0))
  unwind minsn ffffffff8150ad00: SD t#1, 72(t#7) => SD r1, 72(r2) tregs[1]=pv(r2+(-8)) SAVED
  unwind minsn ffffffff8150ad02: ADDI t#8, 0 => tregs[2]=pv(r2+(-80))
  unwind minsn ffffffff8150ad04: GET r18 => tregs[3]=pv(r18+(0))
  unwind minsn ffffffff8150ad06: SD t#1, 48(t#2) => SD r18, 48(r2) tregs[4]=pv(r2+(-32)) SAVED
  unwind minsn ffffffff8150ad08: GET r19 => tregs[5]=pv(r19+(0))
  unwind minsn ffffffff8150ad0a: SD t#1, 40(t#4) => SD r19, 40(r2) tregs[6]=pv(r2+(-40)) SAVED
  unwind minsn ffffffff8150ad0c: GET r20 => tregs[7]=pv(r20+(0))
  unwind minsn ffffffff8150ad0e: SD t#1, 32(t#6) => SD r20, 32(r2) tregs[0]=pv(r2+(-48)) SAVED
  unwind minsn ffffffff8150ad10: GET r21 => tregs[1]=pv(r21+(0))
  unwind minsn ffffffff8150ad12: ADDI t#8, 0 => tregs[2]=pv(r2+(-80))
  unwind minsn ffffffff8150ad14: SD t#2, 24(t#1) => SD r21, 24(r2) tregs[3]=pv(r2+(-56)) SAVED
  unwind minsn ffffffff8150ad16: GET r22 => tregs[4]=pv(r22+(0))
  unwind minsn ffffffff8150ad18: SD t#1, 16(t#3) => SD r22, 16(r2) tregs[5]=pv(r2+(-64)) SAVED
  unwind minsn ffffffff8150ad1a: GET r23 => tregs[6]=pv(r23+(0))
  unwind minsn ffffffff8150ad1c: SD t#1, 8(t#5) => SD r23, 8(r2) tregs[7]=pv(r2+(-72)) SAVED
  unwind minsn ffffffff8150ad1e: CONST 80 => tregs[0]=pv(80)
  unwind minsn ffffffff8150ad20: ADD t#7, t#1 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff8150ad22: SET t#1, r8 => regs[8]=pv(r2+(0))
  unwind minsn ffffffff8150ad24: unknown(a) => STOP SCAN
  End of prologue at 0xffffffff800e5790
  Register $ra at stack offset -8
  Register $fp at stack offset -16
  Register $s1 at stack offset -24
  Register $s2 at stack offset -32
  Register $s3 at stack offset -40
  Register $s4 at stack offset -48
  Register $s5 at stack offset -56
  Register $s6 at stack offset -64
  Register $s7 at stack offset -72
  Frame base is 0xffffffff82e03f30 ($fp + 0x0)
  #5  0xffffffff800e5a20 in tick_nohz_idle_stop_tick ()
  Prologue scan for function starting at 0xffffffff80072900 (limit 0xffffffff80072964)
  decode head: br_type=7, b_offset=22c2b, b_size=18
  decode concat head: br_type=2, b_offset=10628139
  unwind minsn ffffffff814b8156: CONST -48 => tregs[0]=pv(-48)
  unwind minsn ffffffff814b8158: GET r2 => tregs[1]=pv(r2+(0))
  unwind minsn ffffffff814b815a: ADD t#1, t#2 => tregs[2]=pv(r2+(-48))
  unwind minsn ffffffff814b815c: SET t#1, r2 => regs[2]=pv(r2+(-48))
  unwind minsn ffffffff814b815e: GET r8 => tregs[4]=pv(r8+(0))
  unwind minsn ffffffff814b8160: SD t#1, 32(t#3) => SD r8, 32(r2) tregs[5]=pv(r2+(-16)) SAVED
  unwind minsn ffffffff814b8162: GET r9 => tregs[6]=pv(r9+(0))
  unwind minsn ffffffff814b8164: SD t#1, 24(t#5) => SD r9, 24(r2) tregs[7]=pv(r2+(-24)) SAVED
  unwind minsn ffffffff814b8166: GET r18 => tregs[0]=pv(r18+(0))
  unwind minsn ffffffff814b8168: SD t#1, 16(t#7) => SD r18, 16(r2) tregs[1]=pv(r2+(-32)) SAVED
  unwind minsn ffffffff814b816a: ADDI t#8, 0 => tregs[2]=pv(r2+(-48))
  unwind minsn ffffffff814b816c: GET r19 => tregs[3]=pv(r19+(0))
  unwind minsn ffffffff814b816e: SD t#1, 8(t#2) => SD r19, 8(r2) tregs[4]=pv(r2+(-40)) SAVED
  unwind minsn ffffffff814b8170: GET r1 => tregs[5]=pv(r1+(0))
  unwind minsn ffffffff814b8172: SD t#1, 40(t#4) => SD r1, 40(r2) tregs[6]=pv(r2+(-8)) SAVED
  unwind minsn ffffffff814b8174: CONST 48 => tregs[7]=pv(48)
  unwind minsn ffffffff814b8176: ADD t#6, t#1 => tregs[0]=pv(r2+(0))
  unwind minsn ffffffff814b8178: SET t#1, r8 => regs[8]=pv(r2+(0))
  End of prologue at 0xffffffff80072910
  Register $ra at stack offset -8
  Register $fp at stack offset -16
  Register $s1 at stack offset -24
  Register $s2 at stack offset -32
  Register $s3 at stack offset -40
  Frame base is 0xffffffff82e03f60 ($fp + 0x0)
  #6  0xffffffff80072aa0 in do_idle ()
  Prologue scan for function starting at 0xffffffff800730c0 (limit 0xffffffff80073100)
  decode head: br_type=7, b_offset=22a0a, b_size=2057
  decode concat head: br_type=2, b_offset=10627594
  unwind minsn ffffffff814b84d4: GET r2 => tregs[0]=pv(r2+(0))
  unwind minsn ffffffff814b84d6: SUBI t#1, 16 => tregs[1]=pv(r2+(-16))
  unwind minsn ffffffff814b84d8: SET t#1, r2 => regs[2]=pv(r2+(-16))
  unwind minsn ffffffff814b84da: GET r8 => tregs[3]=pv(r8+(0))
  unwind minsn ffffffff814b84dc: SD t#1, 0(t#3) => SD r8, 0(r2) tregs[4]=pv(r2+(-16)) SAVED
  unwind minsn ffffffff814b84de: GET r1 => tregs[5]=pv(r1+(0))
  unwind minsn ffffffff814b84e0: SD t#1, 8(t#5) => SD r1, 8(r2) tregs[6]=pv(r2+(-8)) SAVED
  unwind minsn ffffffff814b84e2: ADDI t#6, -16 => tregs[7]=pv(r2+(-32))
  unwind minsn ffffffff814b84e4: SET t#1, r8 => regs[8]=pv(r2+(-32))
  unwind minsn ffffffff814b84e6: GET r2 => tregs[1]=pv(r2+(-16))
  unwind minsn ffffffff814b84e8: SUBI t#1, 16 => tregs[2]=pv(r2+(-32))
  unwind minsn ffffffff814b84ea: SET t#1, r2 => regs[2]=pv(r2+(-32))
  unwind minsn ffffffff814b84ec: GET r8 => tregs[4]=pv(r2+(-32))
  unwind minsn ffffffff814b84ee: SD t#1, 8(t#3) => SD r2, 8(r2) tregs[5]=pv(r2+(-24)) SAVED
  unwind minsn ffffffff814b84f0: ADDI t#4, -16 => tregs[6]=pv(r2+(-48))
  unwind minsn ffffffff814b84f2: SET t#1, r8 => regs[8]=pv(r2+(-48))
  unwind minsn ffffffff814b84f4: GET r10 => tregs[0]=pv(r10+(0))
  unwind minsn ffffffff814b84f6: LD(64) => STOP SCAN
  End of prologue at 0xffffffff800730d0
  Register $ra at stack offset -8
  Register $fp at stack offset -16
  Frame base is 0xffffffff82e03fa0 ($fp + 0x30)
  #7  0xffffffff80073100 in cpu_startup_entry ()
  Prologue scan for function starting at 0x0000000000000000 (limit 0x0000000000000064)
  #8  0xffffffff82400018 in ?? ()

相关代码已经写成patch，发给刘盈盈上传了。

昨晚也分析了DWARF在这个上面的描述，发现也是和这个分析过程几乎是一一对应的，比如
要unwind函数abcd，那么先查找

DW_AT_name=abcd的DW_TAG_subprogram DIE，从它的DW_AT_low/high_pc得到范围，看
DW_AT_decl_file/line的到代码位置，从DW_AT_low_pc上查行号记录，查到下一条NS EP
（End Prologue）记录，得到跳过End Prologue的地址。（取代这里的Prologue扫描步骤）

然后读DW_AT_frame_base得到堆栈位置，从DW_TAG_variable的到局部变量的位置，所以区
别只是靠读代码扫出来还是靠读dwarf描述得到。这是个总能做到的体力活，我觉得这部分
设计可以先放下了，这一步能搞定，那一步也肯定能搞定的。

20220523
========

今天应该写gdb的设计文档了，但看看现在gdb上反汇编的结果，很不好看。花点时间先弄
这个问题。

gdb反汇编是这样的：::

  #0  gdbarch_print_insn (gdbarch=0x555555dbec30, vma=152992, info=0x7fffffffdba0) at gdbarch.c:3265
  #1  0x00005555556b964b in gdb_disassembler::print_insn (branch_delay_insns=0x0, memaddr=0x255a0, this=0x7fffffffdb98) at /home/kenny/work/linx-qemu-dev/linx-BLK-binutils/gdb/disasm.h:58
  #2  gdb_pretty_print_disassembler::pretty_print_insn (this=0x7fffffffdb60, insn=<optimized out>, flags=...) at disasm.c:285
  #3  0x00005555556b9b65 in dump_insns (gdbarch=<optimized out>, uiout=<optimized out>, low=<optimized out>, high=0x255b0, how_many=-1, flags=..., end_pc=0x0) at disasm.c:311
  #4  0x00005555556bac5c in do_assembly_only (flags=..., how_many=-1, high=0x255b0, low=0x255a0, uiout=0x555555cc73d0, gdbarch=0x555555dbec30) at disasm.c:717
  #5  gdb_disassembly (gdbarch=0x555555dbec30, uiout=0x555555cc73d0, flags=..., how_many=-1, low=0x255a0, high=0x255b0) at disasm.c:832
  #6  0x00005555556636c5 in print_disassembly (gdbarch=gdbarch@entry=0x555555dbec30, name=<optimized out>, low=0x255a0, high=0x255b0, block=0x0, flags=flags@entry=...) at cli/cli-cmds.c:1408
  #7  0x0000555555663979 in disassemble_command (arg=<optimized out>, from_tty=<optimized out>) at cli/cli-cmds.c:1568

linx上没有设置这个函数，所以用的是default_print_insn()->print_insn_riscv()，这
是opcode的代码（opcodes/linx-dis.c）。这个函数提供了一个disassemble_info，里面
提供了read_memory_func可以直接读任何位置的地址。也提供了fprintf_func用于输出结
果。所以，这里一切行为都是可控的。我们可以在这里打印合适的反汇编结果。

但编译器当前的反汇编逻辑是把layer1/2放在同一个空间里面统一编译的，这个逻辑就是
乱的，我暂时不想改这样的代码，给他们提个需求再说。

然后写gdb的设计总结到下午完成了，整体来说没有风险，这个部分就到这里吧。

然后我去弄一下用户态的linx_debug（这里同时指linx_debug指令和-linx_debug opt设置
的断点），看能不能也变成一个断点（过去在用户态是直接退出）。有如下修改：

1. 把exit换成成生成中断异常，一切正常，带-g的时候就会形成断点，不带的时候就会退
   出，完全符合预期。

2. linx_debug断点异常修改到指令执行前，过去只是为了临时停掉省得生成太多日志，多
   执行一点少执行一点无所谓的，现在用作断点，就得精准一些了。

3. 用户态模拟的时候不响应qXfer:features:read:linx-64bit-cpu.xml请求，原因和以前
   一样的，在linx-linux-user.mak里面没有把riscv换成linx，已经修复。

   注：这个处理在gdb_handle_packet()->handle_gen_query()->handle_query_xfer_features()中。
   这里调用get_feature_xml()，查找那个builtin_xml数组，而数组本身是脚本生成的。

20220524
========

中断状态机问题
--------------

昨晚和王州讨论了两句现在的中断处理状态机模型，晚上脑子里建了一个模型，一早回来
写了下来，感觉要调整的地方挺多的。这个要加班加点搞。

和王州讨论了一个状态机，只有两个状态：

.. figure:: 块解码状态机.svg

状态用bpc1/bpc2标识，这两个东西有，就是layer2解码状态。tpc在layer2就是tpc，所以
不需要tpc的信息。

按这个标准检查块解码状态机：

1. layer1切layer2

   1. trans_blk_head()：设置了bpc1/2，从而更改了状态，但没有设置tpc，我们我们认
      为在layer状态上，tpc就是pc，任何时候访问tpc，就用pc代替。所以tpc不存在。

   2. 如果有en，trans_blk_head->gen_helper_blk_do_recovery，
      最终是helper_blk_do_recovery：bpc1/2从块头更新，其他数据恢复，en清除，
      看来逻辑也是好的。

2. layer2切layer1

   1. vld设置的时候没有设置cause。

   2. riscv_cpu_do_interrupt()：bpc1/2有状态就设置bstate和vld，这里没有毛病。再
      次发生的时候，如果无效就清除。但如果是用户态中间打断会如何？这个要分析。
      另外，这里没有清除bpc1/bpc2，相当于状态没有恢复到layer1，需要修改。

我们觉得当前方案最大的破绽是没有M模式的BSTATE，我觉得最好用一个hack的方式，直接
认为m-mode是特殊的，进入m-mode一律不保存BState，bpc1/2清掉，让它在layer1解码，
直到mret回来，这时pc是body里面，但我们状态丢了，如果一概认为是body，也不行，
因为说不定我们用了rv指令，所以这个地方要另外放一个bpc1/2缓存，记住进入m-mode的
译码状态，只要返回，就会恢复这两个寄存器，这时整个译码才能继续下去。


LARM待修改工作
--------------

1. GPR不是prepare阶段写进去的，硬件确定在prepare阶段还是get的时候再取，反正不影
   响 （完成）

2. SBPC的名字修改成CARG，同时要说明它的判断逻辑（特别是和redirect头的用途正交这
   一点）模型。

3. 修改ecall的行为要求，把EN加上去

4. 多个ARG指令产生覆盖，这一点要说明出来

5. BSTATE的优先级状态问题需要明确说出来

6. 王州加了lr/sc的aq/rl语义，但没有详细解释，需要补充。

20220525
========

今天和刘盈盈对齐了一下反汇编的设计，她认为read_memory_func其实只是访问文件中的
内容，不能读到body，我以为真的是这样，打算直接移植qemu的代码算了。

但试了一下，首先发现在gdgarch中直接set_gdbarch_print_insn()是不行的，这个函数自
己不打印（强行打印格式对不上），只是设置参数。而read_memory_func从gdb这边调用，
是会修改成inforior的函数的：::

  #0  target_read_code (memaddr=0xffffffff82201240, myaddr=0x7fffffffda14 "", len=4) at target.c:119
  #1  0x0000555555792241 in linx_print_insn (memaddr=<optimized out>, info=<optimized out>) at linx-tdep.c:3357
  #2  0x00005555556b964b in gdb_disassembler::print_insn (branch_delay_insns=0x0, memaddr=0xffffffff82201240, this=0x7fffffffdbb8)
      at /home/kenny/work/linx-qemu-dev/linx-BLK-binutils/gdb/disasm.h:58
  #3  gdb_pretty_print_disassembler::pretty_print_insn (this=0x7fffffffdb80, insn=<optimized out>, flags=...) at disasm.c:285
  #4  0x00005555556b9b65 in dump_insns (gdbarch=<optimized out>, uiout=<optimized out>, low=<optimized out>, high=0xffffffff82202400, how_many=-1, flags=..., end_pc=0x0) at disasm.c:311
  #5  0x00005555556bac5c in do_assembly_only (flags=..., how_many=-1, high=0xffffffff82202400, low=0xffffffff822011c0, uiout=0x555555cc73d0, gdbarch=0x55555608bcb0) at disasm.c:717
  #6  gdb_disassembly (gdbarch=0x55555608bcb0, uiout=0x555555cc73d0, flags=..., how_many=-1, low=0xffffffff822011c0, high=0xffffffff82202400) at disasm.c:832
  #7  0x00005555556636c5 in print_disassembly (gdbarch=gdbarch@entry=0x55555608bcb0, name=<optimized out>, low=0xffffffff822011c0, high=0xffffffff82202400, block=0x0, flags=flags@entry=...)
      at cli/cli-cmds.c:1408
  #8  0x0000555555663979 in disassemble_command (arg=<optimized out>, from_tty=<optimized out>) at cli/cli-cmds.c:1568

这个功能还是等她们来做。我这里记录一下调研的时候发现的信息：

我这里记录一下调研的时候发现的信息：

1. info.disassembler_options是调用opcode时的参数
2. gdbarch_print_insn其实只是打印反汇编结果，地址是框架打的
3. 这个还不是单纯的打印函数，而且是一个反汇编函数，需要从info里面返回指令的各种
   信息的。但我不知道这是不是必须的。

无论如何，我们那些代码写得很烂，到处warning，128bit的数据直接处理，状态机乱飞，
我真不想动这个代码。留着让编译器的人自己玩吧。

现在调王州修改完的代码，现在的逻辑是进入m-mode不碰block的状态，回来的时候原封不
动继续。这个前提是，M模式一定返回到中断的位置。暂时认为这一点是成立的。

调整后发现问题如下：

1. vld在进入m-mode的时候不能切换，修复
2. 有一种情况没有处理（用bstate.real表示真正使用的寄存器）：

   1. 发生一个s-timer，保存bstate，进入timer_handle，bstate入栈，timer用掉了bstate.real
   2. timer返回，bstate出栈，bstate.en=1
   3. 返回的时候再次检测到中断，bstate恢复为timer_handle遗留的bstate.real，冲掉原始位置的
      bstate，bstate.vld/en=1/1
   4. 中断处理再次返回，用被冲掉的bstate恢复现场，出现错误。

   这里的核心问题是，要不save/recover都用helper在tb里面做，要不都在tb之外做，不
   能一个放一边。

2是关键的问题，我通过sret的时候设置cflag_next_tb CF_NOIRQ强制下一条指令优先执行
（但这有风险，因为这个过程要做取指，而取值会发生异常，这样在没有完成前又掉出来，
就会错）。但我们可以先跟踪一下：::

  ===================> 先有一段正常执行：
   0: 0x7f52ede20840 [0000000000000000/ffffffff802a7ac0/00004201/ff000000] __kmalloc
  Trace-Wed May 25 12:11:01 2022
   0: 0x7f52ede209c0 [0000000000000000/ffffffff8161915e/00004201/ff000000]
  Trace-Wed May 25 12:11:01 2022
   0: 0x7f52edd7cb40 [0000000000000000/ffffffff8021a780/00004201/ff000000] should_failslab
  Trace-Wed May 25 12:11:01 2022
   0: 0x7f52edd7ccc0 [0000000000000000/ffffffff815c3db0/00004201/ff000000]  <----- 最后一个认得的pc应该是个tpc：ffffffff815c3db0
  Stopped execution of TB chain before 0x7f52edd7ccc0 [ffffffff815c3db0]
  ===================> 然后来一个m_timer：
  ------------- CS_OUT(0): Addr(ffffffff815c3db0=>80000520) Priv(1=>3) for `m_timer`
    Body(ffffffff815c3db0-ffffffff815c3dca) Saved
    GPRS:          0000000000000000 ffffffff802a7ae0 ffffffff82e03ed0 ffffffff82ee93a0 ffffffff82e09480 0000000000000040 0000000000000000 0000000000000007 ffffffff82e03f30 ffffffe003201840 ffffffe003201840 0000000000000100 ffffffff82f24010 ffffffff82a16840 000000000000000a 0000000000000000 0000000000000020 ffffffe003600298 0000000000000100 0000000000000900 ffffffff80820e40 ffffffff82eea0a8 ffffffff82eea018 ffffffff82e09160 ffffffff829927c8 000000000000007f 0000000000000000 0000000000000000 0000000000000002 0000000000000402 ffffffff82e825a0 ffffffff82e825c0
    BPC/TPC:       0000000080000520 0000000080000520
  ===================> 一番m-mode代码以后，返回：
  ------------- CS_IN(0) Addr (800005f8=>ffffffff815c3db0) Priv(3=>1)
    Body(ffffffff815c3db0-ffffffff815c3dca) Restored
    GPRS:          0000000000000000 ffffffff802a7ae0 ffffffff82e03ed0 ffffffff82ee93a0 ffffffff82e09480 0000000000000040 0000000000000000 0000000000000007 ffffffff82e03f30 ffffffe003201840 ffffffe003201840 0000000000000100 ffffffff82f24010 ffffffff82a16840 000000000000000a 0000000000000000 0000000000000020 ffffffe003600298 0000000000000100 0000000000000900 ffffffff80820e40 ffffffff82eea0a8 ffffffff82eea018 ffffffff82e09160 ffffffff829927c8 000000000000007f 0000000000000000 0000000000000000 0000000000000002 0000000000000402 ffffffff82e825a0 ffffffff82e825c0
    BPC/TPC:       ffffffff815c3db0 00000000800005f8
  ===================> 返回的时候立即遇上中断：
  ------------- CS_OUT(0): Addr(ffffffff815c3db0=>ffffffff80003180) Priv(1=>1) for `s_timer` <-- 所以，这里的切出地址是前面的tpc，而不是bpc
    GPRS:          0000000000000000 ffffffff802a7ae0 ffffffff82e03ed0 ffffffff82ee93a0 ffffffff82e09480 0000000000000040 0000000000000000 0000000000000007 ffffffff82e03f30 ffffffe003201840 ffffffe003201840 0000000000000100 ffffffff82f24010 ffffffff82a16840 000000000000000a 0000000000000000 0000000000000020 ffffffe003600298 0000000000000100 0000000000000900 ffffffff80820e40 ffffffff82eea0a8 ffffffff82eea018 ffffffff82e09160 ffffffff829927c8 000000000000007f 0000000000000000 0000000000000000 0000000000000002 0000000000000402 ffffffff82e825a0 ffffffff82e825c0
    BPC/TPC:       ffffffff80003180 ffffffff80003180 <-- 这个BPC是对的，因为我们现在要跳转了
    BSTATE.VLD/EN: 1/0
    SBPC:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000
    TREGS:         0000000000000400 0000000000000000 00000000018cfd2a 0000000000000100 0000000000000100 0000000000000900 ffffffe003201840 ffffffe003201840
  然后是一堆的中断向量执行，最后调用了一个__sbi_set_timer_v02，导致了一次M-mode切出：
  ------------- CS_OUT(0): Addr(ffffffff80008900=>80000520) Priv(1=>3) for `supervisor_ecall`
    GPRS:          0000000000000000 ffffffff80008ea0 ffffffff82e03b20 ffffffff82ee93a0 ffffffff82e09480 8000000000000120 0000000000000001 0000000000000007 ffffffff82e03b30 ffffffff82ed6880 000000003b57bf2d 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000054494d45 00000000003d0900 0000000000000000 0000000000000005 8000000000000005 ffffffff82eea018 ffffffff82e09160 ffffffff829927c8 000000000000007f 0000000000000000 0000000000000000 0000000000000002 0000000000000402 ffffffff82e825a0 ffffffff82e825c0
    BPC/TPC:       0000000080000520 0000000080000520
    BSTATE.VLD/EN: 1/0
    SBPC:          ffffffff800088e0
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000
    TREGS:         0000000054494d45 ffffffff82e03b30 0000000000000000 ffffffff82e03b20 0000000000000000 ffffffff82e03b40 fffffffffffffd45 0000000054495000
  然后是一堆看不懂的M-mode代码，再回来：
  ------------- CS_IN(0) Addr (800005f8=>ffffffff80008904) Priv(3=>1)
    GPRS:          0000000000000000 ffffffff80008ea0 ffffffff82e03b20 ffffffff82ee93a0 ffffffff82e09480 8000000000000120 0000000000000001 0000000000000007 ffffffff82e03b30 ffffffff82ed6880 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000054494d45 00000000003d0900 0000000000000000 0000000000000005 8000000000000005 ffffffff82eea018 ffffffff82e09160 ffffffff829927c8 000000000000007f 0000000000000000 0000000000000000 0000000000000002 0000000000000402 ffffffff82e825a0 ffffffff82e825c0
    BPC/TPC:       ffffffff80008904 00000000800005f8
    BSTATE.VLD/EN: 1/0
    SBPC:          ffffffff800088e0
    BSTATE.SGPRS:  0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000
    BSTATE.TREGS:  0000000000000400 0000000000000000 00000000018cfd2a 0000000000000100 0000000000000100 0000000000000900 ffffffe003201840 ffffffe003201840
  然后是内核继续做一堆时钟工作，直到irq_exit，然后有一个很大的块
  IN:
  Priv: 1; Virt: 0
  0xffffffff80003600:  00000004 00000000 ca039c00 6bd55f8b sys_block.concat next:0xffffffff80003620, ptr:0xffffffff8146f354, attr:none, out_reg(), in_reg(sp)
  0xffffffff80003610:  00000014 00007fff 00000000 00000000 sys_block.fall_through
	  0xffffffff8146f354:  0212                  get             sp
	  0xffffffff8146f356:  2509                  const           296                             # 0x128
	  0xffffffff8146f358:  2000                  add             t#2 t#1
	  0xffffffff8146f35a:  0064                  ld              [t#1, 0]
	  0xffffffff8146f35c:  0810005e              sysset          bstate.ext.sbpc, t#1
          ...
  后面还有几个执行，然后回来：
  ------------- CS_IN(0) Addr (ffffffff8146f652=>ffffffff815c3db0) Priv(1=>1)  <-- 这个切回地址是PC，而不是BPC
    GPRS:          0000000000000000 ffffffff802a7ae0 ffffffff82e03ed0 ffffffff82ee93a0 ffffffff82e09480 0000000000000040 0000000000000000 0000000000000007 ffffffff82e03f30 ffffffe003201840 ffffffe003201840 0000000000000100 ffffffff82f24010 ffffffff82a16840 000000000000000a 0000000000000000 0000000000000020 ffffffe003600298 0000000000000100 0000000000000900 ffffffff80820e40 ffffffff82eea0a8 ffffffff82eea018 ffffffff82e09160 ffffffff829927c8 000000000000007f 0000000000000000 0000000000000000 0000000000000002 0000000000000402 ffffffff82e825a0 ffffffff82e825c0
    BPC/TPC:       ffffffff815c3db0 ffffffff8146f652
    BSTATE.VLD/EN: 1/1
    SBPC:          0000000000000000
    BSTATE.SGPRS:  0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000
    BSTATE.TREGS:  0000000000000400 0000000000000000 00000000018cfd2a 0000000000000100 0000000000000100 0000000000000900 ffffffe003201840 ffffffe003201840
  ignore a int ff108001  <-- 这里我要求忽略是生效的：
  ----------------
  IN:
  Priv: 1; Virt: 0
  0xffffffff815c3db0:  0212              slli            tp,tp,4 <-- 但后面根本就不是原来的HEAD了。

  Trace-Wed May 25 12:11:01 2022
   0: 0x7f52ee0d7b40 [0000000000000000/ffffffff815c3db0/00004201/ff108001]
  ----------------
  IN:
  Priv: 1; Virt: 0
  0xffffffff815c3db2:  1021              addi            zero,zero,-24
  0xffffffff815c3db4:  0812              slli            a6,a6,4
  0xffffffff815c3db6:  5001202f          illegal

我找到一个错误点是，s->m模式切换的时候更新了bpc，但把这个删除以后，问题并没有解
决，明天再来吧。

20220525
========

昨晚回家思考了一个状态更新原则：

1. bpc1/2是当前译码模式，表示状态机。无效就是Layer1，有效就是Layer2

2. pc是下一个译码地址

3. bpc是当前处理中的head的地址（仅layer2有效）。

   1. 所以，setbpc的commit不能修改BPC，只有Head译码才能修改bpc
   2. Head译码不成功也不能修改BPC

4. tpc是layer2下的pc。

这样所有寄存器具有唯一语义。任何时候，进入一个可以修改CPU的行动前，都必须保证上
述语义成立。今天按这个来修改，我更新之前的状态机，这样：

.. figure:: 块解码状态机2.svg

然后改代码。

对应一下所有状态切换点的代码位置：

0. 初始化： target/linx/cpu.c:riscv_cpu_reset()

1. tb执行(rv/minsn)：tb翻译的时候已经决定形态

2. tb执行（head）：target/linx/insn_trans/trans_block.c.inc:trans_blk_head的时
   候，根据当前状态决定tb，之后靠pc知道是这个tb（这个地方不直接，需要时刻注意）

3. 中断异常切换，tb异常或者中断，读header异常：riscv_cpu_do_interrupt()

4. 块结束：target/linx/translate.c:riscv_tr_translate_insn()生成。

   各个commit函数在：target/linx/insn_trans/trans_block_prvileged.c.inc:__trans_blk_xxxx()

6. mret: target/linx/op_helper.c:helper_mret

7. sret: target/linx/op_helper.c:helper_sret

修改完了，第一个版本测试运行内核没有问题，上传了。

然后发现用户态出问题了，主要是ecall的返回值问题，这引出下一个bpc如何计算的问题，
我现在通过重新取指来解决问题，但在执行循环外重新取指，会有什么问题？这要明天重
新推理一下。

另外王州评审发现其他的问题，这个让他直接修改了。

现在内核可以跑到ttyS生效，各个文件系统的初始化也完成。

20220527
========

今天重点做两件事：把LARM没有修改完了东西修改完，分析前面那个取指的问题的逻辑。

LARM待修改工作
--------------

把前天的列表拷贝过来：

1. GPR不是prepare阶段写进去的，硬件确定在prepare阶段还是get的时候再取，反正不影
   响 （完成）

2. SBPC的名字修改成CARG，同时要说明它的判断逻辑（特别是和redirect头的用途正交这
   一点）模型。（完成：主要修改了中断处理过程的描述）

3. 修改ecall的行为要求，把EN加上去（完成）

4. 多个ARG指令产生覆盖，这一点要说明出来（完成）

5. BSTATE的优先级状态问题需要明确说出来（完成）

6. 王州加了lr/sc的aq/rl语义，但没有详细解释，需要补充。（完成，补充到内存模式里面了）

取指问题分析
------------

为了从中断或者异常返回的时候知道下一个地址在哪里，我们现在在译码执行循坏之外调
用cpu_ldl_code()。这个函数在user和system的时候用了不同的实现。在system里面，它
调用的是load_helper()，最终是依靠tlb_fill()来获得页表的，这个函数是回调，我们这
里是riscv_cpu_tlb_fill()，它搞不定的时候会直接抛异常。

而抛异常，是要靠调用译码执行循环外面的set_jmp来跳出的，所以在中断处理阶段调用这
个函数，其实是不行的。

我们回到硬件的实现来说，硬件的行为是这样的：

用户发起一个ecall，我实际产生了一个异常，同时把异常的返回地址设置为本指令的下一
条指令，然后我进入异常流程。之后，我的状态机都是自恰的。你软件到时给我这个epc的
地址，我就什么都好了。

qemu有现在的麻烦是因为qemu把上面的动作分成了两个步骤：

1. 遇到了ecall调用，我不修改epc，我直接异常
2. 离开翻译执行循环，这里设置epc

这两者中间其实插入不了其他更新CPU状态的操作，这样我们在1设置一个参数，到2的时候
直接用，这个问题就不存在了。只是这个参数不能翻译的时候设置，要变成中间代码，在
header执行里面设置。


内核当前状态跟踪
----------------

下面深入看看内核的调试状态。我更新了所有代码，第一次运行看到的结果是这个：::

  [    2.125043] debug_vm_pgtable: [debug_vm_pgtable         ]: Validating architecture page table helpers
  [    2.154738] BUG: spinlock already unlocked on CPU#0, swapper/1
  [    2.155345]  lock: 0xffffffff82f24f38, .magic: dead4ead, .owner: <none>/-1, .owner_cpu: -1
  [    2.156232] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g14d6bcf58048-dirty #14
  [    2.157122] Hardware name: riscv-virtio,qemu (DT)
  [    2.157937] Call Trace:
  [    2.158520] [<ffffffff800066c0>] dump_backtrace+0x20/0x40
  [    2.159270] [<ffffffff81459b40>] dump_stack+0x40/0x60
  [    2.159802] [<ffffffff8143fec0>] .L3+0x40/0x60
  [    2.160270] [<ffffffff80091e80>] .L74+0x60/0x80
  [    2.160724] [<ffffffff81465940>] _raw_spin_unlock_irqrestore+0x20/0x40
  [    2.161335] [<ffffffff80834260>] .L526+0x40/0xe0
  [    2.162068] [<ffffffff80835040>] .L626+0x20/0x100
  [    2.162561] [<ffffffff80806e80>] tty_port_open+0x120/0x140
  [    2.163108] [<ffffffff80831360>] uart_open+0x20/0x40
  [    2.163609] [<ffffffff807f3fc0>] .L1073+0x80/0x1c0
  [    2.164110] [<ffffffff802cdac0>] .L44+0x40/0x60
  [    2.164592] [<ffffffff802b7e00>] .L73+0x20/0x60
  [    2.165077] [<ffffffff802bb080>] vfs_open+0x20/0x40
  [    2.165831] [<ffffffff802e8320>] .L1435+0x20/0x80
  [    2.166343] [<ffffffff802e9440>] .L1607+0x20/0x60
  [    2.166839] [<ffffffff802bbe40>] .L546+0x0/0x40
  [    2.167321] [<ffffffff802bc000>] filp_open+0x60/0x80
  [    2.167888] [<ffffffff82202720>] console_on_rootfs+0x20/0x80
  [    2.168462] [<ffffffff82202ca0>] .L255+0x80/0xe0
  [    2.168944] [<ffffffff81459f30>] kernel_init+0x30/0xc0
  [    2.169674] [<ffffffff80003520>] ret_from_exception+0x0/0x40

这还是之前的二次锁的问题。这个我通过开cs调度来跟踪。开了以后内核就没有输出了，有几种可能：

1. 停在udelay上。
2. 停在die上。

我重新开了cflags_next_tb的返回恢复流程，这些问题好像就没有了。这个状态机还是有
问题。我再检查一次，首先验证到一个事实：EN以后马上收到中断的可能性是存在的，我
捕获了一个mtimer。

状态机逻辑是这样的：

1. 软件设置了BSTATE_EXT，sret返回，现在状态回到layer1
2. layer1做RV相关动作，BSTATE的内容不会变
3. layer1遇到一个Head，做恢复

除非layer1做的这个head不是当初的header，我要检测这种情况，加了一个env的变量做了
一个跟踪（sret的时候设置，helper_blk_do_recovery()的时候检查）。没有检测到这种
异常情况。

看一个调度的过程：::

  ------------- CS_OUT(0): Addr(ffffffff8219c312=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer`
    BState Saved. BPC/TPC=ffffffff814287c0/ffffffff8219c312
    SBPC:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         ffffffff82e03c0e ffffffff82e03c0e ffffffff82e03c0f 0000000000000001 ffffffff82e03c0e ffffffff82e03c0f ffffffff82e03c0f ffffffff82977586 
  ------------- CS_OUT(0): Addr(ffffffff80008900=>80000520, ffffffffffffffff) Priv(1=>3) for `supervisor_ecall`
  ------------- CS_IN(0) Addr (800005f8=>ffffffff80008904, ffffffffffffffff) Priv(3=>1) 
  ------------- CS_IN(0) Addr (ffffffff8146f652=>ffffffff814287c0, ffffffff80003660) Priv(1=>1) 
    BSTATE.SGPRS:  0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    BSTATE.TREGS:  ffffffff82e03c0e ffffffff82e03c0e ffffffff82e03c0f 0000000000000001 ffffffff82e03c0e ffffffff82e03c0f ffffffff82e03c0f ffffffff82977586 
  Trace-Fri May 27 08:03:36 2022 <-- 块头执行
    Block Recoverred. BPC/TPC=ffffffff814287c0/ffffffff8219c312 <-- 恢复的两个地址都是正确的

寄存器看来也是一致的。那要不是我加的那个CF_LAST_IO起的作用？这个东西唯一的作用
是翻译的时候起作用，表示生成下一条指令前，在前面加一个gen_io_start()，把cpu->
can_do_io设置为1。它影响io_readx等行为，好像是icount模式下要把io行为移到tb的最
后面。还不知道细节，但也不想看，反正大部分时候这个can_do_io也是1，默认每个循环
都是1，设成1就设成1吧。

再跑了一次，这回多次是这个错误（在王州那里也见过）：::

  #0  0xffffffff80068120 in update_rq_clock ()
  #1  0xffffffff8007c5c0 in sched_rt_period_timer ()
  #2  0xffffffff800c8520 in __hrtimer_run_queues.constprop.0 ()
  #3  0xffffffff800ca3a0 in hrtimer_interrupt ()
  #4  0xffffffff8106b8e0 in riscv_timer_interrupt ()

这是一个WARN_DOUBLE_CLOCK检查，注释是这样的：::

  Issue a WARN when we do multiple update_rq_clock() calls
  in a single rq->lock section

看错误前的日志，是这样的：::

  ------------- CS_OUT(0): Addr(ffffffff8149d5c4=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer`
    BState Saved. BPC/TPC=ffffffff80048d00/ffffffff8149d5c4
  ------------- CS_OUT(0): Addr(ffffffff80008900=>80000520, ffffffffffffffff) Priv(1=>3) for `supervisor_ecall`
  ------------- CS_IN(0) Addr (800005f8=>ffffffff80008904, ffffffffffffffff) Priv(3=>1) 
  ------------- CS_IN(0) Addr (ffffffff8146f652=>ffffffff80048d00, ffffffff80003660) Priv(1=>1) 
    Block Recoverred. BPC/TPC=ffffffff80048d00/ffffffff8149d5c4
  ------------- CS_OUT(0): Addr(ffffffff80069280=>80000520, ffffffffffffffff) Priv(1=>3) for `m_timer`
  ------------- CS_IN(0) Addr (800005f8=>ffffffff80069280, ffffffffffffffff) Priv(3=>1) 
  ------------- CS_OUT(0): Addr(ffffffff81465950=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer`

之前发生了一次块内的s_timer，恢复了，恢复状态我也对了，是一样的（这里省略了）。
然后再来了一个m_timer，这正常回来了。然后又是一个s_timer，这次都没有断在块中间
直接切出，然后在里面遇到这个软件的错误。

要知道代码原来需要知道这里的算法。今年四月份就有patch修过一个误报警，不知道是否
和这个有关：
https://lore.kernel.org/all/20220427080014.18483-2-jiahao.os@bytedance.com/T/

但无论如何，RV不出问题我们就没有理由出问题--是这样吗？我用RV的内核试一下我们的
qemu：

.. note:

   一个小插曲，发现mrproper不能离目录编译，查了一下发现新的Makefile现在多了一个
   判断目标，用一般mrproper是清不掉的：arch/$(SRCARCH)/include/generated，需要
   手工删除。

结果是，RV也起不了，中间进panic了。我bisect一下我们的代码。发现是这个补丁引入的
错误：::

  kenny@kl-dev:~/work/linx-qemu-dev/LinxBlockModel$ git bisect good
  48bf71a813482eb81928d26365a0d5621ea534fe is the first bad commit
  commit 48bf71a813482eb81928d26365a0d5621ea534fe
  Author: Kenneth Lee <liguozhu@hisilicon.com>
  Date:   Thu May 26 10:31:01 2022 +0000
  
      blockisa: fix user mode state machine
  
      Signed-off-by: Kenneth Lee <liguozhu@hisilicon.com>
  
   linux-user/linx/cpu_loop.c                         | 12 +++++-
   target/linx/cpu_helper.c                           |  4 ++
   target/linx/insn_trans/trans_block.c.inc           | 12 ++----
   target/linx/insn_trans/trans_block_prvileged.c.inc | 46 ---------------------
   target/linx/translate.c                            | 48 +++++++++++++++-------
   5 files changed, 52 insertions(+), 70 deletions(-)

我自己引入的锅。误删了异常时设置返回pc的语句。要吸取教训，现在已经不能随手这样
refactor非必要代码了。幸亏只是引入了一天。

恢复后，RV可以跑了，而我们的版本会出现不同错误，比较多的是那个调度锁的错误，不
知道它的原理很难猜场景是什么。可能要去看懂这个代码。

另外再记录一个错误：::

  qemu-system-linx: ../target/linx/op_helper.c:246: helper_blk_do_recovery: Assertion `env->pc == env->sret_addr' failed.

这是我前面加的那个检测了，recover的时候没有回到sret的地址，我看看跟踪记录：::

  ------------- CS_OUT(0): Addr(ffffffff814d6a9a=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer` <-----------------+
    BState Saved. BPC/TPC=ffffffff80091d00/ffffffff814d6a9a      <--- 时钟从中间打断                                             |
    BSTATE.VLD/EN: 1/0                                                                                                           |
  Trace-0: 0x7f2cd6078dc0 [0000000000000000/ffffffff80003180/00004201/ff000000] handle_exception                                 |
  ... 执行一段，还有：generic_handle_arch_irq，irq_enter，...还有不少spin的lock和unlock                                          |
  Trace-0: 0x7f2cd606d540 [0000000000000000/ffffffff80008900/00004201/ff000000] __sbi_set_timer_v02                              |
  ------------- CS_OUT(0): Addr(ffffffff80008900=>80000520, ffffffffffffffff) Priv(1=>3) for `supervisor_ecall`<----------+      |
    BSTATE.VLD/EN: 1/0                                                                                                    |      |
  ...                                                                                                                     |      |
  Stopped execution of TB chain before 0x7f2cd606e980 [00000000800089f6]                                                  |      |
  ... 这些都在固件里面了                                                                                                  |      |
  ------------- CS_IN(0) Addr (800005f8=>ffffffff80008904, ffffffffffffffff) Priv(3=>1) <-出来了 <------------------------+      |
    BSTATE.VLD/EN: 1/0                                                                                                           |
  ------------- CS_OUT(0): Addr(ffffffff80008904=>80000520, ffffffffffffffff) Priv(1=>3) for `m_timer` <-- 又来一个 <-----+      |
    BSTATE.VLD/EN: 1/0                                                                                                    |      |
  ... 又一段固件                                                                                                          |      |
  ------------- CS_IN(0) Addr (800005f8=>ffffffff80008904, ffffffffffffffff) Priv(3=>1) <-- 又出来了 <--------------------+      |
    BSTATE.VLD/EN: 1/0                                                                                                           |
  ... 又是一段时钟相关的内核代码执行                                                                                             |
  ------------- CS_IN(0) Addr (ffffffff81473fb2=>ffffffff80091d00, ffffffff80003660) Priv(1=>1) <------内核时钟出来了------------+
    BPC/TPC:       ffffffff80003660 ffffffff81473fb2
    BSTATE.VLD/EN: 1/1 请求恢复
  ------------- CS_OUT(0): Addr(ffffffff80091d00=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer` <-- 还没有恢复，再来一个时钟
    BPC/TPC:       ffffffffffffffff ffffffff80003180
    BSTATE.VLD/EN: 0/1
  Trace-0: 0x7f2cd6078dc0 [0000000000000000/ffffffff80003180/00004201/ff000000] handle_exception 再进入异常
  ...
  Trace-0: 0x7f2cd5a58140 [0000000000000000/ffffffff80091ce0/00004201/ff000000] do_raw_spin_unlock <-- 异常中的新head发现有en，尝试恢复自己。
  Block Recoverred. BPC/TPC=ffffffff80091ce0/ffffffff814d6a9a <------------我们的检测代码发现非法的恢复，这不是当初我们想恢复的地方。

重新打开锁中断的代码，现在的错误是这样的：::

  [    6.994980] NET: Registered PF_NETLINK/PF_ROUTE protocol family
  [   24.889028] watchdog: BUG: soft lockup - CPU#0 stuck for 21s! [swapper:1]
  [   24.918159] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g14d6bcf58048 #1
  [   24.951481] Hardware name: riscv-virtio,qemu (DT)
  [   24.971536] epc : __might_resched+0x0/0x20
  [   24.999711]  ra : .L1371+0x20/0x40
  [   25.024853] epc : ffffffff8006ff40 ra : ffffffff80070260 sp : ffffffe003233a30
  [   25.055470]  gp : ffffffff82ee93a0 tp : ffffffe003248000 t0 : ffffffe0032bc680
  [   25.085865]  t1 : 0000000000000001 t2 : 00000000000000a7 s0 : ffffffe003233a50
  [   25.115743]  s1 : ffffffff82977620 a0 : ffffffff82977620 a1 : 00000000000005f2
  [   25.145908]  a2 : 0000000000000000 a3 : ffffffe0032bc618 a4 : a00f85c492125d00
  [   25.175879]  a5 : 0000000000000000 a6 : 0000000000000220 a7 : 0000000000000228
  [   25.205278]  s2 : 00000000000005f2 s3 : ffffffe0032bc280 s4 : 0000000000001000
  [   25.234958]  s5 : ffffffe0032bb060 s6 : 0000000000000000 s7 : 0000000000000000
  [   25.264584]  s8 : 00000000000011b4 s9 : ffffffffffffee4b s10: ffffffe00328f598
  [   25.294496]  s11: 0000000000000124 t3 : 0000000000008000 t4 : 0000000000000402
  [   25.323981]  t5 : ffffffe003216010 t6 : ffffffe003216030
  [   25.347629] status: 8000000000006120 badaddr: 0000000000000000 cause: 8000000000000005
  [   25.379811] [<ffffffff8006ff40>] __might_resched+0x0/0x20
  [   25.412646] [<ffffffff81465580>] down_write+0x20/0x60
  [   25.443650] [<ffffffff80407880>] kernfs_add_one+0x20/0x80
  [   25.475094] [<ffffffff8040bb80>] .L336+0x20/0x40
  [   25.502671] [<ffffffff8040cf00>] .L122+0x20/0x40
  [   25.531402] [<ffffffff8040fe60>] .L41+0x20/0x40
  [   25.558200] [<ffffffff80410620>] sysfs_create_group+0x20/0x40
  [   25.590211] [<ffffffff8220c5a0>] .L254+0x20/0x80
  [   25.619365] [<ffffffff822024e0>] .L214+0x20/0x80
  [   25.648971] [<ffffffff82202c00>] .L245+0x40/0x60
  [   25.678804] [<ffffffff8145e7f0>] kernel_init+0x30/0xc0
  [   25.709440] [<ffffffff80003520>] ret_from_exception+0x0/0x40

而且还能往下走，再有其他错误，这个明天看吧。

.. note:

   想跟踪那个那个调度锁错误的问题，我感觉我们还需要这个功能：
   1. 通过linx_debug指令给运行地址设置一个断点。
   2. 在数据被访问的时候写日志，或者根据需要停下来。（这个功能已经有了）

20220527
========

昨晚回家构思了一个跟踪方案：

1. 增加一个linx_debug指令属性，专门用于跟踪软件调度，可以过滤
2. 在软件上这样跟踪：
   1. 调度入口和出口增加调度记录
   2. soft/hard lockup喂狗的时候增加记录
   3. watchdog的时候增加记录

今天开始改代码。

当前内核的linx_debug的实现放在arch/linx/include/asm/bug.h上声明了，实现在
arch/linx/kernel/entry.S中，（正好在__switch_to后面，真巧）。

然后发现需要的基础设施在现在的linx_debug里面都有了，除了不能过滤。我加上过滤就
可以直接改内核了。

下面跟踪一个调度过程：::

  linx_debug(101, 0x0) info: switch from ffffffff82e09480 to ffffffe003248000
   9480的状态：
   忽略，但我们注意到，eVLD=1，它是在块中发生异常而进入调度的，这个状态保存以后，
   运行中会一直保留，直到中断返回，所以，这个任务的中断返回需要恢复body。这个后面
   开CS跟踪看看是否如此。
   eEN      0000000000000000 eVLD     0000000000000001
   8000的状态：
   这个其实也不关心，但我们主要到，VLD在切换的时候是换不掉的，而且其实也没有用。
   eEN      0000000000000000 eVLD     0000000000000001
  然后一堆的时钟，workqueue相关操作很长，时钟也发生多次，然后：
  linx_debug(101, 0x0) info: switch from ffffffe003248000 to ffffffe003248940
   8000的状态 <------------- 这是最重要的，我们要对比的，因为这个是8000的保存状态
   pc       ffffffff800037c0
   mhartid  0000000000000000
   mstatus  8000000a000060a0
   mip      00000000000000a0
   mie      000000000000022a
   mideleg  0000000000000222
   medeleg  000000000000b109
   mtvec    0000000080000520
   stvec    ffffffff80003180
   mepc     ffffffff8142eb20
   sepc     ffffffff8146a290
   mcause   8000000000000007
   scause   8000000000000005
   mtval    0000000000000000
   stval    0000000000000000
   mscratch 0000000080018000
   sscratch 0000000000000000
   satp     80000000000830f0
   x0/zero  0000000000000000 x1/ra    ffffffff8145f8a0 x2/sp    ffffffe003233970 x3/gp    ffffffff82ee93a0
   x4/tp    ffffffe003248000 x5/t0    ffffffe003208380 x6/t1    ffffffffffffffff x7/t2    000000000000001e
   x8/s0    ffffffe003233a40 x9/s1    ffffffe003248940 x10/a0   ffffffe003248000 x11/a1   ffffffe003248940
   x12/a2   0000000000000030 x13/a3   ffffffe003248790 x14/a4   ffffffe0032490d0 x15/a5   147a787e2794ab00
   x16/a6   000000000000000f x17/a7   ffffffffffffffef x18/s2   ffffffff82e11740 x19/s3   ffffffe003248000
   x20/s4   ffffffff82e12740 x21/s5   ffffffff81461410 x22/s6   ffffffff82eea0a8 x23/s7   ffffffff82eea0a8
   x24/s8   ffffffe003248470 x25/s9   0000000000000000 x26/s10  0000000000000000 x27/s11  0000000000000000
   x28/t3   000000000000000f x29/t4   0000000000000402 x30/t5   ffffffff82e0fee8 x31/t6   ffffffff82e0ff08
   TR1      ffffffe003248000 TR2      0000000000000790 TR3      ffffffe003248000 TR4      ffffffe003248790
   TR5      ffffffe003248940 TR6      ffffffe003248940 TR7      ffffffe0032490d0 TR8      ffffffe003248000
   BPC      ffffffffffffffff SBPC     ffffffff8145f860 TPC      ffffffff800037c0
   eTR1     0000000057ac6e9d eTR2     ffffffff82e09480 eTR3     0000000000000000 eTR4     0000000000000000
   eTR5     0000000000000000 eTR6     0000000000000000 eTR7     0000000000000000 eTR8     0000000000000000
   eTPC     0000000000000000 eSBPC    0000000000000000
   eEN      0000000000000000 eVLD     0000000000000000
   8940的状态（忽略）
  linx_debug(101, 0x0) info: switch from ffffffe003248940 to ffffffe003249280 （或者这些中间调度状态）
  linx_debug(101, 0x0) info: switch from ffffffe003249280 to ffffffe003248000 <- 尼玛终于回8000了
   8000的状态
   pc       ffffffff800037f0 （这个不同，但正常，因为这是调度函数本身的地址）
   mhartid  0000000000000000 同
   mstatus  8000000a000060a0 同
   mip      00000000000000a0 同
   mie      000000000000022a 同
   mideleg  0000000000000222 同
   medeleg  000000000000b109 同
   mtvec    0000000080000520 同
   stvec    ffffffff80003180 同
   mepc     ffffffff8142e5c0 不同，这个是机器态的东西，无所谓
   sepc     ffffffff8146a2d0 不同，这是返回地址，不同合理，但对不对就不知道了
   mcause   8000000000000007 同，但无所谓
   scause   8000000000000005 同
   mtval    0000000000000000 同
   stval    0000000000000000 同
   mscratch 0000000080018000 同
   sscratch 0000000000000000 同
   satp     80000000000830f0 同
   x0/zero  0000000000000000 x1/ra    ffffffff8145f8a0 x2/sp    ffffffe003233970 x3/gp    ffffffff82ee93a0 同
   x4/tp    ffffffe003248000 x5/t0    ffffffe003205200 x6/t1    ffffffffffffffff x7/t2    000000000000001e t0不同（可能合理，这是切换临时变量）
   x8/s0    ffffffe003233a40 x9/s1    ffffffe003248940 x10/a0   ffffffe003249280 x11/a1   ffffffe003248000 a0,a1不同（这也合理，这是入口参数）
   x12/a2   0000000000000030 x13/a3   ffffffe003249a10 x14/a4   ffffffe003248790 x15/a5   147a787e2794ab00 a3, a4不同（这个合理，这是两个task的task.thread_ra）
   x16/a6   000000000000000f x17/a7   ffffffffffffffef x18/s2   ffffffff82e11740 x19/s3   ffffffe003248000 同
   x20/s4   ffffffff82e12740 x21/s5   ffffffff81461410 x22/s6   ffffffff82eea0a8 x23/s7   ffffffff82eea0a8 同
   x24/s8   ffffffe003248470 x25/s9   0000000000000000 x26/s10  0000000000000000 x27/s11  0000000000000000 同
   x28/t3   000000000000000f x29/t4   0000000000000402 x30/t5   ffffffff82e0fee8 x31/t6   ffffffff82e0ff08 同
   TR1      0000000000000058 TR2      0000000000000068 TR3      ffffffe0032487f8 TR4      0000000000000000 没有EN，这些都无所谓
   TR5      ffffffe003248790 TR6      ffffffe003248000 TR7      ffffffe0032487f0 TR8      0000000000000000
   BPC      ffffffffffffffff SBPC     ffffffff8145f860 TPC      ffffffff800037f0
   eTR1     0000000057ac6e9d eTR2     ffffffff82e09480 eTR3     0000000000000000 eTR4     0000000000000000
   eTR5     0000000000000000 eTR6     0000000000000000 eTR7     0000000000000000 eTR8     0000000000000000
   eTPC     0000000000000000 eSBPC    0000000000000000
   eEN      0000000000000000 eVLD     0000000000000000

看起来，整个调度行为都是合理的。我查了整个跟踪记录，那个ffffffff82e09480再也没
有回去过，应该是swap线程，没到空闲应该不会回去了。

我再找一个从中间断开的流程跟踪一下：::

  linx_debug(101, 0x0) info: switch from ffffffe003248000 to ffffffe003248940 <-- 8000被切走
   pc       ffffffff800037c0
   mhartid  0000000000000000
   mstatus  8000000a000060a0
   mip      00000000000000a0
   mie      000000000000022a
   mideleg  0000000000000222
   medeleg  000000000000b109
   mtvec    0000000080000520
   stvec    ffffffff80003180
   mepc     ffffffff814bd07c
   sepc     ffffffff8004f460
   mcause   8000000000000007
   scause   8000000000000005
   mtval    0000000000000000
   stval    0000000000000000
   mscratch 0000000080018000
   sscratch 0000000000000000
   satp     80000000000830f0
   x0/zero  0000000000000000 x1/ra    ffffffff8145f8a0 x2/sp    ffffffe0032338c0 x3/gp    ffffffff82ee93a0
   x4/tp    ffffffe003248000 x5/t0    ffffffe003208600 x6/t1    ffffffffffffffff x7/t2    000000000000005f
   x8/s0    ffffffe003233990 x9/s1    ffffffe003248940 x10/a0   ffffffe003248000 x11/a1   ffffffe003248940
   x12/a2   0000000000000030 x13/a3   ffffffe003248790 x14/a4   ffffffe0032490d0 x15/a5   147a787e2794ab00
   x16/a6   000000000000000f x17/a7   ffffffffffffffef x18/s2   ffffffff82e11740 x19/s3   ffffffe003248000
   x20/s4   ffffffff82e12740 x21/s5   ffffffff81461410 x22/s6   ffffffff82eea0a8 x23/s7   ffffffff82eea0a8
   x24/s8   ffffffe003248470 x25/s9   0000000000000008 x26/s10  ffffffff822000e0 x27/s11  0000000000000000
   x28/t3   000000000000000f x29/t4   0000000000000001 x30/t5   0000000000000000 x31/t6   00000000000003ff
   TR1      ffffffe003248000 TR2      0000000000000790 TR3      ffffffe003248000 TR4      ffffffe003248790
   TR5      ffffffe003248940 TR6      ffffffe003248940 TR7      ffffffe0032490d0 TR8      ffffffe003248000
   BPC      ffffffffffffffff SBPC     ffffffff8145f860 TPC      ffffffff800037c0
   eTR1     ffffffe003248000 eTR2     0000000001969930 eTR3     ffffffe003248000 eTR4     0000000000000000
   eTR5     ffffffff82e0f5b8 eTR6     ffffffff82e0f5b8 eTR7     ffffffff8004edc0 eTR8     ffffffff82e0f5b8
   eTPC     0000000000000000 eSBPC    0000000000000000
   eEN      0000000000000000 eVLD     0000000000000001
   ...
  linx_debug(101, 0x0) info: switch from ffffffe00324d340 to ffffffe003248000 <--- 这里切回
   pc       ffffffff800037f0
   mhartid  0000000000000000
   mstatus  8000000a000060a0
   mip      00000000000000a0
   mie      000000000000022a
   mideleg  0000000000000222
   medeleg  000000000000b109
   mtvec    0000000080000520
   stvec    ffffffff80003180
   mepc     ffffffff80073180
   sepc     ffffffff80066e00
   mcause   8000000000000007
   scause   8000000000000005
   mtval    0000000000000000
   stval    0000000000000000
   mscratch 0000000080018000
   sscratch 0000000000000000
   satp     80000000000830f0
   x0/zero  0000000000000000 x1/ra    ffffffff8145f8a0 x2/sp    ffffffe0032338c0 x3/gp    ffffffff82ee93a0
   x4/tp    ffffffe003248000 x5/t0    ffffffe003205b00 x6/t1    ffffffffffffffff x7/t2    000000000000005f
   x8/s0    ffffffe003233990 x9/s1    ffffffe003248940 x10/a0   ffffffe00324d340 x11/a1   ffffffe003248000
   x12/a2   0000000000000030 x13/a3   ffffffe00324dad0 x14/a4   ffffffe003248790 x15/a5   147a787e2794ab00
   x16/a6   000000000000000f x17/a7   ffffffffffffffef x18/s2   ffffffff82e11740 x19/s3   ffffffe003248000
   x20/s4   ffffffff82e12740 x21/s5   ffffffff81461410 x22/s6   ffffffff82eea0a8 x23/s7   ffffffff82eea0a8
   x24/s8   ffffffe003248470 x25/s9   0000000000000008 x26/s10  ffffffff822000e0 x27/s11  0000000000000000
   x28/t3   000000000000000f x29/t4   0000000000000402 x30/t5   ffffffff82e0fee8 x31/t6   ffffffff82e0ff08
   TR1      0000000000000058 TR2      0000000000000068 TR3      ffffffe0032487f8 TR4      0000000000000000
   TR5      ffffffe003248790 TR6      ffffffe003248000 TR7      ffffffe0032487f0 TR8      ffffffff822000e0
   BPC      ffffffffffffffff SBPC     ffffffff8145f860 TPC      ffffffff800037f0
   eTR1     ffffffe003248000 eTR2     0000000001969930 eTR3     ffffffe003248000 eTR4     0000000000000000
   eTR5     ffffffff82e0f5b8 eTR6     ffffffff82e0f5b8 eTR7     ffffffff8004edc0 eTR8     ffffffff82e0f5b8
   eTPC     0000000000000000 eSBPC    0000000000000000
   eEN      0000000000000000 eVLD     0000000000000000
  
RV的内容基本上都对，block的内容都没有恢复。这理论上应该在中断返回的时候恢复。所
以，我现在需要跟踪的是CS_OUT进来的状态，再看从CS_IN回去的状态，看看两者是否一致。

这次抓到一次soft lockup，但数据上找不到位置，改进了一个捕获点，晚点再说。先看调
度过程：::

  ------------- CS_IN(0) Addr (800005f8=>ffffffff814b6eca, ffffffff8006af00) Priv(3=>1)  后面没有执行，直接进入CS_OUT
  ------------- CS_OUT(0): Addr(ffffffff814b6eca=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer` <-- 这是8000线程的状态
    BState Saved. BPC/TPC=ffffffff8006af00/ffffffff814b6eca
    GPRS:          0000000000000000 ffffffff8006af00 ffffffe003233b60 ffffffff82ee93a0 ffffffe003248000 ffffffe003208380 15976f918b248600 000000000000007a ffffffe003233b70 ffffffe003208340 0000000000000000 8000000000006022 0000000000000000 ffffffe003208380 15976f918b248600 15976f918b248600 ffffffffffffffff fffffffffffffffe ffffffff82e100f8 ffffffe003208368 ffffffff82e100f8 ffffffe003233c40 ffffffff829751d0 ffffffff82eea0a8 ffffffff82e100e0 0000000000000000 0000000000000000 0000000000000000 15976f918b248600 0000000000000402 ffffffff82e0fee8 ffffffff82e0ff08 
    BPC/TPC:       ffffffffffffffff ffffffff80003180
    BSTATE.VLD/EN: 1/0
    SBPC:          ffffffff8006af00
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         ffffffff82eea0a8 0000000000000070 ffffffe003233b60 0000000000000000 15976f918b248600 ffffffe003233af0 ffffffff82e100f8 ffffffff8006af00 
  ------------- CS_OUT(0): Addr(ffffffff80008880=>80000520, ffffffffffffffff) Priv(1=>3) for `supervisor_ecall`
  ... 跳过一堆的调度
  ------------- CS_OUT(0): Addr(ffffffff8146a2d0=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer` <- 时钟
  ------------- CS_OUT(0): Addr(ffffffff80008880=>80000520, ffffffffffffffff) Priv(1=>3) for `supervisor_ecall`
  ------------- CS_IN(0) Addr (800005f8=>ffffffff80008884, ffffffffffffffff) Priv(3=>1) <- m-mode请求结束
  ------------- CS_IN(0) Addr (ffffffff81474060=>ffffffff8146a2d0, ffffffff80003600) Priv(1=>1) <- 这里时钟中断已经返回了
  ------------- CS_OUT(0): Addr(ffffffff821ab95a=>80000520, ffffffff81438920) Priv(1=>3) for `m_timer` <- 12862627
  ------------- CS_IN(0) Addr (800005f8=>ffffffff821ab95a, ffffffff81438920) Priv(3=>1) <- 12862667 <- m-mode请求结束
  linx_debug(101, 0x0) info: switch from ffffffe003249280 to ffffffe003248000    <-- 现在又来切换，恢复的状态里显然没有被body
  ------------- CS_OUT(0): Addr(ffffffff80066e00=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer`  <-- 后面是新的时钟了。

好了，到这里我想我不用跟下去了，我已经猜到我们的问题是什么了：我们的body状态是
靠中断保存来保存的，但内核的调度并非每次都靠中断返回回到原来的位置的，switch_to
恢复的时候并不考虑恢复body，这样一旦出现上面的场景，我们的状态机就完蛋了。

过程中又检测到一次恢复时中断：::

  qemu-system-linx: ../target/linx/op_helper.c:247: helper_blk_do_recovery: Assertion `env->pc == env->sret_addr' failed.

这说明cflags_next_tb的保护并不保险。这个地方需要完整想想这个整体构架逻辑是什么了。

我对此建一个逻辑模型：

传统的RV处理器，所有状态都是寄存器，一条指令，无论它中间发生异常还是中断，这条
指令retired了，这条指令我们就当它不存在了，现在CPU的状态稳定地被寄存器列表代表。
之后，如果我们要恢复原来的执行状态，只要恢复所有的寄存器，并跳转到固定的位置，
就是对它的恢复。

而块指令不同，如果一个块被打断了，我们的状态被通用寄存器和BSTATE_EXT共同代表，
但其实不止如此，它还被打断位置的那个块所代表。我们不能直接拿着BSTATE_EXT恢复一
个状态，我们还要保证我们恢复的时候，那个位置上必然还是原来那个指令才行。

操作系统调度，其实只有一个契机，就是schedule（最终是调用__switch_to）。我们可以
这样理解它：你调用scheduler了，scheduler把调用者的通用上下文全部保存，然后换成
被调度方的上下文，然后就没有调用者了，等到一轮调度回来的时候，刚才的调用者的上下文
被恢复回来，感觉就是从scheduler退出了。

如果是从中断进入的，中断处理程序一样是一番处理后，调用schedule，产生一次调度。
那么原来从中断进来的线程，从schedule返回以后，会怎么样呢？它会认为自己仍在中断
里，然后做一次中断返回，这么说，就算没有中断返回，我们也会主动做中断返回？

我尝试跟踪这种情况，写了一个脚本（在devlog中，叫find_switch.py），跟踪body中断
后调度的场景，跑了5次，没有抓到这种场景，body中断很多，但没有调度就返回了。

然后我再跟踪一下soft lockup：::

  Trace-0(08:31:29): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(102, 0x0) info: watchdog feed 1=>19  <------------ 最后一次喂狗在这里
  Trace-0(08:31:29): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(101, 0x0) info: switch from ffffffe0032e0000 to ffffffe003248000
  ...
  Trace-0(08:31:33): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(103, 0x0) info: watchdog check 19, 19, 23, 0 <--- 之后4s左右更新一次时钟
  Trace-0(08:31:37): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(103, 0x0) info: watchdog check 19, 19, 26, 0
  Trace-0(08:31:41): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(103, 0x0) info: watchdog check 19, 19, 30, 0
  Trace-0(08:31:45): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(103, 0x0) info: watchdog check 19, 19, 34, 0
  Trace-0(08:31:49): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(103, 0x0) info: watchdog check 19, 19, 38, 0
  ... 中间时钟没有停，调度没有断
  Trace-0(08:31:53): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(101, 0x0) info: switch from ffffffe00324c0c0 to ffffffe00324ef00
  Trace-0(08:31:53): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(101, 0x0) info: switch from ffffffe00324ef00 to ffffffe00324ca00
  Trace-0(08:31:53): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(101, 0x0) info: switch from ffffffe00324ca00 to ffffffe0032e2500
  Trace-0(08:31:53): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(101, 0x0) info: switch from ffffffe0032e2500 to ffffffe00324c0c0
  Trace-0(08:31:53): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(101, 0x0) info: switch from ffffffe00324c0c0 to ffffffe00324ef00
  Trace-0(08:31:53): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(101, 0x0) info: switch from ffffffe00324ef00 to ffffffe00324ca00
  ... 看起来中间有很多format_decode，不过这是lib/vsprintf.c的，应该问题不大。
  Trace-0(08:31:53): 0x7fd9b241f200 [0000000000000000/ffffffff80003880/00004201/ff000000] qemu_debug_str
  linx_debug(103, 0x0) info: watchdog check 19, 19, 41, 22 <-- now=41，中间22秒没有更新了。
  linx_debug(103, 0x0) info: soft lockup

然后我再跟踪一下前面提到sret返回没有回到目标块的assert：::

  之前导致这个异常的代码看来是：serial8250_early_in
  ------------- CS_OUT(0): Addr(ffffffff8159d5b0=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `load_page_fault`
    BState Saved. BPC/TPC=ffffffff801d0cb0/ffffffff8159d5b0
    GPRS:          0000000000000000 ffffffff801d0bf0 ffffffe0032d75d0 ffffffff82ee93a0 ffffffe00324ef00 ffffffff82e14110 ffffffffffffffff 0000000000000000 ffffffe0032d7600 0000000000000008 0000000000000001 0000000000000007 0000000000000000 ffffffe0032d7618 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe0032d7610 0000000000000008 000000000000000d ffffffe0032d7930 00000000ee6b2800 0000000000000000 0000000000000001 0000000000000001 ffffffff82e85300 ffffffff801198b0 00000000000f0000 0000000000000037 0000000000000206 00000000000003ff 
    BPC/TPC:       ffffffffffffffff ffffffff80003180
    BSTATE.VLD/EN: 1/0
    SBPC:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000008 0000000000000001 0000000000000008 0000000000000000 ffffffe00324f668 0000000000000007 0000000000000000 
  Trace-0(11:45:42): 0x7fd740791fc0 [0000000000000000/ffffffff80003180/00004201/ff000000] handle_exception
  Trace-0(11:45:42): 0x7fd740792140 [0000000000000000/ffffffff81473970/00004201/ff000000] 
  Trace-0(11:45:42): 0x7fd740792340 [0000000000000000/ffffffff800031a0/00004201/ff000000] handle_exception
  ...
  ------------- CS_IN(0) Addr (ffffffff814740b0=>ffffffff801d0cd0, ffffffff80003600) Priv(1=>1) 
    GPRS:          0000000000000000 ffffffff801d0bf0 ffffffe0032d75d0 ffffffff82ee93a0 ffffffe00324ef00 ffffffff82e14110 ffffffffffffffff 0000000000000000 ffffffe0032d7600 0000000000000008 0000000000000001 0000000000000007 0000000000000000 ffffffe0032d7618 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe0032d7610 0000000000000008 000000000000000d ffffffe0032d7930 00000000ee6b2800 0000000000000000 0000000000000001 0000000000000001 ffffffff82e85300 ffffffff801198b0 00000000000f0000 0000000000000037 0000000000000206 00000000000003ff 
    BPC/TPC:       ffffffff80003600 ffffffff814740b0
    BSTATE.VLD/EN: 1/1
    SBPC:          0000000000000000
    BSTATE.SGPRS:  0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    BSTATE.TREGS:  0000000000000000 0000000000000008 0000000000000001 0000000000000008 0000000000000000 ffffffe00324f668 0000000000000007 0000000000000000 
  Trace-0(11:45:42): 0x7fd740fedd80 [0000000000000000/ffffffff801d0cd0/00004201/ff108001] 
  Trace-0(11:45:42): 0x7fd7404ac140 [0000000000000000/ffffffff8159d5be/00004201/ff000000] 
  Trace-0(11:45:42): 0x7fd7404ac380 [0000000000000000/ffffffff801d0c70/00004201/ff000000] 
  Trace-0(11:45:42): 0x7fd7404ac500 [0000000000000000/ffffffff8159d596/00004201/ff000000] 
  ...
  Trace-0(11:45:42): 0x7fd7404aa200 [0000000000000000/ffffffff801d0bd0/00004201/ff000000] copy_from_kernel_nofault
  Trace-0(11:45:42): 0x7fd7404aa380 [0000000000000000/ffffffff8159d518/00004201/ff000000] 
  Trace-0(11:45:42): 0x7fd7404aa800 [0000000000000000/ffffffff801d0bb0/00004201/ff000000] copy_from_kernel_nofault_allowed
  Trace-0(11:45:42): 0x7fd7404aa980 [0000000000000000/ffffffff8159d4fe/00004201/ff000000] 
  Trace-0(11:45:42): 0x7fd7404aac00 [0000000000000000/ffffffff801d0bf0/00004201/ff000000] copy_from_kernel_nofault
    Block Recoverred. BPC/TPC=ffffffff801d0cb0/ffffffff8159d5b0 <-- 这是assert前最后一个打印了
  
这里的问题不是中断没有挡住的问题，这里成功回去了，但中间居然执行了几条其他指令，
才碰到一个块头，为什么会这样？看前面CS_COUT的时候(BPC=801d0cb0，TPC=8159d5b0），返回的时候
返回地址是801d0cd0，确实就是当初的BPC，但执行的时候居然没有去恢复，难道这个还能不是头？

是头而不恢复的可能有几个：

1. 没有body。这个看前面的执行流，应该不是
2. 原子块。原子块确实可以异常，这个分支我修改的状态机时候略过了，请王州去修改状
   态吧。但copy_from_kernel_nofault会产生原子块吗？

20220530
========

问题跟踪
--------

看了王州上周写的两个问题：

1. 如何检测get没有用完所有的寄存器（这个让他和硬件的掰吧），我觉得不检查也没啥
2. 保存的时候没有保存tregs的下标，这个问题和周若愚讨论后发现关键在于他们保存的
   时候是按最后更新的顺序保存的，不需要这个下标。

-enable_delay_block_intr
------------------------

现在状态有关的问题越来越多，我决定还是先加一下块内不能中断这个特性。这包括对几
个结论的调查：

* cpu->cflags_next_tb其实就是一个很简单的全局变量，进入cpu_exec前设置，用过就清
  除。如果你加了NOIRQ，tb循环就不退出来给IRQ流程处理。

* NOIRQ不但阻塞中断，也阻塞异常，所以我们设置这个标志的时候要分开情况决定是否禁
  止下一个tb前处理中断。特别是要注意生成代码的是否，生成那些能产生异常的指令，
  要把设置noirq的工作放在最后一步，免得那些异常报不出去。

这样就很简单了，我加一个全局控制参数：eanble_delay_block_intr，用
-enable-delay_block_intr控制，使能的时候，每次完成一个翻译，而还没有到块终止
（包括异常）的时候，我一概产生一个helper，在helper里面设置cflags_next_tb。

第一个版本修改完成以后，我检查了一下log，还跟踪到body内被中断打断的情形，定位发现是
我把NOIRQ放在tb退出的地方了，忘了还有chain的场景，修正以后就好了。只有从异常跳出去的
才会从中间打断body。（也可以是异常中断后，遇到外部中断，这时会处理完异常，再处
理中断，然后才有恢复）

同时发现又出现sret返回地址不是设想地址的情况，这次加了in_asm，发现两个错误：

1. trans_blk_head中没有设置is_jmp。
2. helper_blk_do_recovery()对ext.tpc的判断条件边界不对，我修改成一个报错的逻辑
   （原来是不恢复的逻辑）了。

都修复好，状态正常了。上传一个版本，发现李飘也修改了之前的tregs下标的问题，两个
合并以后，现在用-enable_delay_block_intr版本可以稳定跑到一个状态。先用这个版本
推进到一定程度再说。

softlock问题
------------

用-enable_delay_block_intr版本运行可以稳定出现softlockup的现象（不一定超时），
像这样：::

  [    3.905523] NET: Registered PF_NETLINK/PF_ROUTE protocol family
  [   20.501229] iommu: Default domain type: Translated

到44秒的时候出来了一个打印（这时早应该有softlock打印了，但实际没有）：::

  [   44.672769] task:swapper         state:D stack:    0 pid:    1 ppid:     0 flags:0x00000000
  [   44.696352] Call Trace:
  [   44.705808] [<ffffffff8145f9b0>] .L285+0x60/0x80
  [   44.723816] [<ffffffff8145ff30>] .L762+0x10/0x60
  [   44.741319] [<ffffffff81468820>] .L356+0x20/0x40
  [   44.758444] [<ffffffff81460ad0>] .L11+0x50/0x80
  [   44.774151] [<ffffffff80d9d6d0>] devtmpfs_submit_req+0xa0/0xe0
  [   44.794186] [<ffffffff80d9d8b0>] .L88+0x20/0x40
  [   44.811263] [<ffffffff80d79d70>] .L1596+0x20/0x40
  [   44.827078] [<ffffffff80d7b170>] .L1876+0x20/0x60
  [   44.843547] [<ffffffff80d7b210>] device_create+0x20/0x60
  [   44.861580] [<ffffffff8222f6e0>] .L178+0x0/0x40
  [   44.878285] [<ffffffff822024e0>] .L214+0x20/0x80
  [   44.895618] [<ffffffff82202c00>] .L245+0x40/0x60
  [   44.913065] [<ffffffff8145e980>] kernel_init+0x30/0xc0
  [   44.930975] [<ffffffff800034e0>] ret_from_exception+0x0/0x40
  [   44.951079] task:kthreadd        state:S stack:    0 pid:    2 ppid:     0 flags:0x00000000
  [   44.973375] Call Trace:
  [   44.981615] [<ffffffff8145f9b0>] .L285+0x60/0x80
  [   44.998862] [<ffffffff8145ff30>] .L762+0x10/0x60
  [   45.016433] [<ffffffff80057d90>] .L269+0x20/0x40
  [   45.033694] [<ffffffff800034e0>] ret_from_exception+0x0/0x40
  [   45.052908] task:kworker/0:0     state:I stack:    0 pid:    3 ppid:     2 flags:0x00000000
  qemu-linx: invalid tpc for block to resume, tpc=ffffffff8159d698, bpc1/2=(ffffffff81473e6c, ffffffff81473e72)
  [   45.082874] Oops - illegal instruction [#1]

原因很明显，就是show_state()的时候发生了非法指令了，而这个非法指令是因为恢复导
致的，但为什么这里会发生恢复呢？

看日志：::

  Trace-0(11:17:52): 0x7f4ef23e5c00 [0000000000000000/ffffffff80003890/00004201/ff000000] qemu_debug_str
  linx_debug(103, 0x0) info: watchdog check 18, 41, 23                    <--- 这里开始检查狗
  ...
  Trace-0(11:17:53): 0x7f4ef1dc9180 [0000000000000000/ffffffff801d0d50/00004201/ff000000] copy_from_kernel_nofault
  Trace-0(11:17:53): 0x7f4ef1dc9300 [0000000000000000/ffffffff8159d690/00004201/00100000] 
  Trace-0(11:17:53): 0x7f4ef1dc9440 [0000000000000000/ffffffff801d0d70/00004201/ff000000] 
  Trace-0(11:17:53): 0x7f4ef1dc95c0 [0000000000000000/ffffffff8159d696/00004201/00100000] 
  ------------- CS_OUT(0): Addr(ffffffff8159d698=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `load_page_fault` <-- 然后导致了一次缺页（我猜是因为show_state需要用户上下文？）
    BState Saved. BPC/TPC=ffffffff801d0d70/ffffffff8159d698    <-- 从d698进入异常
  ... <- 异常处理
  ------------- CS_OUT(0): Addr(ffffffff80003580=>80000520, ffffffff80003580) Priv(1=>3) for `illegal_instruction` <-- 然后在异常中出现非法指令，进入m-mode，这样就没有什么可说的了。

这个再说，因为我听过其他同事的使用以及和曾博的讨论，我又发现一个新的问题了。我
先建模这个问题。

块内返回的逻辑建模
------------------

块如果在中间被打断，它的状态bstate保存在bstate_ext中，并且bstate_ext.vld.vld设
置为1。理论上，我们可以通过把bstate_ext恢复到bstate，恢复同一个块。

问题是，我在什么时机使用这个恢复呢？我们过去的设想，先设置bstate_ext，并且把
bstate_ext.en.en置1，然后执行sret，sret后面的指令用bstate_ext恢复现场。

但在sret和下一条指令中间插入一个中断或者异常，就会导致下一个指令不是要恢复的指
令，而是异常向量的指令。恢复就恢复到一个错误的地方了。

我们也不能让Xret和下一条指令中间不能有其他行为，因为取指本身就会造成异常，进了
异常就有可能发生常常的执行，甚至发生调度。

我们更不能用几天前qemu的错误实现，根据恢复的tpc是否在块定义的范围内决定是否恢复，
因为块身是可以被多个指令复用的。

我们也不能提前恢复bstate，等取指的时候再用，因为取指的时候我们不知道bstate是过
去的残留，还是刚刚的设置，如果我们要知道是刚刚的设置，我们就需要一个类似en的东
西，这样又回到最初的故事了。

我们也不能让Xret和下一条指令的执行合并成一个原子操作，因为CPU不能把执行和取指连
接在一起，这样会最终导致可能可以制造一个连续的原子联结序列（Xret返回到Xret）。

我现在能想到的方法是：

1. 每种特权级都有自己的state_ext，称为state_ext_m, sstate_ext_s。进入那个特权级，
   我们谈的state_ext就是那个特权级的版本。state_ext简称ext。

2. 块内发生中断或者异常，bstate填充bstate_ext，并且ext.vld=1，这个vld保持到下一
   次再发生中断或者异常。

3. ext.en可以被指令设置。任何一个块执行前，en被设置了，就执行恢复，如果恢复失败，
   产生异常，完全按2执行。（这导致en之前设置的ext状态丢失）。

4. 块具有noirq属性，当这种属性被执行的时候，cpu.noirq属性被置位，块被执行的时候
   不检查中断，这个属性一直维持到下一个没有noirq的块被执行。

下面是软件的使用策略：

1. 中断或者异常向量（无论哪个特权级），检查ext.vld，并保存ext作为恢复上下文。过程
   中如果出现异常，panic。

2. 恢复的时候，用保存的信息恢复ext，如果中间出现异常，panic。

3. 把设置ext.en的指令和Xret指令放在同一个noirq块中。然则，Xret到下一条指令都不会被
   中断打断。

4. 如果指令恢复不成功，进入异常向量，ext变成恢复header的那个现场，异常cause是恢
   复不成功，那么如果是跨级异常，就杀下级，如果是平级异常，就panic。


20220531
========

异常上下文问题继续
------------------

今天我们得全力搞定这个状态机的问题的全局统一。否则其他工作可能都没有什么意义。

我列一下工作列表：

1. 修改LARM，上午前就要完成，马上评审，没有意见立即进入实施。

   1. 定义en的使用方式
   2. 定义新的cause寄存器
   3. 定义sret恢复的方式
   4. 定义禁止中断的属性
   5. 定义新的ecall返回地址行为

2. Qemu, Kernel完成修改和联调，用户态通知到，并且进行尽力而为的联调。

3. 然后做下一步的其他问题定位。

上午讨论以后，综合讨论的最新结论如下：

之前的方案有个理解不对：我认为一条指令启动的时候无法判断是从头开始的，还是从中
间恢复的，但其实可以，就是每条块指令结束前恢复状态，恢复指令恢复到一个新的状态，
这样新执行的指令就可以从一个确定的状态开始执行了。唯一有一个寄存器的内容不同，
就是TPC，TPC的初始状态必须是无效地址，如果启动一个块，这个TPC为无效，那使用块
的Body地址，否则用设定的地址。

这样我们只需要解决原子性的问题，之前的原则变成这样：

1. 每种特权级都有自己的bstate_ext，称为mbstate_ext, sbstate_ext。进入那个特权级，
   我们谈的bstate_ext就是那个特权级的版本。bstate_ext简称ext。

2. 块内发生中断或者异常，如果bstate有没有提交的状态，bstate填充bstate_ext，并且
   ext.vld=1，这个vld保持到下一次再发生中断或者异常。否则，ext.vld=0。也就是说，
   vld是中断入口判断是否存在ext的条件。

3. Xret指令用ext的内容恢复bstate的内容，并且pc转移到下一个块。这中间产生异常，
   完全按2执行。（这导致en之前设置的ext状态丢失）。Xret可以带参数，表恢复还是不
   恢复ext执行。

4. 块具有noirq属性，当这种属性的块被执行的时候，cpu.noirq=1，块被执行的时候
   不检查中断，这个属性一直维持到下一个没有noirq的块被执行。

下面是软件的使用策略：

1. 中断或者异常向量（无论哪个特权级），检查ext.vld，并保存ext作为恢复上下文。过程
   中如果出现异常，panic。

2. 恢复的时候，用保存的信息恢复ext，如果中间出现异常，panic。

3. 恢复中断打断的上下文的时候，先把保存的上下文恢复到ext，这中间如果有异常，
   panic。恢复ext后，调用noirq属性的sret，后者用ext恢复bstate，并把pc设置到恢复
   地址的bpc

4. 返回地址如果再发生异常，循之前的执行流程，一样进入异常向量，bstate的状态按当
   时的情况回到异常处理向量。

   特别解释：假设有线程块内访问内存异常，陷入中断处理，保存当时上下文ext1，中断
   处理恢复页面，调度，任意处理后，回到返回点，恢复ext=ext1，然后Xret回到原来的
   块，如果块可以继续执行，即使再发生异常，也是新的ext=ext2，ext1已经没有用。如
   果是读块头本身又缺页，按逻辑应该是ext1再次保存到ext，只有原因这次变成了缺页，
   缺页的细细在通用寄存器中，而不是ext中，等再次回来，恢复ext1就可以了。

这个方案还是有毛病：还是要修改。后面的不记录了，我最终把版本写到LARM里面了。

20220601
========

今天评审了新的sret方案，合入LARM，等其他人修改再说，我先看看论文。

20220601
========

昨天都是看资料，今天再考虑一下那个softlockup的问题，死锁很大可能是因为关中断和
互相等待导致的，所以我今天再加一个跟踪器：跟踪所有的CSR修改操作，看看哪些部分是
在关中断的情况下做的。

相应的功能上配置库了。

20220606
========

假期用RV分析了一下调度过程跟踪，觉得跟踪CSR还是不好用，所以我写了一个直接在-d
exec上现实中断和preempt状态的跟踪器，回来上传上去了。

当前的qemu没法跑新的内核，出了一些错误，这些错误要等qemu修改。但这些错误导致的
另外一些问题让我发现了另外两个错误，我修正了：

1. 反汇编译码不成功的时候多打印了一次，而且译码长度返回0了，这会导致译码永远循
   环下去，这个事情我记得我提过，结果还是要我自己改。
2. 中do_interrupt的时候用设置调试中断的方法报告状态错误，这种方法是不行的，因为
   这个函数最后会覆盖这个设置。我可以调整这个流程，但王州正在修改这个逻辑，我还
   是简单把这里写成assert，等他去调整吧。

为此我还分析了一下cpu_exec在中断和异常上的两层循环的逻辑，我写在这里了
:ref:`cpu_exec_two_level_loop`\ 。

剩下的时间都是LARM的ecall升级，以及分析Linux内核的调度器，总结写在这里了：
:ref:`linux_sched`\ 。

20220607
========

今天主要是做LxR的框架的评审，剩下一点时间更新了一下Linux的调度器分析。

20220608
========

今天做一个glibc的问题分析，写在这里了：http://3ms.huawei.com/km/blogs/details/12354165?l=zh-cn。

然后试了一下编译，发现用linx-unknown版本的工具链无法编译，我尝试换编译器，发现
共享服务器不工作了，这个事情先放一下。

同时发现编译脚本有一个很奇怪的命令：::

  cat /dev/null > /dev/null

我不明白有什么用，春华说是因为他的平台上这个设备上会产生残留。这个Pattern我太明
白发生什么事了，肯定是/dev/null被人覆盖成了普通文件了。确认一样果然如此。

这是一个不要用root权限工作的又一个例子。

另一个比较大的问题是这个glibc是用了两次编译来完成编译的，春华也解释不了为什么这
样就是可以的，我明天专门分析一下这个问题。

20220609
========

今天王州把新版本的qemu放出来了。我review了一下，大方向没有看到什么错，那就开始查问题吧。

默认运行在plic_init()的时候触发fault_store异常。跟踪结果是
write_store_buf_to_mem()里面提交了一个非法地址：::

  0xffffffff82429340:  0000a000 00000000 6cc00c01 6b4e7c0b std_block.concat next:0xffffffff82429360, ptr:0xffffffff81994826, attr:acquire, out_reg(), in_reg(a3,a5)
  0xffffffff82429350:  fffffff5 00007dbd 00000000 00000000 std_block.fall_through
	  0xffffffff81994826:  0f12                  get             a5
	  0xffffffff81994828:  0d12                  get             a3
	  0xffffffff8199482a:  040e                  sw              t#2, [t#1, 0]
  ...
  OP:
   ld_i32 tmp1,env,$0xfffffffffffffff0
  
   ---- ffffffff81994812
   mov_i32 ibpc,$0x0
   mov_i64 TR1,x15/a5
   mov_i32 ibpc,$0x1
  
   ---- ffffffff81994814
   mov_i64 TR2,x13/a3
   mov_i32 ibpc,$0x2
  
   ---- ffffffff81994816
   mov_i64 tmp5,TR2
   call store_data,$0x0,$0,env,tmp5,TR1,$0x4
   mov_i64 TR3,tmp5
   mov_i32 ibpc,$0x3
   call write_store_buf_to_mem,$0x0,$0,env
   call handle_exec_and_branch,$0x0,$0,env,$0xffffffff81994816
   exit_tb $0x0
  
  Trace-XX 0(04:40:11): 0x7fbe5463e7c0 [0000000000000000/ffffffff81994812/00004201/ff300600] 
  kenny: write store buffer to addr ffffffd000002080
  riscv_cpu_do_interrupt: hart:0, async:0, cause:0000000000000007, epc:0xffffffff81994812, tval:0xffffffd000002080, desc=fault_store

打点收缩范围，现在收缩到这里：::

  static inline void plic_toggle(struct plic_handler *handler,
				  int hwirq, int enable)
  {
	  static char buf[100];
	  u32 __iomem *reg = handler->enable_base + (hwirq / 32) * sizeof(u32);
	  u32 hwirq_mask = 1 << (hwirq % 32);
  
	  sprintf(buf, "spin lock=%lx, reg=%lx, handler->enable_base=%lx", (uint64_t)&handler->enable_lock, reg, handler->enable_base);
	  qemu_debug_str(101, buf);
	  raw_spin_lock(&handler->enable_lock);
	  qemu_debug_hit(102); <---------- 这个点到了
	  if (enable) {
		  qemu_debug_hit(103);
		  writel(readl(reg) | hwirq_mask, reg);
	  } else {
		  qemu_debug_hit(104);  <---------------- 走了这边
		  writel(readl(reg) & ~hwirq_mask, reg);  <--- 这里其实有两个独立内存行为，一个是读reg，一个是写reg，这个地址都不对。
	  }
	  qemu_debug_hit(105);  <--------------- 这个点没有出来。
	  raw_spin_unlock(&handler->enable_lock);
	  qemu_debug_hit(106);
  }

  上面101位置的输出：
  linx_debug(101, 0x0) info: spin lock=ffffffff82e930e8, reg=ffffffd000002080, handler->enable_base=ffffffd000002080

这可能是个配置错误的问题，但我想我们首先要厘清这个问题：

LARM手册上aquired的块是原子块属性，但readl和writel可不需要原子性的，现在我们需
要一起对齐一个共识：到底原子块的aquire仅仅是个barrier，还是一个原子属性？如果这
是原子属性，那么编译器在这个地方编译的方法是不对的。

这个问题和孙文博对齐，理解是一致的，就是LARM上当前的定义，在和Kernel陈立福对齐，
确认他们错用了。

然后发现qemu的反汇编现实的块原子属性是不对的。陈立福去掉这个原子属性，可以不出
这个错误。

但这样不对，原子块只能算是多余的问题，不应该出错。所以又怀疑到是原子块的更新行
为不对。

我Review的一下代码，至少发现如下问题：

1. 在gen_blk_store的时候，是根据CF_ATOMIC来区分是否原子操作的，这不对，难道
   CF_ATOMIC就一定是原子块？这个判断不安全。

2. blk_store的时候，如果cpu->store_addr为0就更新它，这个也不对，因为0是有效的地址。
   而且，这个地址必须读的时候也要更新的，这个没有做。

3. 允许长度是64字节，这是cacheline的长度吗？需要定义一个宏。

4. 用一个mask来表示哪个字节被更新过，但mask用store size做为大小，这根本不对，应
   该用cacheline的字节数作为大小啊。

5. 一个判断范围是否越限的函数叫做is_64B_aligned。

这个算法的问题多到好像没有review没有测试一样。等待王州重新设计前我什么都干不了，
我来建个模吧。

1. 原子块初始化的时候，is_ab=true，ab_cacheline_addr=-1（无效），ab_mask=0（无读写）。
   否则is_ab=false，后两个变量无意义。

2. 任何读写操作，如果is_ab=true：

   1. 如果ab_cacheline_addr=-1，就初始化为cacheline对齐地址，ab_mask初始化
      为全0。
   2. 如果ab_cacheline_addr!=-1，读写地址x在cacheline对齐后必须等于它，否则异常。
      如果是写，x-ab_cacheline_addr开始的size个字节的ab_mask置1

3. 块提交的时候，根据ab_mask对ab_cacheline_addr相关字节赋值。

要单元测试。

20220610
========

glibc编译的问题
---------------

今天有时间看看glibc的问题了。现在glibc能成功编译的过程是这样的：

1. 先配置成--enable-static=yes --enable-shared=yes，然后make
2. 再配置成--enable-static=yes --enable-shared=no

（实际上我还没有试过，因为现在我都没有linx-linux-elf-gcc工具链。）

所以我们先来看看这两个选项的作用：

glibc使用标准的autoconf配置框架，所以这个东西还是好分析，首先--enable-static没
有对应的AC_ARG_ENABLE，唯一的作用作用在AC_SUBST，相当于增加了一个全局的static变
量。

--enable-share倒是有对应的AC_ARG_ENABLE，但只是为了提供帮助，最终还是AC_SUBST在
起作用。

但glibc没有使用autmake，所以这两个变量的用途更好找了，AC_CONFIG_FILES就生成两个
文件：config.make和Makefile。最终就是设置了一个参数：build-shared。
--enable-static就没有用了。

build-shared这个变量就用得广泛了，几乎每个子Makefile里面都有。我随机选择了10个
看内容，大部分都是这种类型：::

  ifeq (yes,$(build-shared))
  $(addprefix $(objpfx),$(tests)): $(objpfx)libcrypt.so
  else
  $(addprefix $(objpfx),$(tests)): $(objpfx)libcrypt.a
  endif

而且我看到都的都是和生成test文件有关的，这个东西很难说会有问题还是没有问题。关
键是我觉得定位出来也没有价值。我的判断是干脆在没有弄gcc -shared -fPIC前，不值得
弄这个。

kernel的问题
------------

立福说仅仅修改readl和writel的原子性，这个问题就不报错了，这和我的设想不一致，我
要去看看为什么。

首先，去掉那个原子块定义后，那个地方不出错了，我打印了一下内容：::

  linx_debug(101, 0x0) info: plic_toggle ffffffd000002080

和前面的地址是一样的，也就是说，都是访问一样的地址，原子块的访问指令有错，而普
通gen出来的代码没有错。

跟踪过去，发现这个调用过程是这样的：::

  cpu_stb_data()
    +->cpu_stb_data_ra()
         +->cpu_stb_mmuidx_ra()
              +->cpu_stb_mmu()
                   +->cpu_store_helper()
                        +->store_helper()

最后这个实现会根据不同的目标地址进行不同的处理，而我们那个翻转plic寄存器是个IO，
是不允许字节操作的，而原子块对它进行了一个一个字节的单字节操作……所以，就不用我
说了吧？

softlockup问题
--------------

去掉几个qemu几个错判的assert后，我终于可以开始调试两周前的softlockup问题了。下
面是一组观察：

1. switch_to的后面不能加跟踪点，这时还没有完全恢复上一个上下文，要等返回的，所
   以在switch_to前面做跟踪动作。

2. 几乎所有的线程的preempt_count()在初始化阶段都是2，都是抢占不了的。

3. 我没有想过初始化阶段有怎么多线程的：现在看见的有：swaper, kthreadd, kworker,
   mm_percpu_wq，rcu_tasks_trace，ksoftirqd，kdevtmpfs，netns。但从来没有看到有
   切换到stop里面去的，也许这说明了为什么没有人喂狗了，因为内核阶段根本就没有喂
   狗的人，内核不认为自己启动的过程要那么久的。

4. 全程开中断的时间不多，大部分切换都是主动切换，比如do_raw_spin_unlock()接一个
   try_to_wake_up()引起的调度。

如果不softlock，会随机死在几个位置（好像都和printf相关的函数有关，我总觉得是不
是和堆栈太高有关，这种问题不好查，考虑先对比一下出问题的时候sp和栈顶）：::

  [    8.085436] Unable to handle kernel paging request at virtual address ffffffff814abdb8
  [    8.115942] Oops [#1]
  [    8.126097] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g834fc209d4f8-dirty #40
  [    8.150285] Hardware name: riscv-virtio,qemu (DT)
  [    8.164302] epc : vsnprintf+0x0/0x940
  [    8.180952]  ra : add_uevent_var+0x40/0x1e0
  [    8.199715] epc : ffffffff814c6c20 ra : ffffffff814abd40 sp : ffffffe003233820
  [    8.220488]  gp : ffffffff82ee9060 tp : ffffffe003248000 t0 : ffffffe003208dc0
  [    8.241303]  t1 : 62d47fb8b0dd3100 t2 : 000000000000009f s0 : ffffffe003233860
  [    8.262099]  s1 : ffffffe003219000 a0 : ffffffe00321926d a1 : 00000000000007af
  [    8.282911]  a2 : ffffffff82a543a0 a3 : ffffffe003233860 a4 : 62d47fb8b0dd3100
  [    8.303816]  a5 : 000000000000003f a6 : ffff0a00ffffff04 a7 : 0000000000000228
  [    8.324302]  s2 : 0000000000000800 s3 : ffffffe00321a000 s4 : ffffffff82eea0a8
  [    8.344926]  s5 : ffffffff829c8c98 s6 : 0000000000000000 s7 : ffffffe003219000
  [    8.365426]  s8 : ffffffff828e6880 s9 : ffffffe003208d80 s10: ffffffff829f36b0
  [    8.386056]  s11: 0000000000000000 t3 : 0000000000008000 t4 : 0000000000000402
  [    8.406221]  t5 : ffffffff82e82420 t6 : ffffffff82e82440
  [    8.422312] status: 8000000000006120 badaddr: ffffffff814abdb8 cause: 000000000000000f
  [    8.444025] ebstate.st: 0000000000000401
  [    8.456466]  bstate.r0  : ffffffff82252000 bstate.r1  : ffffffe003233810
  [    8.476792]  bstate.r2  : ffffffe003219000 bstate.r3  : ffffffe003233818
  [    8.496999]  bstate.r4  : ffffffe003233790 bstate.r5  : ffffffff814abd40
  [    8.517196]  bstate.r6  : ffffffe003233818 bstate.r7  : 0000000000000090
  [    8.537284]  bstate.r8  : ffffffe003233820 bstate.r9  : 0000000000000000
  [    8.557300]  bstate.r10 : 0000000000000000 bstate.r11 : ffffffe003233790
  [    8.577472]  bstate.r12 : 0000000000000000 bstate.r13 : 0000000000000000
  [    8.597456]  bstate.r14 : 0000000000000000 bstate.r15 : 0000000000000000
  [    8.617462]  bstate.r16 : 0000000000000000 bstate.r17 : ffffffe003233820
  [    8.637657]  bstate.r18 : 0000000000000000 bstate.r19 : 0000000000000000
  [    8.657660]  bstate.r20 : 0000000000000000 bstate.r21 : 0000000000000000
  [    8.677653]  bstate.r22 : 0000000000000000 bstate.r23 : 0000000000000000
  [    8.697656]  bstate.r24 : 0000000000000000 bstate.r25 : 0000000000000000
  [    8.717694]  bstate.r26 : 0000000000000000 bstate.r27 : 0000000000000000
  [    8.737685]  bstate.r28 : 0000000000000000 bstate.r29 : 0000000000000000
  [    8.757656]  bstate.r30 : 0000000000000000 bstate.r31 : 0000000000000000
  [    8.777632]  bstate.r32 : 0000000000000000 bstate.r33 : 0000000000000000
  [    8.797673]  bstate.r34 : 0000000000000000 bstate.r35 : 0000000000000000
  [    8.817670]  bstate.r36 : 0000000000000000 bstate.r37 : 0000000000000000
  [    8.837649]  bstate.r38 : 0000000000000000 bstate.r39 : 0000000000000000
  [    8.857661]  bstate.r40 : 0000000000000000 bstate.r41 : 0000000000000000
  [    8.878063] [<ffffffff814c6c20>] vsnprintf+0x0/0x940
  [    8.897787] [<ffffffff8111c760>] of_device_uevent+0x60/0x320
  [    8.919263] [<ffffffff80de7bc0>] dev_uevent+0xe0/0x4a0
  [    8.940458] [<ffffffff814ac400>] kobject_uevent_env+0x520/0xe80
  [    8.963276] [<ffffffff814ad780>] kobject_uevent+0x20/0x40
  [    8.984788] [<ffffffff80deb140>] device_add+0xa20/0x1820
  [    9.006423] [<ffffffff8111ba20>] of_device_add+0x60/0xe0
  [    9.026824] [<ffffffff8111d8e0>] of_platform_device_create_pdata+0x140/0x220
  [    9.051812] [<ffffffff8111dda0>] of_platform_bus_create+0x3e0/0x5c0
  [    9.075073] [<ffffffff8111de60>] of_platform_bus_create+0x4a0/0x5c0
  [    9.098374] [<ffffffff8111e260>] of_platform_populate+0x80/0x1e0
  [    9.121249] [<ffffffff82438fe0>] of_platform_default_populate_init+0x180/0x1c0
  [    9.146220] [<ffffffff82402560>] do_one_initcall+0xe0/0x300
  [    9.167485] [<ffffffff82402c80>] kernel_init_freeable+0x3e0/0x500
  [    9.190100] [<ffffffff814ed4e0>] kernel_init+0x30/0x2c0
  [    9.211237] [<ffffffff80003500>] ret_from_exception+0x0/0x40
  [    9.239705] ---[ end trace 80f479f59189f3e4 ]---
  [    9.261601] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
  [    9.283719] ---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b ]---

第二个：BUG_ON：lib/kasprintf.c:30。

第三个：::

  Trace-XX 0(09:29:23): 0x7efe88100540 [0000000000000000/ffffffff814bdb80/00004201/ff000000] format_decode
  Trace-XX 0(09:29:23): 0x7efe881009c0 [0000000000000000/ffffffff8224c000/00004201/ff000000] 
  riscv_cpu_do_interrupt: hart:0, async:0, cause:000000000000000c, epc:0x0000000000000000, tval:0x0000000000000000, desc=exec_page_fault

下周再查吧。

20220613
========

周六其他人关掉块内中断可以跑到用户态，所以今天我重点看看块内中断发生了什么：::

  ------------- CS_OUT(0): Addr(ffffffff81a2d104=>ffffffff80003180, ffffffffffffffff) Priv(1=>1) for `s_timer`
    BState Saved. BPC/TPC=ffffffff8242dbc0/ffffffff81a2d104
    GPRS:          0000000000000000 ffffffff8242dbc0 ffffffff82e03f40 ffffffff82ee9060 ffffffff82e09480 ffffffe00320af40 ffffffe00320ac00 0000000000000001 ffffffff82e03f70 ffffffe00320ac00 0000000000000001 0000000000000000 ffffffe003218fa0 ffffffe003218000 0000000000000000 0000000000000000 ffffffe00320af20 ffffffff8283f298 0000000000000800 ffffffff82f24128 ffffffff829f3318 0000000000000000 ffffffff82eea018 ffffffff82e09160 ffffffff829df6a8 000000000000007f 0000000000000000 0000000000000000 0000000000000000 0000000000000402 0000000000000000 0000000000000000 
    BPC/TPC:       ffffffffffffffff ffffffff80003180
    EBSTATE.ST: 401
    SBPC:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
  ...
  ------------- CS_IN(0) Addr (ffffffff8150287e=>ffffffff8242dbc0, ffffffff800035c0) Priv(1=>1) 
    GPRS:          0000000000000000 ffffffff8242dbc0 ffffffff82e03f40 ffffffff82ee9060 ffffffff82e09480 ffffffe00320af40 ffffffe00320ac00 0000000000000001 ffffffff82e03f70 ffffffe00320ac00 0000000000000001 0000000000000000 ffffffe003218fa0 ffffffe003218000 0000000000000000 0000000000000000 ffffffe00320af20 ffffffff8283f298 0000000000000800 ffffffff82f24128 ffffffff829f3318 0000000000000000 ffffffff82eea018 ffffffff82e09160 ffffffff829df6a8 000000000000007f 0000000000000000 0000000000000000 0000000000000000 0000000000000402 0000000000000000 0000000000000000 
    BPC/TPC:       ffffffff800035c0 ffffffff8150287e   <----- 恢复时的TPC没有对上，这需要确定一下是不是打印的问题（这个可能性比较高）
    EBSTATE.ST: 401
    SBPC:          0000000000000000
    BSTATE.SGPRS:  0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    BSTATE.TREGS:  0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 

看了一个实例，后面太多了，还是用以前的find_switch脚本看（脚本的Pattern需要简单
修改一下，我更新了），没有找到中断以后发生切换的情形，看来这个问题不需要发生内
部切换也会错。

对于Softlockup的问题，我试着关掉-d exec，也关掉块内中断，这可以顺利跑到用户态，
看来这个softlockup的问题还是原来那个字符串处理速度慢的问题，可能不需要查。

但在用户态会出这个错误：::

  buildroot login: root
  [   17.025861] sh[52]: unhandled signal 11 code 0x2 at 0x0000003fe9cc7300
  [   17.026789] CPU: 0 PID: 52 Comm: sh Not tainted 5.16.0-rc3-gdaccf01344c9 #42
  [   17.027563] Hardware name: riscv-virtio,qemu (DT)
  [   17.028054] epc : 0000003fe9cc7300 ra : 0000003fe9cc7300 sp : 0000003fe9cc6ec0
  [   17.029054]  gp : 0000002acc153d80 tp : 0000003fb4fd2440 t0 : 0000000008000000
  [   17.029831]  t1 : 0000002acc074d7c t2 : 0000002acc14d1df s0 : 0000000000000001
  [   17.030556]  s1 : 0000000000000001 a0 : 0000000000000011 a1 : 0000003fe9cc6ec0
  [   17.031269]  a2 : 0000003fe9cc6f40 a3 : 0000000000000000 a4 : 0000000000000000
  [   17.031998]  a5 : 0000002acc1552a0 a6 : 31304e4d4c506d6c a7 : 0000000000000104
  [   17.032722]  s2 : 0000002acc158eb0 s3 : 0000002acc1552a0 s4 : 0000000000000002
  [   17.033824]  s5 : 0000000000000001 s6 : 0000003fe9cc7350 s7 : 0000003fb5004200
  [   17.034566]  s8 : 0000000000000001 s9 : 0000002acc153eb8 s10: 0000003fe9cc734c
  [   17.035279]  s11: 0000000000000004 t3 : 0000003fb4f10774 t4 : 0000002acc12e910
  [   17.036006]  t5 : 0000000000000000 t6 : 0000000000000021
  [   17.036561] status: 0000000000000020 badaddr: 0000003fe9cc7300 cause: 000000000000000c
  [   17.037555] ebstate.st: 0000000000000400

11是segment fault，cause=12，是Instruction Page Fault。我认为这最有可能的是：访
问vdso没有访问到。

那就先不管那个了，等其他人去弄，我专心看这个调度过程。整体上，我要专项跟踪切出，
切入中，切入完成三个位置的CPU状态，是不是完全一致的。先加更新代码再说。

加代码中我觉得当前实现对TPC的定义还是很模糊，我来尝试建一个模：

1. 首先PC总是当前要翻译的地址，而RV的上下文包含这个状态了。PC的更新不是实时的，
   而是在每个TB结束以后人为更新一次的。在执行的时候如果要使用pc，通常不从env读，
   而是在翻译的时候根据翻译地址直接决定，因为翻译的时候其实知道当时被翻译指令
   所在的PC，不需要从env动态知道。
2. TPC是一个内部状态，而且，和PC一样，不能随着执行自动更新。
3. 在任意一个CPU静止的时间中（也就是离开TB执行的时候），TPC的值要用如下方法同步：

   1. 如果PC在TPC1/2之间，TPC就是PC，必须用PC更新

   2. 如果PC不在TPC1/2之间，说明前一个块已经结束，这时TPC应该为无效，如果不是无
      效，说明前一个块要求下一个块重启，这时TPC的状态就是它本身的值的状态，这时
      BPC应该为无效（因为BPC必须在识别到一个块的时候设置为有效，而块终止的时候
      设置为无效）

4. 在运行中，引用TPC，必须直接使用PC（也就是静态的编译地址），比如ADDTPC指令执
   行的时候。

代码执行，我们永远有唯一的状态去决定CPU要怎么做：

0. CPU永远基于当前给定的状态启动一条指令。我们再增加一条约束：commit阶段不可中
   断和异常，所以，最后一条微指令执行成功，commit阶段一定成功。
1. BPC和TPC都无效，则启动一条新指令或者一个新块。（断言：BPC有效，则TPC1/2必然
   有效，其他块头数据也必然有效。）
2. BPC无效，但TPC有效，说明下一个块的TPC是恢复的，不是重头开始的。译码还是从PC按
   前一个规则译码，只是遇到块的时候，块的启动逻辑不一样，TPC用已有的值，而不是
   根据bpc1来设置。TPC增加一个功能，如果它等于新块的BPC，则重新执行它的CARG
   Delay行为，这种情况，所有的针对块头对这个CARG的约束继续生效。
3. BPC有效，译码地址在TPC1/2之间，则执行微指令。（执行的时候PC和TPC都不可靠，但
   进入译码的时候，译码地址就是PC）

所以没有什么如果前面是sret，就把carg设置为XX这种说法。是我们的代码希望把CPU设置
到什么状态，就按规则设置状态，不要考虑执行历史。

最后补充一个carg的模型：

1. carg表示commit阶段剩下还没有做的事情，它和header的属性共同起作用。
2. 如果header是跳转类型，Commit阶段只能剩下寄存器提交和跳转两个行为，CARG用作跳
   转参数，没有其他功能。
3. 如果header不是跳转类型，carg表示剩余还没有做的工作。这种工作现阶段只能有一个
   （未来可能更多），这些工作有可能会异常，但异常只能发生在所有状态已经提交之后。

看王州是否同意这个逻辑，如果同意，我们需要调整一次逻辑。

这之前，我来对甜根和立福提出来的一个新问题建模。Linux有一种叫fixup的代码处理模
式，比如在uacce.S中，它会先把内核态设置为不可访问，然后读用户态的内存，读到边界
的地方，遇到异常的时候就缩小访问范围，一步步收缩，最后收缩到最小的空间。

这种情况下，如果用块来实现这个功能，如果一个块异常了，我们需要把中间结果提交了，
然后放弃这个块。然后用缩小的范围来访问这个块。其实严格来说，我们完全可以做一样的
事情，因为内存访问不是最后提交的，而是当场生效的，只是寄存器没有提交，但寄存器
的结果可以在ebstate里面约定就可以了，所以如果完全用一样的方法来做，虽然有点难看，
但其实是搞得定的。

但我们能不能用块本身的特点来解决这样的问题？我们块内的指令一次访问8字节，发现太
大了，缺页，我们希望变成4字节访问，再大了，缺页，我们变成2字节，如此类推。

用块的思维，这种情况最优的约束方法好像是：我们准备几个块，不同块有不同的入口参
数（比如从哪里开始继续拷贝），用不同的大小来访问内存，一旦缺页了，调整入口参数，
切换到不同的块入口继续。这也是前面这个思路。也许这个思路就是这个问题的解答了？

20220614
========

今天立福提供了另一个块内有条件恢复的例子，他的描述如下：::

  再提供一种将来的场景kprobe，块内异常，跳出异常块后：

  1）需要异常块的上下文信息在新块执行，新块结束后回到异常块跳过异常指令从下一条
     指令继续执行；或者
  2）需要异常块的上下文信息在新块执行，新块结束后回到异常块从异常位置指令（该位
     置被重写了）继续执行。

  kprobe第一种情况：微指令被ebreak替换（kprobe的实现机制），执行到这条指令之后，
  块内异常跳出块，在新块执行kprobe逻辑之后，需要执行一遍原指令。原指令可以通过
  模拟的方式执行（例如set gpr可以模拟执行），可以通过out-of-line的方式在新的位
  置执行（例如sysset csr需要在新块执行），执行原指令时，需要用到异常块的未提交
  的上下文信息，并跳过该微指令。（这里会带来另外一个新的问题）第2种情况：异常逻
  辑跟第一种一样，只是kprobe逻辑完成后，将原指令写回原位置，回到异常块内继续执
  行原指令。当然，这里跟kprobe的实现方案有关系，从块的逻辑来看也许有新的实现方
  案，但当前的理解会有这样的情况。---上面提到的新问题，异常指令被跳过，如果异常
  指令是块内的最后一条指令，跳过该指令会导致tpc超出了异常块的body范围，回到异常
  块执行时，如果根据tpc是否在原块的body范围，可能会导致判断错误。

我觉得这是个很好的架构例子，说明为什么不要用代码逻辑去说话，而要用人的逻辑去说
话。ebreak是现在的实现，不表示我们就得这样实现，这个地方的原始意图其实就是一个
gdb的行为：设置断点，遇到断点后执行断点行为，然后恢复或者不恢复断点行为。

然后把昨天的模型优化和调试了一下，看来可以正常运行到用户态，我先上传一个版本吧

20220615
========

今天上午主要写专利了，下午重点处理两个问题：

1. 分析qemu-user的fork到底是怎么工作的。
2. 分析kernel的fixup代码在块指令的场景中怎么实现的问题。

先看fork的问题，用户态所有系统调用都被接管为TB执行的跳出，然后根据设定的异常类
型调用host的真正系统调用，在重新设置CPU状态，把结果返回回去。fork也一样，实现在
do_fork中，但它相比其他系统调用复杂一点，因为fork有可能导致产生新的线程，而新的
线程需要创建新的CPUState去管理。

所以do_fork首先判断了CLONE_VM标记（表示是否创建线程），如果是创建线程，那就不做
fork了，马上拷贝一份原来的CPUState，直接用pthread创建线程，然后用这个线程做CPU
循环。

如果没有CLONE_VM标记，这反而简单了，就真的clone一个进程出来，父子进程都拿着之前
的上下文继续回到TB循环里面继续模拟就好了。

而qemu的原子执行有两个模拟方式，一个是真的去调Host的原子操作，这是没有问题的，
另一个是通过停下其他模拟CPU，再独占执行相关过程。这在没有CLONE_VM的fork操作下，
是没有原子操作的效果的，因为你没法停止另一个进程中的CPU。实在要做，除非fork以后
创建一个进程间互相协同的机制，保证锁上了再开始工作。这是昨天会议王州表达的无法
用fork测试原子行为的实际意思。但其实我们可以用CLONE_VM标记来测试。

再看第二个问题，RV的fixup宏是这样写的：::

  定义：
	  .macro fixup op reg addr lbl
  100:
	  \op \reg, \addr                <-- 实际执行给定的指令
	  .section __ex_table,"a"        <-- 放一个表项到__ex_table中
	  .balign RISCV_SZPTR
	  RISCV_PTR 100b, \lbl           <-- 实际代码的地址+宏指定的终结地址
	  .previous                      <-- 恢复原来的段
	  .endm
  使用：
	  fixup lb      a5, 0(a1), 10f   <-- 执行lb a5, 0(a1)，保存本指令和10f的地址到表格中

vmlinux.lds用这两个变量访问__ex_table：__start___ex_table和__stop__ex_table。

todo: 这个地方没有完成分析。

然后我试着静态编译buildroot，看看能否正常工作，发现问题是一样的，我启动了一个跟
踪，发现是这个指令导致的：::

  riscv_cpu_do_interrupt: hart:0, async:0, cause:0000000000000009, epc:0xffffffff80008940, tval:0x0000000000000000, desc=supervisor_ecall
  linx_debug(201, 0x0) info: context switch from init(2) to S01syslogd(2) <-- 切换到出错的线程
  linx_debug instruction: attr=0x220, para=0x0
  riscv_cpu_do_interrupt: hart:0, async:0, cause:000000000000000f, epc:0xffffffff8226425e, tval:0x0000003fd5605d30, desc=store_page_fault <- 这里现有一个执行错误，但我没有跟踪内核
  ----------------
  IN: 
  Priv: 0; Virt: 0
  0x000000000004f4a4:  01100713          addi            a4,zero,17   <-------- 这是signal_handler的入口
  0x000000000004f4a8:  b701b783          ld              a5,-1168(gp)
  0x000000000004f4ac:  02e50263          beq             a0,a4,36        # 0x4f4d0
  
  Trace-XX 0(10:03:11): 0x7fcfb6d0de40 [0000000000000000/000000000004f4a4/00004200/ff000000] 
  ----------------
  IN: 
  Priv: 0; Virt: 0
  0x000000000004f4d0:  00100713          addi            a4,zero,1
  0x000000000004f4d4:  04e7a423          sw              a4,72(a5)
  0x000000000004f4d8:  1707b703          ld              a4,368(a5)
  0x000000000004f4dc:  fc071ae3          bnez            a4,-44          # 0x4f4b0
  
  Linking TBs 0x7fcfb6d0de40 [000000000004f4a4] index 0 -> 0x7fcfb6d0e000 [000000000004f4d0]
  Trace-XX 0(10:03:11): 0x7fcfb6d0e000 [0000000000000000/000000000004f4d0/00004200/ff000000] 
  ----------------
  IN: 
  Priv: 0; Virt: 0
  0x000000000004f4e0:  00008067          ret             <--- 这是触发错误的位置，它自己的地址是对的，但返回地址是0x0000003fd5606170，这是个非法地址。
  
  Linking TBs 0x7fcfb6d0e000 [000000000004f4d0] index 1 -> 0x7fcfb6d0e200 [000000000004f4e0]
  Trace-XX 0(10:03:11): 0x7fcfb6d0e200 [0000000000000000/000000000004f4e0/00004200/ff000000] 
  riscv_cpu_do_interrupt: hart:0, async:0, cause:000000000000000c, epc:0x0000003fd5606170, tval:0x0000003fd5606170, desc=exec_page_fault
  riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000007, epc:0xffffffff81573f44, tval:0x0000000000000000, desc=m_timer
  riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000005, epc:0xffffffff81573f44, tval:0x0000000000000000, desc=s_timer
  riscv_cpu_do_interrupt: hart:0, async:0, cause:0000000000000009, epc:0xffffffff80008940, tval:0x0000000000000000, desc=supervisor_ecall

查buildroot的代码，这个函数似乎是act.sa_handler设置的异常入口，异常的设置函数是
./busybox-1.34.1/libbb/signals.c:sigaction_set()，最终是调用
sigaction()->__libc_sigaction()->__syscall_rt_sigaction()。这个uclibc里面有一个实现：这最
终是个系统调用：::

  #define __NR___syscall_rt_sigaction __NR_rt_sigaction
  _syscall4(int, __syscall_rt_sigaction, int, signum,
	    const struct sigaction *, act, struct sigaction *, oldact,
	    size_t, size)
  #endif


这看起来像是想回到vdso上啊，我再看看内核vdso的代码逻辑：

在RV上，vdso的配置是这样的：::

  obj-$(CONFIG_MMU) += vdso.o vdso/

其中vdso.c负责利用一个全局的vdso.info提供vdso首尾地址（符号），初始化一个
pagelist。而在vdso目录中，除了编译vdso.so的文件外，主要就是vdso.S，它做的
是个无条件的包含动作：::

  ;vdso.S
	  .globl vdso_start, vdso_end
	  .balign PAGE_SIZE
  vdso_start:
	  .incbin "arch/riscv/kernel/vdso/vdso.so"
	  .balign PAGE_SIZE
  vdso_end:

所以如果我们不打算搞定这个vdso的话，这个地方其实可以强行用一个RV编译好的vdso.so
来搞定的。

20220616
========

继续昨天vdso的问题，今天我们开了晨会，甜根提供了另一个逻辑，实际上，内核的do_signal()->
handle_signal()逻辑是在返回前用setup_rt_frame()创建返回堆栈，而信号处理的代码是
在这个堆栈里面现填的。所以解决vdso不解决这个问题，需要调整这里的逻辑。

换成甜根的实现，也没解决问题，表现是这样的：::

  linx_debug(102, 0x0) info: copy return code 8b00893 73 to 3ff19844f0
  riscv_cpu_tlb_fill ad ffffffff807055c0 rw 2 mmu_idx 1
  page table SV39 walk level 0: pte(83e93ff0)=23fff801, two_state=0, first_state=1, base=83e93000, bg=0, vm=8
  page table SV39 walk level 1: pte(8fffe018)=202000eb, two_state=0, first_state=1, base=8fffe000, bg=0, vm=8
  ...
  riscv_cpu_tlb_fill ad 3ff19844f0 rw 1 mmu_idx 1
  page table SV39 walk level 0: pte(83e937f8)=20fa9c01, two_state=0, first_state=1, base=83e93000, bg=0, vm=8
  page table SV39 walk level 1: pte(83ea7c60)=20fa8801, two_state=0, first_state=1, base=83ea7000, bg=0, vm=8
  page table SV39 walk level 2: pte(83ea2c20)=209bbcd7, two_state=0, first_state=1, base=83ea2000, bg=0, vm=8
  riscv_cpu_tlb_fill address=3ff19844f0 ret 0 physical 00000000826ef4f0 prot 3
  ...
  riscv_cpu_tlb_fill ad 3ff19844f0 rw 2 mmu_idx 0
  page table SV39 walk level 0: pte(83e937f8)=20fa9c01, two_state=0, first_state=1, base=83e93000, bg=0, vm=8
  page table SV39 walk level 1: pte(83ea7c60)=20fa8801, two_state=0, first_state=1, base=83ea7000, bg=0, vm=8
  page table SV39 walk level 2: pte(83ea2c20)=209bbcd7, two_state=0, first_state=1, base=83ea2000, bg=0, vm=8
  riscv_cpu_tlb_fill address=3ff19844f0 ret 1 physical 0000000000000000 prot 0
  riscv_cpu_do_interrupt: hart:0, async:0, cause:000000000000000c, epc:0x0000003ff19844f0, tval:0x0000003ff19844f0, desc=exec_page_fault

唯一有区别的是mmu_idx不同，这个在实现上等于特权级，同时Walk过程显示pte也拿到了，
只是权限只有读写，不能执行。但这是合理的呀，堆栈本来就不能执行。RV怎么会用这种
手段的？

重新看代码，发现其实RV也不能执行，RV根本不是用这种方法解决的，甜根只是用了NOMMU
的一个算法，RV是靠VDSO来解决的。

这样就没有什么好说的了，还是用VDSO吧，我认真看了一下实现，发现这个vdso其实原来很简单：

1. .incbin把整个二进制包进去
2. 用脚本生成每个符号的地址，写在头文件中
3. 把vdso映射到用户态

简单调整了一下，现在就可以正常启动到RV的用户态了，很多命令也可以正常运行。

但部分后台服务会报这样一些错误：::

  Starting syslogd: OK
  Starting klogd: OK
  [   11.045312] printk: klogd (53): Attempt to access syslog with CAP_SYS_ADMIN but no CAP_SYSLOG (deprecated).
  Running sysctl: OK
  [   18.819443] BUG: scheduling while atomic: klogd/53/0x00000000
  [   18.821160] CPU: 0 PID: 53 Comm: klogd Not tainted 5.16.0-rc3-g882a19a9d7e7-dirty #58
  [   18.822555] Hardware name: riscv-virtio,qemu (DT)
  [   18.823612] Call Trace:
  [   18.824530] [<ffffffff80006680>] dump_backtrace+0x20/0x40
  [   18.826127] [<ffffffff814f5850>] dump_stack+0x40/0x60
  [   18.827302] [<ffffffff800676e0>] __schedule_bug+0xe0/0x120
  [   18.828448] [<ffffffff814f7020>] __schedule+0x670/0x7c0
  [   18.829608] [<ffffffff814f7220>] schedule+0x70/0x1b0
  [   18.830678] [<ffffffff80003500>] ret_from_exception+0x0/0x40
  Saving random seed: [   21.306109] random: dd: uninitialized urandom read (512 bytes read)
  OK
  Starting network: OK
  Starting dhcpcd...
  dhcpcd-9.4.1 starting
  bget/bset error, bget/bset: 0x00000004/0x00000002, should be: 0x00000004/0x00000402, bpc: 0xffffffff800036e0
  [   26.107220] Oops - illegal instruction [#1]
  [   26.108806] CPU: 0 PID: 79 Comm: dhcpcd Tainted: G        W         5.16.0-rc3-g882a19a9d7e7-dirty #58
  [   26.110172] Hardware name: riscv-virtio,qemu (DT)
  [   26.111058] epc : handle_syscall_trace_exit+0x0/0x40
  [   26.112354]  ra : ret_from_syscall+0x0/0x20
  [   26.113370] epc : ffffffff800036e0 ra : ffffffff800034c0 sp : ffffffe003c83d80
  [   26.114591]  gp : ffffffff82eec060 tp : ffffffe003ce1280 t0 : 00000000000001c1
  [   26.115813]  t1 : 0000000000000020 t2 : ffffffff82808ed4 s0 : ffffffff80040580
  [   26.117022]  s1 : 0000000000000020 a0 : 0000000000000000 a1 : 000000000000006e
  [   26.118226]  a2 : ffffffe003cacb70 a3 : 0000000000000006 a4 : ec05799c2b3f2e00
  [   26.119437]  a5 : ec05799c2b3f2e00 a6 : 0000000000000001 a7 : 0000000000000004
  [   26.120646]  s2 : 0000000000045110 s3 : 0000000000000000 s4 : 0000000000000008
  [   26.121887]  s5 : 0000000000070da0 s6 : 0000000000118db0 s7 : 0000000000072030
  [   26.123097]  s8 : 000000000006b000 s9 : 000000000005bc88 s10: 000000000005b228
  [   26.124309]  s11: 0000000000209800 t3 : 000000000000006f t4 : 0000000000000025
  [   26.125510]  t5 : ffffffe003cac800 t6 : 00000000000001c1
  [   26.126459] status: 0000000000000120 badaddr: 0000000007ef5c8b cause: 0000000000000002
  [   26.127726] ebstate.st: 0000000000000400
  [   26.128576] [<ffffffff800036e0>] handle_syscall_trace_exit+0x0/0x40
  [   26.131403] ---[ end trace 03aa0f52ee0ce952 ]---
  Segmentation fault
  [   28.153051] random: dhcpcd: uninitialized urandom read (112 bytes read)
  [   28.341449] Oops - illegal instruction [#2]
  [   28.343079] CPU: 0 PID: 84 Comm: dhcpcd Tainted: G      D W         5.16.0-rc3-g882a19a9d7e7-dirty #58
  [   28.344651] Hardware name: riscv-virtio,qemu (DT)
  [   28.345476] epc : handle_syscall_trace_exit+0x0/0x40
  [   28.346758]  ra : ret_from_syscall+0x0/0x20
  [   28.347707] epc : ffffffff800036e0 ra : ffffffff800034c0 sp : ffffffe003c93d80
  [   28.348904]  gp : ffffffff82eec060 tp : ffffffe003ce0000 t0 : 00000000000001c1
  [   28.350102]  t1 : 0000000000000020 t2 : ffffffff82808ed4 s0 : ffffffff80040580
  [   28.351287]  s1 : 0000000000000020 a0 : 0000000000000000 a1 : 000000000000006e
  [   28.352461]  a2 : ffffffe003cac370 a3 : 0000000000000006 a4 : ec05799c2b3f2e00
  [   28.353647]  a5 : ec05799c2b3f2e00 a6 : 0000000000000001 a7 : 0000000000000004
  [   28.354831]  s2 : 0000000000045110 s3 : 0000000000000000 s4 : 0000000000000008
  [   28.356024]  s5 : 0000000000070da0 s6 : 0000000000118db0 s7 : 0000000000072030
  [   28.357208]  s8 : 000000000006b000 s9 : 000000000005bc88 s10: 000000000005b228
  [   28.358391]  s11: 0000000000209800 t3 : 000000000000006f t4 : 0000000000000025
  [   28.359565]  t5 : ffffffe003cac000 t6 : 00000000000001c1
  [   28.360495] status: 0000000000000120 badaddr: 0000000007ef5c8b cause: 0000000000000002
  [   28.361754] ebstate.st: 0000000000000400
  [   28.362515] [<ffffffff800036e0>] handle_syscall_trace_exit+0x0/0x40
  [   28.365346] ---[ end trace 03aa0f52ee0ce953 ]---
  [   28.727246] Oops - illegal instruction [#3]
  [   28.728528] CPU: 0 PID: 85 Comm: dhcpcd Tainted: G      D W         5.16.0-rc3-g882a19a9d7e7-dirty #58
  [   28.729735] Hardware name: riscv-virtio,qemu (DT)
  [   28.730559] epc : handle_syscall_trace_exit+0x0/0x40
  [   28.731823]  ra : ret_from_syscall+0x0/0x20
  [   28.732767] epc : ffffffff800036e0 ra : ffffffff800034c0 sp : ffffffe003d2fd80
  [   28.733970]  gp : ffffffff82eec060 tp : ffffffe003ce2e40 t0 : 00000000000001c1
  [   28.735157]  t1 : 0000000000000020 t2 : ffffffff82808ed4 s0 : ffffffff80040580
  [   28.736334]  s1 : 0000000000000020 a0 : 0000000000000000 a1 : 000000000000006e
  [   28.737516]  a2 : ffffffe003cad370 a3 : 0000000000000006 a4 : ec05799c2b3f2e00
  [   28.738717]  a5 : ec05799c2b3f2e00 a6 : 0000000000000001 a7 : 0000000000000004
  [   28.739902]  s2 : 0000000000045110 s3 : 0000000000000000 s4 : 0000000000000008
  [   28.741096]  s5 : 0000000000070da0 s6 : 0000000000118db0 s7 : 0000000000072030
  [   28.742288]  s8 : 000000000006b000 s9 : 000000000005bc88 s10: 000000000005b228
  [   28.743474]  s11: 0000000000209800 t3 : 000000000000006f t4 : 0000000000000025
  [   28.744660]  t5 : ffffffe003cad000 t6 : 00000000000001c1
  [   28.745592] status: 0000000000000120 badaddr: 0000000007ef5c8b cause: 0000000000000002
  [   28.746877] ebstate.st: 0000000000000400
  [   28.747626] [<ffffffff800036e0>] handle_syscall_trace_exit+0x0/0x40
  [   28.750337] ---[ end trace 03aa0f52ee0ce954 ]---
  [   29.377399] Oops - illegal instruction [#4]
  [   29.378951] CPU: 0 PID: 81 Comm: dhcpcd Tainted: G      D W         5.16.0-rc3-g882a19a9d7e7-dirty #58
  [   29.380509] Hardware name: riscv-virtio,qemu (DT)
  [   29.381336] epc : handle_syscall_trace_exit+0x0/0x40
  [   29.382601]  ra : ret_from_syscall+0x0/0x20
  [   29.383548] epc : ffffffff800036e0 ra : ffffffff800034c0 sp : ffffffe003d33d80
  [   29.384749]  gp : ffffffff82eec060 tp : ffffffe003ce2500 t0 : 00000000000001c1
  [   29.385944]  t1 : 0000000000000020 t2 : ffffffff82808ed4 s0 : ffffffff80040580
  [   29.387134]  s1 : 0000000000000020 a0 : 0000000000000000 a1 : 000000000000006e
  [   29.388307]  a2 : ffffffe003cac370 a3 : 0000000000000006 a4 : ec05799c2b3f2e00
  [   29.389493]  a5 : ec05799c2b3f2e00 a6 : 0000000000000001 a7 : 0000000000000004
  [   29.390664]  s2 : 0000000000045110 s3 : 0000000000000000 s4 : 0000000000000008
  [   29.391842]  s5 : 0000000000070da0 s6 : 0000000000118db0 s7 : 0000000000072030
  [   29.393023]  s8 : 000000000006b000 s9 : 000000000005bc88 s10: 000000000005b228
  [   29.394195]  s11: 0000000000209800 t3 : 000000000000006f t4 : 0000000000000025
  [   29.395363]  t5 : ffffffe003cac000 t6 : 00000000000001c1
  [   29.396279] status: 0000000000000120 badaddr: 0000000007ef5c8b cause: 0000000000000002
  [   29.397542] ebstate.st: 0000000000000400
  [   29.398294] [<ffffffff800036e0>] handle_syscall_trace_exit+0x0/0x40
  [   29.400890] ---[ end trace 03aa0f52ee0ce955 ]---

20220617
========

今天定位新的状态机随机出现非法指针的问题。现象如下：::

  riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000005, epc:0xffffffff81731d78, tval:0x0000000000000000, desc=s_timer  <--- 先发生一次块内中断
  linx_save_bstate: tpc=ffffffff81731d78, carg=0, pc=ffffffff81731d78    <--- 保存状态
  riscv_cpu_do_interrupt: hart:0, async:0, cause:0000000000000009, epc:0xffffffff80008900, tval:0x0000000000000000, desc=supervisor_ecall
  riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000007, epc:0xffffffff800e91a0, tval:0x0000000000000000, desc=m_timer
  riscv_cpu_do_interrupt: hart:0, async:0, cause:0000000000000009, epc:0xffffffff80008900, tval:0x0000000000000000, desc=supervisor_ecall
  riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000007, epc:0xffffffff80008904, tval:0x0000000000000000, desc=m_timer
  linx_do_delay_insn <-- 做延迟执行
  linx_recovery_bstate_by_ebstate: tpc=ffffffff81731d78, carg=0, pc=ffffffff8150b122 <-- 恢复
  riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000005, epc:0xffffffff803851e0, tval:0x0000000000000000, desc=s_timer <-- 再发生中断
  the bstate is not this block, tpc: ffffffff81731d78, tpc1: ffffffff8150aca8, tpc2: ffffffff8150acb6 这应该是个中断向量，不能处理恢复
  riscv_cpu_do_interrupt: hart:0, async:0, cause:0000000000000002, epc:0xffffffff80003180, tval:0x0000000000000000, desc=illegal_instruction

这要检查是否中断没有正确判断这是一个正在恢复的bstate。一个bstate是有效的，不但
可以是bpc是有效，也可以是bpc无效，但tpc有效，因为这时是块没有打开，但块内状态有
效。检查中断处理没有处理bpc无效但tpc有效的场景，合理的判断应该是：只要tpc有效，
块就是有效的。这时如果bpc有效，说明块在打开的情况下中断，需要同步tpc过来才保存
状态，否则是恢复的时候中断，还没有打开块头，这时无条件保存原来的bstate就可以了。

修改有还是有错误，这次的信息是这样的：::

  linx_do_delay_insn: carg=40, bpc=ffffffff800035c0   <-- 有人做了一个sret并要求恢复状态
  sret in block ffffffff800035c0
  linx_recovery_bstate_by_ebstate: tpc=ffffffff81786acc, carg=0, pc=ffffffff8150b122, bpc=ffffffffffffffff  <-- 然后恢复了原来的bstate，这个bstate的carg=0，恢复到b122
  中间发生了一个m-mode的调用，我们略过
  riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000005, epc:0xffffffff81500fb0, tval:0x0000000000000000, desc=s_timer <-- 这时又有一个中断
  linx_save_bstate: tpc=ffffffffffffffff, carg=0, pc=ffffffff81500fb0, bpc=ffffffffffffffff <-- 再次保存
  这里又有一个m-mode调用，略过
  linx_do_delay_insn: carg=40, bpc=ffffffff800035c0 新中断又做sret
  sret in block ffffffff800035c0
  linx_recovery_bstate_by_ebstate: tpc=ffffffffffffffff, carg=0, pc=ffffffff8150b122, bpc=ffffffffffffffff <-- 又恢复刚才的bstate，地址还是要求回到b122
  riscv_cpu_do_interrupt: hart:0, async:0, cause:000000000000000f, epc:0xffffffff81786ade, tval:0x000000000000002a, desc=store_page_fault <-- 然后就异常了。

这让我感到我之前写的状态机抽象还是有毛病，让我重新整理一下：

首先我们抽象有多少种（和块相关的）激励（叶子节点是定义状态变量后补上来的）：

1. 执行一个TB，这是一种激励。这有两种可能：

   1. TB正常退出

      * riscv_tr_translate_insn: 层1给trans_blk_head；层2处理微指令，到尾提交linx_gen_blk_commit
      * trans_blk_head: 综合head和bstate，生成新状态
      * linx_gen_blk_commit：提交寄存器，有carg参数linx_do_delay_insn，复位Bstate或者设置Bstate（xret），设置pc，
      * helper_sret：

   2. TB内部异常

      * riscv_cpu_do_interrupt: bpc有效或者tpc有效，

2. 中断处理

   * riscv_cpu_do_interrupt: 

没有了，我们现在激励感觉很复杂是因为TB里面有sret，commit这些特殊的可能性，这种
每个时机按状态机语义来弄就可以了。

现在看状态变量定义

1. 首先，CPU状态是所有激励的判断基础，我们只讨论什么状态下，什么激励呈现什么行
   为，不管之前发生过什么。状态包括两部分，env的一般变量和BState变量。BState变
   量是块本身的，每个新指令执行，都基于这两部分状态。前一个刺激通过设定下一个指
   令所有合理的状态，保证下一个指令合理运行。

2. BPC决定一个块是否被启动了，如果它被启动了，BPC就会有效，其他相应的块头信息也
   都有效，包括TPC。

3. BPC无效的情况下TPC也可以有效，TPC有效表明BState有效。如果TPC有效，而BPC无效，
   这只是表明这个状态是可用的，只是使用它的块头还没有被执行。如果TPC等于BPC（这
   是按TPC正常语义不会发生的一种状态），表示这个块仅需要重启Delay行为）

4. 解码器根据BPC是否有效决定如何译码，BPC有效，则按层2译码，否则按层1译码。

按这种方法检查了一次，还是有错，真是打脸。只能一步步查了。

晚上的一些记录
--------------

晚上重整了整个LOG_CS的的打印逻辑，精准打印中断切换上下文的逻辑，看到的情况是这
样的（过滤了m-mode的跟踪）：::

  ------------- CS_OUT(0): Addr(ffffffff818e3d92=>ffffffff80003180) with bpc=ffffffff80638cc0(ffffffff818e3d92,ffffffff818e3dd4), tpc=ffffffffffffffff, Priv(1=>1) for `s_timer` <-- 中断
    GPRS:          0000000000000000 ffffffff824305e0 ffffffe003233c50 ffffffff82eec060 ffffffe003248000 ffffffe0032bd5e0 ffffffffffffffff 000000000000000d ffffffe003233c70 ffffffff82430540 ffffffff829f7478 0000000000000000 ffffffe00fa6f1a0 0000000000000002 0000000000000001 fffffffffffff000 ffffffe003499000 00000000000007de ffffffff82ea0018 0000000000000000 ffffffff82eed0a8 ffffffff82eed008 ffffffff8260f170 ffffffff82600480 ffffffff82a67a20 0000000000000008 ffffffff824000e0 0000000000000000 ffffffff82eea430 0000000000000402 ffffffe003216010 ffffffe003216030 
    EBSTATE.ST:    0000000000000400
    SBPC:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
  ------------- CS_IN(0) Addr (ffffffff8150b15e=>ffffffff8150b15e) Priv(1=>1) bstate recover
    GPRS:          0000000000000000 ffffffff824305e0 ffffffe003233c50 ffffffff82eec060 ffffffe003248000 ffffffe0032bd5e0 ffffffffffffffff 000000000000000d ffffffe003233c70 ffffffff82430540 ffffffff829f7478 0000000000000000 ffffffe00fa6f1a0 0000000000000002 0000000000000001 fffffffffffff000 ffffffe003499000 00000000000007de ffffffff82ea0018 0000000000000000 ffffffff82eed0a8 ffffffff82eed008 ffffffff8260f170 ffffffff82600480 ffffffff82a67a20 0000000000000008 ffffffff824000e0 0000000000000000 ffffffff82eea430 0000000000000402 ffffffe003216010 ffffffe003216030 
    EBSTATE.ST:    0000000000000401
    SBPC:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    block recoverred with header ffffffff80638cc0(ffffffff818e3d92, ffffffff818e3dd4), TPC=ffffffff818e3d92
  linx_debug(201, 0x0) info: context switch from swapper(2) to kdevtmpfs(2)
  linx_debug instruction: attr=0x220, para=0x0
  linx_debug(201, 0x0) info: context switch from kdevtmpfs(2) to swapper(2)
  linx_debug instruction: attr=0x220, para=0x0
    save to ebstate: tpc=ffffffff81ab1000, carg=0
  ------------- CS_OUT(0): Addr(ffffffff81ab1000=>ffffffff80003180) with bpc=ffffffff8090bc40(ffffffff81ab0ff6,ffffffff81ab106c), tpc=ffffffffffffffff, Priv(1=>1) for `s_timer`
    GPRS:          0000000000000000 ffffffff82430de0 ffffffe003233c50 ffffffff82eec060 ffffffe003248000 ffffffe0032bd5e0 ffffffffffffffff 000000000000000d ffffffe003233c70 ffffffff82430dc0 0000000000000000 00000000000007e5 ffffffe00fa6f1a0 0000000000000002 17ddb6757a8b9200 0000000000000000 ffffffe003499000 00000000000007e6 ffffffffffffffff 0000000000000000 ffffffff82eed0a8 ffffffff82eed008 ffffffff8260f190 ffffffff82600480 ffffffff82a67a20 0000000000000008 ffffffff824000e0 0000000000000000 0000000000008000 ffffffff829f7484 ffffffe003216010 ffffffe003216030 
    EBSTATE.ST:    0000000000000401
    SBPC:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 ffffffe003233c10 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         ffffffffffffffc0 ffffffe003233c50 ffffffe003233c10 0000000000000000 ffffffe003233c70 0000000000000000 0000000000000000 0000000000000000 
    recover from ebstate: tpc=ffffffff81ab1000, carg=0
  ------------- CS_IN(0) Addr (ffffffff8150b15e=>ffffffff8150b15e) Priv(1=>1)
    GPRS:          0000000000000000 ffffffff82430de0 ffffffe003233c50 ffffffff82eec060 ffffffe003248000 ffffffe0032bd5e0 ffffffffffffffff 000000000000000d ffffffe003233c70 ffffffff82430dc0 0000000000000000 00000000000007e5 ffffffe00fa6f1a0 0000000000000002 17ddb6757a8b9200 0000000000000000 ffffffe003499000 00000000000007e6 ffffffffffffffff 0000000000000000 ffffffff82eed0a8 ffffffff82eed008 ffffffff8260f190 ffffffff82600480 ffffffff82a67a20 0000000000000008 ffffffff824000e0 0000000000000000 0000000000008000 ffffffff829f7484 ffffffe003216010 ffffffe003216030 
    EBSTATE.ST:    0000000000000401
    SBPC:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 ffffffe003233c10 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 ffffffffffffffc0 ffffffe003233c50 ffffffe003233c10 0000000000000000 ffffffe003233c70 0000000000000000 
    block recoverred with header ffffffff8090bc40(ffffffff81ab0ff6, ffffffff81ab106c), TPC=ffffffff81ab1000
    save to ebstate: tpc=ffffffff81ab1000, carg=0
  ------------- CS_OUT(0): Addr(ffffffff81ab1000=>ffffffff80003180) with bpc=ffffffff8090bc40(ffffffff81ab0ff6,ffffffff81ab106c), tpc=ffffffff81ab1000, Priv(1=>1) for `store_page_fault`


这个打印函数不好看，我调整了一个补丁，然后运行了7、8次，问题再也不出了，莫名其
妙，下周再查吧，先把版本合上去。

20220620
========

今天继续查上周的问题，更新了内核后，捕获这样一次错误：::

  [    5.857205] BUG: sleeping function called from invalid context at include/linux/sched/mm.h:230
  [    5.878097] in_atomic(): 0, irqs_disabled(): 1, non_block: 0, pid: 1, name: swapper
  [    5.899165] preempt_count: 0, expected: 0
  [    5.911966] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g38a67bbea187-dirty #66
  [    5.934896] Hardware name: riscv-virtio,qemu (DT)
  [    5.948113] Call Trace:
  [    5.957578] [<ffffffff80006600>] dump_backtrace+0x20/0x40
  [    5.977769] [<ffffffff814b23a0>] dump_stack+0x40/0x60
  [    5.998502] [<ffffffff8006e8e0>] __might_resched+0x260/0x2a0
  [    6.020371] [<ffffffff8006e9a0>] __might_sleep+0x80/0x100
  [    6.041646] [<ffffffff802a2980>] __kmalloc_track_caller+0x3c0/0x860
  [    6.064621] [<ffffffff80205840>] kstrdup_const+0xa0/0x120
  [    6.084589] [<ffffffff806c01c0>] kvasprintf_const+0xc0/0x160
  [    6.105288] [<ffffffff8146e240>] kobject_set_name_vargs+0x60/0x200
  [    6.128614] [<ffffffff80db7fe0>] dev_set_name+0x20/0x80
  [    6.147273] [<ffffffff810eba20>] of_device_alloc+0x3e0/0x860
  [    6.168849] [<ffffffff810ebf60>] of_platform_device_create_pdata+0xc0/0x220
  [    6.193712] [<ffffffff810ec4a0>] of_platform_bus_create+0x3e0/0x5c0
  [    6.216860] [<ffffffff810ec560>] of_platform_bus_create+0x4a0/0x5c0
  [    6.239992] [<ffffffff810ec960>] of_platform_populate+0x80/0x1e0
  [    6.262735] [<ffffffff822380e0>] of_platform_default_populate_init+0x180/0x1c0
  [    6.287404] [<ffffffff82202500>] do_one_initcall+0xe0/0x2e0
  [    6.308609] [<ffffffff82202c00>] kernel_init_freeable+0x3e0/0x500
  [    6.331043] [<ffffffff814b2770>] kernel_init+0x30/0x2c0
  [    6.349556] [<ffffffff80003500>] ret_from_exception+0x0/0x40
  [    7.617383] ------------[ cut here ]------------
  [    7.634436] initcall of_platform_default_populate_init+0x0/0x1c0 returned with disabled interrupts
  [    7.669687] WARNING: CPU: 0 PID: 1 at init/main.c:1314 do_one_initcall+0x260/0x2e0
  [    7.696880] CPU: 0 PID: 1 Comm: swapper Tainted: G        W         5.16.0-rc3-g38a67bbea187-dirty #66
  [    7.722195] Hardware name: riscv-virtio,qemu (DT)
  [    7.735296] epc : do_one_initcall+0x260/0x2e0
  [    7.753794]  ra : do_one_initcall+0x260/0x2e0
  [    7.772302] epc : ffffffff82202680 ra : ffffffff82202680 sp : ffffffe002e33c70
  [    7.792207]  gp : ffffffff82d012e8 tp : ffffffe002e48000 t0 : ffffffff82c16f38
  [    7.812083]  t1 : ffffffffffffffff t2 : 00000000000000a7 s0 : ffffffe002e33d00
  [    7.831828]  s1 : ffffffff82237f60 a0 : 0000000000000057 a1 : ffffffff82c853b8
  [    7.851625]  a2 : ffffffe002e339d8 a3 : fffffffffffffffe a4 : 8f05fd8993126d00
  [    7.871506]  a5 : 8f05fd8993126d00 a6 : 0000000000000002 a7 : 0000000000000000
  [    7.891046]  s2 : 0000000000000000 s3 : 0000000000000000 s4 : ffffffff82d020a8
  [    7.910583]  s5 : ffffffff82d02008 s6 : ffffffff8240ec10 s7 : ffffffff82400468
  [    7.930519]  s8 : ffffffff82867f40 s9 : 0000000000000008 s10: ffffffff822000e0
  [    7.950278]  s11: 0000000000000000 t3 : 000000000000000f t4 : ffffffff827be3e0
  [    7.969890]  t5 : ffffffff82c85420 t6 : ffffffff82c85440
  [    7.985489] status: 0000000000000120 badaddr: 0000000000000000 cause: 0000000000000003
  [    8.006331] ebstate.st: 000000000000040f
  [    8.018454]  bstate.r0  : ffffffff814c1cb2 bstate.r1  : 0000000000000000
  [    8.038032]  bstate.r2  : 0000000000000000 bstate.r3  : 0000000000000000
  [    8.057341]  bstate.r4  : 0000000000000000 bstate.r5  : 0000000000000000
  [    8.076591]  bstate.r6  : 0000000000000000 bstate.r7  : 0000000000000000
  [    8.095883]  bstate.r8  : 0000000000000000 bstate.r9  : 0000000000000000
  [    8.115168]  bstate.r10 : 0000000000000000 bstate.r11 : 0000000000000000
  [    8.134597]  bstate.r12 : 0000000000000000 bstate.r13 : 0000000000000000
  [    8.154077]  bstate.r14 : 0000000000000000 bstate.r15 : 0000000000000000
  [    8.173504]  bstate.r16 : 0000000000000000 bstate.r17 : 0000000000000000
  [    8.192931]  bstate.r18 : 0000000000000000 bstate.r19 : 0000000000000000
  [    8.212348]  bstate.r20 : 0000000000000000 bstate.r21 : 0000000000000000
  [    8.231796]  bstate.r22 : 0000000000000000 bstate.r23 : 0000000000000000
  [    8.251232]  bstate.r24 : 0000000000000000 bstate.r25 : 0000000000000000
  [    8.270678]  bstate.r26 : 0000000000000000 bstate.r27 : 0000000000000000
  [    8.290113]  bstate.r28 : 0000000000000000 bstate.r29 : 0000000000000000
  [    8.309507]  bstate.r30 : 0000000000000000 bstate.r31 : 0000000000000000
  [    8.328905]  bstate.r32 : 0000000000000000 bstate.r33 : 0000000000000000
  [    8.348324]  bstate.r34 : 0000000000000000 bstate.r35 : 0000000000000000
  [    8.367720]  bstate.r36 : 0000000000000000 bstate.r37 : 0000000000000000
  [    8.387120]  bstate.r38 : 0000000000000000 bstate.r39 : 0000000000000000
  [    8.406520]  bstate.r40 : 0000000000000000 bstate.r41 : 0000000000000000
  [    8.426098] [<ffffffff82202680>] do_one_initcall+0x260/0x2e0
  [    8.447668] [<ffffffff82202c00>] kernel_init_freeable+0x3e0/0x500
  [    8.470223] [<ffffffff814b2770>] kernel_init+0x30/0x2c0
  [    8.488765] [<ffffffff80003500>] ret_from_exception+0x0/0x40
  [    8.512137] ---[ end trace c17fea093d78bdb3 ]---
  **
  ERROR:../target/linx/op_helper.c:546:helper_blk_do_recovery: code should not be reached
  Bail out! ERROR:../target/linx/op_helper.c:546:helper_blk_do_recovery: code should not be reached
  Aborted

这是一个might_alloc()里报的错误，也不知道是不是倒换导致的问题，我先查没有恢复到
原地的问题（就是最后那个assert错误）：::

  Trace-_X 0(02:45:27): 0x7f5b30f024c0 [0000000000000000/ffffffff82202680/00004201/ff000000] do_one_initcall
  Trace-_X 0(02:45:27): 0x7f5b30f02640 [0000000000000000/ffffffff814c1cb2/00004201/ff000000] 
  ------------- CS_OUT_IN_BLK(0): Addr(ffffffff814c1cb2=>ffffffff80003180) with bpc=ffffffff82202680(ffffffff814c1cb2,ffffffff814c1cb4), tpc=ffffffffffffffff, Priv(1=>1) for `breakpoint`
    GPRS:          0000000000000000 ffffffff82202680 ffffffe002e33c70 ffffffff82d012e8 ffffffe002e48000 ffffffff82c16f38 ffffffffffffffff 00000000000000a7 ffffffe002e33d00 ffffffff82237f60 0000000000000057 ffffffff82c853b8 ffffffe002e339d8 fffffffffffffffe 8f05fd8993126d00 8f05fd8993126d00 0000000000000002 0000000000000000 0000000000000000 0000000000000000 ffffffff82d020a8 ffffffff82d02008 ffffffff8240ec10 ffffffff82400468 ffffffff82867f40 0000000000000008 ffffffff822000e0 0000000000000000 000000000000000f ffffffff827be3e0 ffffffff82c85420 ffffffff82c85440 
    EBSTATE.ST:    000000000000040e
    CARG:          0000000000000000
    TPC:           ffffffffffffffff
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
  Trace-_X 0(02:45:27): 0x7f5b30754500 [0000000000000000/ffffffff80003180/00004201/ff000000] handle_exception
  ...
  Trace-XX 0(02:45:28): 0x7f5b307b9840 [0000000000000000/ffffffff814c639e/00004201/ff000000] 
  ------------- CS_IN_REV(0) Addr (ffffffff814c639e=>ffffffff814c639e) Priv(1=>1)
    GPRS:          0000000000000000 ffffffff82202680 ffffffe002e33c70 ffffffff82d012e8 ffffffe002e48000 ffffffff82c16f38 ffffffffffffffff 00000000000000a7 ffffffe002e33d00 ffffffff82237f60 0000000000000057 ffffffff82c853b8 ffffffe002e339d8 fffffffffffffffe 8f05fd8993126d00 8f05fd8993126d00 0000000000000002 0000000000000000 0000000000000000 0000000000000000 ffffffff82d020a8 ffffffff82d02008 ffffffff8240ec10 ffffffff82400468 ffffffff82867f40 0000000000000008 ffffffff822000e0 0000000000000000 000000000000000f ffffffff827be3e0 ffffffff82c85420 ffffffff82c85440 
    EBSTATE.ST:    000000000000040f
    CARG:          0000000000000000
    TPC:           ffffffff814c1cb2
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
  ------------- CS_OUT_IN_BLK(0): Addr(ffffffff822026a0=>ffffffff80003180) with bpc=ffffffffffffffff(ffffffffffffffff,ffffffffffffffff), tpc=ffffffff814c1cb2, Priv(1=>1) for `s_timer`
    GPRS:          0000000000000000 ffffffff82202680 ffffffe002e33c70 ffffffff82d012e8 ffffffe002e48000 ffffffff82c16f38 ffffffffffffffff 00000000000000a7 ffffffe002e33d00 ffffffff82237f60 0000000000000057 ffffffff82c853b8 ffffffe002e339d8 fffffffffffffffe 8f05fd8993126d00 8f05fd8993126d00 0000000000000002 0000000000000000 0000000000000000 0000000000000000 ffffffff82d020a8 ffffffff82d02008 ffffffff8240ec10 ffffffff82400468 ffffffff82867f40 0000000000000008 ffffffff822000e0 0000000000000000 000000000000000f ffffffff827be3e0 ffffffff82c85420 ffffffff82c85440 
    EBSTATE.ST:    000000000000040f
    CARG:          0000000000000000
    TPC:           ffffffff814c1cb2
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
  Trace-XX 0(02:45:28): 0x7f5b30754500 [0000000000000000/ffffffff80003180/00004201/ff000000] handle_exception
  ...
  Trace-XX 0(02:45:28): 0x7f5b307b9840 [0000000000000000/ffffffff814c639e/00004201/ff000000] 
  ------------- CS_IN_REV(0) Addr (ffffffff814c639e=>ffffffff814c639e) Priv(1=>1)
    GPRS:          0000000000000000 ffffffff82202680 ffffffe002e33c70 ffffffff82d012e8 ffffffe002e48000 ffffffff82c16f38 ffffffffffffffff 00000000000000a7 ffffffe002e33d00 ffffffff82237f60 0000000000000057 ffffffff82c853b8 ffffffe002e339d8 fffffffffffffffe 8f05fd8993126d00 8f05fd8993126d00 0000000000000002 0000000000000000 0000000000000000 0000000000000000 ffffffff82d020a8 ffffffff82d02008 ffffffff8240ec10 ffffffff82400468 ffffffff82867f40 0000000000000008 ffffffff822000e0 0000000000000000 000000000000000f ffffffff827be3e0 ffffffff82c85420 ffffffff82c85440 
    EBSTATE.ST:    000000000000040f
    CARG:          0000000000000000
    TPC:           ffffffff814c1cb2
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
  Trace-XX 0(02:45:28): 0x7f5b30b42dc0 [0000000000000000/ffffffff822026a0/00004201/ff000000] do_one_initcall
    block recoverred with header ffffffff822026a0(ffffffff814c1cb4, ffffffff814c1cc2), TPC=ffffffff814c1cb2
  the bstate is not this block, tpc: ffffffff814c1cb2, tpc1: ffffffff814c1cb4, tpc2: ffffffff814c1cc2 

这个应该是立福修改了BUG_ON的逻辑以后的打印，先是do_on_initcall里面发生了一个
BUG_ON，触发breakpoin断点，位置在ffffffff814c1cb2，恢复以后，还没有执行，就发生
了下一个中断，中断返回的时候切换到另一个块作特殊处理（这是立福上午说的逻辑），
但TPC和那个新块对不上。

这里首先有一个问题：我们不应该制造块内断点，这个平白增加块内状态的保存。要想办
法优化。然后立福可能需要调整一下逻辑，处理这个切换块带了状态的问题。我初步的想
法是要不给ebrk一个参数，表示不保存状态。

上面的错误留个立福解决，我接着看下一个错误：::

  [    4.446897] Unable to handle kernel paging request at virtual address ffffffe002e16010
  [    4.479937] Oops [#1]
  [    4.491017] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g38a67bbea187-dirty #67
  [    4.517428] Hardware name: riscv-virtio,qemu (DT)
  [    4.532610] epc : 0xffffffe002e16010
  [    4.547796]  ra : 0xffffffe002e16010
  [    4.562816] epc : ffffffe002e16010 ra : ffffffe002e16010 sp : ffffffe002e33910
  [    4.585567]  gp : ffffffff82d012e8 tp : ffffffe002e38000 t0 : ffffffe002e8cb00
  [    4.608552]  t1 : 148302105402ae00 t2 : 000000000000005f s0 : 0000000000000000
  [    4.631119]  s1 : 0000000000000000 a0 : ffffffe003215860 a1 : 0000000000000000
  [    4.653691]  a2 : 0000000000000190 a3 : 0000000000000220 a4 : 000000000000002d
  [    4.676135]  a5 : 000000000007ffff a6 : 0000000000000001 a7 : 0000000000000a20
  [    4.698542]  s2 : 000000000000ffff s3 : 0000000000000000 s4 : 0000000000000000
  [    4.720853]  s5 : 0000000000000000 s6 : 0000000000000000 s7 : 0000000000000000
  [    4.743641]  s8 : 0000000000000000 s9 : 0000000000000000 s10: 0000000000000000
  [    4.766367]  s11: 0000000000000000 t3 : 0000000000000000 t4 : 0000000000000003
  [    4.789047]  t5 : 0000000000000400 t6 : 0000000000000403
  [    4.806941] status: 8000000000006120 badaddr: ffffffe002e16010 cause: 000000000000000c
  [    4.831465] ebstate.st: 0000000000000400
  [    4.857310] ---[ end trace 34eb60ee0ff73f24 ]---
  [    4.881901] note: swapper[1] exited with preempt_count 2
  [    4.911509] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
  [    4.935932] ---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b ]---

Qemu的日志分析如下：::

  Trace-_X 0(06:05:38): 0x7f19704d8d80 [0000000000000000/ffffffff8146a8c0/00004201/ff000000] idr_alloc_u32
  Trace-_X 0(06:05:38): 0x7f19704d8f00 [0000000000000000/ffffffff821d75b0/00004201/ff000000] 
  Trace-_X 0(06:05:38): 0x7f19704d9200 [0000000000000000/ffffffff81477ee0/00004201/ff000000] idr_get_free
  Trace-_X 0(06:05:38): 0x7f19704d9380 [0000000000000000/ffffffff821defe6/00004201/ff000000] 
  Trace-_X 0(06:05:38): 0x7f19704d9680 [0000000000000000/ffffffff821df000/00004201/ff000000] 
  Stopped execution of TB chain before 0x7f19704d9680 [ffffffff821df000]  <--- 这时一个中间打断的块（这会影响什么，待查）
      Trace-_X 0(06:05:38): 0x7f1970312fc0 [0000000000000000/0000000080000520/00004203/ff000000] 这是跑到m-mode里面了（我关掉了m-mode的切换跟踪）
      ...
      Trace-_X 0(06:05:38): 0x7f1970318280 [0000000000000000/00000000800005f4/00004203/ff000000]
  从m-mode返回马上触发一个内核的之中中断，所在bpc=ffffffff81477ee0，tpc的显示其实无效，它应该就是PC：ffffffff821df000
  这时，按我们的逻辑，sepc应该会设置成bpc
  ------------- CS_OUT_IN_BLK(0): Addr(ffffffff821df000=>ffffffff80003180) with bpc=ffffffff81477ee0(ffffffff821defe6,ffffffff821df078), tpc=ffffffffffffffff, Priv(1=>1) for `s_timer`
    GPRS:          0000000000000000 ffffffff8146a8e0 ffffffe002e33910 ffffffff82d012e8 ffffffe002e38000 ffffffe002e8cb00 148302105402ae00 000000000000005f ffffffe002e33980 ffffffe002e16010 ffffffe002e16010 ffffffe002e33918 0000000000000a20 000000007fffffff 00000000000000ec 0000000000000000 0000000000000004 ffffffff827ee410 ffffffe002e33984 0000000000000000 ffffffff82d020a8 ffffffe002e8ca80 ffffffff82d020a8 0000000000000001 00000000000041ed ffffffff82d020a8 ffffffff82d02bc0 ffffffe002e16010 0000000000000000 0000000000000003 0000000000000400 0000000000000403 
    EBSTATE.ST:    0000000000000400 <-- 这个是旧值，其实没有什么意义
    CARG:          0000000000000000
    TPC:           ffffffffffffffff <-- 这个也是没有同步前的值也没有什么意义
    SGPRS:         0000000000000000 0000000000000000 ffffffe002e33890 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 ffffffe002e338e8 ffffffe002e33890 ffffffff82d020a8 ffffffe002e338e0 ffffffe002e33900 ffffffe002e33984 ffffffe002e338f0 
  Trace-_X 0(06:05:38): 0x7f19707624c0 [0000000000000000/ffffffff80003180/00004201/ff000000] handle_exception <--然后是中断处理
  Trace-_X 0(06:05:38): 0x7f1970762640 [0000000000000000/ffffffff814c6160/00004201/ff000000] 
  ...
  Trace-XX 0(06:05:38): 0x7f19707a53c0 [0000000000000000/ffffffff800035c0/00004201/ff000000] 
  Trace-XX 0(06:05:38): 0x7f19707a5540 [0000000000000000/ffffffff814c65da/00004201/ff000000] 
  ------------- CS_IN_REV(0) Addr (ffffffff814c65da=>ffffffff814c65da) Priv(1=>1) <-- 中断返回，目标地址是ffffffff81477ee0，这个对上bpc
    GPRS:          0000000000000000 ffffffff8146a8e0 ffffffe002e33910 ffffffff82d012e8 ffffffe002e38000 ffffffe002e8cb00 148302105402ae00 000000000000005f ffffffe002e33980 ffffffe002e16010 ffffffe002e16010 ffffffe002e33918 0000000000000a20 000000007fffffff 00000000000000ec 0000000000000000 0000000000000004 ffffffff827ee410 ffffffe002e33984 0000000000000000 ffffffff82d020a8 ffffffe002e8ca80 ffffffff82d020a8 0000000000000001 00000000000041ed ffffffff82d020a8 ffffffff82d02bc0 ffffffe002e16010 0000000000000000 0000000000000003 0000000000000400 0000000000000403 
    EBSTATE.ST:    0000000000000401
    CARG:          0000000000000000
    TPC:           ffffffff821df000
    SGPRS:         0000000000000000 0000000000000000 ffffffe002e33890 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         ffffffe002e33900 ffffffe002e33984 ffffffe002e338f0 0000000000000000 ffffffe002e338e8 ffffffe002e33890 ffffffff82d020a8 ffffffe002e338e0 
    TREGS和原来的值对不上，但这是因为ibpc导致的，理论上可能不会错，我调整一下跟踪，以后让输出和ibpc一致
  Trace-XX 0(06:05:38): 0x7f19704d9200 [0000000000000000/ffffffff81477ee0/00004201/ff000000] idr_get_free   <--- 这里重新解释了块头
    block recoverred with header ffffffff81477ee0(ffffffff821defe6, ffffffff821df078), TPC=ffffffff821df000 <--- 对上tpc1/2，tpc的地址也对上了
  Trace-XX 0(06:05:38): 0x7f19704d9680 [0000000000000000/ffffffff821df000/00004201/ff000000] 
  Trace-XX 0(06:05:38): 0x7f19704d9e80 [0000000000000000/ffffffff81477f00/00004201/ff000000] idr_get_free
  ...
  Trace-_X 0(06:05:38): 0x7f19704e7a40 [0000000000000000/ffffffff81478240/00004201/ff000000] idr_get_free
  Trace-_X 0(06:05:38): 0x7f19704e7bc0 [0000000000000000/ffffffff821df21a/00004201/ff000000] 
  Trace-_X 0(06:05:38): 0x7f19704e7f80 [0000000000000000/ffffffff81478260/00004201/ff000000] idr_get_free
  Trace-_X 0(06:05:38): 0x7f19704e8100 [0000000000000000/ffffffff821df246/00004201/ff000000] 
  跑了没多久，就出错了
  ------------- CS_OUT(0): Addr(ffffffe002e16010=>ffffffff80003180) with bpc=ffffffffffffffff(ffffffffffffffff,ffffffffffffffff), tpc=ffffffffffffffff, Priv(1=>1) for `exec_page_fault`
    GPRS:          0000000000000000 ffffffe002e16010 ffffffe002e33910 ffffffff82d012e8 ffffffe002e38000 ffffffe002e8cb00 148302105402ae00 000000000000005f 0000000000000000 0000000000000000 ffffffe003215860 0000000000000000 0000000000000190 0000000000000220 000000000000002d 000000000007ffff 0000000000000001 0000000000000a20 000000000000ffff 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000003 0000000000000400 0000000000000403 
    EBSTATE.ST:    0000000000000401
    CARG:          0000000000000000
    TPC:           ffffffffffffffff
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 

所以，还是看不出问题是什么。我调整了CS的打印，再来。这次错误在这里：::

  [    0.788357] Unable to handle kernel NULL pointer dereference at virtual address 0000000000000018
  [    0.817788] Oops [#1]
  [    0.827512] CPU: 0 PID: 0 Comm: swapper Not tainted 5.16.0-rc3-g38a67bbea187-dirty #67
  [    0.850509] Hardware name: riscv-virtio,qemu (DT)
  [    0.863849] epc : insert_header+0x140/0x920
  [    0.882426]  ra : insert_header+0x1c0/0x920
  [    0.900697] epc : ffffffff803efb20 ra : ffffffff803efba0 sp : ffffffff82c03cf0
  [    0.920426]  gp : ffffffff82d012e8 tp : ffffffff82c0c480 t0 : ffffffe002e22500
  [    0.940133]  t1 : ffffffe002e224f8 t2 : 0000000000000037 s0 : ffffffff82c03d80
  [    0.959689]  s1 : 0000000000000016 a0 : 0000000000000007 a1 : ffffffff827c14f1
  [    0.979068]  a2 : ffffffff827c16d4 a3 : 0000000000000001 a4 : ffffffe002e2e878
  [    0.998612]  a5 : ffffffe002e2ea10 a6 : ffffffff800231e0 a7 : ffffffff80023280
  [    1.018322]  s2 : ffffffff827c16c8 s3 : 0000000000000016 s4 : ffffffe002e2eb10
  [    1.037892]  s5 : ffffffe002e2e800 s6 : ffffffff82c11058 s7 : ffffffe002e22400
  [    1.057619]  s8 : ffffffff82d387d8 s9 : 000000000000000c s10: ffffffe002e2ea10
  [    1.077125]  s11: ffffffff827c14f0 t3 : ffffffff80025380 t4 : 0000000000000000
  [    1.096636]  t5 : 0000000000000376 t6 : 00000000000003ff
  [    1.111763] status: 8000000000006120 badaddr: 0000000000000018 cause: 000000000000000d
  [    1.132392] ebstate.st: 0000000000000401
  [    1.144372]  bstate.r0  : ffffffff8172b000 bstate.r1  : 0000000000000000
  [    1.164041]  bstate.r2  : 0000000000000000 bstate.r3  : 0000000000000000
  [    1.183425]  bstate.r4  : 0000000000000000 bstate.r5  : 0000000000000000
  [    1.202771]  bstate.r6  : 0000000000000000 bstate.r7  : 0000000000000000
  [    1.222181]  bstate.r8  : ffffffe002e2ea10 bstate.r9  : 0000000000000000
  [    1.241731]  bstate.r10 : 0000000000000000 bstate.r11 : 0000000000000000
  [    1.261295]  bstate.r12 : 0000000000000000 bstate.r13 : 0000000000000000
  [    1.280886]  bstate.r14 : 0000000000000000 bstate.r15 : 0000000000000000
  [    1.300450]  bstate.r16 : 0000000000000000 bstate.r17 : 0000000000000000
  [    1.320072]  bstate.r18 : 0000000000000000 bstate.r19 : 0000000000000000
  [    1.339612]  bstate.r20 : 0000000000000000 bstate.r21 : 0000000000000000
  [    1.359140]  bstate.r22 : 0000000000000000 bstate.r23 : 0000000000000000
  [    1.378706]  bstate.r24 : 0000000000000000 bstate.r25 : 0000000000000000
  [    1.398282]  bstate.r26 : 0000000000000000 bstate.r27 : 0000000000000000
  [    1.417862]  bstate.r28 : 0000000000000000 bstate.r29 : 0000000000000000
  [    1.437455]  bstate.r30 : 0000000000000000 bstate.r31 : 0000000000000000
  [    1.457064]  bstate.r32 : 0000000000000000 bstate.r33 : 0000000000000000
  [    1.476599]  bstate.r34 : 0000000000000000 bstate.r35 : 0000000000000000
  [    1.496106]  bstate.r36 : 0000000000000000 bstate.r37 : 0000000000000000
  [    1.515702]  bstate.r38 : 0000000000000000 bstate.r39 : 0000000000000000
  [    1.535284]  bstate.r40 : 0000000000000000 bstate.r41 : 0000000000000000
  [    1.555201] [<ffffffff803efb20>] insert_header+0x140/0x920
  [    1.577008] [<ffffffff803f1ea0>] __register_sysctl_table+0x4a0/0x1100
  [    1.600854] [<ffffffff803f2d80>] register_leaf_sysctl_tables+0x280/0x4a0
  [    1.625365] [<ffffffff803f2c60>] register_leaf_sysctl_tables+0x160/0x4a0
  [    1.649875] [<ffffffff803f34a0>] __register_sysctl_paths+0x2c0/0x560
  [    1.673530] [<ffffffff803f37a0>] register_sysctl_table+0x20/0x40
  [    1.696426] [<ffffffff8220b1a0>] sysctl_init+0x20/0x40
  [    1.715229] [<ffffffff822222c0>] proc_sys_init+0x40/0x60
  [    1.734970] [<ffffffff82221d00>] proc_root_init+0x180/0x1c0
  [    1.755232] [<ffffffff82202340>] start_kernel+0x11a0/0x12c0
  [    1.783835] ---[ end trace ae68f3c20b1593ed ]---
  [    1.802489] Kernel panic - not syncing: Attempted to kill the idle task!
  [    1.821474] ---[ end Kernel panic - not syncing: Attempted to kill the idle task! ]---

跟踪：::

  Trace-XX 0(07:42:36): 0x7fd60897d680 [0000000000000000/ffffffff803efb20/00004201/ff000000] insert_header
  Trace-XX 0(07:42:36): 0x7fd60897d800 [0000000000000000/ffffffff8172affe/00004201/ff000000] 
  Trace-XX 0(07:42:36): 0x7fd60897d940 [0000000000000000/ffffffff8172b000/00004201/ff000000] <-- 这是内核的代码
  Stopped execution of TB chain before 0x7fd60897d940 [ffffffff8172b000]                     <-- 又是一个来自m-mode的打断
  Trace-XX 0(07:42:36): 0x7fd608312fc0 [0000000000000000/0000000080000520/00004203/ff000000] 
  ...
  Trace-_X 0(07:42:36): 0x7fd608318280 [0000000000000000/00000000800005f4/00004203/ff000000] 
  还是一个套路，m-mode返回马上是个时钟中断，块对上前面的insert_header
  ------------- CS_OUT_FROM_BLK(0): Addr(ffffffff8172b000=>ffffffff80003180) with bpc=ffffffff803efb20(ffffffff8172affe,ffffffff8172b01a), Priv(1=>1) for `s_timer`
    GPRS:          0000000000000000 ffffffff803efba0 ffffffff82c03cf0 ffffffff82d012e8 ffffffff82c0c480 ffffffe002e22500 ffffffe002e224f8 0000000000000037 ffffffff82c03d80 0000000000000016 0000000000000007 ffffffff827c14f1 ffffffff827c16d4 0000000000000001 ffffffe002e2e878 ffffffe002e2ea10 ffffffff800231e0 ffffffff80023280 ffffffff827c16c8 0000000000000016 ffffffe002e2eb10 ffffffe002e2e800 ffffffff82c11058 ffffffe002e22400 ffffffff82d387d8 000000000000000c ffffffe002e2ea10 ffffffff827c14f0 ffffffff80025380 0000000000000000 0000000000000376 00000000000003ff 
    TREG_P:        1
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe002e2ea10 
  Trace-_X 0(07:42:36): 0x7fd6087624c0 [0000000000000000/ffffffff80003180/00004201/ff000000] handle_exception
  Trace-_X 0(07:42:36): 0x7fd608762640 [0000000000000000/ffffffff814c6160/00004201/ff000000] 
  ...
  Trace-XX 0(07:42:36): 0x7fd6087b8ec0 [0000000000000000/ffffffff800035c0/00004201/ff000000] 
  Trace-XX 0(07:42:36): 0x7fd6087b9040 [0000000000000000/ffffffff814c65da/00004201/ff000000] 
  ------------- CS_IN_REV(0) Addr (ffffffff814c65da=>ffffffff803efb20) Priv(1=>1)
    GPRS:          0000000000000000 ffffffff803efba0 ffffffff82c03cf0 ffffffff82d012e8 ffffffff82c0c480 ffffffe002e22500 ffffffe002e224f8 0000000000000037 ffffffff82c03d80 0000000000000016 0000000000000007 ffffffff827c14f1 ffffffff827c16d4 0000000000000001 ffffffe002e2e878 ffffffe002e2ea10 ffffffff800231e0 ffffffff80023280 ffffffff827c16c8 0000000000000016 ffffffe002e2eb10 ffffffe002e2e800 ffffffff82c11058 ffffffe002e22400 ffffffff82d387d8 000000000000000c ffffffe002e2ea10 ffffffff827c14f0 ffffffff80025380 0000000000000000 0000000000000376 00000000000003ff 
    TREG_P:        0
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe002e2ea10 
  Trace-XX 0(07:42:36): 0x7fd60897d680 [0000000000000000/ffffffff803efb20/00004201/ff000000] insert_header
    block recoverred with header ffffffff803efb20(ffffffff8172affe, ffffffff8172b01a), TPC=ffffffff8172b000
  Trace-XX 0(07:42:36): 0x7fd60897d940 [0000000000000000/ffffffff8172b000/00004201/ff000000]  <--- 一进去就出问题了。
  ------------- CS_OUT_FROM_BLK(0): Addr(ffffffff8172b000=>ffffffff80003180) with bpc=ffffffff803efb20(ffffffff8172affe,ffffffff8172b01a), Priv(1=>1) for `load_page_fault`
    GPRS:          0000000000000000 ffffffff803efba0 ffffffff82c03cf0 ffffffff82d012e8 ffffffff82c0c480 ffffffe002e22500 ffffffe002e224f8 0000000000000037 ffffffff82c03d80 0000000000000016 0000000000000007 ffffffff827c14f1 ffffffff827c16d4 0000000000000001 ffffffe002e2e878 ffffffe002e2ea10 ffffffff800231e0 ffffffff80023280 ffffffff827c16c8 0000000000000016 ffffffe002e2eb10 ffffffe002e2e800 ffffffff82c11058 ffffffe002e22400 ffffffff82d387d8 000000000000000c ffffffe002e2ea10 ffffffff827c14f0 ffffffff80025380 0000000000000000 0000000000000376 00000000000003ff 
    TREG_P:        0
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe002e2ea10 
  Trace-XX 0(07:42:36): 0x7fd6087624c0 [0000000000000000/ffffffff80003180/00004201/ff000000] handle_exception
  Trace-XX 0(07:42:36): 0x7fd608762640 [0000000000000000/ffffffff814c6160/00004201/ff000000] 

这一次看来整个行为都是正常的，但很明显，恢复以后，很可能完全正常的代码运行在不
正常的状态上了。这个运行过程肯定有什么依托的寄存器没有恢复到我们预期的状态上。

那个Stopped execution of TB chain是在chained中间退出的时候就会有打印，我找不到
chain哪里做了这个退出判断，但我粗暴地直接加了个nochain，还是类似错误：::

  [    2.147180] Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
  [    2.183167] Oops [#1]
  [    2.194876] CPU: 0 PID: 9 Comm: kdevtmpfs Not tainted 5.16.0-rc3-g38a67bbea187-dirty #67
  [    2.222954] Hardware name: riscv-virtio,qemu (DT)
  [    2.238871] epc : link_path_walk.part.0.constprop.0+0x0/0x680
  [    2.266327]  ra : path_lookupat+0xa0/0x4a0
  [    2.287542] epc : ffffffff802da4c0 ra : ffffffff802db2e0 sp : ffffffe002e87ae0
  [    2.311611]  gp : ffffffff82d012e8 tp : ffffffe002e4ca00 t0 : ffffffe002e20780
  [    2.335661]  t1 : 0000000000000000 t2 : 000000003c08191c s0 : ffffffe002e87b20
  [    2.359375]  s1 : ffffffe002e78020 a0 : ffffffe002e78020 a1 : ffffffe002e87b28
  [    2.383362]  a2 : ffffffff82c8f2a8 a3 : ffffffe003212000 a4 : 0000000000000002
  [    2.407147]  a5 : 0000000000000051 a6 : 0000000000000008 a7 : ffffffff82cea330
  [    2.430744]  s2 : ffffffe002e87b28 s3 : ffffffe002e87c88 s4 : fffffffffffff000
  [    2.454722]  s5 : 0000000000000001 s6 : ffffffff82d020a8 s7 : ffffffff82d020a8
  [    2.478448]  s8 : ffffffe002e48db0 s9 : 0000000000000000 s10: 0000000000000000
  [    2.501970]  s11: 0000000000000000 t3 : ffffffff82cea4c0 t4 : 0000000000000402
  [    2.525577]  t5 : ffffffff82c12ee8 t6 : ffffffff82c12f08
  [    2.544259] status: 8000000000006120 badaddr: 0000000000000000 cause: 000000000000000f
  [    2.569289] ebstate.st: 0000000000000401
  [    2.583782]  bstate.r0  : ffffffff8168a000 bstate.r1  : ffffffe002e87a50
  [    2.607472]  bstate.r2  : 0000000000000000 bstate.r3  : 0000000000000080
  [    2.630631]  bstate.r4  : ffffffe002e87ad0 bstate.r5  : ffffffe002e87b20
  [    2.654258]  bstate.r6  : 0000000000000000 bstate.r7  : ffffffffffffff70
  [    2.677572]  bstate.r8  : ffffffe002e87ae0 bstate.r9  : 0000000000000000
  [    2.700918]  bstate.r10 : 0000000000000000 bstate.r11 : ffffffe002e87a50
  [    2.724439]  bstate.r12 : 0000000000000000 bstate.r13 : 0000000000000000
  [    2.747730]  bstate.r14 : 0000000000000000 bstate.r15 : 0000000000000000
  [    2.771039]  bstate.r16 : 0000000000000000 bstate.r17 : 0000000000000000
  [    2.794284]  bstate.r18 : 0000000000000000 bstate.r19 : 0000000000000000
  [    2.817614]  bstate.r20 : 0000000000000000 bstate.r21 : 0000000000000000
  [    2.840962]  bstate.r22 : 0000000000000000 bstate.r23 : 0000000000000000
  [    2.864375]  bstate.r24 : 0000000000000000 bstate.r25 : 0000000000000000
  [    2.887692]  bstate.r26 : 0000000000000000 bstate.r27 : 0000000000000000
  [    2.910958]  bstate.r28 : 0000000000000000 bstate.r29 : 0000000000000000
  [    2.934239]  bstate.r30 : 0000000000000000 bstate.r31 : 0000000000000000
  [    2.957491]  bstate.r32 : 0000000000000000 bstate.r33 : 0000000000000000
  [    2.980748]  bstate.r34 : 0000000000000000 bstate.r35 : 0000000000000000
  [    3.003997]  bstate.r36 : 0000000000000000 bstate.r37 : 0000000000000000
  [    3.027291]  bstate.r38 : 0000000000000000 bstate.r39 : 0000000000000000
  [    3.050587]  bstate.r40 : 0000000000000000 bstate.r41 : 0000000000000000
  [    3.074256] [<ffffffff802da4c0>] link_path_walk.part.0.constprop.0+0x0/0x680
  [    3.103783] [<ffffffff802dc900>] filename_lookup+0xa0/0x260
  [    3.129145] [<ffffffff802dcd40>] kern_path+0x40/0x80
  [    3.152735] [<ffffffff82220120>] init_mount+0x20/0xe0
  [    3.175097] [<ffffffff822317a0>] devtmpfs_setup+0x60/0xe0
  [    3.198413] [<ffffffff814b3470>] devtmpfsd+0x20/0x240
  [    3.220362] [<ffffffff800559a0>] kthread+0x220/0x2c0
  [    3.242789] [<ffffffff80003500>] ret_from_exception+0x0/0x40
  [    3.278406] ---[ end trace cb09055ed1f70898 ]---
  [    3.302925] note: kdevtmpfs[9] exited with preempt_count 1

跟踪：::

  Trace-_X 0(09:19:06): 0x7eff68d90700 [0000000000000000/ffffffff81689ff2/00004201/ff000200] 
  Trace-_X 0(09:19:06): 0x7eff68d90880 [0000000000000000/ffffffff8168a000/00004201/ff000200] 
  Stopped execution of TB chain before 0x7eff68d90880 [ffffffff8168a000]  <--- 这个打印还是有，nochain好像没有起作用
  Trace-_X 0(09:19:06): 0x7eff6876ae80 [0000000000000000/0000000080000520/00004203/ff000200]  <-- 开始进入m-mode
  ...
  Trace-_X 0(09:19:06): 0x7eff6876e500 [0000000000000000/00000000800005f4/00004203/ff000200] 
  回来就是时钟中断
  ------------- CS_OUT_FROM_BLK(0): Addr(ffffffff8168a000=>ffffffff80003180) with bpc=ffffffff802da4c0(ffffffff81689ff2,ffffffff8168a070), Priv(1=>1) for `s_timer`
    GPRS:          0000000000000000 ffffffff802db2e0 ffffffe002e87ae0 ffffffff82d012e8 ffffffe002e4ca00 ffffffe002e20780 0000000000000000 000000003c08191c ffffffe002e87b20 ffffffe002e78020 ffffffe002e78020 ffffffe002e87b28 ffffffff82c8f2a8 ffffffe003212000 0000000000000002 0000000000000051 0000000000000008 ffffffff82cea330 ffffffe002e87b28 ffffffe002e87c88 fffffffffffff000 0000000000000001 ffffffff82d020a8 ffffffff82d020a8 ffffffe002e48db0 0000000000000000 0000000000000000 0000000000000000 ffffffff82cea4c0 0000000000000402 ffffffff82c12ee8 ffffffff82c12f08 
    TREG_P:        3a
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 ffffffe002e87a50 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         ffffffe002e87a50 0000000000000000 0000000000000080 ffffffe002e87ad0 ffffffe002e87b20 0000000000000000 ffffffffffffff70 ffffffe002e87ae0 
  Trace-_X 0(09:19:06): 0x7eff6876e6c0 [0000000000000000/ffffffff80003180/00004201/ff000200] handle_exception
  Trace-_X 0(09:19:06): 0x7eff6876e840 [0000000000000000/ffffffff814c6160/00004201/ff000200] 
  ...
  Trace-XX 0(09:19:06): 0x7eff687caf80 [0000000000000000/ffffffff800035c0/00004201/ff000200] 
  Trace-XX 0(09:19:06): 0x7eff687cb100 [0000000000000000/ffffffff814c65da/00004201/ff000200] 
  Trace-XX 0(09:19:06): 0x7eff687cd300 [0000000000000000/ffffffff814c680c/00004201/ff000200] 
  ------------- CS_IN_REV(0) Addr (ffffffff814c680c=>ffffffff802da4c0) Priv(1=>1)
    GPRS:          0000000000000000 ffffffff802db2e0 ffffffe002e87ae0 ffffffff82d012e8 ffffffe002e4ca00 ffffffe002e20780 0000000000000000 000000003c08191c ffffffe002e87b20 ffffffe002e78020 ffffffe002e78020 ffffffe002e87b28 ffffffff82c8f2a8 ffffffe003212000 0000000000000002 0000000000000051 0000000000000008 ffffffff82cea330 ffffffe002e87b28 ffffffe002e87c88 fffffffffffff000 0000000000000001 ffffffff82d020a8 ffffffff82d020a8 ffffffe002e48db0 0000000000000000 0000000000000000 0000000000000000 ffffffff82cea4c0 0000000000000402 ffffffff82c12ee8 ffffffff82c12f08 
    TREG_P:        0
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 ffffffe002e87a50 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         ffffffe002e87a50 0000000000000000 0000000000000080 ffffffe002e87ad0 ffffffe002e87b20 0000000000000000 ffffffffffffff70 ffffffe002e87ae0 
  Trace-XX 0(09:19:06): 0x7eff68d90580 [0000000000000000/ffffffff802da4c0/00004201/ff000200] link_path_walk.part.0.constprop.0
    block recoverred with header ffffffff802da4c0(ffffffff81689ff2, ffffffff8168a070), TPC=ffffffff8168a000
  Trace-XX 0(09:19:06): 0x7eff68d90880 [0000000000000000/ffffffff8168a000/00004201/ff000200]  <-- 回来也是只走了一步，就出错了。TB还是原来的TB
  ------------- CS_OUT_FROM_BLK(0): Addr(ffffffff8168a000=>ffffffff80003180) with bpc=ffffffff802da4c0(ffffffff81689ff2,ffffffff8168a070), Priv(1=>1) for `store_page_fault`
    GPRS:          0000000000000000 ffffffff802db2e0 ffffffe002e87ae0 ffffffff82d012e8 ffffffe002e4ca00 ffffffe002e20780 0000000000000000 000000003c08191c ffffffe002e87b20 ffffffe002e78020 ffffffe002e78020 ffffffe002e87b28 ffffffff82c8f2a8 ffffffe003212000 0000000000000002 0000000000000051 0000000000000008 ffffffff82cea330 ffffffe002e87b28 ffffffe002e87c88 fffffffffffff000 0000000000000001 ffffffff82d020a8 ffffffff82d020a8 ffffffe002e48db0 0000000000000000 0000000000000000 0000000000000000 ffffffff82cea4c0 0000000000000402 ffffffff82c12ee8 ffffffff82c12f08 
    TREG_P:        0
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 ffffffe002e87a50 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         ffffffe002e87a50 0000000000000000 0000000000000080 ffffffe002e87ad0 ffffffe002e87b20 0000000000000000 ffffffffffffff70 ffffffe002e87ae0 
  Trace-XX 0(09:19:06): 0x7eff6876e6c0 [0000000000000000/ffffffff80003180/00004201/ff000200] handle_exception
  Trace-XX 0(09:19:06): 0x7eff6876e840 [0000000000000000/ffffffff814c6160/00004201/ff000200] 

我看了一下反汇编的结果：::

   0xffffffff802da4c0:    bstart  b.aux, bnext.concat, battr:none, bget:0x0ffc0f06, bset:0x0188c104, ptr:------ size:0x3f, bnext:------,
   0xffffffff802da4d0:    bstart.concat         bnext.cond ptr:0xffffffff81689ff2 bnext:0xffffffff802da500
        0xffffffff81689ff2:    const    0xffffffffffffff70 # -144
        0xffffffff81689ff4:    get      sp
        0xffffffff81689ff6:    add      t#1,t#2
        0xffffffff81689ff8:    set      sp,t#1
        0xffffffff81689ffa:    const    0x80 # 128
        0xffffffff81689ffc:    add      t#3,t#1
        0xffffffff81689ffe:    get      s0
        0xffffffff8168a000:    sd       t#1,[t#2,0]   <--------------------- a000在这里，如果就是这条指令，恢复的时候，T#2是看来是fff...fff70，而不是0
        0xffffffff8168a002:    get      s3
        0xffffffff8168a004:    sd       t#1,[t#7,104]
        0xffffffff8168a006:    addi     t#8,0
        0xffffffff8168a008:    get      s7
        0xffffffff8168a00a:    sd       t#1,[t#2,72]
        0xffffffff8168a00c:    get      s8
        0xffffffff8168a00e:    sd       t#1,[t#4,64]
        0xffffffff8168a010:    const    0x88 # 136
        0xffffffff8168a012:    add      t#6,t#1
        0xffffffff8168a014:    get      ra
        0xffffffff8168a016:    addi     t#8,0
        0xffffffff8168a018:    sd       t#2,[t#3,0]
        0xffffffff8168a01a:    get      s1
        0xffffffff8168a01c:    sd       t#1,[t#3,120]
        0xffffffff8168a01e:    get      s2
        0xffffffff8168a020:    sd       t#1,[t#5,112]
        0xffffffff8168a022:    get      s4
        0xffffffff8168a024:    sd       t#1,[t#7,96]
        0xffffffff8168a026:    addi     t#8,0
        0xffffffff8168a028:    get      s5
        0xffffffff8168a02a:    sd       t#1,[t#2,88]
        0xffffffff8168a02c:    get      s6
        0xffffffff8168a02e:    sd       t#1,[t#4,80]
        0xffffffff8168a030:    get      s9
        0xffffffff8168a032:    sd       t#1,[t#6,56]
        0xffffffff8168a034:    get      s10
        0xffffffff8168a036:    addi     t#8,0
        0xffffffff8168a038:    sd       t#2,[t#1,48]
        0xffffffff8168a03a:    get      s11
        0xffffffff8168a03c:    sd       t#1,[t#3,40]
        0xffffffff8168a03e:    const    0x90 # 144
        0xffffffff8168a040:    add      t#5,t#1
        0xffffffff8168a042:    set      s0,t#1
        0xffffffff8168a044:    lconst   0x167805a # 23560282
        0xffffffff8168a04e:    addtpc   t#1 # 0xffffffff82d020a8 <__stack_chk_guard>
 
没办法，开cpu跟踪太慢根本跑不起来，要不看mmu再看看吧，这次这个错误：::

  [    8.623967] Unable to handle kernel NULL pointer dereference at virtual address 0000000000000020
  [    8.655303] Oops [#1]
  [    8.665134] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g38a67bbea187-dirty #67
  [    8.688382] Hardware name: riscv-virtio,qemu (DT)
  [    8.701786] epc : kernfs_link_sibling+0x180/0x240
  [    8.721205]  ra : kernfs_add_one+0x160/0x360
  [    8.738342] epc : ffffffff803f9fe0 ra : ffffffff803fcee0 sp : ffffffe002e33a80
  [    8.758440]  gp : ffffffff82d012e8 tp : ffffffe002e48000 t0 : ffffffe002eb5780
  [    8.778535]  t1 : 0000000000000001 t2 : 00000000000000a7 s0 : ffffffe002e33ac0
  [    8.798236]  s1 : 0000000000000000 a0 : ffffffe002eb5700 a1 : 00000000d96d615b
  [    8.818031]  a2 : 0000000000000000 a3 : ffffffff8280750f a4 : ffffffe002eb56c8
  [    8.837954]  a5 : 000000007ffffffe a6 : 0000000000000220 a7 : 0000000000000228
  [    8.857690]  s2 : ffffffff8261c340 s3 : ffffffe002eb5700 s4 : 0000000000001000
  [    8.877597]  s5 : ffffffff82d020a8 s6 : ffffffff8240ec28 s7 : ffffffff82400470
  [    8.897671]  s8 : ffffffff82867fc0 s9 : 0000000000000008 s10: ffffffff822000e0
  [    8.917516]  s11: 0000000000000000 t3 : 0000000000000000 t4 : ffffffff8286ac98
  [    8.937179]  t5 : 000000000000003d t6 : 0000000000000000
  [    8.952473] status: 8000000000006120 badaddr: 0000000000000020 cause: 000000000000000f
  [    8.973376] ebstate.st: 0000000000000401
  [    8.985475]  bstate.r0  : ffffffff81731000 bstate.r1  : 0000000000000000
  [    9.005170]  bstate.r2  : 0000000000000000 bstate.r3  : ffffffe002eb5700
  [    9.024729]  bstate.r4  : ffffffe002eb5718 bstate.r5  : 0000000000000000
  [    9.044242]  bstate.r6  : 0000000000000000 bstate.r7  : ffffffe002eb5718
  [    9.063741]  bstate.r8  : 0000000000000000 bstate.r9  : 0000000000000000
  [    9.083050]  bstate.r10 : 0000000000000000 bstate.r11 : 0000000000000000
  [    9.102549]  bstate.r12 : 0000000000000000 bstate.r13 : 0000000000000000
  [    9.122041]  bstate.r14 : 0000000000000000 bstate.r15 : 0000000000000000
  [    9.141527]  bstate.r16 : 0000000000000000 bstate.r17 : 0000000000000000
  [    9.160951]  bstate.r18 : 0000000000000000 bstate.r19 : ffffffe002eb5718
  [    9.180634]  bstate.r20 : 0000000000000000 bstate.r21 : 0000000000000000
  [    9.200274]  bstate.r22 : 0000000000000000 bstate.r23 : 0000000000000000
  [    9.219765]  bstate.r24 : 0000000000000000 bstate.r25 : 0000000000000000
  [    9.239261]  bstate.r26 : 0000000000000000 bstate.r27 : 0000000000000000
  [    9.258765]  bstate.r28 : 0000000000000000 bstate.r29 : 0000000000000000
  [    9.278214]  bstate.r30 : 0000000000000000 bstate.r31 : 0000000000000000
  [    9.297698]  bstate.r32 : 0000000000000000 bstate.r33 : 0000000000000000
  [    9.317150]  bstate.r34 : 0000000000000000 bstate.r35 : 0000000000000000
  [    9.336650]  bstate.r36 : 0000000000000000 bstate.r37 : 0000000000000000
  [    9.356100]  bstate.r38 : 0000000000000000 bstate.r39 : 0000000000000000
  [    9.375543]  bstate.r40 : 0000000000000000 bstate.r41 : 0000000000000000
  [    9.395384] [<ffffffff803f9fe0>] kernfs_link_sibling+0x180/0x240
  [    9.416500] [<ffffffff803fcee0>] kernfs_add_one+0x160/0x360
  [    9.436590] [<ffffffff80400fe0>] __kernfs_create_file+0x120/0x1c0
  [    9.458157] [<ffffffff804022e0>] sysfs_add_file_mode_ns+0xa0/0x2c0
  [    9.480017] [<ffffffff80402760>] sysfs_create_file_ns+0xa0/0x140
  [    9.501541] [<ffffffff8220c140>] param_sysfs_init+0x120/0x720
  [    9.521917] [<ffffffff82202540>] do_one_initcall+0xe0/0x2e0
  [    9.543235] [<ffffffff82202c40>] kernel_init_freeable+0x3e0/0x500
  [    9.565862] [<ffffffff814b2970>] kernel_init+0x30/0x2c0
  [    9.584498] [<ffffffff80003500>] ret_from_exception+0x0/0x40
  [    9.612305] ---[ end trace 411105faf462e16f ]---
  [    9.633725] Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
  [    9.654614] ---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b ]---

看跟踪：::

  Trace-_X 0(10:41:43): 0x7f21789d0000 [0000000000000000/ffffffff803f9fe0/00004201/ff000200] kernfs_link_sibling
  Trace-_X 0(10:41:43): 0x7f21789d0180 [0000000000000000/ffffffff81730ff4/00004201/ff000200] 
  Trace-_X 0(10:41:43): 0x7f21789d0340 [0000000000000000/ffffffff81731000/00004201/ff000200] 
  Stopped execution of TB chain before 0x7f21789d0340 [ffffffff81731000] 
  Trace-_X 0(10:41:43): 0x7f21787695c0 [0000000000000000/0000000080000520/00004203/ff000200] 又是这个m-mode的中断
  ...
  Trace-_X 0(10:41:43): 0x7f217876cc40 [0000000000000000/00000000800005f4/00004203/ff000200] 
  然后回来发生普通时钟中断
  ------------- CS_OUT_FROM_BLK(0): Addr(ffffffff81731000=>ffffffff80003180) with bpc=ffffffff803f9fe0(ffffffff81730ff4,ffffffff81731016), Priv(1=>1) for `s_timer`
    GPRS:          0000000000000000 ffffffff803fcee0 ffffffe002e33a80 ffffffff82d012e8 ffffffe002e48000 ffffffe002eb5780 0000000000000001 00000000000000a7 ffffffe002e33ac0 0000000000000000 ffffffe002eb5700 00000000d96d615b 0000000000000000 ffffffff8280750f ffffffe002eb56c8 000000007ffffffe 0000000000000220 0000000000000228 ffffffff8261c340 ffffffe002eb5700 0000000000001000 ffffffff82d020a8 ffffffff8240ec28 ffffffff82400470 ffffffff82867fc0 0000000000000008 ffffffff822000e0 0000000000000000 0000000000000000 ffffffff8286ac98 000000000000003d 0000000000000000 
    TREG_P:        6
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe002eb5718 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 ffffffe002eb5700 ffffffe002eb5718 0000000000000000 0000000000000000 ffffffe002eb5718 0000000000000000 
  ...
  Trace-XX 0(10:41:43): 0x7f21787c9840 [0000000000000000/ffffffff814c65da/00004201/ff000200] 
  Trace-XX 0(10:41:43): 0x7f21787cba40 [0000000000000000/ffffffff814c680c/00004201/ff000200] 
  ------------- CS_IN_REV(0) Addr (ffffffff814c680c=>ffffffff803f9fe0) Priv(1=>1)
    GPRS:          0000000000000000 ffffffff803fcee0 ffffffe002e33a80 ffffffff82d012e8 ffffffe002e48000 ffffffe002eb5780 0000000000000001 00000000000000a7 ffffffe002e33ac0 0000000000000000 ffffffe002eb5700 00000000d96d615b 0000000000000000 ffffffff8280750f ffffffe002eb56c8 000000007ffffffe 0000000000000220 0000000000000228 ffffffff8261c340 ffffffe002eb5700 0000000000001000 ffffffff82d020a8 ffffffff8240ec28 ffffffff82400470 ffffffff82867fc0 0000000000000008 ffffffff822000e0 0000000000000000 0000000000000000 ffffffff8286ac98 000000000000003d 0000000000000000 
    TREG_P:        0
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe002eb5718 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 ffffffe002eb5700 ffffffe002eb5718 0000000000000000 0000000000000000 ffffffe002eb5718 0000000000000000 
  Trace-XX 0(10:41:43): 0x7f21789d0000 [0000000000000000/ffffffff803f9fe0/00004201/ff000200] kernfs_link_sibling
    block recoverred with header ffffffff803f9fe0(ffffffff81730ff4, ffffffff81731016), TPC=ffffffff81731000
  Trace-XX 0(10:41:43): 0x7f21789d0340 [0000000000000000/ffffffff81731000/00004201/ff000200] 
  riscv_cpu_tlb_fill ad 20 rw 1 mmu_idx 1
  page table SV39 walk level 0: pte(82f08000)=0
  riscv_cpu_tlb_fill address=20 ret 1 physical 0000000000000000 prot 0
  ------------- CS_OUT_FROM_BLK(0): Addr(ffffffff81731000=>ffffffff80003180) with bpc=ffffffff803f9fe0(ffffffff81730ff4,ffffffff81731016), Priv(1=>1) for `store_page_fault`
    GPRS:          0000000000000000 ffffffff803fcee0 ffffffe002e33a80 ffffffff82d012e8 ffffffe002e48000 ffffffe002eb5780 0000000000000001 00000000000000a7 ffffffe002e33ac0 0000000000000000 ffffffe002eb5700 00000000d96d615b 0000000000000000 ffffffff8280750f ffffffe002eb56c8 000000007ffffffe 0000000000000220 0000000000000228 ffffffff8261c340 ffffffe002eb5700 0000000000001000 ffffffff82d020a8 ffffffff8240ec28 ffffffff82400470 ffffffff82867fc0 0000000000000008 ffffffff822000e0 0000000000000000 0000000000000000 ffffffff8286ac98 000000000000003d 0000000000000000 
    TREG_P:        0
    CARG:          0000000000000000
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe002eb5718 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 
    TREGS:         0000000000000000 0000000000000000 ffffffe002eb5700 ffffffe002eb5718 0000000000000000 0000000000000000 ffffffe002eb5718 0000000000000000 
  Trace-XX 0(10:41:43): 0x7f217876ce00 [0000000000000000/ffffffff80003180/00004201/ff000200] handle_exception
  Trace-XX 0(10:41:43): 0x7f217876cf80 [0000000000000000/ffffffff814c6160/00004201/ff000200] 

汇编是这样的：::

   0xffffffff803f9fe0:   bstart  b.std, bnext.concat, battr:none, bget:0x00084200, bset:0x00000c02, ptr:------ size:0x11, bnext:------,
   0xffffffff803f9ff0:   bstart.concat         bnext.call ptr:0xffffffff81730ff4 bnext:0xffffffff814790e0
        0xffffffff81730ff4:    get      s3
        0xffffffff81730ff6:    addi     t#1,24
        0xffffffff81730ff8:    set      a0,t#1
        0xffffffff81730ffa:    get      s1
        0xffffffff81730ffc:    sd       t#1,[t#4,24]
        0xffffffff81730ffe:    const    0x0 # 0
        0xffffffff81731000:    sd       t#1,[t#6,32]   <--- 地址在这里，非法地址是0x20，也就是说t#6是0，但从这里看，照理说我们应该拿到ffffffe002eb5700才对。
        0xffffffff81731002:    const    0x0 # 0
        0xffffffff81731004:    sd       t#1,[t#8,40]
        0xffffffff81731006:    addi     t#8,0
        0xffffffff81731008:    get      a4
        0xffffffff8173100a:    sd       t#2,[t#1,0]
        0xffffffff8173100c:    get      s3
        0xffffffff8173100e:    ld       [t#1,8]
        0xffffffff81731010:    const    0x48 # 72
        0xffffffff81731012:    add      t#2,t#1
        0xffffffff81731014:    set      a1,t#1

这样，我就知道错误是什么了：::

  static TCGv get_blk_t(DisasContext *ctx, int offset)
  {
      CPUState *cs = ctx->cs;
      CPURISCVState *env = cs->env_ptr;
      int index = (env->ibpc - offset -1 + 8) % 8;
      if(index < 0) {
          generate_exception(ctx, RISCV_EXCP_ILLEGAL_INST);
      }
      return blk_t[index];
  }
  
ibpc是翻译阶段根据指令距离生成的，但如果保存了再回来，原来计算的距离就不对了。

比如原来第10条指令，输出寄存器就固定是1，但我们的备份方法是把1备份到
bstate.treg-0的位置，回来的时候ibpc设置成0，但这样没有用，因为生成指令的时候已
经写了1。要解决这个问题，我们必须在运行的时候动态读ibpc的值，但这个值读出来还需
要作为下标求treg变量的位置。这个代码不好写啊。关键是，每次执行一条微指令后，还
要更新ibpc，才能保证这个下标在动态运行的时候就是正确的。

要不换个思路。如果我们维持翻译时的下标，那么恢复的时候我们必须知道备份的时候的
ibpc，虽然硬件接口定义不支持保存这个参数，但我们多放一个寄存器谁都管不着，这样
也能hack过去，但这样好恶心。

先想想吧。

20220621
========

针对T寄存器指针的问题，昨晚想了半天，没有什么好办法，只能硬上了。用动态的方式计
算T寄存器。

这就需要把原来模模糊糊的TCGv的原理深入分析清楚，看看怎么深度引用了。

我把这个分析单独写在这里：:ref:`TCGv`\。

根据这个分析，根本没有接口对Temp寄存器进行二次索引。更简单的方法是根本不要用
Temp寄存器，干脆直接用内存，这样读写Treg都是内存操作，执行的时候先从内存中读出
内容，完成计算写回内存中就好了。但这样意味着几乎所有指令都要重写，相对于我们的
证明来说，这很不值得。

其实还有一个更流氓的做法：把Bstate的SR寄存器的数量做成一个动态参数，要保存多少让
块引擎告诉OS，这样，在我们的版本中，我们增加一个寄存器，就用来保存ibpc，这个问题
就解决了。我觉得对于我们的模拟，这个方法是最有效的。

要不我们走这条路？

这个问题留给王州他们，我看看双核cpuinfo找不到的问题。首先是多核使能的修改还没有
合入主线，这个等合入主线再查，另一方面，cpuinfo的内容怎来的，我分析了一半，就
分析完吧：

cpuinfo是fs_initcall()函数，创建方法是proc_create("cpuinfo", ..., &ops)。内容是
seq_open(&cpuinfo_op)实现的。所以这就是个seq文件。cpuinfo_op的实现是分平台的，linx
在arch/linx/kernel/cpu.c中，里面就是根据cpu_online_mask一个记录一个记录打印而已。

seq_file依赖四个函数：start/next/stop/show，算法如下：

* p = start(handle, &index) 初始化，index从0开始
* show(handle, &p)
* p = next(handle, p, &index) 循环
* stop(handle, p)

只要p不是错误，就一直循环，所以，如果这个东西显示不出来，只能说是某条记录返回了
错误，如此而已，随便花点时间都可以查出问题来，我就不去参和了。

20220622
========

评审了一下多核使能的代码，基本上是把单核的head.S重新做了一次，我觉得也没有什么
可评审的。

今天glibc的用户态也初步使能完成了，vi等命令可以正常运行。

20220627
========

大部分时间都在考虑下一步的工作，特别是去RV化的工作。我总体的考虑是把现在我们用
不到的功能都删除或者改造，这样我们就在现有的基础上有一个新的定义了。这包括：

1. 重排系统寄存器，看能能不能创建一个更大的系统寄存器空间。这可以把我们用到的系
   统寄存器都提取出来，不用的删掉，用的换给接近的名字改掉，基本上就全有了。

2. 换页表，这个随便选一个格式进行创新，同时看看能不能把块头和块身的页表分别标记
   出来，这样不容易被通过简单的方法攻击（能改块头不一定能改块身）。也可以考虑能
   否换成MidGard的方式。

3. 我暂时还不太能够接受ACR，但可以用少一些级别的ACR取代现在的特权级方案。相应也
   可以换掉那些异常寄存器，处理模式，Delication模式等设计。

4. 相应异常流程是否还需要保留scratch？这个可以深入设计一下真正的执行流程

5. PMP是否必须，这个也可以特别进行设计

6. Order部分整个可以换一个描述从RV继承。（这个地方需要深入思考一下，其实对外说
   我们是RV上发展的，说不定可以持续利用RV上的收益，这不一定是坏事）

期间解决了一个立福报告的qemu Assert的问题：没有body的块没有复位TPC，导致下一个
块启动了非意图的恢复操作，这算是逻辑不完整的错误吧，Assert很好起到滤出问题的目
的，算是开发中的一环吧。（但事实证明这个分析不对，看明天的分析说明）

20220628
========

首先，早例会上王州指出我昨天的分析不对，无body的块并不需要复位TPC，因为它应该继
承前一个块的初始化结果，这里只应该加assert，而不是初始化它。这有道理，这个问题
等他们继续查，我需要先逻辑上设计一下他们发现的另一个更严重的问题：

RV有一个明显明显“程序顺序”，就算可以OOO执行，但仍可以通过barrier强制这个顺序。
而块指令的程序顺序是不明显的。

我们现在强制了一个内存上的“程序顺序”，但在寄存器上，这种程序顺序还是不存在的，
如果一个块在执行的中间打断了，那么，执行过的内存会被提交，但寄存器不会，如果我
们想确定一个寄存器和内存上的先后顺序，这其实是不成立的。

这个问题我在推动态ebstate的LARM分支上描述过，我尽快确认一下文博和若愚的意见，把
分支合并过来。

晚上讨论了下一个阶段的工作列表，曾博的期望是做去RV的工作，我开始着手准备这方面
的事情。

20220630
========

昨天请假了，中间抽空建模了一下程序和内存的顺序模型，今天回来放到LinxTechAnalyse
工程中。短期我取向于在qemu中暴露寄存器的更新状态，让内核在中断上下文中基于更新
的SP建立堆栈。长期我取向于升级块头的定义，在块头中说明堆栈准备要求。

20220705
========

之前去弄下一个阶段的建模去了，没有开发日志，主要是更新了LARM，采用了暴露寄存器
更新状态的方法解决SP的问题。

今天本来也想去弄下一个建模的，但立福有一个问题拖了几天了，我帮忙看看。我在他的
现场看了，问题是内核返回到用户态，目标地址在USR_SIG的入口上，但要求恢复指令。我
和立福讨论说，这个最大可能是内核在返回用户态的时候发现有信号处理，所以回到信号
入口，但当初设置了中断点是块内，所以出的错。我本来想回来帮忙跟一下流程的，但我
用最新的内核和qemu有其他错误，设计新的页表方案那里我有新的灵感，让立福继续跟吧。

中间给王州建了一下qemu-user发生ebreak如何保存ebstate的建模：::

  你一个用户程序，里面知道自己是块，在块中间主动断出去，你认为你去了内核，等内
  核干点活，你认为后面内核会恢复你的执行上下文，你什么都不知道的情况下，就会从
  下一条微指令继续下去。这是“被模拟的用户程序”的世界。而这个世界就是你qemu提供
  的，现在你qemu模拟到了ebreak，把用户程序停了，自己去干自己的私货，干完了，你
  不就是回到原来的下一条微指令继续执行下去吗？哪里来的ebstate？

20220706
========

页表的设计完成层1的设计了，我发给quxinhao作为我们使能其他替换RV功能的参考，他说
他已经做了页表的设计了，比Midgard更好，我让他给我一个抽象设计。今天还没有收到，
我不是太相信他的宣传，还是先做完层2的设计。

下午帮忙推进李飘和陆春华的一个问题。这个问题出在这个代码上：::

  if (jp) {
        -->1
        struct procstate *ps = &jp->ps[jp->nprocs++];
        -->2
        ps->ps_pid = pid;
        -->3
        ...
  }

现在观察到的现象是，在1,3两个点上插入跟踪，会发现nprocs执行完后就值会从1变成3
（fixme：好像是这样变的？），而不是预期的1变成2。

但如果在2的位置上插入一个跟踪点，错误就不发生了。这有两种可能：

1. 编译器生成了错误的代码，两行代码生成在一个块里面的时候，计算错误。
2. Qemu执行错误

我看了一下他们的跟踪（这个结果他们是用反汇编来看内容的，这么隔一道我都不明白他
们怎么会不担心的，我请他们重新跟踪了），但我可以先简单看看：

合在一起的块：::

  get s0             临时变量，我猜这东西可能是堆栈某个指针
  ld [t#1, -40]      读成员，我猜是jp
  ld [t#1, 16]       这就应该是jp->ps
  ld [t#3，-40]      又读一次（这个模式像这个成员是个volatile的啊，否则用t#2做变址就可以了）
  lw [t#1, 28]       变址访问另一个成员，应该是jp->nprocs
  addi t#1, 0        桥接前一条指令，应该还是jp->nprocs                        <------------- 反正要修改addi，这个地方也可以输出一下输出寄存器
  slli t#1, 32       左移32位
  srai t#1, 32       算术右移回来（这可能是个标准32位符号扩展操作）
  addi t#1, 1        加1，这条指令应该是jp->nprocs++                           <------------- 这里一个点，输出一下输出寄存器
  slli t#1, 32       加完马上又做一次标准符号扩展操作
  addi t#8, 0        桥接前面的jp->ps
  srai t#2, 32       完成前面的符号扩展
  set a2, t#2        a2=jp->ps
  get s0             又拿堆栈内的指针
  addi t#8, 0        桥接前面没有加之前的jp->nprocs                            <------------- 这个位置也可以输出一下
  ld [t#2, -40]      再拿jp
  set a3, t#1        a3=jp
  sw t#7, [t#2, 28]  28这个位置是jp->nprocs，但回写的是jp->ps，这个代码不对吧？除非[s0+16]那个位置才是nprocs，但按数据结构它不在ps前面啊。
  slli t#7,32        jp->ps无符号数符号扩展
  srli t#1,32        
  slli t#1,4         乘16，这应该是ps[0]的大小
  add t#7, t#1       之前的nproc的值
  get s0             又来
  sd t#2, [t#1, -32] 写到一个新位置中，我猜这是pid
  ld [t#2,-32]       又读回来（难道这个pid也是volatile的？）
  set a5, t#1        写到a5
  lw [t#4,-56]       这是一个新的局部变量，只能认为是ps->ps_pid。
  set a4, t#1        写到a4
  sw t#2, [t#4, 0]   这应该是pid写入ps->ps_pid                 <----------- 后面都不碰nprocs了，前面的输出如果同时输出一下内存里面的值，应该可以定位到哪一步不对了。

20220707
========

昨天的问题后来直接看李飘机器上的测试结果，初步定位是块内缺页后，内核没有按块内
缺页给参数返回，导致块重启了。等内核去定位。

下午完成页方案的高层设计了。

20220711
========

这两天是试了一下glibc的编译过程，基本方法我写在这里：:ref:`compile_glibc`\ 。

没有编译过去，最后遇到的问题是这个：::

  ../sysdeps/linx/bits/wordsize.h:22:3: error: #error unsupported ABI
     22 | # error unsupported ABI

这个sysdeps目录是glibc自己的，根据字长来决定各种定义，字长靠编译器提供：::

  xlen=`$CC $CFLAGS $CPPFLAGS -E -dM -xc /dev/null | sed -n 's/^#define __riscv_xlen \(.*\)/\1/p'`

一路找过去初步看到是出错的时候工程用了g++，而不是linx64-linux-gnu-gcc去编译程序。
陆春华提示加上CXX的定义就好了。



.. MARK: NEW LOG IS HERE


综合分析
============

GDB Remote Serials协议速查表
----------------------------

调试老要用这个，放一张速查表，遇到新的就补充进来：

?
        Report why the target halted.

c, C, s and S
        Continue or step the target (possibly with a particular signal). A
        minimal implementation may not support stepping or continuing with a
        signal.

        Server收到通知返回的信息类似这样：T05thread:p01.01;

D
        Detach from the client.

g and G
        Read or write general registers.

qC and H
        Report the current thread or set the thread for subsequent operations.
        The significance of this will depend on whether the target supports
        threads.

k
        Kill the target The semantics of this are not clearly defined. Most
        targets should probably ignore it.

m and M
        Read or write main memory.

p and P
        Read or write a specific register.

qOffsets
        Report the offsets to use when relocating downloaded code.

qSupported
        Report the features supported by the RSP server. As a minimum, just the
        packet size can be reported.

qSymbol::
        (i.e. the qSymbol packet with no arguments). Request any symbol table
        data. A minimal implementation should request no data.

qfThreadInfo/qsThreadInfo
        请求线程信息，返回会是mp01.01这样的形式。

vCont?
        Report what vCont actions are supported. A minimal implementation
        should return an empty packet to indicate no actions are supported.

X
        Load binary data.

z and Z
        Clear or set breakpoints or watchpoints.

H
        To specify which thread a subsequent command should apply to.

q
        The query packets related to threads, qC, qfThreadInfo, qsThreadInfo,
        qGetTLSAddr and qThreadExtraInfo will need to be implemented.

T
        To report if a particular thread is alive.

vCont
        To specify step or continue actions specific to one or more threads.
        例如： vCont;c:p1.-1

git bisect方法
--------------

原理很简单，先找一个版本确定没有问题的，比如v1，做如下操作：::

  git bisect start              #开始bisect
  git bisect bad                #说明现在的版本是坏的
  git bisect good v1            #说明v1是好的
                                #这个命令完成后git给你选一个测试的版本
  git bisect good/bad/skip      #测试并报告这是好的还是坏的，如此循环，编译不过的就skip，最后就有结果了。
  git bisect reset              #退出bisect

.. _`cpu_exec_two_level_loop`:

cpu_exec两层循环的逻辑
----------------------

cpu_exec的两重循环是这样组织的：::

  set_jmp();
  while cpu_handle_exception():
    while cpu_handle_interrupt():
      tb翻译执行...

首先，任何时候你都可以用cpu_loop_exit()系列函数跳回到set_jmp()上，然后重新开始
做两个判断。所以，其实cpu_loo_exit()是给你个机会重新判断一次有没有中断和异常，
然后决定是否执行下一个tb翻译执行。

内层的判断是没有异常只有IO的场景，所以不用重新开始，但如果你有异常，这个地方也
得退出去。所以，这个循环有两个控制变量：cpu->interrupt_request和cpu->exception_index。
前者是硬件中断类型，比如复位，外设（外设算一种）等。有这个有值了，同时要设置
cpu->exception_index为EXCP_INTERRUPT，这样才能跳出第一层循环的限制。

但cpu_handle_interrupt()这个名字很不好，因为它并非在处理中断，而是
cpu_need_to_handle_interrupt()，如果need_to了，就退出循环，好到外面去处理中断。

而cpu_handle_exception()倒真的处理了一部分的异常，只是处理不了的，它还是要返回一种类型，
从而离开exec_cpu，并且用这个cpu->exception_index作为返回值，让你知道exec_cpu()的结果，
从而采取相应的行动。

其实我觉得这个地方特别误导人，构造得好的话，应该一层循环都可以代表所有情况了。

.. _`linux_sched`:

Linux调度器分析
---------------

这个小节独立分析一下Linux的调度器算法，作为分析挂起问题的理论准备。

主调度函数是__schedule()，这有详细的注释，说明有可能调用这个函数的地方是什么：

1. 主动的调度函数，比如mutex，或者io等待的时候会做cond_resched()（语义是有条件
   就调度）

2. 中断或者异常返回而TIF_NEED_RESCHED被设置了

抢占的原理也在这里解释了：CONFIG_PREEMPT决定的是内核自身能否发生调度，没有配这
个，调度只能发生在主动调度和从内核返回用户态的时候。如果有配置，那么每个中断返
回都可以调度，前提是没有preempt_disable()，preempt_disable()是个counter，可以一
层层叠加，然后对称减少，变成0的时候，就是可以抢占了。

进入调度的时候会调用__might_sleep()，这个函数的含义是这里可以调度了，主要会做一
些状态机检查，避免不应该的地方进入调度了。

现在回到__scheduler()，它有一个参数sched_mode，用来给定调度策略（比如RT方式的抢
占之类的），这改变算法。

然后我们总结一下这个函数一些特征：

1. rq是个percpu变量，而不是每个算法一个，所有算法都作用在唯一个rq上。但其实每个
   算法有自己的控制结构。rq只是代表，不是真的就一个统一的queue。每个任务是知道
   自己属于哪个rq的（task->sched_class）。任务放回rq的时候用的是对应class的放回
   函数（put_prev_task）。所以其实一开始就已经分开了。

2. rq_lock()是个大设计，它主要控制rq的读写，需要在task->pi_lock下工作。它主要要
   避免同时拿着两个cpu的rq（要用需要用double_rq_lock）。要拿也要保证先拿小cpuid
   的，然后才轮到大的。拿到rq_lock以后才轮到驱动时钟hrtimer的锁。

3. task->state/on_rq/on_cpu是task的三个关键调度状态，都插在各个关键流程中更新的，
   不能随便就用或者更改。其中state主要是信号，是否休眠这些状态标志。

4. rq->clock是rq的调度时钟

5. ttwu是try to wake up的意思

6. 然后所有算法都在pick_next_task()里面了，这个完成以后，如果pick出来的任务和原
   来的不同，就会调用switch_to()，这就纯是个适配问题了。

从这个流程看，如果有调度而没有调到高优先级任务，只能说内部算法出了什么问题了。

然后我们看kernel/sched/core.c:pick_next_task()里面的算法，这个函数本层其实主要
是做迁移和cpu offline，真正的实现在__pick_next_task()，这个函数调用的前提是这个
core没有离线。

__pick_next_ask()的算法挺特殊，它是如果当前线程的sched_class小于fair，同时总线
程数等于fire的线程数，就直接跳过其他调用用fair来调度。如果这个条件不成立，才把
当前任务挂回去，枚举所有的class来进行调度。

枚举里面主要包括这样一些类型：::

   1   2645  kernel/sched/deadline.c <<GLOBAL>>
             .pick_next_task = pick_next_task_dl,
   2  11755  kernel/sched/fair.c <<GLOBAL>>
             .pick_next_task = __pick_next_task_fair,
   3    511  kernel/sched/idle.c <<GLOBAL>>
             .pick_next_task = pick_next_task_idle,
   4   2629  kernel/sched/rt.c <<GLOBAL>>
             .pick_next_task = pick_next_task_rt,
   6    129  kernel/sched/stop_task.c <<GLOBAL>>
             .pick_next_task = pick_next_task_stop,

其他算法是纯粹的算法，我这里先不管，我重点看管理性的stop和idle的算法。这两个在
整个枚举的一头一尾，有stop就肯定不会调用其他，有其他就一定不会调用idle。

先看stop，这里其实就rq->stop一个任务，创建的地方在kernel/stop_machine.c:
smpboot_register_percpu_thread()，线程名就是大名鼎鼎的migration。kthread_create
没有地方可以指定这个任务是这个class的，而是直接通过sched_set_stop_task()强行修改
任务的sched_class来修改的。stop任务只是循环执行workqueue里面的work，没有work就
退出（对的，是退出），而work基本上查到的都是关闭cpu。所以，我怀疑这个work永远不
会停的，只是在不真关闭的时候，在work里面yield而已。

CPU offline就kthread_park，online再kthread_unpark回来。

而喂狗（touch_nmi_watchdog）是所有模块都要做的，不是仅这个模块在做，只是这个模
块也需要做而已。

idle的原理是类似的，也是只有一个线程，sched_init()->init_idle()里面直接设置
class，它就是start_kernel自己，初始化到最后，做一个kthread_create创建init，
自己就调用cpu_startup_entry()，里面其实就是while(1)do_idle()。

WARN_ONCE()
-----------

Linux这个功能我一直没有看，也不关心细节上有什么用，现在调试qemu多了，发现这个功
能还是挺必要的，因为一旦出错就会没完没了打印。所以我顺手看了一下它的实现：发现
它干的活还挺精巧的：它根据所在的文件和行，创建了一个段，在段里面设置一个标记，
第一次调用后填上这个标记，这样这个位置上的的打印就不会再出来了。

.. _`TCGv`:

TCGv原理分析
------------

按TCGv的注释，它是一个目标字长的变量，但它的内容不是这个变量，而是一个针对这个
变量的索引，而这个索引如何编码，这是个内部的问题，使用的一方管不着。

定义上它是一个struct TCGv_xx_d的指针，但这个结构完全不存在，定义这个指针只是为
了传参的时候类型不匹配可以报错而已，这些TCGv_i32/i64/ptr/vec其实都是同一个
target_length长度的变量。

真正的变量是CPUState->ctx中的一片内存，这片内存的规模就比较大了，它的格式是
TCGTemp，里面有名字有状态，不仅仅是要用到的那片内存（用到的内存是TCGTemp->val）。
TCGTemp和TCGv之间通过内部算法进行互相映射，现在的映射算法是&TCGTemp-&ctx。也就
是这个变量的在变量区的偏移用作TCGv的值。

两者的转换函数是tcgv_xx_temp()（直接转temp），tcgv_xx_arg()(转成指令参数，其实
也是temp），和temp_tcgv_xx()（转成tcgv），

中间码生成的时候，仅仅就是记录中间操作码和操作数（也就是tcgv_xx_arg），到每个
native平台的运行的时候，它会先做寄存器分配，先用寄存器进行计算，计算完后，把
output寄存器的内容同步到tcgv_xx_arg中（这意味着，至少一条Guest指令内部，可以连
续使用寄存器，只有离开一条指令了，才需要同步到内存）。

这个同步的生成代码在tcg_reg_alloc_op()->temp_sync()->
tcg_out_sti(ctx, r->type, r->reg, r->mem_base->reg, r->mem_offset)中实现。

可以看到，所有需要的描述，都在寄存器自己的描述中有了，reg就是映射的寄存器，
mem_xx是它在内存中的位置。

signal(7)阅读笔记
-----------------

解决qemu的用户态gdb模拟的时候需要用到这个概念，我一直对它理解得很浅，这个东西后
面迟早要面对，我这里深入读一次手册。

Linux的信号有两种，一种是标准的所谓“可靠信号”，一种是后来扩展的所谓“realtime信
号”。后者的特点是可以发多个不会丢失，顺序有保证，前者不保证。下面提到的函数都需要
使用rt_版本才能工作。

每个信号有一个disposition（任命），可以是Term（终止），Ign（忽略），Core
（Coredump），Stop（停止）和Cont（继续）。除非明确说明，每个信号都可以指配一个
信号处理函数，这个函数默认使用当前堆栈，但可以修改。

信号处理通过signal()和sigaction()设置，前者不建议，兼容性不好。可以用raise给自
己发，用kill给别人发，但qemu注释说raise有bug，所以它全部用kill。kill有线程版本
的pthread_kill()。

实时信号用sigqueue发，这种信号不是同步的，可以写到队列中。

pause和sigsuspend用于停下等信号，后者可以暂时打开掩码等。
sigwaitinfo/sigtimewait()等可以同步等待并处理信号。而不需要靠异步回调。

信号可以被掩盖，用sigprocmask()操作，有pthread的版本。如果有多个线程，针对进程
的信号找没有掩盖的线程调度。

信号处理过程如下：

1. 内核回用户态前发现当前进程有信号，而且符合处理条件，在用户态分配堆栈，保存上
   下文。
2. 内核为信号上下文分配堆栈
3. 禁止同一个信号重入，返回信号处理函数
4. 信号处理返回，先回到trampoline程序
5. trampoline调用sigreturn系统调用，重新返回内核，重新进行调度。

SIGKILL和SIGSTOP都不能被mask，block或者catch。

标准中断也可以queue，但细节没有定义。

setpgid(2)笔记
--------------

这是分析一个用户态执行问题的时候写的笔记，原来只记得进程组用来管理一个管道序列，
但不记得具体接口了，这里快速补一下要素：

1. pg和session是接近但不完全重合的概念。pg是一组共享信号的进程，把多个pid加到一
   个pg id中，那么你可以用kill(-pg_id, sig)的方法给pg中所有进程发信号，它仅管理
   这个概念。而session是一个新的pg，当你调用setsid()的时候，你得到一个新的pg，
   你自己是pg的头（Leader），但之后你要不要创建更多的pg，那是你自己决定的。控制
   台的输入会发到一个pg的Leader，而信号会发给pg的每个成员。

2. session内多个pg的概念，主要用来支持前后端的管理。如果你把一个pg放到后台，你不
   会希望控制台的信号会发给它们。

.. _`compile_glibc`:

编译glibc的方法
---------------

当前建议的方法是这样的：::

  git clone ssh://git@codehub-dg-y.huawei.com:2222/LinxBaseSoftware/OS/glibc-linx.git
  
  cd $build-dir
  $glibc-linx/configure CC=linx64-linux-gnu-gcc CXX=linx64-linux-gnu-g++ --host=linx-linux-gnu --with-headers=/usr/include --prefix=$install_dir --enable-shared=yes
  make -j            # 这一步唯一是生成依赖，后面的错误不用管
  
  $glibc-linx/configure CC=linx64-linux-gnu-gcc CXX=linx64-linux-gnu-g++ --host=linx-linux-gnu --with-headers=/usr/include --prefix=$install_dir --enable-shared=no
  make -j       # 这一步可能有这种warning：Warning: side effects to X6 are ignored during the instruction mapping for pseudo tail，可以忽略

  make install

其中/usr/include目录下必须有Linux内核头文件，在Ubuntu下面这个文件被另外放到
x86_64_xxx目录下了，需要进行特殊处理。

那个enable_shared=yes是强行做的，反正也编译不过，是为了前面的一些初步拷贝操作可
以完成了，从而让第二步可以完成下去。
