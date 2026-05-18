JWJ开发日志
***********

20220407
============

目前还在处理 LXR 的事，在弄的 virt machine 移植遇到了些状况。在这里记录下。

1. 因为 LXR 的 interrupt control 相关的东西还没有出来，然后也不能直接使用 sifive 的
   plic（sifive_plic.c 中会调用 riscv 下的 ``riscv_claim_interrupt``）。
   所以，plic 相关的东西都将其注释或用 NULL 替换了。相关的 device 包括 uart0。

2. system 模式下的异常处理没有实现，这个需要留给他们来弄了。

3. 因为直接搬运 riscv 的 virt，它的 rom 中的 boot program 还是用的是 riscv 的指令，
   这个我没有改，异常也没有实现，system 起来，直接就到了 monitor 界面。

   **todo**
   
       ``rom_add_blob_fixed_as`` 看起来是将 boot program 烧写进 rom 的操作。细节还
       没有看。有待了解。

4. 之后更改了 ``lxr_setup_rom_reset_vec`` 中的 boot program。可以启起来了，日志也显
   示到，程序跑到了 ram 处，去运行 bare-metal 的程序。但是根据日志里来看，有重复执行
   的问题。

   即，执行了 I1、I2、I3...In(最后我是弄了个死循环，防止跑飞)，JL(0) 指令没有按照预期
   的执行，程序从 I2 开始执行，然后到 In。这里反倒 JL(0) 起作用了。。

   JL 的操作是：link=PC+4, PC=PC+imm

   从中间码来看，不确定是不是 ``goto_tb`` 引起::

       ---- 000000008000004c
       mov_i64 link,$0x80000050                 sync: 0  dead: 0 1  pref=0xffff
       goto_tb $0x0
       mov_i64 pc,$0x8000004c                   sync: 0  dead: 0 1  pref=0xffff
       exit_tb $0x2b080cdd56c0
       set_label $L0
       exit_tb $0x2b080cdd56c3

5. singlestep 运行到第一个 stw 指令后，进入死循环。。从日志来看，它反复执行该条指令。
   
6. 不知道是不是把 plic 相关的移除了，uart0 的 ``interrupt-parent`` 给注释掉了。现在
   往 uart0 写字符，没有打印在屏幕了。（riscv 运行没有毛病）


virt 和 sifive_u 的 reset vector 不同。virt 的是 0x1000，sifive_u 的是 0x1004。但
是 rom 的地址都是 0x1000 开始，sifive_u 在 0x1000 这个位置用了 ``s->msel`` 来占位。


**todo** LXR 遗留问题

    + exception 处理实现 -> 优先级：高
    + JL(0) 指令第一次执行时未起作用 -> 优先级：高
    + uart 未打印 -> 优先级：中
    + singlestep 运行遇到第一条 stw 指令后，进入死循环 -> 优先级：低


20220411
============

disas 的指令解码在 ``decode_inst_opcode``，解码后，就在 dec->op 里设置一个枚举值，该
值定义于 ``rv_op``。里面值以 ``rv_op_`` 开头。

op 主要是用来索引 ``opcode_data`` 这个数组，定义如下::

    typedef struct {
        const char * const name;
        const rv_codec codec;       /* 指令操作数提取 */
        const char * const format;  /* 汇编输出格式 */
        const rv_comp_data *pseudo; /* 伪指令相关 */
        const short decomp_rv32;    /* 以下用于 RISCV 的压缩指令 */
        const short decomp_rv64;
        const short decomp_rv128;
        const short decomp_data;
    } rv_opcode_data;

本来是想着是定义个枚举 ``blk_op``，但是这样也需要再弄个 block 的 ``opcode_data``，这
样感觉有些冗余了。然后，Block 这边的话，就想以 ``blk_op_`` 开头，同时在
``opcode_data`` 里加上 block 指令的信息。

.. note:: 

    Block 暂时未实现压缩指令。

    QEMU 中的 riscv disas 不支持 call 这种由两条指令组成的伪指令。
    Block 的伪指令在 wiki 汇编手册最后定义了两条，在 LARM 中未看到。暂时不实现。


20220412
============

`RISC-V 的 disas 总结 <https://wiki.huawei.com/domains/4821/wiki/12455/WIKI2022031000966?title=a154851c-34ff-f535-aec8-27a7af2f2d4a>`_ 

目前 BlockISA 的 disas 只需要实现指令解码、操作数的提取以及汇编指令的输出。

关于解码部分，head的指令解码与 RISCV 的一起解码，而微指令用 block.decode 文件生成出
来的 switch-case 判断。disas 中 head 的解码还需要获取 bpc1 和 bpc2 用来表明是否在块
内。

操作数提取这一块，RISC-V 是自己通过移位获得各个字段的，比如说 j 类型指令的 imm::

    static int32_t operand_jimm20(rv_inst inst)
    {
        return (((int64_t)inst << 32) >> 63) << 20 |
            ((inst << 33) >> 54) << 1 |
            ((inst << 43) >> 63) << 11 |
            ((inst << 44) >> 56) << 12;
    }

而在 decodetree 里，使用 extract 和 sextract 等去提取，不过，微指令
中各字段是连续编码的，且指令数量不多，倒不太需要依靠 decodetree。

指令编码这部分也需要更改，因为 head 指令由 128 位组成，如果是粘头，还需要提取 256 位。
除了 head 外，还有一些变长的指令，lconst(32bit~48bit~64bit~80bit)，sysset.const
(这个还没有实现)。


20220413
============

现在 block 的 disas 一运行就用可能会进入死循环，我觉的是在 disas 里，``inst_length``
的问题。它的默认情况返回的是 0，即，如果有个 opcode 是 0x7f 开头的指令，其 len 就认为
是 0 了。

而在该 print_insn 的外头那个函数 ``target_disas`` 是按照这个 len 移动 PC。

.. code-block:: C

    void target_disas(FILE *out, CPUState *cpu, target_ulong code,
    target_ulong size)
    {
        ...
        /* code 是 tb 的 pc_first，size 是 tb 的 size */
        for (pc = code; size > 0; pc += count, size -= count) {
        fprintf(out, "0x" TARGET_FMT_lx ":  ", pc);
        /* 这里的 count 是 inst_length 返回的指令长度，字节为单位 */
        count = s.info.print_insn(pc, &s.info);
        fprintf(out, "\n");
        ...
        }
    }

刚好 block 里 lr 和 sc 指令是 0x7f 开头，但也不一定是这两指令造成死循环。因为 disas
还未支持 block 指令，解析 block 时，inst_length 不一定按照预期，错误堆积过程，就可
能刚好下一条指令我就提取到错误的部分。

关于 head 的处理，其指令编码为 128 bit 或 256 bit。一开始是想着尽量不大改动，将
rv_inst 定义为一个 256 位的变量。大概是下面这样::

    typedef struct { uint64_t elements[2]; } uint128_t;
    typedef struct { uint128_t elements[2]; } uint256_t;

但这种形式不能直接赋值，即 ``uint128_t insn = 0x1000``，而且不能对它直接进行左移右移
去操作数。


20220414
============

对于异常和中断，disas 是怎么处理的？
in_asm 的定义就是在翻译阶段结束，执行阶段之前，打印 guest 指令，直接就把当前 TB 的指令
都打印出来了。并没有反映出程序执行的一个情况。（或是我理解有误，需要验证下）
假设异常发生在 tb 中的某条指令，就比如说，load 指令加载地址未对齐，那在执行的时候，就
会去到异常处理程序，去那里跑指令，之后在回到 laod 指令。那么日志这里怎么体现出，是
load 指令产生了异常？

之后可以写一个 RISC-V 的 bare-metal 测例跑跑看看。现在先暂时不管，先把 block 指令长度
处理弄好，然后跑一遍，看看昨天的想法是否正确。


20220415
============

in_asm 进入死循环的问题，加上对 block 指令长的支持后，可以顺利的走下去，看来的确是因为
判断指令长那个函数默认值为 0 导致的。

对于 64 位以上的指令，我想的是将其指令编码按 64 位拆分，存进一个数组中。
disas 里还是通过判断 PC 是否在 bpc1 和 bpc2 之间，来决定是 rv 解码还是 blk 解码。

head 的指令编码打算单独输出一行，然后里面的字段就另一起一行再输出::

    PC: inst
    bstart b.std bnext.concat bget bset size
    bstart.concat bnext.fall ptr bnext


20220416
============

head 的输出方式想了下面两种，第一种类似于编译器的输出，但是我们可以直接通过指令编码
长度就可以知道这个头是粘头还是普通头，不管是粘头还是普通头都是一个 header。
所以我倾向于第二种方式。

0x0000000000000000: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
bstart b.std bnext.concat ireg() oreg() size:0 battr:
bstart.concat bnext.fall ptr:0x000 bnext:0x1000

0x0000000000000000: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 bstart b.std bnext.fall battr ireg() oreg() size:0 ptr:0x000 bnext:0x1000

今天王州和国柱在群里聊了下 disas。我理解的意思是说，有两级 PC，一级 PC 用来指向 rv 和
header 指令，二级 PC 用来指向块内的微指令。一级 PC 即 bpc，bpc 指向 header 和 rv 指
令，用 bpc 取代 rv 的 cpu_pc。二级 PC 即 tpc。将一级 PC 传入 ``log_target_disas``
中。而 ``log_target_disas`` 的 size 就表明这个 tb 有多少个 header 或 rv 指令。
然后，当解析到 header 的时候，把它的 body 一同打出来。

但有个问题，header 和 body 是分属不同 tb 的，因为 header 和 body 在内存上可能是不连
续，所以解析完 header 后，会有个跳出 header tb 并查找 body tb 的操作。（话说为什么
tb 要在跳转的时候结束 TB？按着我当前的工作来看，即站在 disas 角度看，tb 是一串在内存上
是连续的指令块，disas 翻译的就是这一块连续内存空间，pc 通过指令长来自增。如果 header
和 body 处在同一个块，而内存不连续。。。）
那么 header 那个 tb 打出了 header 和 body 后，去到 body 的 tb，那应该是打还是不打
呢？

现在先实现了 header 操作数提取以及汇编输出。这一块与上面的定义不冲突。

国柱提交了一个 disas 框架，还没有去看。现在弄的，基于 pc 的 disas 提交到了
linx-jwj-disas 分支上。不过目前只能打印头部，实现上还有点问题，用的是定长的字符数
组 buf[128]，header 会有部分内容打不出来。。。

.. note:: 

    话说 get、set 指令和 header 的 get、set 掩码好像很容易出问题啊！看 kernel 那边这
    里出了几次错误了。这个不算是语法错误？编译器能不能弄一弄？


20220418
============

上午跟国柱对了下反汇编框架的思想。

之前我实现的框架是基于先有 head 后有 body 的情况。对当前架构具有依赖性，当架构允许 
body 跳 body 的情况，因为解析出来的 bpc1 和 bpc2 还属于上一个 block，把跳过来的
body 当作 rv 来解。

国柱当前提供的反汇编框架直接把 rv&head 和 body 完全分开，解到 head 时，将其 body
一同打出来，而 pc 指向 body 时，不对其解码。

micor instruction 的解码部分直接拿 decode 生成的来用，节省时间。当前框架默认 body 是
2 字节，解码 body 指令时，还需要返回该指令的长度。


20220419
============

微指令解 opcode 部分用 decodetree 替代后，只需要填空就行。但当前微指令的反汇编中，还
有以下几个问题或待优化的：

1. 因为存在变长指令，指令编码的打印需要变动。
2. 未知指令的 default 选项未处理。
3. 微指令操作数对齐输出。
4. const 和 lconst 的立即数用十六进制和十进制输出。

前两条已完成


20220420
============

今天主要是修复一些反汇编上的 bug 以及格式调整，并了解 kernel 中的 head.S。

修复的 bug 主要还是输出格式的改变，包括块指令操作数的左对齐，以及 lconst 和 const
立即数的输出。

其中能记录的也只有 lconst 提取 64 位立即数的 bug。该 bug 在于 extract16 返回的是
uint16_t 类型的变量，因为未先将其转为 uint64_t，其左移操作仍是在 16 位上操作。


20220421
============

user mode 的 linx_debug 参数添加，还不如 system 的。user 的需要自己去解参数，而
system 有相应的模板，看国柱把参数是添加在 qemu-options.hx 文件中，这个应该是为了
方便 help 的输出吧。重点还是在 vl.c 中，定义了个 ``qemu_linx_debug_opts`` 结构
体，其中定义了 linx_debug 下的各个参数及其类型。解参交给 qemu 解决，我们只需要去
opt_get 就行。

user 的需要自己解参。。在其中就用了 g_strsplit 来 split 字符串，g_strv_length 获取
split 出的字符数组的长度。

user 的退出还不知道怎么处理。。user mode 运行程序感觉就是在 Linux 运行个程序，而这些
程序用的大概就是 exit(0)。但好像又哪里不对。

持续整理个 :ref:`Linux 启动<qemu_linux_startup>`


20220422
============

昨天给 user mode 添加 linx_debug 参数时，让 qemu 停止直接是用 exit(0)，其产生的结果
是，当程序运行到这，也就直接停了，没有后面的什么内存释放之类的操作。所以，想着能不能给
qemu 设个 exit 信号，让 qemu 自己去模拟 exit 操作，然后在正常的退出 qemu。

QEMU 模拟程序里写的 exit 最终走的是系统调用，riscv 中使用的 syscall num 是 94，即，
exit_group。在关闭线程前，QEMU 会将 gdb remote 关闭，刷新 tb，plugin 退出。对应函数
``preexit_cleanup``。
之后就调用 POSIX 的 ``syscall`` 去关闭当前线程及子线程。QEMU 模拟 exit 操作::

    case TARGET_NR_exit_group:
        preexit_cleanup(cpu_env, arg1);
        return get_errno(exit_group(arg1));

    exit_group(arg1) => syscall(__NR_exit_group, arg1)

当 exit_group man 手册里写的有点怪，在 Description 里说::

    This  system  call  is  equivalent to exit(2) except that it terminates not
    only the calling thread, but all threads in the calling process's thread
    group.

而在 notes 里却说，exit(2) 使用的系统调用是 exit_group::

    Since glibc 2.3, this is the system call invoked when the exit(2) wrapper
    function is called.

好吧。。我还是跟着 qemu，用 ``syscall`` 来使用 exit_group。但是 arg1 这个参数是什么
意思？man 里倒是给了 exit_group 的函数原型，但是没说 status 是干啥的::

    void exit_group(int status);

我就先用着 0 吧，这个值是从之前程序运行的日志里看到的。因为声明 ``preexit_cleanup``
的头文件未在 include 目录下，cpu_exec.c 还无法直接就包含，我就把里面的内容提取出来放
到 ``linx_debug_check_tb`` 里，内容如下::

    shutdown:
    #ifndef CONFIG_USER_ONLY
        //qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        printf("linux_debug: machine paused, try to use monitor to diagnose\n");
        vm_stop(RUN_STATE_PAUSED);
    #else
    #ifdef CONFIG_GPROF
        _mcleanup();
    #endif
    #ifdef CONFIG_GCOV
        __gcov_dump();
    #endif
        /* 这里的 0 于 exit_group 的 status 是同个值 */
        gdb_exit(0);
        qemu_plugin_user_exit();
    #endif /* CONFIG_USER_ONLY */

编译运行，出现问题。程序结束后 shell。周日再回来看看哪里有问题，问题如下::

    jiawenjie@linux-dev:build$ ./qemu-linx -linx_debug max_insn=5000 ./test
    test_getw: passed
    ...
    cpu 0: linx_debug max insn (5000) reach on 5009
    从这后，程序无反应。

王州昨天提了个小问题，下面这个 body 被拆成了两个 tb::

    IN:
    Priv: 1; Virt: 0
    0x00000000802002e0:  00000200 00000406 81602400 abd19c8b aux_block.concat   next:0x82208440, ptr:0x813abff8, attr:none, ireg(ra,sp,a0), oreg(s1)
    0x00000000802002f0:  00000011 00000200 00000000 00000800 aux_block.call
    0x00000000813abff8:  0000000001857ffe060a  lconst           25526270                        # 0x1857ffe
    0x00000000813ac002:  0022                  addtpc          t#1
    0x00000000813ac004:  0292                  set             sp, t#1
    0x00000000813ac006:  0912                  get             s1
    0x00000000813ac008:  0a92                  set             a0, t#1

    Trace 0: 0x7ff64c0c33c0 [0000000000000000/00000000802002e0/00004201/ff000200]
    ----------------
    IN:
    Priv: 1; Virt: 0
    (block body) 0x813abff8+10 in (0x813abff8-0x813ac00a)

    Trace 0: 0x7ff64c0c3540 [0000000000000000/00000000813abff8/00004201/ff000200]
    ----------------
    IN:
    Priv: 1; Virt: 0
    (block body) 0x813ac002+8 in (0x813abff8-0x813ac00a)

    Trace 0: 0x7ff64c0c3680 [0000000000000000/00000000813ac002/00004201/ff000200]
    ----------------
    IN:

.. note::

    这个问题被国柱查出来是因为到了页边界，所以另起了个 tb。（这里的页边界地址看的是
    guest 的地址，还是 host 的？）如果是 guest 的，0x813abff8+10 不是超过了
    0x813abfff？


20220424
============

接着周五的问题。
定位到是 ``qemu_plugin_user_exit`` 引起的程序异常，往这函数里看，看到个
``start_exclusive``，其注释写着::

    Start an exclusive operation.
    Must only be called from outside cpu_exec.

虽然知道了该函数需要在 ``cpu_exec`` 外调用，但还是想了解下为什么。

下午去给 Linx RISC 调整反汇编那个脚本，还是写个文档说明怎么用，怎么改吧。免得下次还
得找我。


20220425
============

``start_exclusive`` 要求运行在 cpu_exec 外，其实应该是要在 ``cpu_exec_end`` 后
运行。``start_exclusive`` 里定义

可以在调用前使用 ``cpu_exec_end`` 释放互斥区。
我先提交到其他分支上去，之后再看看。

关于之前说，qemu system 跑 image，反汇编里没有符号信息，然后想可不可以写个脚本，
用 kernel 根目录下的 System.map 来与日志里各 TB 块里首条指令的地址做个对比。
但这样需要区分物理地址和虚拟地址，刚好当前物理和虚拟地址在前 32 位上不同，虚拟地址前
32 位都为 1。

脚本这个李飘已经写好，放在 devlog 下了，就是 logTrans.py


20220427
============

关于 linx_debug，王州和国柱都有说明，记一下自己的理解。

在这里，我们分两个方面去考虑，一个是 kernel 层面，一个是 QEMU 层面。
并不是说 kernel 那边提出了各种形式的需求，我们这边就需要为其实现，一如立福在提出能打出
字符串的基础上，我们能不能再弄个打印 ulong，16进制打印。其实这种功能完全可以在 kernel 
那边使用 sprintf 格式化字符串。

我们这边需要实现的其实是通用性的接口，能使得 kernel 只使用我们这些接口实现大部分功能。


20220428
============

什么是架构设计，这个让人有点摸不着头脑，感觉有条线能抓，又抓不到。

国柱昨晚提供了个硬编码 Tag 的方案，在程序中添加一条调试指令，直接硬编码进去，可以避免之
前的函数调用，之前的函数调用使用一条 header 指令，以及 RISC-V 的 ret 指令。这里的话，
只需要使用一条 header 就行。

立福那边发现在启用 mmu 后，无法打印字符串，同时日志里显示 "memory unaccessible"，国柱
怀疑是因为虚拟地址引起的。看 monitor 里对内存打印也是区分了物理地址和虚拟地址，即命令
x 与 xp。可以去看 ``memory_dump``。

首先，得有个方法来区分上层软件传过来的地址是虚拟的还是物理的。虽然说当前 kernel 开启
mmu 后，将 kernel 映射到高地址段，以 FFFFF... 开头，但是当在 kernel 跑去应用时，这时
候的地址可就不是 F 开头了。目前只想到了一下几个方案。

1. 先用物理的读一遍，看看是不是 unacess 的，是的话在用虚拟的读一遍。这应该是最不靠谱
   的。
2. 将 dump memory/string 各拆成两个，也就是使用四个 attr。但这样需要在 kernel 里区分
   什么时候开了 mmu，然后再决定用哪一个。跑应用，就直接用虚拟地址的。
3. 额外使用个函数参数，来表明地址的属性。但这种不适合硬编码，虽然说当前的都不能兼容硬编
   码。

暂时想不出来还有没有其他解决方案，目前感觉第二点会好点？

之后，还需要怎么去打印虚拟地址内存。这一部分可以参考 monitor 的实现。

经过国柱的讲解，知道了为啥 QEMU 的 monitor 会提供 x（读虚拟地址的内存）和
xp（读物理地址的内存）。

首先，OS 传出来的都是虚拟地址，不管 mmu 有没有开。mmu 开了，也只是做个映射。
好的，我们之后来说下 x 与 xp。给 x 一个地址，不管是物理的还是虚拟的，他都会经过 mmu。
mmu 开了，那就用映射后的地址去找内存；没开，那还是进 mmu 前的地址。那么我们读内存的功能
就能用 x 这条指令的 function 来做。

xp 指令就是不管来的是虚拟地址，还是物理地址，都会不经过 mmu，直接去内存去读取数据。

::

                       +---+   0x1234    +---+
                       | M | ----------> | M |
    +---+   0x1234     | M |             | E |
    | C | -----------> | U |             | M |
    | P |  0xffff1234  +---+             | O |
    | U |                                | R |
    +---+ -----------------------------> | Y |
                                         +---+


20220503
============

ecall 在 user mode 也出现了问题，与 system mode 一样。都是 ecall 那个块结束后，
直接往下执行。

ecall 是当前块结束后再去处理异常，而 trap 是立即去处理。

ecall 测试用例如下::

  BFUNC blk_ecall, HEAD_TYPE_SYS, BRANCH_FALL_THROUGH, 16, BLK_COMMON, 9, 0, 0b10000111<<10, 0b11<<10
  BLOCK_HEAD_16B_ALIGN HEAD_TYPE_STD, BRANCH_RET, (16 / 2 + 9), BLK_COMMON, LINX_RET_LEN, 0, 0, 0b10
      ECALL()
      GET(a0)
      GET(a1)
      CONST(1)
      SET(a0, 0)
      SET(a1, 3)
      SET(a2, 3)
      CONST(64)
      SET(a7, 0)

      LINX_RET

``linx_generate_exception`` 里有句话，在这里需要说一下::

    tcg_gen_movi_tl(cpu_pc, ctx->base.pc_next-2);

这里有点奇怪，我觉得是在做 user mode 的时候，cpu_loop 里用的是 riscv ecall，这会使
pc += 4，但是 block 这边，微指令是两字节的，应该是 pc += 2。所以为了 trap 返回的时候
能回到下一条指令，这里给它减了个 2。

但是，这种情况下 ecall 觉得它不行了。（ecall 和 trap 都使用的是
``RISCV_EXCP_U_ECALL``）
ecall 是块结束后，才会去处理异常，这时候的 pc 是指向块最后一条指令的，等去到
``cpu_loop`` 后，pc += 2，那就去到最后一条伪指令后的下一条微指令了。

trap 是直接处理异常，在去处理前，需要保存上下文，也就是 bstate 那一套。处理完回来后，
先去到块头，恢复上下文，再去到 trap 的下一条指令，也就是 pc += 2.（如果 trap 是块的最
后一条指令呢？），前面的是普通块的流程，原子块是处理完异常后，重新执行块。

ecall 是块提交阶段去处理，先去提交，后去处理。提交时 pc/bpc 会根据 branch type 设置，
但最后 pc/bpc 是由异常处理程序决定。一般情况下，在里面不会改动 pc/bpc。

但现在 do_jump 里赋值完 cpu_pc 和 bpc 之类的，就会 exit tb，还不能用 do_jump 设置
bpc 之类的，再去 handle exception。

**还有，LINX_EXCP_U_ECALL 这个没有用到过。** 当前 kernel 还是使用的 riscv
exception code。


20220504
============

trap 保存现场到 bstate 是在 do_interrupt 里，而这函数是只有 system mode 才会用到，
那如果是 user mode 的话，它怎么去保存？之后去到 head 也就不会有恢复的操作。。

一开始的实现是，会将当前异常指令的 pc 先往上移一个微指令大小，即 2byte。然后去到异常
处理程序，在这是 QEMU 的 cpu_loop 内的 switch-case，这时候，pc 往下移一个 rv 指令，
也就是 4 byte，那么刚好指向下一个微指令。而且也不会改变 bstate，所以异常返回也没有什么问题，测例能通过。。但这不合规。

现在更改了实现，异常产生时，pc 保存的是 bpc。
这里先梳理下 trap 在 QEMU 里新的执行流程:

1. user mode

当异常产生时，保存当前异常指令的块头pc--bpc 到 pc 寄存器中。
去到软件这边，在这里是 cpu_loop，qemu 在 user 模式下实现的 syscall，使用的是 host
上的 syscall。之后，因为当前还未支持 block，所以仍然用着 rv 的行为，也就是将 pc 指向
下一条指令。

那么这里就有问题了，因为异常产生时，QEMU 的 pc 保存的是 bpc，去到异常处理的时候，pc
是指向下一条 rv 指令，也就是往下移了 4 个字节，而这时候，其指向的是块头内的 attr 的
位置。QEMU 解码的时候，也就会产生 illegal instruction 异常。

2. system mode

异常产生时，pc 先指向 bpc，然后去到硬件的异常处理流程中(cpu_loop)。
先保存 bstate，再将 sepc 指向块头，pc 指向软件定义的异常处理函数入口。
当前 kernel 内的实现似乎只是将 sepc 往下移了个微指令。。没有区分 trap 和 ecall。
（见 entry.S 中的 save_a0_and_update_epc）。

trap 已知的流程（还未看到有定义）:

+ 遇到 trap 指令时，立马处理异常
+ 保存现场 bstate。t0~t9 寄存器、shadow register 0~31、sbpc、tpc
+ 置位 bstate 的 vld；
+ 将异常 pc (epc) 赋值为当前块头地址 (bpc)（存疑！没看到哪里写）
+ 去到异常处理程序，软件将 en 置位。
+ 异常处理函数返回，pc 指向 epc 位置
+ 块头检查 en 是否置位，如果是，恢复 Bstate

ecall 已知的流程（还未看到有定义）：

+ 遇到 ecall 指令后，延迟执行异常
+ 当前块进入提交阶段时，将影子寄存器提交
+ 将异常 pc (epc) 赋值为当前块头地址 (bpc)（存疑！没看到哪里写）
+ 进入异常处理程序，sepc 指向下一个块（这部分似乎也没有定义）
+ 异常返回

这里对 ecall 有个问题，之前 rv 的 ecall 异常处理程序是将 sepc + 4，
也就是指向 ecall 的下一条 rv 指令。但是当前 block 的 ecall 有它的一个块
属性，假设，当前 ecall 的块是一个条件块，里面有条件跳转类型的指令，这个块
走完后，会拿到一个 next block 的值。那么我觉得 ecall 的异常 pc 不应该是
bpc，而是计算出来下个块的地址。

.. note::

    LARM 没有定义异常处理的行为，那我们可不可以来点 block 独有的风格，如果是
    fall through 的块，ecall 异常处理返回就去到下一个块；call 块，ecall 异
    常处理返回就去到对应函数去。其实就还是按着块的跳转属性来。

20220507
============

今天看了下 trap 类型的，块内处理异常的指令。其保存上下文 bstate 的步骤被
放到了 do_interrupt 内，这会导致 user mode 测试该条指令时，无法保存现场，
而且在 cpu_loop 里也没有对 ext.en 的置位操作。

这个下星期再回来看看怎么更改。


20220509
============

今天没有时间去看 user mode 怎么实现 ecall/trap。

不过目前还是有点疑问，因为当前 user mode 为了能兼容 rv 指令，其对 ecall
异常的处理是将 pc 指向 ecall 的下一条指令。而 block 相应的是指向下
一个块。也就是需要在异常处理的地方，区分是 rv 还是 block。(kernel 那边
没有这个烦恼，kernel 内都是用 block 指令实现的。)

上午修的 store 类型指令其实是因为使用旧的指令定义实现的，会将下一个存储
地址，也就是存储数据的地址加上数据类型大小，作为目的 T 寄存器的值，而 LARM
里定义其值是存储数据的地址。


20220510
============

因为前面出现了几次实现与 LARM 定义不同的情况，所以每个人分配了点指令检查。
我这边检查 load/store、conditional setbpc 类型指令。

**l[b|h|w|d|bu|hu|wu] [<T#M>, <imm-sXLEN>]**

<T#M> 定为 link0 寄存器，<imm> 为有符号立即数。

有符号扩展加载：b-byte，8 位；h-half word，16位；w-word，32位；
无符号扩展加载：bu-byte，8 位；hu-half word，16位；wu-word，32位；
d-double word，64位；

half word 因为是两字节，其地址最低1位应该为0，为了节省空间，l[h|hu] 的
imm 不保留最低1位。其他类型数据同理。那么地址计算时，就需要先将 imm 就行
移位。例如，l[h|hu] 的 imm 需要左移1位。

将移位后的 imm 与 T#M 相加即为数据所在内存地址，将数据取出后，将其存到目的
T 寄存器中。

**s[b|h|w|d] <T#N>, [<T#M>, <imm-sXLEN>]**

<T#M> 定为 link0 寄存器，<imm> 为有符号立即数，<T#N> 定位 link1 寄存器。

内存地址的计算同上。将 T#N 中的值存进计算后的内存地址，并将内存地址返回给目的
T 寄存器。

**setbpc.condi <T#M>, <imm-XLEN>**

cond:

带 u 的为无符号，进行比较时，将 <T#M> 与 <imm-XLEN> 进行无符号比较。
其他的，比较时，进行有符号比较。

+ eqi-equal，                 <T#M> 等于与   <imm-XLEN> 
+ nei-unequal，               <T#M> 不等于   <imm-XLEN>
+ gei-greater than and equal，<T#M> 大于等于 <imm-XLEN>
+ lti-less than，             <T#M> 小于     <imm-XLEN>
+ geiu-greater than and equal，同 gei
+ ltiu-less than，同 lti

条件满足，sbpc 设为 bpc 加上地址偏移 label_offset，label_offset 由 head 里的 
bnext 计算，如果是粘头，bnext_offset2 作为 label_offset 的高 32 位，bnext_offset1
先左移 4 位后(因为块头是16字节对齐)，作为 label_offset 的低 32 位；非粘头，
label_offset 只用 bnext_offset1。

conditional setbpc 只有 condition head 才能用。同时，没有输出，不会用到目的 T 寄存
器。

下午了解下 atomic 实现，看看怎么实现 user mode 的原子块。


20220514
============

打算从 EXCP_ATOMIC 着手，看看 QEMU 怎么处理原子的，再想怎么实现 user mode
的原子块。

user mode 与 system mode 的不同：

+ user mode 没有中断
+ system mode 异常交由 guest 软件处理，user mode 通过使用 host 的系统调用和信号模
  拟 guest 的异常处理。

原子块被中断或产生异常时，所有对寄存器和内存的修改都不会改变。这里的异常是指造成
块指令异常终止的情况，如：trap、brk等，这些架构状态为块执行前的状态。而像 ecall、
ebrk等，会让块指令提交后再产生异常，架构状态为块提交后的状态。

原子块指令只能访问一个对齐 Cacheline 的内存地址，否则产生 illegal instruction
异常。

那么当前 QEMU 是怎么做的呢？

当在解析块头时，发现当前是原子块的话，会产生一个异常 EXCP_ATOMIC_BLK（来自于
EXCP_ATOMIC），当前 cpu 会跳出 normal 的执行流（tcg_cpus_exec），等待其他
cpu 从互斥区出来，进入 atomic 执行流（cpu_exec_multi_steps_atomic）。之后，
当前 cpu 申请进入互斥区，生成/获取一个不会被中断、不链接的 TB(如果是 EXCP_ATOMIC，
这个 TB 内只有一条指令，而 block 的装的是整个 body。)

执行时，分有异常和无异常情况。

无异常，TB 是在互斥区内执行的，不会被其他 cpu 影响::

  ->mttcg_cpu_thread_fn
    ->tcg_cpus_exec
      ->cpu_exec
        ->helper_jump_to_atomic_context  --->发现是原子块，就产生 EXCP_ATMOIC_BLK
  ->mttcg_cpu_thread_fn
    ->cpu_exec_multi_steps_atomic  --->解头时，已将 pc 指向 body
      ->cpu_tb_exec  --->块正常执行并提交

有异常，这里分为使块异常终止，以及正常提交。正常提交同无异常，原子块已结束。
异常终止情况，如：trap 类指令。块执行终止，保存了 body 上下文（此处不同于 wiki），
跳出当前 atomic 执行流，进入异常向量。处理返回后，块头重新解码，发现是原子块，不恢复上下文，同时再次申请进入互斥区::

  ->mttcg_cpu_thread_fn
    ->tcg_cpus_exec
      ->cpu_exec
        ->helper_jump_to_atomic_context  --->发现是原子块，就产生 EXCP_ATMOIC_BLK
  ->mttcg_cpu_thread_fn
    ->cpu_exec_multi_steps_atomic  --->解头时，已将 pc 指向 body
      ->cpu_tb_exec  --->块执行时，产生异常
  ->mttcg_cpu_thread_fn
    ->tcg_cpus_exec
      ->cpu_exec  --->在上一轮执行中，已经设置了 exception_index
        ->riscv_cpu_do_interrupt  --->将 pc 指向异常向量
      ->cpu_exec  --->执行异常向量，返回后，重新解块头
  ->mttcg_cpu_thread_fn
    ->cpu_exec_multi_steps_atomic
      ->cpu_tb_exec  --->块正常执行并提交
    
上面的是 system，那么 user 是怎样的::

  ->cpu_loop
    ->cpu_exec  -->检测到是原子块，产生异常
  ->cpu_loop
    ->cpu_exec_multi_steps_atomic  -->若产生异常，设置 exception_index，并退出去
  ->cpu_loop  --->使用 host 的系统调用或信号量，模拟 guest 异常处理
    ->cpu_exec  --->重新解析块头，发现是原子块，产生 EXCP_ATOMIC_BLK
  ->cpu_loop
    ->cpu_exec_multi_steps_atomic

为了实现，块被中断或异常终止时，对内存的修改不会提交，并且原子块只能访问
一个对齐 Cacheline 的内存地址。QEMU 使用了个 64Byte 的 store_buf，只
有提交时，才会将 store_buf 里的内容写入内存。

由上，可知道 user mode 在处理原子块的流程上同 system mode 大体上相同。
在代码上只需要打开 user_only 的宏，在 cpu_loop 中添加对 EXCP_ATOMIC_BLK
的支持。

将宏打开后，修改了下测例。之前的测例选用原子加做测试，不停的对一个内存加1，
为了防止有缺页的情况，将该块内存映射到巨页。但 user mode 没有这种页缺失的
情况，所以只需要一个普通的变量即可。

运行出现问题。sd 指令没有生效，没有往内存里写入值。在 store buf 相关的
helper 函数里，加了各种打印，发现 store_buf 里居然是空的。再一细看，
memcpy 的地址指针增加有问题，原来是这样的::

    // store_buf 是一个长度为8的64位无符号整型数组。指针加1，就会往后移8B
    memcpy(env->store_buf + extract64(addr, 0, 6), &data, size);

改成这样::

    memcpy((uint8_t *)env->store_buf + extract64(addr, 0, 6), &data, size);

拉最新分支跑后，出现问题::

    *** stack smashing detected ***: terminated

基于当前分支，另起了个本地分支，并另建了个 work tree，回退到之前的版本，
5.11 的 3a86ea8121。跑了之后没有问题。那么就是 qemu 代码的问题。该问题
似乎是因为越界访问或是访问释放后的内存导致的。看了下出问题的 log，它连
测例的 head 都没有跑到。

逐步的往前找，找到了个提交，自从它开始，就会有该异常。5.13 的 8dc8fc1709。
这个分支是娟姐合入国柱那个 Handel ecall 的 patch。

这个周一再回来查。

疑点有：

+ 为什么 TB 不能 chained？chained 后，TB 运行就顺着往下走，不回到 cpu_exec?
+ CF_PARALLEL、CF_NO_GOTO_TB、CF_NO_GOTO_PTR、CF_NOIRQ 具体如何实现的
+ 中断是设了 icount_desc 的高位，听说是会跳过当前 TB？还需要去看看
+ store_buf 只有 64 byte，所以我们就只模拟一个 Cacheline size 为 64B 的？
+ 日志加了 singlestep 后，原子块是否还能正常运行？
+ 当编译器在编原子块时，能否确保body里 st 的地址在同一个 Cacheline？

下面有待整理

pending_cpus 代表处在互斥区内的 cpu 线程数量。

TB 的 cflags CF_NO_GOTO_TB 和 CF_NO_GOTO_PTR 是为了防止 TB 链起来，一个
是 goto_tb，一个是 goto_ptr。

goto_tb index:

退出当前的 TB，
设置了两个跳转槽（jump slot），一个 taken 时，tb 的地址，一个时 not taken 的地址。

.. code-block:: C

    struct TCGContext {
        ...
        /* goto_tb support */
        tcg_insn_unit *code_buf;
        uint16_t *tb_jmp_reset_offset; /* tb->jmp_reset_offset */
        uintptr_t *tb_jmp_insn_offset; /* tb->jmp_target_arg if direct_jump */
        uintptr_t *tb_jmp_target_addr; /* tb->jmp_target_arg if !direct_jump */
        ...
    }

taken 和 not taken 的地址
RISC-V 中只有间接跳转类型指令，如：jal rd,imm 的目标地址需要通过 PC 与 imm 相加得到；
jalr rd,rs1,imm 的目标地址需要通过 rs1 + imm 得到。这些都需要借助于 pc 或寄存器。


20220516
============

光看那个提交，也看不出来啥。我就让这两个 qemu（syscall patch 前的，后的）跑同一个
可执行二进制程序。通过比较日志看看。

一开始就不一样了，sp 的值不同。。这个应该是相同才对吧，运行的程序是相同的，也只是改动
了 user mode 的 ecall 模拟代码。在进到测例前，就有段程序运行不同，在函数 strchr。

虽然在 strchr 前有几次系统调用，但是将它调用返回值以及，之后把一轮判断后 pc 的变化打
到日志里去看，也没有发现什么问题。

sp 的值::

    run pass                run fail
    0000004000800460        0000004000800450

run pass 的程序执行过程::

  IN: strchr
  0x000000000012f304:  00757713          andi            a4,a0,7
  0x000000000012f308:  87aa              mv              a5,a0
  0x000000000012f30a:  0ff5f693          andi            a3,a1,255
  0x000000000012f30e:  e719              bnez            a4,14           # 0x12f31c

  IN: strchr
  0x000000000012f31c:  0007c703          lbu             a4,0(a5)
  0x000000000012f320:  fed719e3          bne             a4,a3,-14       # 0x12f312

  IN: strchr
  0x000000000012f312:  0785              addi            a5,a5,1
  0x000000000012f314:  0077f613          andi            a2,a5,7
  0x000000000012f318:  c355              beqz            a4,164          # 0x12f3bc

  IN: strchr
  0x000000000012f31a:  c619              beqz            a2,14           # 0x12f328

  IN: strchr
  0x000000000012f31c:  0007c703          lbu             a4,0(a5)
  0x000000000012f320:  fed719e3          bne             a4,a3,-14       # 0x12f312

  IN: strchr
  0x000000000012f312:  0785              addi            a5,a5,1
  0x000000000012f314:  0077f613          andi            a2,a5,7
  0x000000000012f318:  c355              beqz            a4,164          # 0x12f3bc

  IN: strchr
  0x000000000012f31a:  c619              beqz            a2,14           # 0x12f328

  ============================

  IN: strchr
  0x000000000012f328:  0ff5f593          andi            a1,a1,255
  0x000000000012f32c:  00859613          slli            a2,a1,8
  0x000000000012f330:  8e4d              or              a2,a2,a1
  0x000000000012f332:  01061713          slli            a4,a2,16
  0x000000000012f336:  8f51              or              a4,a4,a2
  0x000000000012f338:  02071313          slli            t1,a4,32
  0x000000000012f33c:  00e36333          or              t1,t1,a4
  0x000000000012f340:  00037817          auipc           a6,225280       # 0x166340
  0x000000000012f344:  9b883803          ld              a6,-1608(a6)
  0x000000000012f348:  5e7d              addi            t3,zero,-1
  0x000000000012f34a:  638c              ld              a1,0(a5)
  0x000000000012f34c:  853e              mv              a0,a5
  0x000000000012f34e:  07a1              addi            a5,a5,8
  0x000000000012f350:  00b348b3          xor             a7,t1,a1
  0x000000000012f354:  01058733          add             a4,a1,a6
  0x000000000012f358:  01088633          add             a2,a7,a6
  0x000000000012f35c:  8f2d              xor             a4,a4,a1
  0x000000000012f35e:  01164633          xor             a2,a2,a7
  0x000000000012f362:  8f71              and             a4,a4,a2
  0x000000000012f364:  01076733          or              a4,a4,a6
  0x000000000012f368:  ffc701e3          beq             a4,t3,-30       # 0x12f34a

run fail 的程序执行过程::

  IN: strchr
  0x000000000012f304:  00757713          andi            a4,a0,7
  0x000000000012f308:  87aa              mv              a5,a0
  0x000000000012f30a:  0ff5f693          andi            a3,a1,255
  0x000000000012f30e:  e719              bnez            a4,14           # 0x12f31c

  IN: strchr
  0x000000000012f31c:  0007c703          lbu             a4,0(a5)
  0x000000000012f320:  fed719e3          bne             a4,a3,-14       # 0x12f312

  IN: strchr
  0x000000000012f312:  0785              addi            a5,a5,1
  0x000000000012f314:  0077f613          andi            a2,a5,7
  0x000000000012f318:  c355              beqz            a4,164          # 0x12f3bc

  IN: strchr
  0x000000000012f31a:  c619              beqz            a2,14           # 0x12f328

  IN: strchr
  0x000000000012f31c:  0007c703          lbu             a4,0(a5)
  0x000000000012f320:  fed719e3          bne             a4,a3,-14       # 0x12f312

  IN: strchr
  0x000000000012f312:  0785              addi            a5,a5,1
  0x000000000012f314:  0077f613          andi            a2,a5,7
  0x000000000012f318:  c355              beqz            a4,164          # 0x12f3bc

  IN: strchr
  0x000000000012f31a:  c619              beqz            a2,14           # 0x12f328

  ============================

  IN: strchr
  0x000000000012f31c:  0007c703          lbu             a4,0(a5)
  0x000000000012f320:  fed719e3          bne             a4,a3,-14       # 0x12f312

这能看个啥。得走其他路径。strchr 的原型是这样的::

    char *strchr(const char *s, int c);

从字符串 s 中搜索第一次出现字符 c（一个无符号字符）的位置。那么是不是因为开了个 work
tree 来运行 pass 的程序，然后这两个 qemu 的目录不同，导致在 strchr 这里运行结果不同？

之后，我在同个目录来运行这两个 qemu，strchr 这一块相同了，连 sp 也相同了。==

再跑一下，有个明显的差异点。

run pass::

  IN: __clone
  0x000000000014e0f6:  15c1              addi            a1,a1,-16
  0x000000000014e0f8:  e188              sd              a0,0(a1)
  0x000000000014e0fa:  e594              sd              a3,8(a1)
  0x000000000014e0fc:  8532              mv              a0,a2
  0x000000000014e0fe:  863a              mv              a2,a4
  0x000000000014e100:  86be              mv              a3,a5
  0x000000000014e102:  8742              mv              a4,a6
  0x000000000014e104:  0dc00893          addi            a7,zero,220
  0x000000000014e108:  00000073          ecall           

  Trace 0: 0x7fd4e8041dc0 [0000000000000000/000000000014e0f6/00004200/00000200] __clone
  ----------------
  IN: __clone
  0x000000000014e10c:  00054563          bltz            a0,10           # 0x14e116

  Trace 0: 0x7fd4e8000100 [0000000000000000/000000000014e10c/00004200/00080200] __clone
  ----------------
  IN: __clone
  0x000000000014e10c:  00054563          bltz            a0,10           # 0x14e116

  Trace 1: 0x7fd4e8000100 [0000000000000000/000000000014e10c/00004200/00080200] __clone


run failed::

  ----------------
  IN: __clone
  0x000000000014e0f6:  15c1              addi            a1,a1,-16
  0x000000000014e0f8:  e188              sd              a0,0(a1)
  0x000000000014e0fa:  e594              sd              a3,8(a1)
  0x000000000014e0fc:  8532              mv              a0,a2
  0x000000000014e0fe:  863a              mv              a2,a4
  0x000000000014e100:  86be              mv              a3,a5
  0x000000000014e102:  8742              mv              a4,a6
  0x000000000014e104:  0dc00893          addi            a7,zero,220
  0x000000000014e108:  00000073          ecall           

  Trace 0: 0x7f1338041dc0 [0000000000000000/000000000014e0f6/00004200/00000200] __clone
  ----------------
  IN: __clone
  0x000000000014e10c:  00054563          bltz            a0,10           # 0x14e116

  Trace 0: 0x7f1338000100 [0000000000000000/000000000014e10c/00004200/00080200] __clone
  ----------------
  IN: __clone
  0x000000000014e108:  00000073          ecall           

  Trace 1: 0x7f1338000240 [0000000000000000/000000000014e108/00004200/00080200] __clone

从上面可以看得出来，线程0 clone 一个新线程1后，正常应该是从 ecall 后的下的下一条开始，
但是 failed 的还是从 ecall 指令开始执行，那么它就还会再次 clone。那么就会导致栈溢出。

修改了下 pc 变化的代码，测例终于通过了。稍微看了下 QEMU 中的 do_fork 实现。里面使用的
当前线程的 env 去创建 cpu 实例。新线程也就基于 env->pc 来运行，而该 pc 指向的就是那个
调用 clone 的 ecall。

哦。不对提交的 fix ecall 的 patch 似乎有点不对。。现在跑两个线程后，能跑过，但多个
跑飞了。


20220517
============

QEMU 没有毛病，是我有毛病。程序还在跑着呢，我就给它停了。让它多跑一会儿，测例能通过。它
只是执行的慢。之所以在 system mode 跑 atomic 的测例快，是因为在 atomic 的测例是生成
cpu个数的线程，然后启动 qemu-system 一般只启两个核 -smp 2。useratomic 延用了 atomic
的测例，这一块没有改，一启动就建了 80 线程（56机有80核，是真的多）。这一块给它限制下，
最多 10 线程。

之前 fix ecall 的提交，对 block 的 restart syscall 处理有问题。当 block 处理
syscall 时，为了避免块指令重新执行提交，所以当遇到 restart syscall 的时候，给它
exception_index 重新赋回 LINX_EXCP_U_ECALL。让它只需要重新模拟系统调用即可。

但是 pc 的变化这块还存在疑问。blk.trap 是将 pc 指向当前块头，blk.ecall 是将 pc
指向下一个块头。当前 pc 的更新只有 blk.ecall。（之前群里还讨论将 trap 删掉）

这块可不可以在产生 syscall 异常的时候，就将 next pc 存到 pc 中？blk.trap 存入
bpc，blk.ecall 存入 next bpc。

为什么 riscv 的定义是异常产生时，epc 保存的是当前指令的 pc？仅仅是因为硬件设计简单？不
需要去解码判读是不是某某条指令？


20220519
============

原子块指令存在着5种属性，之前只实现了 relaxed 的，这里分析下这些属性:

+ relaxed，不对读写顺序进行限制。
+ acquire，所有 acquire 原子块后的内存操作都不会出现在该原子块之前。
+ release，所有 release 原子块前的内存操作都不会出现在该原子块之后。
+ aq/rl，acquire 和 release 的结合体。要求该原子块之前的内存操作先于该块，
  之后的内存操作后于该块。
+ far，这个还没有弄懂，王州说这个暂时没有定义好，这类型在QEMU没有实现。

BlockISA 除了定义原子块指令，还有一些原子微指令（lr/sc类指令）。

riscv 的 lr/sc 指令里会有两位（aq/rl）来表示所用的内存序，分别代表 acquire、
release。如果 aq 和 rl 都置位，则该指令的内存操作将以 acquire&release 对待。
block 的 lr/sc 没有定义这两位，想来应该是用原子块属性实现。

.. note::

    国柱考虑原子块内的写操作是先写到 cache 中，提交后，再写回到内存中。这里应该是说
    store_buf 吧，内存块的写操作都会将数据暂存到 store_buf 中，等块提交后，才会写
    进内存，而 sc 的定义是立马将数据写进到内存中（如果 reservation 有效的话），这
    与原子块定义冲突。

rv 的 lr/sc:

lr rd,(rs1) 从 rs1 指向的内存地址中有符号加载数据到 rd 中，并对该内存地址注册个
reservation set，用来保存加载出来的数据。

sc rd,rs2,(rs1)，如果 rs1 指向的内存存在 reservation，同时 reservation set 包含
将要写入的数据 rs2

sc failed 的情况有:

::

  The SC must fail if the address is not within the reservation set of the most
  recent LR in program order. The SC must fail if a store to the reservation set
  from another hart can be observed to occur between the LR and SC. The SC must
  fail if a write from some other device to the bytes accessed by the LR can be
  observed to occur between the LR and SC. (If such a device writes the
  reservation set but does not write the bytes accessed by the LR, the SC may or
  may not fail.) An SC must fail if there is another SC (to any address) between
  the LR and the SC in program order

1. sc 的地址未被 reserve
2. reservation set 被其他 Hart 修改（话说，这个 reservation set 被修改只有 lr 指令
   吧）
3. sc 前一个 lr reserve 的地址的数据被写过（如果有其他设备往 reservation set 标记的
   地址段写数据后，但是没有修改 sc 最近的 lr reserve 的地址上的数据，sc 可以 failed，
   可不 failed。）
4. 在该 sc 到前一个 lr 之间，如果有其他 sc 指令，则当前 sc 会 failed。


对于 reservation 和 reservation set 不太明白它们的定义。一开始以为 reservation 是保
存 reserve 的地址，然后 reservation set 保存着该地址上的数据。之所以这么认为
reservation set，是通过这句话::

  An implementation can register an arbitrarily large reservation set on each
  LR, provided the reservation set includes all bytes of the addressed data
  word or doubleword. 

但是根据上面 sc failed 的条件里说，如果 reservation set 被修改，但是 reserve 地址上
的数据没有被该改。由感到有点奇怪。

block 的 lr/sc

lr [<T#M>]，从 <T#M> 执行的地址加载数据到 T 寄存器中，并对该内存地址设一个
reservation。

sc <T#N>, [<T#M>]，如果 reservation 有效，那么将 <T#N> 存到 <T#M> 所指向的空间，
并写 0 到 T 寄存器中。否则返回非0值。

QEMU 在 fence 操作上，对 TCG_BAR_(LDAR/STRL/SC) 似乎没有实现，只有
TCG_MB_(LD_LD/ST_LD/LD_ST/ST_ST/ALL) 有实现。也可能实现了，只是用的
不是这个枚举值。

在原子块前的 memory barrier 中间码，没有生成对应的 host 指令。这个还有什么用？

tcg_gen_mb 生成出来的 rv host 指令是 fence。是这样的::

    static void tcg_out_mb(TCGContext *s, TCGArg a0)
    {
        tcg_insn_unit insn = OPC_FENCE;

        if (a0 & TCG_MO_LD_LD) { // fence r,r
            insn |= 0x02200000;
        }
        if (a0 & TCG_MO_ST_LD) { // fence w,r
            insn |= 0x01200000;
        }
        if (a0 & TCG_MO_LD_ST) { // fence r,w
            insn |= 0x02100000;
        }
        if (a0 & TCG_MO_ST_ST) { // fence r,r
            insn |= 0x02200000;
        }
        tcg_out32(s, insn);
    }

.. note::

    为啥 TCG_MO_ST_ST 也是用 fence r,r 而不是 fence w,w?


然后 x86 只关心 TCG_MO_ST_LD::

    static inline void tcg_out_mb(TCGContext *s, TCGArg a0)
    {
        /* Given the strength of x86 memory ordering, we only need care for
           store-load ordering.  Experimentally, "lock orl $0,0(%esp)" is
           faster than "mfence", so don't bother with the sse insn.  */
        if (a0 & TCG_MO_ST_LD) {
            tcg_out8(s, 0xf0);
            tcg_out_modrm_offset(s, OPC_ARITH_EvIb, ARITH_OR, TCG_REG_ESP, 0);
            tcg_out8(s, 0);
        }
    }

是因为跟内存模型有关吗？RVWMO 以及 TSO。这个需要去了解下。


20220523
============

LR/SC 指令作为系统块的微指令，其存在的块不能有原子属性。

block 的 lr/sc 的功能实现的功能如下：

lr [<T#M>]，从 <T#M> 执行的地址加载数据到 T 寄存器中，并对该内存地址设一个
reservation。

sc <T#N>, [<T#M>]，如果 reservation 有效，那么将 <T#N> 的值存到 <T#M> 所指向的空
间，并写 0 到 T 寄存器中，否则返回非0值。最后失效 reservation。

目前是通过实现 FAA(Fetch-And-Add) 操作测试 LR/SC 指令，即，起两个线程，每个线程都先从
同一块内存中来取一个值，将这个值加一后，将结果写回去。每个线程都运行这段程序一百万次，
最后判断该内存中的值是否为两百万。

现在新加 lr.aq 以及 sc.rl 指令使得 lr 指令支持 acquire 内存序，sc 指令支持 rl 内存
序。关于让 lr/sc 支持 aq 和 rl 内存序，是为了使得程序更高效，尽量减少 sc 失败的情况。

LR/SC 指令有 acquire、release 和 acquire-release 三种内存序。acquire 用来屏障当前
指令之后的所有访存操作，release 用来屏障其之前的所有访存操作。

通过下面的例子来理解 acquire 和 release::

    *a0 += 1 可以分为：1) t0 = load *a0，2) t0 = t0 + 1，3) store *a0 = t0

对应的 rv 汇编实现为::

    1:
    lr.d t0,(a0)
    addi t1,t0,1
    sc.d t2,t1,(a0)
    bnez t2,1f  # jump to label 1 if t2 is non-zero
    ret

单 CPU 的情况是这样的::

    # section A
    lr.d t0,(a0)
    addi t1,t0,1
    sc.d t2,t1,(a0)
    bnez t2,1f
    ret
    # section B
    lr.d t0,(a0)
    addi t1,t0,1
    sc.d t2,t1,(a0)
    bnez t2,1f
    ret
    # section C
    lr.d t0,(a0)
    addi t1,t0,1
    sc.d t2,t1,(a0)
    bnez t2,1f
    ret

假设没有实现 aq 和 rl，那么上面的程序就有可能出现下面的情况，CPU1 运行的 section B 早
于 CPU0 运行的 section C，但晚于其写值，这种情况，在大小核这种频率相差较大的情况里，似
乎很容易出现::

    CPU0              CPU1
    lr.d t0,(a0)                       # section A start
                      lr.d t0,(a0)     # section B start
    addi t1,t0,1
    sc.d t2,t1,(a0)
    bnez t2,1f
    ret
    lr.d t0,(a0)                       # section C start
                      addi t1,t0,1
    addi t1,t0,1
    sc.d t2,t1,(a0)                    # section C 早于 B 写入
                      sc.d t2,t1,(a0)  #CPU1发现 a0 位置内存被修改，向 t2 写入非零
    bnez t2,1f
    ret
                      bnez t2,1f
                      ret

那么如果实现了 sc.rl，那么 section A 的 sc 指令之前不能有 section B、C 的访存指令。
（这样的话，程序流补给箱单 CPU 的了？有点懵了。）

那么 QEMU 怎么通过测例来测试 lr/sc 指令的 aq 和 rl 是否有用并按照预期呢？
按照当前的了解，块指令间可以乱序执行，块内的微指令不能乱序。那么块内微指令的执行无须内
存序约束。而内存序的约束就针对于块指令之间。lr.aq 和 sc.rl 在 QEMU 的实现就可以在进入
body 前，或者执行该微指令前，添加 memory barrier。


20220527
============

今天要实现 bstate_ext.vld.cause，在写代码前先把信息收集起来，分析下。

1. cause 在 vld 寄存器的位域表示
2. cause 的值有什么
3. cause 什么时候起效
4. cause 什么时候失效
5. bstate recovery 的时候要复原它嘛

首先，cause 用 bstate_ext.vld[1:9] 表示。可用值如下：

1-EC_SET_REGS: Invalid set_regs detected
2-EC_GET_REGS: Invalid get_regs detected
3-EC_ECALL: ecall generated exception
4-EC_INT: External Interrupt
5-Reserved
6-EC_DUP_SET: Duplicated set to the same GPR

那么 vld.cause 什么时候更新？

vld.cause 的更新可以放在产生异常前，也就是 generate_exception 前，但这
只适用于异常，中断无法更新 cause，那么放在 do_interrupt 中，但是这个只有
system mode 能进来，user mode 是直接去到 cpu_loop 中，进不了这。而且，
中断只有 system mode 有，所以就在 do_interrupt 里判断外部中断，异常在
设置 exception_index 的时候，更新 cause。

vld.cause 可以类似于 rv 的 cause，没有硬件清除的时候。同时 LARM 中定义
BSTATE_EXT 将保持不变，直到新的块中断，那么 cause 也用同样的逻辑。


20220530
============

本想等晓强把 get/set 掩码执行时检查的功能上传后，再把 vld.cause 的再合进去，
但是架构上对这块可能有问题，他那里暂时搁置了。那这些掩码检查就放在翻译时检查。
然后，重复 set 这个之前没有实现，还得另加。

之前的设计过于简单了，存在一些问题，当前 vld.cause 的值并未完全覆盖到所有
异常/中断，这样可能会对调试产生误导。例如：一个定时器中断，使得 body 的执行
被打断，这时 vld 是置位的，但是 vld.cause 可能还留存着之前的异常值。

当前似乎可以清零 vld.vld 时，顺便把 vld.cause 复位。但 vld.vld 置位以及复位
只有 system mode 有处理，user mode 反倒没有相关操作。因为 block 中能让 body
被打断的，只有 trap 指令，但是这个没有用到过。

没有跟内核对，就提上去了。他们那边是通过判断 vld 是否为零/非零，来保存 BSTATE
上下文，并置位 en。目前已经 revert 了这个 patch。


20220606
============

ext 更新为 ebstate，地址有发生变化。原先 ebstate 里固定定义的 T0-T7 以及 SR0-SR31
更改为统一的 R0-R41(等调试方案详细定义。)为了兼容之前 QEMU 的实现，这里还是先定义：
R0 为 TPC，R1-R8 为 T0-T7，R9-R40 为 SR0-SR31，R41 为 CARG(SBPC)。

块内中断有三种情况，sret restore、sret redo_ecall 到下一个块之间产生中断，以及 body
内产生异常。现在就需要有东西来区分这三种情况：
1.restore 时，ebstate 会恢复进 bstate 里，tpc 指向异常微指令，pc 指向 sepc（指向
layer1的指令）。在去到块头或rv指令前，如果遇到中断，需要 sepc 指向 pc 所指向的块头，
保存 bstate 到 ebstate，将 ebstate.st.vld 置位并初始化 bstate。

2.redo_ecall，sbpc 设置标识 redo ecall，ebstate.st.cause 置为 EC_ECALL，产生
ECALL 异常，将 vld 置为 0。pc 恢复为 sepc，指向 ecall 指令的块头。在去到块头或rv
指令前，如果遇到中断，同restore，sepc 需要指向当前 pc 所指向的块头，保存 bstate，
vld 置位。软件返回时，恢复 bstate。重新去到 ecall 的块，头解码发现 sbpc 的 bit0
被置位了，那么它会

3.body内异常，如 get/set 掩码检查是发现不一致

异常返回时，用的是 m/sret，当前添加了T寄存器作为参数。T寄存器中的值有以下含义：

1.RRAT_DEFAULT(0): 复位 BSTATE。BSTATE 里对 treg 和 sgpr 复位为 0，tpc
为 -1，carg(sbpc) 为 0, itpc 为 0，tpc1 和 tpc 2 为 -1。（这里 -1 是代表无效
地址）

2.RRAT_RESTORE(1): 恢复上下文，从 ebstate 里恢复数据到 bstate。

3.RRAT_REDO_ECALL(2): Block 重启系统调用只能重新产生异常，而不能重新执行这个块。那么
这个时候，其不同于正常的异常流程，ebstate 是无效的，ST.vld 为 0。ST.cause 更新为
EC_ECALL，这个可以用来区分是 block 还是 rv 的 ecall。ST.sz 用来指示当前块头是粘头
(256)，还是普通头(128)，这个给软件来移动 PC 到下一个块指令/rv指令。
（LARM 上 ECALL 这章没有说要设置 ST.sz，这里是所有块内中断/异常产生都需要设置）

4.其他。产生 illegal instruction 异常，并将 ST.cause 赋为 EC_INVAL。

上面一至三点是提交阶段处理，而第四点会在执行指令的时候出错。现在延迟提交的，晓强和
王州在修改。之前只是用简单的 DELAY_SRET 标记延迟执行 SRET，但是现在 SRET 有多种情况，
再去修改即将要废弃的代码不合算，所以这一块先放一放。

块头解码的时候，需要判断 BSTATE 是否是复原的。是复原的就将 pc 指向 tpc，否则，pc 从
第一个微指令开始。

正常执行，到提交阶段的时候，需要复位 BSTATE，为下一个块指令提供干净的运行环境。


20220607
============

延迟提交的，王州他们已经修改好了，现在需要把 xret 相关的以及 carg 定的各个位修改下。
这里先记录下还有哪些需要更新。

1.env->sbpc 名称修改。（LARM 里 BSTATE 还是说的 SBPC，EBSTATE 里改为了 CARG，这个不
确定是否需要修改）
2.sret 延迟执行时的操作。（现在只有对sbpc的设置，提交阶段处理没有实现）
3.ebreak、ecall（ebreak 和 ecall 先放一放）
4.ebstate.ST.sz，每次块内中断/异常都需要设置，解头时，有一个 next_bpc，可以根据这个值
与 bpc 的差值知道是否是粘头。
5.ebstate 更新 sbpc 的 bit4 作为条件跳转的标志位，trans 相关的 setbpc.cond 需要更新对 sbpc 的修改。
6.redo ecall 的实现，这个需要考虑下整体的逻辑。

软件那边通过使用 sret 的 rra.type 为 redo ecall，返回到 ecall 的头，并告诉硬件该块指令只需要重启系统调用，不重新执行。那么在 sret 返回的块头解码时，需要产生个 ecall 异常，并将 ST.cause 设为 EC_ECALL，并设置 ST.sz 告诉软件当前头是否是粘头。


20220611
============

贻鹏那边的原子块测例在 QEMU 上测试会概率性的出现测例失败问题，然后我这边就在看 QEMU
现在 user mode 的实现。然后发现了一个问题::

    void cpu_loop(CPURISCVState *env)
    {
        ...
            switch (trapnr) {
            case RISCV_EXCP_U_ECALL:
                if (get_field(env->sebstate.st, EBSTATE_ST_CAUSE) == LINX_EC_ECALL)     {
                    env->pc = env->next_bpc;
                    env->next_bpc = LINX_ILLEGAL_INSTR_ADDR;
                } else {
                    env->pc += 4;
                }
        ...
    }

ebstate 中的各个寄存器其实是不清的，所以当有一条 block 的 ecall 指令是，cause 被
修改成了 EC_ECALL，之后再来个 rv 的ecall，这时候会走到第一个分支，而 next_bpc 已经
被复位成 -1 了，这时候再跑程序就直接挂掉了。而且除了这个问题外，还有个本身的实现问题。

在这里用了 s-mode 的 ebstate，sebstate，一开始是想着 uebstate 没有定义，就让 user
mode 用 sebstate。但，QEMU user mode 或许不应该暴露特权级相关的接口出去，也就是说
像 sebstate 这些不应该在 user mode 有使用。

但是，如果不用 ST.cause，又怎么区分 RV 的 ecall 还是 BLK 的 ecall。原先是通过分成
两个不同 exception code-LINX_EXCP 和 RV_EXCP 告诉内核我这 ecall 是要 pc 往下移
一条 rv 指令还是一条块头指令（话说内核那时候怎么知道是不是粘头？），但是现在都统一用的
RISCV_EXCP，用 ST.cause 和 ST.sz 这一组合，告诉内核是什么 ecall，怎么移 pc。

user mode 不用 ebstate，这时候没法用 ST 寄存器来区分了。所以，在 QEMU 内产生异常的
时候，给它区分下，用回 RV_EXCP 和 LINX_EXCP。唯一影响的是 xcause 把什么值给内核/
SBI。在 system mode 的 do_interrupt 里 hack 一下，遇到 LINX_EXCP 的时候，把它
转为 RV_EXCP。

这个方案看起来是没有问题，需要讨论下。现在也只有这样一种情况会有问题，一条 block 的
ecall，设置了 ST.cause，pc 往下移了一个块头，之后再来一条 rv 的 ecall，一看 ST.cause
设值了，QEMU 也认为它是 block 的 ecall，也把 pc 往下移动了一个块头，但是下个块头由一
个变量 next_bpc 决定，这个值在 user mode 里没有清（这是个问题，得考虑 user mode 怎
么清 bstate，下面会分析。），程序就由跑回之前定义的块头。如果清了 bstate，next_bpc
会设为非法地址 -1，程序直接挂掉，不过这也比不清强。

接下来分析下 user mode 怎么清 bstate。

昨天跑贻鹏的测例一直能 pass，还没发复现测例失败的，今天一拉取最新的 QEMU 下来跑贻鹏的
测例，QEMU 直接挂了。问题是这样的::

    ERROR:../target/linx/op_helper.c:214:helper_handle_exec_and_baranch: code should not be reached

之后打开日志看，最后两条块指令是这样的::

    ----------------
    IN: __tls_init_tp
    0x00000000000ff6b0:  00000000 00000000 09500400 3e767f8b sys_block.concat next:0xff6d0, ptr:0x3de16, attr:none, out_reg(), in_reg()
    0x00000000000ff6c0:  ffffffff fffffff0 00000000 00000000 sys_block.fall_through
    >---0x000000000003de16:  0053                  ecall-----------

    ----------------
    IN: __tls_init_tp
    0x00000000000ff6d0:  00000000 00001000 09302000 3e749c8b aux_block.concat next:0xff6f0, ptr:0x3de18, attr:none, out_reg(a2), in_reg()
    0x00000000000ff6e0:  ffffffff fffffff0 00000000 00000000 aux_block.fall_through
    >---0x000000000003de18:  00000000001b8cfe060a  lconst          1805566                         # 0x1b8cfe
    >---0x000000000003de22:  0022                  addtpc          t#1
    >---0x000000000003de24:  0084                  lbu             [t#1, 0]
    >---0x000000000003de26:  0c92                  set             a2, t#1

然后产生异常的 QEMU 代码::

    case BRANCH_FALL_THROUGH:
        if (env->sbpc) {
            if (env->blktype == HEAD_TYPE_SYS)
                linx_do_delay_insn(env);
            else
                linx_debug_not_reached(); /* 这里报的异常 */

然后，最近的提交里晓强更新了一个 patch，将头里的 sbpc 清零。由此可以推测出是 sbpc 没有
清导致的。打印下 sbpc 就可以看到第二个块指令没有延迟执行指令，它的 sbpc 也会有值。之前
实现 LARM 最新定义也是没有考虑 user mode，只把 system mode 的处理了。现在来考虑下
user mode 怎么清 bstate。

user mode 没有中断，异常的处理由 host 来模拟，所以当产生块内异常时，user mode 是不需
用上下文的保存。相应的，因为 user mode 块内异常没有上下文的保存，所以对异常处理的时
候，不能直接把 bstate 直接初始化，需要区分是块内/块间异常。但是现在没有在块内产生
异常的指令，都是在 commit 阶段触发异常的。所以异常处理完后，可以直接就把 bstate 给
复位了。

改好后，接着跑贻鹏的测例，好了，100%测例失败。排查原因。发现下面一个奇怪点。

在处理 block 原子块前，也就是 EXCP_ATOMIC_BLK 下面把 cpu_index 打出来，从日志里
看只有 CPU: 0，似乎线程创建失败了，但根据贻鹏所说，他是起了两个线程去跑的。那我在
ecall 那里加个，当 syscall num 是 clone 时，把它的系统调用返回值给打出来，也可以
看到是有两次 clone 系统调用。现在不知道是 QEMU 日志打印有问题，还是系统调用问题。
打出来的返回值，其中一个是返回的 0。看起来是线程创建失败了？看了下手册::

    As with fork(2), clone3() returns in both the parent and the child.  It returns 0 in the child process and returns the PID of the child in the parent.

在子进程中返回 0，在父进程中返回 pid。看起来一个线程的创建，是应该有两个打印，但是
再把 cpu_index 跟系统调用返回值一起打出来。cpu_index 都是 0。


20220613
============

拿到了贻鹏的代码，他用的是 fork 创建一个进程，用父子两个进程跑测例，不同于 qemu 实现的
原子测例，用的是 pthread_create 创建线程。软件中的 fork 在 QEMU 的模拟是 fork 一个
CPU 进程出来，而 pthread_create 对于 QEMU 来说是 clone 一个 CPU 线程出来::

    static int do_fork(CPUArchState *env, unsigned int flags, abi_ulong newsp,
                       abi_ulong parent_tidptr, target_ulong newtls,
                       abi_ulong child_tidptr)
    {
        ...
        if (flags & CLONE_VM) {
            ...
            /* we create a new CPU instance. */
            new_env = cpu_copy(env);
            info.env = new_env;
            ret = pthread_create(&info.thread, &attr, clone_func, &info);
        } else {
            /* if no CLONE_VM, we consider it is a fork */
            ...
            ret = fork();
        }
    }

而当前 QEMU 对 Block 原子块的处理是当前 CPU 线程等待其他 CPU 线程停止，然后进入互斥
区，再对内存进行操作。进入互斥区的操作里有用到锁，而这个锁进程间不共享，造成一个 CPU
进程进入互斥区对内存操作时，另一个进程也可以进入互斥区，这样不能保证原子块执行时，其他
CPU 被阻塞。


20220614
============

把贻鹏的测例中原子操作用 rv 的实现替换，跑了一遍 QEMU，程序并没有产生 EXCP_ATOMIC 这
个异常，跟一开始以为的不一样，原子指令执行时，都会产生一个异常，跳出来，单独为这条指令
生成一个 tb，并阻塞其他 CPU 去执行这个 tb。

QEMU 之所以加了这个异常主要是为了让 32 位的机子能去模拟 64 位的原子指令::

    Allow qemu to build on 32-bit hosts without 64-bit atomic ops.

    Even if we only allow 32-bit hosts to multi-thread emulate 32-bit
    guests, we still need some way to handle the 32-bit guest using a
    64-bit atomic operation.  Do so by dropping back to single-step.

之后，把 CONFIG_ATOMIC64 关了后，跑测例，也会出现跟 block 一样，每个 CPU 进程都可以进
入自己进程的互斥区。测例也会概率性失败。（不会吧，QEMU 自己功能都没有实现好）现在就得想
怎么才能真正实现执行原子块时，阻塞其他 CPU。


20220617
============

今天检查下贻鹏测例运行失败问题，现在有两个问题：

1. 最新提交下，qemu 会报 test fail 的错
2. reset 掉昨天提的 sret 和 ecall 的 patch 后，会出现断言问题::

       pc = 19b030, tpc1/2 = ffffffffffffffff/ffffffffffffffff, bpc=19b010
       qemu-linx: ../target/linx/translate.c:764: riscv_tr_translate_insn: Assertion `ctx->base.pc_next >= env->tpc1 && ctx->base.pc_next < env->tpc2' failed.
       qemu-linx: ../accel/tcg/cpu-exec.c:1070: cpu_exec: Assertion `!have_mmap_lock()' failed.

先排查第一个问题。qemu 中报 test fail 是为了适配编译器那边实现的 function model 跑
supertest 加的。当程序跑到 -1 这个地址时，判断 gpr[10]->a0 是不是 0，为 0 就是
pass。

从日志看，最后执行的指令是一个 ecall 指令，ecall 在提交的时候会清 next_bpc，而 user
mode 在处理 ecall 指令的时候，会先将 pc 挪到 next_bpc。这时候，pc 就变为 -1 了。


20220620
============

user mode 的整体设计开始提上日程了，需要把之前不规范的临时提交重新修改。

6月11号的 devlog 写的 ecall 分析需要修改下。当前 bstate 在提交阶段就被清除了，等去到
user mode 处理异常时，已经拿不到块的信息，单单使用 LINX_EXCP_U_ECALL 已经不能满足当前
现状。

当前 user mode 暴露出来以下问题：

1. 使用特权级状态 ebstate
2. ecall 如何区分 blk ecall 和 rv ecall
3. redo ecall 使用了提交阶段会清除的 next_bpc
4. reset bstate 操作已经从异常处理阶段提前到提交阶段

为了让 user mode 的异常处理能区分 blk 和 rv 的 ecall，小组讨论是额外定义个异常值，
LINX_EXCP_U_ECALL_16 以及 LINX_EXCP_U_ECALL_32 分别定义为普通头和粘头的 ecall
异常。如果需要往软件上报 cause 时，再把异常值转为 RV 的。

next_bpc 在硬件上是怎么体现的？块头解码的时候，应该会有个东西来记录是普通头还是粘头。
不然，不得每次需要知道块头信息时，都得解码一次块头？ST.sz 是不是可以替代 next_bpc？


20220623
============

ibpc 作为 QEMU 这个硬件独有寄存器，它被微指令用于索引t寄存器。在真硬件上，可以单周期
实现 t 寄存器的循环移位，对于所有微指令来说，同一个偏移值都使用的是同一个 t 寄存器。
但是 QEMU 从性能上说，不能每条微指令，都对 t 寄存器进行移位，所以引入了 ibpc 这个
索引（其实一开始不是这个原因，是因为对架构上理解偏差引入的。不知道硬件上是会移t寄存器）

因为 QEMU 有个翻译的过程，在 QEMU 翻译微指令的时候，用 ibpc 去使用 t 寄存器时，就将
用到的 t 寄存器硬编码到该指令中去。因为 tb 具有执行时不被中断的特性，如果 body 只被
翻译出一个 tb 自然没有问题，但是现在存在分成多个 tb 的情况，就需要考虑块内中断的情况，
ibpc 作为运行时的上下文，当发生块内中断，也是需要保存这个值。中断返回的时候，把这个值
恢复出来，让微指令接着这个值继续往下走。现在就添加了一些架构未明确定义的寄存器于 ebstate，
由硬件实现去决定这些寄存器的含义。R42 就先定为 QEMU ibpc 寄存器。

在块内中断产生的时候，加上 ibpc 打印，发现有时候 body 的第一条指令的 ibpc 不等于 0，
再在解头的时候加一个 assert 判断 ibpc 是否等于 0，却没有触发，情况如下::


  ------------- CS_OUT_FROM_BLK(0): Addr(ffffffff819c178a=>ffffffff80003180) with bpc=ffffffff807f5410(ffffffff819c178a,ffffffff819c17da), Priv(1=>1) for `s_timer`
    ibpc: 34                                                                                                                                                                                   GPRS:          0000000000000000 ffffffff807f5410 ffffffff82c03f20 ffffffff82d012e8 ffffffff82c0c480 ffffffe002e0add8 0000000000000000 0000000000000007 ffffffff82c03f40 ffffffe002e0ac00 ffffffe002e0acc0 ffffffff827f5fe0 ffffffff82d3b910 0000000000000002 ffffffffffffffff ffffffffdead4ead 0000000000000020 ffffffe003200298 ffffffff82d3b910 ffffffff82d3c128 ffffffff827f38b8 0000000000000000 ffffffff82d02018 ffffffff82400018 0000000080013100 000000000000007f 0000000000000000 0000000000000000 0000000000000002 0000000000000402 ffffffff82c85420 ffffffff82c85440-
    CARG:          0000000000000000
    TREG_P:        22
    SGPRS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000-
    TREGS:         0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000

  对应块指令的汇编:
  0xffffffff807f5410 <+224>:   bstart  b.aux, bnext.concat, battr:none, bget:0x00000204, bset:0x00048306, ptr:------ size:0x28, bnext:------,
  0xffffffff807f5420:    bstart.concat         bnext.ret ptr:0xffffffff819c178a bnext:------
      0xffffffff819c178a:    lconst   0x753 # 1875

看了下 linx_cs_log，TREG_P 也是打的 ibpc，以十六进制打印的。多加了。。

昨天国柱指出了两个代码规范上的问题。
第一个，cpu_helper 中 linx ecall 异常处理在代码上过于重复。一开始打算弄成下面这样，
支持 st.cause，弄了个 switch-case，目前看，ecall_16 和 ecall_32 的重复性太高了，
唯一的区别是 sz 设置的值不同，但现在不知道怎么在代码中对 sz 进行统一的设值。最简便
的地方，就是解头时候，给它设值。但 st 寄存器在 LARM 上定义是在 jump out 的时候再对
其设置。

.. code-block:: C

    switch (cause) {
    case LINX_EXCP_SET_REGS:
        blk_cause = LINX_EC_SET_REGS;
        cause = RISCV_EXCP_ILLEGAL_INST;
        break;
    case LINX_EXCP_GET_REGS:
        blk_cause = LINX_EC_GET_REGS;
        cause = RISCV_EXCP_ILLEGAL_INST;
        break;
    case LINX_EXCP_U_ECALL_16:
        ebstate->st = set_field(ebstate->st, EBSTATE_ST_SZ,
                                EBSTATE_ST_SZ_SZ128);
        blk_cause = LINX_EC_ECALL;
        cause = RISCV_EXCP_U_ECALL;
        break;
    case LINX_EXCP_U_ECALL_32:
        ebstate->st = set_field(ebstate->st, EBSTATE_ST_SZ,
                                EBSTATE_ST_SZ_SZ256);
        blk_cause = LINX_EC_ECALL;
        cause = RISCV_EXCP_U_ECALL;
        break;
    ...

第二个问题，根据国柱的意思，使用一个变量记录指令长，pc 再根据这个指令长去上下移动，
riscv 的 restart syscall 其实也可以直接就给它把异常产生了，反正它进去执行也是直接
产生个异常。

.. code-block:: diff

    @@ -33,6 +33,7 @@ void cpu_loop(CPURISCVState *env)
         int trapnr, signum, sigcode;
         target_ulong sigaddr;
         target_ulong ret;
    +    int inst_len = 0;

         for (;;) {
             cpu_exec_start(cs);
    @@ -58,13 +59,14 @@ void cpu_loop(CPURISCVState *env)
             case LINX_EXCP_U_ECALL_16:
             case LINX_EXCP_U_ECALL_32:
                 if (trapnr == LINX_EXCP_U_ECALL_16) {
    -                env->pc += 16;
    +                inst_len = 16;
                 } else if (trapnr == LINX_EXCP_U_ECALL_32) {
    -                env->pc += 32;
    +                inst_len = 32;
                 } else {
    -                env->pc += 4;
    +                inst_len = 4;
                 }

    +            env->pc += inst_len;
    @@ -82,15 +84,13 @@ void cpu_loop(CPURISCVState *env)
                                      0, 0);
                 }
                 if (ret == -TARGET_ERESTARTSYS) {
    -                if (trapnr == RISCV_EXCP_U_ECALL)
    -                    env->pc -= 4;
    +                env->pc -= inst_len;
    +                cs->exception_index = trapnr;
    -                /* Linx regenerate the exception by reset the flag but run
    -                 * again the block, which may contain other operation(s) */
    -                if (trapnr == LINX_EXCP_U_ECALL_16) {
    -                    env->pc -= 16;
    -                    cs->exception_index = LINX_EXCP_U_ECALL_16;
    -                } else if (trapnr == LINX_EXCP_U_ECALL_32) {
    -                    env->pc -= 32;
    -                    cs->exception_index = LINX_EXCP_U_ECALL_32;


20220627
============

王州在 BRANCH_RET 的位置添加上了 assert 来判断 pc 地址是否被设为 0。
下面是出错的位置::

    0xffffffff81474ae0 <+192>:   bstart  b.std, bnext.concat, battr:none, bget:0x00000004, bset:0x001c0306, ptr:------ size:0x14, bnext:------,
    0xffffffff81474af0:    bstart.concat         bnext.ret ptr:0xffffffff821dd078 bnext:------
        0xffffffff821dd078:    get      sp
        0xffffffff821dd07a:    ld       [t#1,40]
        0xffffffff821dd07c:    set      ra,t#1
        0xffffffff821dd07e:    ld       [t#3,32]
        0xffffffff821dd080:    set      s0,t#1
        0xffffffff821dd082:    ld       [t#5,24]
        0xffffffff821dd084:    set      s1,t#1
        0xffffffff821dd086:    ld       [t#7,16]
        0xffffffff821dd088:    set      s2,t#1
        0xffffffff821dd08a:    addi     t#8,0
        0xffffffff821dd08c:    get      sp
        0xffffffff821dd08e:    ld       [t#1,8]
        0xffffffff821dd090:    set      s3,t#1
        0xffffffff821dd092:    ld       [t#3,0]
        0xffffffff821dd094:    set      s4,t#1
        0xffffffff821dd096:    const    0x30 # 48
        0xffffffff821dd098:    add      t#6,t#1
        0xffffffff821dd09a:    addi     t#8,0
        0xffffffff821dd09c:    set      sp,t#2  <--- 这里 sp 加了个 48，打印的时候把它给减回来了
        0xffffffff821dd09e:    setbpc   t#2

从栈上面读取值并设到 ra 中去。在这个块提交的时候把状态以及栈上的内容打出来::

    x0/zero  0000000000000000 x1/ra    0000000000000000 x2/sp    ffffffe002e9b2c0 x3/gp    ffffffff82d012e8

    dump memory 0xffffffe002e9b290+128:
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  <--- 可以看到原先存在栈上的内容被清了
    b0 b3 e9 02 e0 ff ff ff 20 97 3f 80 ff ff ff ff
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    f2 05 00 00 00 00 00 00 30 44 7c 82 ff ff ff ff

在这里加了个 helper 函数，用来打印 load/store 的地址以及值。然后可以看到这个块，
在执行的时候，load ra 的值也是 0::

    Trace-XX 0(09:27:04): 0x7fa918891cc0 [0000000000000000/ffffffff81474ae0/00004201/ff000000] __radix_tree_preload
    Trace-XX 0(09:27:04): 0x7fa918891e40 [0000000000000000/ffffffff821dd078/00004201/ff000000]-
    load addr:ffffffe002e9b288, value: 0 <--- 这个时候，sp 的值是 0xffffffe002e9b260，ra 读出来是 0
    load addr:ffffffe002e9b280, value: 0

再往日志上面滚一下，看到它之前是从中断恢复过来的::

    ------------- CS_IN_RECOVER(0) Addr (ffffffff814c64d0=>ffffffff81474a20) Priv(1=>1)  <--- 被中断的块是 ffffffff81474a20

      GPRS:          0000000000000000 ffffffff81477ce0 ffffffe002e9b290 ffffffff82d012e8 ffffffe002e70000 ffffffe002e35200 0000000000000001 0000000000000087 ffffffe002e9b2a0 ffffffe002e35180 0000000000000cc0 000000000000000b 0000000000000000 ffffffe002e35200 1e439a3283010700 1e439a3283010700 1e439a3283010700 ffffffff827c4fe0 ffffffe002e14000 0000000000001000 ffffffff827c4fe0 0000000000000000 0000000000000000 0000000000000002 0000000000008124 ffffffff82d020a8 ffffffff82d02bc0 0000000000000124 0000000000008000 0000000000000000 0000000000000400 0000000000000400-  CARG:          0000000000000000
      TREG_P:        2a
      SGPRS:         0000000000000000 0000000000000000 ffffffe002e9b260 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe002e9b290 ffffffff82cea2d8 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffffffbfffff 000000000000000b 0000000000000000 0000000000000000 000000000000000b 0000000000000000 0000000000000cc0 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000-  TREGS:         ffffffff82cea2d8 0000000000000001 000000000000000b 0000000000000cc0 000000000000000b 000000000000000b ffffffe002e70008 0000000000b26d00-
    Trace-XX 0(09:27:04): 0x7fa91888fd80 [0000000000000000/ffffffff81474a20/00004201/ff000000] __radix_tree_preload
      block recoverred with header ffffffff81474a20(ffffffff821dcfa0, ffffffff821dd00e), TPC=ffffffff821dd008

被中断的块与当前的块是相对应的。当前块是从栈上读 ra，被中断的是把 ra 写到栈上::

    0xffffffff81474a20 <+0>:     bstart  b.aux, bnext.concat, battr:none, bget:0x001c0f16, bset:0x001cc304, ptr:------ size:0x37, bnext:------,
    0xffffffff81474a30:    bstart.concat         bnext.cond ptr:0xffffffff821dcfa0 bnext:0xffffffff81474ac0
        0xffffffff821dcfa0:    const    0xffffffffffffffd0 # -48
        0xffffffff821dcfa2:    get      sp
        0xffffffff821dcfa4:    add      t#1,t#2
        0xffffffff821dcfa6:    set      sp,t#1  <--- 这里将 sp 往下挪了 48 字节，sp 的原值是 0xffffffe002e9b290
        0xffffffff821dcfa8:    get      s0
        0xffffffff821dcfaa:    sd       t#1,[t#3,32]
        0xffffffff821dcfac:    get      s4
        0xffffffff821dcfae:    sd       t#1,[t#5,0]
        0xffffffff821dcfb0:    get      ra
        0xffffffff821dcfb2:    sd       t#1,[t#7,40]
        0xffffffff821dcfb4:    addi     t#8,0
        ......
        0xffffffff821dcffe:    lconst   0xb26d00 # 11693312
        <--从这里，被分成了两个 TB，在这两个 TB 间有个 Timer 中断-->
        0xffffffff821dd008:    addtpc   t#1 # 0xffffffff82d03d08 <radix_tree_node_cachep>
        0xffffffff821dd00a:    set      s3,t#1
        0xffffffff821dd00c:    setbpc.geu       t#8,t#6

在执行第一个 tb 的时候，store 的情况如下::

    Trace-_X 0(09:27:04): 0x7fa91888fd80 [0000000000000000/ffffffff81474a20/00004201/ff000000] __radix_tree_preload
    Trace-_X 0(09:27:04): 0x7fa91888ff00 [0000000000000000/ffffffff821dcfa0/00004201/ff000000]-
    store addr:ffffffe002e9b280, value: ffffffe002e9b2a0
    store addr:ffffffe002e9b260, value: ffffffff827c4fe0
    store addr:ffffffe002e9b288, value: ffffffff81477ce0 <--- 可以看到 ra 是有值的
    store addr:ffffffe002e9b278, value: ffffffe002e35180
    store addr:ffffffe002e9b270, value: ffffffe002e14000
    store addr:ffffffe002e9b268, value: 1000

那么就可以断定栈被踩的地方是 kernel 处理中断的时候。然后就看到 CS OUT 的时候 sp 的值
还是 0xffffffe002e9b290，没有更新::

    ------------- CS_OUT_FROM_BLK(0): Addr(ffffffff821dd008=>ffffffff80003180) with bpc=ffffffff81474a20(ffffffff821dcfa0,ffffffff821dd00e), Priv(1=>1) for `s_timer`
      GPRS:          0000000000000000 ffffffff81477ce0 ffffffe002e9b290 ffffffff82d012e8 ffffffe002e70000 ffffffe002e35200 0000000000000001 0000000000000087 ffffffe002e9b2a0 ffffffe002e35180 0000000000000cc0 000000000000000b 0000000000000000 ffffffe002e35200 1e439a3283010700 1e439a3283010700 1e439a3283010700 ffffffff827c4fe0 ffffffe002e14000 0000000000001000 ffffffff827c4fe0 0000000000000000 0000000000000000 0000000000000002 0000000000008124 ffffffff82d020a8 ffffffff82d02bc0 0000000000000124 0000000000008000 0000000000000000 0000000000000400 0000000000000400-
      CARG:          0000000000000000
      TREG_P:        2a
      SGPRS:         0000000000000000 0000000000000000 ffffffe002e9b260 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffe002e9b290 ffffffff82cea2d8 0000000000000000 0000000000000000 0000000000000000 0000000000000000 ffffffffffbfffff 000000000000000b 0000000000000000 0000000000000000 000000000000000b 0000000000000000 0000000000000cc0 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000 0000000000000000-
      TREGS:         ffffffff82cea2d8 0000000000000001 000000000000000b 0000000000000cc0 000000000000000b 000000000000000b ffffffe002e70008 0000000000b26d00

同时 kernel 在处理中断的时候，会将 gpr 以及 sebstate 等都压栈，但是这个 sp 是块
0xffffffff81474a20 压栈前用的栈，那么当然就会被踩啦。

总结来说就是，一个块在做压栈操作，sp 被修改的操作是需要提交的时候才会更改。在这中间
如果产生个中断，也进行压栈操作，那么就会把之前压栈的数据给覆盖着咯。

这段出问题的代码是 C 文件由编译器编出来的，感觉是编译器实现有点儿问题？


20220629
============

增凯那边在 system mode 下测试 locking-selftest 出现 rcu 相关的 panic，查看日志中
panic 的位置时，可以看到是产生了 breakpoint 异常，出现异常的函数是 rcu_irq_enter。
rcu_irq_enter 里使用了 lockdep_assert_irqs_disabled，该函数在开启 CONFIG_PROVE_LOCKING
会使用 WARN_ON_ONCE，同 BUG_ON 一样最终会使用 ebreak 指令，只是 WARN 的话不应该导致内核 panic，
还得再细看。

kernel 对 breakpoint 异常处理主要分成了 k/uprobe、用户态 ebreak、kernel BUG，如果上
面都不是，那内核就会调用 die，直接 panic。用户态 ebreak 会发出信号，gdb 获取信号后，
会在 ebreak 下一条指令停下。kernel BUG 通过 st.cause 是否等于 EC_BREAK 和
is_valid_bugaddr 来判断这个 ebreak 是否有效，is_valid_bugaddr 用于 rv，st.cause
用于 block。现在是 st.cause 没有设，也不是 rv 的 ebreak，所以就走了最后一个分支，
调用 die，导致内核 panic。

::

    int is_valid_bugaddr(unsigned long pc)
    {
        bug_insn_t insn;

        // 判断是不是属于内核空间
        if (pc < VMALLOC_START)
            return 0;
        // 根据 pc 取指令编码
        if (get_kernel_nofault(insn, (bug_insn_t *)pc))
            return 0;

        if ((insn & __INSN_LENGTH_MASK) == __INSN_LENGTH_32) {
            // rv 的 ebreak
            return (insn == __BUG_INSN_32);
        } else {
            // rv 的 c.ebreak
            return ((insn & __COMPRESSED_INSN_MASK) == __BUG_INSN_16);
        }
    }

上面还没有理清函数的作用，不明白为什么 kernel 需要调用 rcu_irq_enter 告知 RCU 从
kernel 从 idle 进入 irq 了。

QEMU 中有两种断点异常，一种是对应平台定义的，对应的异常值是 XXX_EXCP_BREAKPOINT，另一
种是 QEMU 自己定义，用于 QEMU GDB 的 watchpoint 以及 breakpoint，对应的异常值是
EXCP_DEBUG。

EXCP_DEBUG 和 XXX_EXCP_BREAKPOINT 之间的关系还弄的不是很明白。而且有个奇怪的点，
rv 的 user mode 这两个异常是不同的处理，但是 arm 的 bkpt 与 qemu debug 是一同处理。
rv 这两个处理的区别就是有没有报 ebreak 的 pc。

user mode 的程序里直接有条 ebreak 指令与用 QEMU GDB 设的断点有什么区别吗？为什么
EXCP_DEBUG 可以不需要 ebreak 的 pc？

关于 EXCP_DEBUG 不需要 pc，我猜想是因为这个异常是由 QEMU GDB 自己设置的断点，它自己
使用 bp 命令时，QEMU 会添加一个断点到 CPUState 的 breakpoints 队列里去，等到 tb 执行
的时候，再与队列中的断点比对 pc。

但是 GDB 断下来后，它怎么知道下一条指令是在哪里呢？自己根据这个 pc 去解码指令？那么
linx 的 GDB 有没有实现这个功能，可以去解码 rv 指令和 header（现在是把 bpc 报给 epc 的。）？

这些疑问还需要再理一理才能把 blk 的 breakpoint 异常加上去，不然可能导致 GDB 连上 QEMU 后
不能调试 block 的程序。


20220706
============

将块内中断关闭后，lr/sc 自测试用例会出现失败的情况，而且出错的几率很大。因为直接使能日
志功能，会将内核启动阶段执行情况一起显示在日志中，所以先改变了下 QEMU 的代码，使得
QEMU 日志功能一开始失效，直到测例调用使能日志的函数时，再开始打日志。

加了上面的功能后，lr/sc 反倒没有出现失败的情况了。。问题还不清楚在哪里，应该是受时间的
影响。


20220708
============

lr 指令会先从指定地址读取内存，并记录这个值，标记这个地址。
sc 指令先判断将要写数据的地址与 lr 标记的地址是否相同。相同，接着从这块地址中读取数据
与 lr 指令记录的值比较，这个操作是原子的。相同，返回 0。其他情况都返回 1。最后都会将
这块地址的标记去掉。

起了多线程后，日志显示混乱，经常打印到一半就切到另一个 cpu 去了。日志实在是没法看，看
了下 qemu_log 的实现，发现有实现 rcu，但是需要把想要输出的整合到一起，再调用这个函数。

但是按照现有的日志功能，输出的内容存在很多不相关内容，看不出来出现问题的点，自己写了个
helper 函数，用来看 lr/sc 的情况，输出大概是这样的::

    CPU: 2, bpc: 11b880, res addr: ffffffffffffffff, res value: 26d, load addr: 18caa8, load value: 26d
    CPU: 2, bpc: 11b890, res addr: 18caa8, res value: 26d, store addr: 18caa8, store value: 26e, succ: 0

统计了一些信息::

    grep -c 'succ: 0' qemu_log      => 2000000
    grep -c 'succ: 1' qemu_log      => 706280
    grep -c 'load value' qemu_log   => 2706280
    grep -c 'store value' qemu_log  => 2706280

succ 是 sc 的返回值，0 代表写入成功。统计 succ 为 0 的行数，同测例期望结果相同，出现
这种情况怀疑是两次成功的写入都是写了同样的值。写一个脚本看看，是否跟猜想的一样::

    CPU: 0, bpc: 11b880, res addr: ffffffffffffffff, res value: f4e49, load addr: 18caa8, load value: f4e49
    CPU: 0, bpc: 11b890, res addr: 18caa8, res value: f4e49, store addr: 18caa8, store value: f4e4a, succ: 0
    CPU: 0, bpc: 11b880, res addr: ffffffffffffffff, res value: f4e49, load addr: 18caa8, load value: f4e49
    CPU: 0, bpc: 11b890, res addr: 18caa8, res value: f4e49, store addr: 18caa8, store value: f4e4a, succ: 0

的确是，而且还是一个 cpu，中间也没有中断或异常，也没有线程切换。这有点奇怪。写值进去后，
再读的值却是之前的。但是 store 操作要先往内存写了值后，才会清 reserve，那么第二条 lr
是前面 sc 执行完了，才读的值。看起来是写值失败了。没头绪。。下周回来再看

测例反汇编是这样的::

    0x000000000011b880:  00000400 00001000 00000c00 0003038b sys_block.fall_through next:0x11b890, ptr:0x11b8b0, attr:none, out_reg(a2), in_reg(a0)
    >---0x000000000011b8b0:  0a12                  get             a0
    >---0x000000000011b8b2:  029f                  lr.d.rl         t#1     --> 这里测例写错了，但应该不影响测试。
    >---0x000000000011b8b4:  0c92                  set             a2, t#1

    0x000000000011b890:  00001c00 00000000 fff01800 00026f8b sys_block.conditional next:0x11b880, ptr:0x11b8b6, attr:none, out_reg(), in_reg(a0,a1,a2)
    >---0x000000000011b8b6:  0a12                  get             a0
    >---0x000000000011b8b8:  0b12                  get             a1
    >---0x000000000011b8ba:  0c12                  get             a2
    >---0x000000000011b8bc:  0400                  add             t#1, t#2
    >---0x000000000011b8be:  61df                  sc.d.aq         t#1, [t#4]
    >---0x000000000011b8c0:  0111                  setbpc.eqi      t#1, 1

    0x000000000011b8a0:  00000000 00000000 00300400 0002278b sys_block.direct_link next:0x11b8d0, ptr:0x11b8c2, attr:none, out_reg(), in_reg()
    >---0x000000000011b8c2:  0009                  const           0                               # 0x0

    0x000000000011b8d0:  8082              ret


20220711
============

上周五怀疑是 store 出现了问题，先看下 sc 调用的 store 实现。它调用的是
tcg_gen_atomic_cmpxchg_tl，从对应地址取值，并与记录的旧值比较，如果相等，
则将新值写入对应地址，不相等则不写入。返回一开始取出的值。这个 CAS 操作要
求是原子执行的，在 QEMU 具体实现上会有不同。

当 QEMU 运行在串行上下文上时（通常来说，这个情况出现在单 cpu 或处在互斥区中），
QEMU 会认为 CAS 操作不会受其他 cpu 的影响，直接使用 load 和 store 去模拟
CAS。当处在并行上下文的时候，如果 host os 有原子 CAS 实现，QEMU 就会去调用 os
实现的原子 CAS 接口，如果 host os 没有实现，那 QEMU 会产生一个 EXCP_ATOMIC 的
异常，阻塞其他 cpu，让当前 cpu 进入互斥区，也就是让 QEMU 进入一个串行的上下文，
然后按串行的来处理。

当开启延迟中断后，会覆盖 cflags 为 CF_NOIRQ，导致 CF_PARALLEL 被清掉，让 QEMU
认为它是在一个串行的上下文中。

关于这个还是有点疑问，从日志上来看，是同一个 cpu store 了同一个值，这种情况其实
应该可以认为是在一个串行上下文，当目前开起来是在 store1 前，load2 了一下值到 r1，
store1 后，store2 用 load2 的 r1 去写值。但物理 cpu 不是应该保证从结果上看，是
按着程序顺序执行的嘛。。怎么结果不对的。。


20220816
============

块内跳转已经确认开始实施，先分析下块内跳转在 QEMU 中怎么实现。

块内跳转从软件上来说，是为了解决部分单个功能被拆成多个块，造成的碎片化，
例如 lr/sc，因为没有块内跳转，当 sc 存储值失败的时候，需要重新取值时，
需要拆分成两个块。从当前内核的实现上来看，每个块中的微指令数量较少，块
引擎都塞不满，硬件上呈现着资源使用不饱和。

新增了四条指令用于块内跳转，两个无条件跳转指令，j 与 jr，以及两个条件
跳转指令，bcond 和 bcondi。

j 是直接索引跳转，opcode 是 `0000_0111`，高 8 位([15:8]) 作为立即数，
用作 tpc 的偏移，为了保持地址 2 字节对齐，节省了偏移地址的最后一位，所
以最后实际的地址是 tpc + sext(imm8[7:0] * 2, XLEN)。

jr 是间接索引跳转，opcode 是 `0010_0111`，高 3 位([15:13]) 作为 T
寄存器索引，同时也为了节省一位数据，最后实际跳转的地址是 tpc + <T#M> * 2。
（这里是否有必要节省一位嘛？T 寄存器长度不是 XLEN，看到这里，也对 RV
的 JALR 指令产生了疑问，为什么它会需要 rs1 加上一个 offset 作为偏移，
这个操作看起来是可以单独拆出来。是为了节省指令量嘛。）

bcond 和 bcondi 都是当 <T#M> 非 0 时，才会跳转。

bcondi 是直接索引跳转，opcode 待定，目标地址计算方式同 j 类似，不过该
指令立即数只有 5 位。

bcond 是间接索引跳转，opcode 也未定义，目标地址计算方式同 jr。

跳转的位置可以分为块前、块中、块后。

当跳转到块前时，将触发一个非法指令异常。

块中有两种情况，一种是跳到指令对齐的地址（这里说的指令对齐是 2 byte），
另一种是非指令对齐。因为现在偏移地址都是 2 byte 对齐的，所以只有当块内
出现指令长不等于 2 byte 的指令，如 lconst 和 sysget/sysset 之类的，才
会有非指令对齐的情况，而该情况硬件无法处理，所以这里有一个约束，禁止该
类指令出现在有块内跳转的块，目前来说是，只有标准块指令才用于块内跳转。

块后统一认为是直接进入块提交阶段。

上面的是块内跳转的基本定义，下面就说当前特性与之前特性以及其他将要实现
特性间的相互作用。

对当前块的 get/set 掩码检查的影响，bget 和 bset 分别表示当前块 get 指
令用到的 GPR 以及 set 指令用到的 GPR。加入了块内跳转的话，也要求有掩码
检查，是对执行的 get/set 指令进行检查。掩码检查产生的错误有：1. bget 同
实际 get 不匹配。2. bset 同实际 set 不匹配。3. 重复 set 同一个 GPR。
这就要求每条 get/set 指令执行的时候，与 bget/bset 进行比较，判断当前
GPR 是否在掩码中，同时也需要记录该 GPR，在提交阶段时，判断是否同
bget/bset 相同。set 除了以上之外，还需要判断当前 GPR 是否存在于历史 set
GPR 记录中。当前 QEMU 的实现是动态检查的，不需要修改。

当引入 I/O 寄存器时，bget/bset 将作为 I/O 寄存器的索引，同之前的语义
不再相同。这个特性待分析。

对 T 寄存器索引的影响，因为之前微指令的执行是按着内存中的顺序，依次执行，
但有了块内跳转，当前指令所用的 T 寄存器不再是基于内存中的顺序而来，而是
按着指令真正执行的顺序来。当前 QEMU 是将翻译与执行分开，翻译时，明确每条
指令所用的 t 寄存器，执行的时候，直接使用该寄存器即可，不需用做索引操作。
但根据当前定义，需要修改成执行时，动态索引 T 寄存器。

set immediately 特性，待分析。

fixup 块，待分析。

块内内存，待分析。


20220818
============

之前说的 QEMU 修改 T 寄存器索引方式，这里需要再细分析下。

当指令执行的时候，硬件可以较为方便的移位每个 T 寄存器，但是 QEMU 这里 T
寄存器使用的是数组，没法便捷的将 T 寄存器循环移位，如果要 QEMU 每次执行一
条指令都循环移位 T 寄存器，会造成不小的性能开销。所以，我们使用了一个 ibpc
变量，用来指向当前指令将要存放 output 的 T 寄存器，那么当块执行的过程中被
打断，就会将 ibpc 保存在中断上下文中。此外，我们发现 T 寄存器索引是与 PC
静态关联的，可以在翻译的时候直接确定每条微指令将要用的 t 寄存器，就去掉了
每条指令在执行时索引 T 寄存器的步骤。

当块内跳转引入，T 寄存器索引方式将按照程序动态执行的顺序索引，没法在翻译的
时候确定微指令所用的 T 寄存器，所以现在需要将指令执行时，动态索引 T 寄存器
的功能加回来。


20220819
============

in-block jump 类型指令会基于 tpc 来跳转，按道理也会更新 tpc。但是 QEMU 当
前的实现中，tpc 基本没有使用的地方，只有以下地方用到了 tpc：

1. 产生块内中断/异常时，才会将 pc（当前的块内微指令的地址）赋给 tpc，并将其
   保存到 ebstate 中。
2. 异常返回时，会从 ebstate 恢复 tpc。当恢复块内状态时，会根据 tpc 重新取指。

那么 tpc 有没有可能跟 pc 一样，能动态的更新呢？在考虑这个方案前，先得知道
QEMU 中，tb 执行完后，下一个 tb 的 pc 是怎么拿到的？

目前 tb 有两个结束的标志，一个是 DISAS_TOO_MANY，另一个是 DISAS_NORETURN。
NORETURN 这个主要是用于分支指令以及可能产生异常的指令，这些类型都会在跳出
前，都会显式更新 pc。接下来看 TOO_MANY 的情况，这个是当 tb 中 target 指令
或中间码数量过多，或者下一条指令属于下一个页时，将会拆分 tb，这个时候会在
调用 tb_stop 时，会使用 gen_goto_tb 隐式的更新 pc。

tpc 若要同 pc 一样，能在执行的过程中更新，在 TOO_MANY 的情况下，需要判断当
前是否在块内，当在块内时，tpc 更新同 pc，否则不更新。而 NORETURN，只需要在
块内跳转指令加上 tpc 的更新就行。


20220824
============

因为块内跳转特性的引入，使用 t 寄存器需要明确知道程序中指令的执行情况，所以
得有个东西能保存一下 j 指令

scratch 寄存器的读/写的指令分别是 sget/sset。

sget <S#M> 表示从 <S#M> 中取值到目的 T 寄存器中，低 8 位 opcode[0:7] 为
'1101_0010'，5 位[8:12] 作为 S 寄存器的索引，高 3 位[13:15] 未使用。

sset <S#M>,<T#N> 表示将 <T#M> 的值写到 <S#M> 中，低 8 位 opcode[0:7] 为
'1111_0010'，5 位[8:12] 作为 S 寄存器的索引，高 3 位[13:15] 作为 T 寄存器
索引。

5 位的编码支持 32 个数字，但 scratch 目前只有 8 个，保不齐后面会增加。如果
索引值超过了 7，则会产生非法指令异常。

异常/中断产生时，S 寄存器作为块内状态会保存于 ebstate.reg 的 R10-17。


20220831
============

在往主线合块内跳转代码的时候，想了下，好像没有在 rv 的内核去跑我们的 hello test，
就启了个内核去跑。好了，QEMU 挂掉了，异常如下::

    qemu-system-linx: ../target/linx/translate.c:766: riscv_tr_translate_insn: Assertion `ctx->base.pc_next >= env->tpc1 && ctx->base.pc_next < env->tpc2' failed.

最后的日志如下::

    bpc: 118d80, tpc: 118d90, tpc1: 118d90, tpc2: 118d92  --> 这个是翻译的时候，打印的。
    in-block jump is out of block. bpc: 0x118d80, tpc: 0x118d90, next_pc: 0x118d98, body_range:(0x118d90-0x118d92)  --> 跑的是 j 的异常测例中的跳出块外
    ------------- CS_OUT(0): Addr(118d90=>80000520) with bpc=118d80(118d90,118d92), Priv(0=>3) for `illegal_instruction`

    ------------- CS_IN(0) Addr (800005f4=>ffffffff80002f04) Priv(3=>1) --> ffffffff80002f04 是 handle_exception 的入口。

    bpc: 118d80, tpc: ffffffff80004318, tpc1: 118d90, tpc2: 118d92 --> ffffffff80004318 是 do_trap_insn_illegal 的入口。

从上面可以看到，非法指令异常是先从 U->M，然后又从 M->S，在 S 这里面出现了 pc 不在
tpc1 和 tpc2 的问题。通过排查发现，QEMU 是把 M mode 当做一个不会改变 bstate 的黑盒，
在进入 M mode 时，会先将 bpc 保存，再将其失效。当从 M mode 返回时，也就是执行 mret 指
令时，会将 bpc 恢复，一般来说，返回后会接着从出现异常的 rv 指令/ block微指令继续执行。

但是这个却不太一样，M 没有直接回到 U 出现异常的地方，而是跑去了其他地方执行了。QEMU 中
是根据 bpc 是否有效决定是用 rv 指令解码器，还是 block 微指令解码器。因为在 U 中跑的测
例是 block 的代码，所以它的 bpc 是有效的。当陷入 M 时，会保存 bpc，从 M 返回，会恢
复 bpc。那么 QEMU 就会用 block 微指令解码器去解码指令，其中，在 block 微指令解码器
中，会有个判断 tpc 是否在 tpc1 和 tpc2 间。将要执行的 S 代码与出现异常的块不是同一
个，tpc 肯定也就不在异常块的 tpc1 和 tpc2 间。(用 block 微指令解码器这个事肯定也不
对，因为 S 是 rv 的代码。)

但是为什么之前跑 Block 没有这样的事发生呢？这个去跑了下，没有出现 U->M,M->S 这种情况。
都是些 U->M,M->U (m_timer) 和 S->M,M-S (m_timer, secall)，都是返回到出现异常的地
方。

那么用 block 的 kernel 跑 j 异常测例会怎么样呢？现象是 QEMU 居然没有挂掉，出现的现场
如下::

    # /mnt/test j
    [  180.853096] test[93]: unhandled signal 4 code 0x1 at 0x0000000000118d90 in test[10000+174000]
    [  180.874994] CPU: 0 PID: 93 Comm: test Not tainted 5.16.0-rc3-00003-g83b5cbaceb0e #49
    [  180.883283] Hardware name: riscv-virtio,qemu (DT)
    [  180.891364] epc : 0000000000118d90 ra : 00000000000175c6 sp : 0000003fddd68ac0
    [  180.897219]  gp : 000000000018b2e0 tp : 00000000001917e0 t0 : 0000000000000002
    [  180.903196]  t1 : 0000000000000000 t2 : 000000006fffff41 s0 : 0000003fddd68b00
    [  180.908039]  s1 : 0000000000000002 a0 : 0000000000000000 a1 : 0000000000000000
    [  180.913173]  a2 : 0000000000000001 a3 : 00000000001903d8 a4 : 0000000000000000
    [  180.919155]  a5 : 0000000000118d80 a6 : 1999999999999999 a7 : 0000000000000063
    [  180.924109]  s2 : 0000003fddd68d28 s3 : 0000000000000001 s4 : 0000003fddd68d40
    [  180.929385]  s5 : 0000000000017e40 s6 : 0000000000000001 s7 : 0000000000000001
    [  180.935471]  s8 : 0000003f7f7c2100 s9 : 0000003f7f782500 s10: 0000003f7f7c21b0
    [  180.940547]  s11: 0000000000000000 t3 : 000000000016b2f0 t4 : 000000000016bbf0
    [  180.945236]  t5 : 0000000000000005 t6 : ffffffffffffffff
    [  180.949421] status: 0000000000000020 badaddr: 0000000000010407 cause: 0000000000000002
    [  180.956569] ebstate.st: 0000000000000000
    [  181.224583] test[94]: unhandled signal 4 code 0x1 at 0x0000000000118dc0 in test[10000+174000]
    [  181.235061] CPU: 0 PID: 94 Comm: test Not tainted 5.16.0-rc3-00003-g83b5cbaceb0e #49
    [  181.240252] Hardware name: riscv-virtio,qemu (DT)
    [  181.244025] epc : 0000000000118dc0 ra : 00000000000175c6 sp : 0000003fddd68ac0
    [  181.249002]  gp : 000000000018b2e0 tp : 00000000001917e0 t0 : 0000000000000002
    [  181.253460]  t1 : 0000000000000000 t2 : 000000006fffff41 s0 : 0000003fddd68b00
    [  181.259347]  s1 : 0000000000000002 a0 : 0000000000000000 a1 : 0000000000000000
    [  181.264008]  a2 : 0000000000000001 a3 : 00000000001903d8 a4 : 0000000000000000
    [  181.268918]  a5 : 0000000000118db0 a6 : 1999999999999999 a7 : 0000000000000063
    [  181.273369]  s2 : 0000003fddd68d28 s3 : 0000000000000001 s4 : 0000003fddd68d40
    [  181.279174]  s5 : 0000000000017e40 s6 : 0000000000000001 s7 : 0000000000000001
    [  181.284111]  s8 : 0000003f7f7c2100 s9 : 0000003f7f782500 s10: 0000003f7f7c21b0
    [  181.289153]  s11: 0000000000000000 t3 : 000000000016b2f0 t4 : 000000000016bbf0
    [  181.293515]  t5 : 0000000000000005 t6 : ffffffffffffffff
    [  181.297493] status: 0000000000000020 badaddr: 0000000000010007 cause: 0000000000000002
    [  181.303650] ebstate.st: 0000000000000000
    test_j: passed

.. note::

    测例是启一个子进程去运行会产生非法指令的测例，比如说块内跳转指令不在标准块的情况。
    子进程就会产生一个非法指令，内核会传一个非法指令的信号，导致子进程异常终止。父进程
    可以通过读取子进程结束状态，来判断是否是因为非法指令信号终止的子进程。
    (不知道出现 unhandled signal 是否是正常的)

为什么 block kernel 运行测例不会导致 QEMU 挂掉呢？
(block 的日志与上面 rv 的日志相比，除了没有最后一行，因为翻译出问题而打的 log 外，其他
都差不多，这里就不再附上了。)

出现问题的地方是在将要执行 do_trap_insn_illegal() 的时候，在执行它前还有个
handled_exception()，handled_exception 在启起来 kernel 时，都不知道执行了多少次，
QEMU 早已把它缓存起来了，可以跳过翻译阶段。用 block 的 kernel 去执行
handel_exception 块后将会提交，而该阶段会初始化 bstate，往下走也就不会导致翻译出现问
题。


20221024
============

QEMU 在跑系统模式的时候，在运行 opensbi 前，会有一段程序用来引导 opensbi。这段程序是
硬编码在 ROM 中，由 QEMU 将这段程序烧写在其中。这段程序定义在 QEMU 的
riscv_setup_rom_reset_vec，大体如下::

    1:
    auipc  t0, %pcrel_hi(fw_dyn)
    addi   a2, t0, %pcrel_lo(1b)
    csrr   a0, mhartid
    ld     a1, 32(t0)
    ld     t0, 24(t0)
    jr     t0

    .dword start_addr
    .dword fdt_load_addr

    fw_dyn:

实际的汇编如下::

    PC                   INST         ASM
    0x0000000000001000:  00000297     auipc           t0,0            # 0x1000
    # 相当于 t0 = (PC + (0 << 12))

    0x0000000000001004:  02828613     addi            a2,t0,40
    # a2 = t0 + 0x28 => a2 = 0x1028
    # OpenSBI 的 FW_DYNAMIC 会从 a2 中获取结构体 fw_dynamic_info 的地址。
    # 这个结构体在 riscv_rom_copy_firmware_info 中设置。

    0x0000000000001008:  f1402573     csrrs           a0,mhartid,zero
    # csrrs(csr read and set)，第三个参数对应位如果位1，则会将 csr 对应设为 1
    # 那么这里就是， a0 = mhartid = 0，a0 是 opensbi 要求的。

    0x000000000000100c:  0202b583     ld              a1,32(t0)
    # a1 = mem(t0 + 0x20)，也就是加载 0x1020 位置的值。即，fdt_load_addr

    0x0000000000001010:  0182b283     ld              t0,24(t0)
    # t0 = mem(t0 + 0x18)，也就是加载 0x1018 位置的值。即，start_addr

    0x0000000000001014:  00028067     jr              t0
    # 跳转到 firmware start addr（如果 --bios none，就是 kernel start addr）

    0x0000000000001018:  .dword start_addr
    # 这个定的是 DRAM 的地址，也是 opensbi 的起始位置

    0x0000000000001020:  .dword fdt_load_addr


20221025
============

pcrel_hi 和 pcrel_lo 搞清楚是什么东西了。官方解释如下：

`RISC-V Assembler Modifiers <https://sourceware.org/binutils/docs/as/RISC_002dV_002dModifiers.html>`_

%pcrel_hi(symbol) 是获取当前 pc 与 symbol 偏移值的高 20 位。
%pcrel_lo(label) 是获取当前 pc 与 symbol 偏移值的低 12 位，这个 symbol 来自于
label 标记汇编指令中的 pcrel_hi。

BlockISA 也有个类似的东西，叫 %tpcrel_hi(symbol) 和 %tpcrel_lo(symbol)。用法是
下面这样的::

    lconst %tpcrel_hi(msg)
    addtpc t#1, %tpcrel_lo(msg)

lconst 具体的值先不计算，等遇到 addtpc 后，再以 addtpc 的地址计算 addtpc 与 msg 之间
的偏移值，然后再更新 lconst 的值为 addtpc 与 msg 的偏移值。但是 lconst 的值还得依赖于
下一条指令就挺怪异的，虽然这个操作是仿 RV 的，但 RV 可不是这样的，RV 汇编 auipc 指令
时，%pcrel_hi 能直接算出来结果。

根据《lconst指令问题分析》，给出的解决方案如下::

       lconst %tpcrel(symbol, 1f)
    1: addtpc t#(1)

直接计算 label 1 与 symbol 的偏移值作为 lconst 的值。但这个编译器还没有实现。


20221026
============

在实现 reset vector(引导 bios 的那段程序) 后，运行报错了，产生的错误如下::

    ERROR: head within M-mode

查到是 head 解码里添加了一段逻辑，要求 M mode 中运行的都是 RV 指令，否则就会
导致 qemu 运行终止。之后，项目会去 RV 化，这段约束也不在起作用，这段代码就先
删掉。


20221031
============

马鹏在实现 trace manger 的时候，会实现一个 QEMU 的 plugin，在QEMU 启动的时候，
加上plugin 就会导致内核在启动的过程中出现下面这个问题::

    [   27.790187] SCSI subsystem initialized
    [   28.207875] usbcore: registered new interface driver usbfs
    [   28.272758] usbcore: registered new interface driver hub
    [   28.316471] usbcore: registered new device driver usb
    [   30.335973] hrtimer: interrupt took 29633200 ns
    [   30.347414] clocksource: Switched to clocksource riscv_clocksource
    [  139.286672] BUG: workqueue lockup - pool cpus=0 node=0 flags=0x0 nice=0 stuck for 38s!
    [  139.474429] Showing busy workqueues and worker pools:
    [  139.530716] workqueue events: flags=0x0
    [  139.582356]   pwq 0: cpus=0 node=0 flags=0x0 nice=0 active=1/256 refcnt=2
    [  139.599932]     pending: vmstat_shepherd
    [  139.686464] workqueue mm_percpu_wq: flags=0x8
    [  139.718271]   pwq 0: cpus=0 node=0 flags=0x0 nice=0 active=1/256 refcnt=2
    [  139.728504]     pending: vmstat_update

QEMU system 启动参如下::

    $QEMU -m 1024M -smp 1 -kernel $KERNEL -M virt \
    -append "console=ttyS0 earlycon root=/dev/vda rw" \
    -device virtio-blk-device,drive=hd0 \
    -drive if=none,file=$ROOTFS,format=raw,id=hd0 \
    -fsdev local,id=p9fs,path=$SHARE,security_model=mapped \
    -device virtio-9p-pci,fsdev=p9fs,mount_tag=p9 \
    -plugin $QEMU_ROOT/build/contrib/plugins/libcache.so

如果是多核(smp = 3)的时候，这个可以起来

产生 "BUG: workqueue lockup - pool" 的地方在 workqueue.c 中的 wq_watchdog_timer_fn 中，
当 workpool 的时间戳加上 wq 的 watchdog 拍数小于当前 kernel 执行的拍数，就会
输出以上信息。

目前猜测是因为 plugin 往 tb 里加了一些自己的东西，导致执行 tb 的时间过长。根据
之前了解的，kernel 感知到的时间其实是 tb 运行的时间，具体还要再细看下 QEMU icount
的实现。


20221102
============

之前 LARM 对块内跳转指令在原子块的行为定义了，要求将其偏移值作为无符号来看待。
之所以这样定义，其实是为了让块内跳转指令在原子块不会往后跳(target PC < current PC)，
这样是为了避免恶意程序在原子块内弄个死循环，导致总线一直被锁（原子块通过锁总线来避免相
应内存被其他 CPU 修改）（这种情况 OS 不会对此进行处理的嘛？）。

对于带立即数的块内跳转指令，其偏移值计算方式如下::

    offset = zext(imm:0, XLEN)

那对于使用 t 寄存器的块内跳转指令，它偏移值已经是 XLEN 位的，零扩展后的值不变。所以
它的偏移值计算如下::

    offset = <T#M> * 2

这里有个问题，假设现在的 tpc 是 0x2，然后 t 寄存器中的值是 -1(0xFFFF FFFF FFFF FFFF)，
那么它的目标地址计算出来就是 (-1) * 2 + 0x2 = 0x0。这样其实是往回跳的。


20221103
============

马鹏那边出现了两个问题。

第一个，tcg 初始化的时候，会创建一个 plugin tb(ptb)。之后每次翻译 tb 的时候，对它就行
初始化。ptb 为 tb 中每条指令创建了 plugin insn(pinsn)，在 tb 翻译过程中，每翻译一条
指令，就会将指令的数据：如指令编码、pc 等塞到 pinsn 中。等到这个 tb 翻译结束后，会调
用 plugin 的 vcpu_tb_trans 回调。

TraceManger 会在 vcpu_tb_trans 中，为 ptb 中每条 pinsn 注册 exec_before 回调(字面
意思，就是这条指令将要执行时，会触发该回调。)，并设置这个回调用到的参数。
现在的现象是，测例文件中某个块头(pc = 0xdcff0)在取指过程中，取粘头时发生缺页异常。也就
是这条指令翻译到一半的时候，跑去处理异常了，但是处理异常回来后，没有先重新翻译，而是去
调用 plugin 的回调去了，调完了才重新翻译。

第二个，Function Model 与 QEMU 跑同一个测例，执行过程不同。具体细节没有了解，目前还在
投入第一个问题中。


20221105
============

第一个问题，从 TraceManger 角度看，0xdcff0 所在的 tb 调用了一次 vcpu_tb_trans，但是
里面有有两条 pc 都为 0xdcff0 的指令，一个长度为 16B，另一个为 32B。从 QEMU 角度看，
在 trans_* 函数中，每调用一次 translator_* 就会往 pinsn 中的 data 塞它读的数据(一般
是指令编码)。在翻译 0xdcff0 这个块时，首先，它先从 ptb 中拿到一个 pinsn。然后，边取指
令并往里面塞数据，取到一半发生异常，那就退出这次的翻译流程。（异常处理那块代码之前已经
翻译过了，就直接执行。）异常回来，又重新开始翻译 0xdcff0 块，然后从 ptb 中重新取了个
pinsn，再往里塞数据。所以现在来看就是，ptb 中有两条 pc 为 0xdcff0 的 pinsn。

去看 ptb 初始化相关的代码。ptb 的结构如下:

.. code-block:: C

    struct qemu_plugin_tb {
        GPtrArray *insns; //这个就是 pinsn 集合。
        size_t n; // 指令数量
        uint64_t vaddr; // tb 的 pc
        ...
        GArray *cbs[PLUGIN_N_CB_SUBTYPES]; // ptb 的回调
    };

QEMU plugin 在翻译阶段涉及的函数如下::

    plugin_gen_tb_start   /* tb 翻译开始时，对 ptb 进行初始化，并生成一个空 tcg 中间码 PLUGIN_GEN_FROM_TB */
    plugin_gen_insn_start /* 每开始翻译一条指令前，对 pinsn 初始化，并生成一个空 tcg 中间码 PLUGIN_GEN_FROM_INSN */
    plugin_gen_insn_end   /* 指令翻译结束后，生成一个 空 tcg 中间码 PLUGIN_GEN_AFTER_INSN */
    plugin_gen_tb_end     /* tb 翻译结束时，触发 vcpu_tb_trans 回调。补全之前的 tcg 中间码等等 */


在开始翻译前，更新了下 ptb 的 vaddr，并生成一个空的 tcg 中间码等。唯独没有将 n 设为
0。tb 翻译结束后，触发 vcpu_tb_trans 回调，填写内容到空 tcg 中间码中，最后才把 n 设
为 0。所以只要翻译中间出现异常，再次进入 tb 翻译函数时，n 不一定等于 0，也就是 ptb 还
保留着之前的翻译结果。按道理说，每次开始翻译 tb 时，tb 里的指令数就应该是 0。


20221202
=========

CA Model 组在跑 SPECint 测例时，反馈 QEMU 在仿真 BlockISA 架构相比 RISCV 或 ARM
慢很多，导致他们跑一个测例需要一两周的时间，所以，要我们优化下 QEMU 的速度，使之能与
RISCV 或 ARM 持平。

为了能够知道 Block 和 RISCV 分别启动内核的时间差距有多大，同时也为了有可比性，挑选
了一条 block type 未使用的 header，当执行到这个 header 时，输出 QEMU Virtual 时间。
然后在内核中相同位置加上了这条指令，一个是在 _start，另一个是在 kernel_init 处。(以下
结果是去除了动态掩码检查情况所测的数据)

跑出的结果为:

+-------+-------+
|       | Time  |
+-------+-------+
| RISCV | 4.0s  |
+-------+-------+
| BLOCK | 47.6s |
+-------+-------+
|  B/R  | 11.9  |
+-------+-------+

带上 TraceManger 编写的 plugin 来统计运行时指令数，数据如下:

+-------+-------+-------------+-----------+------------+-------------+
|       | Time  | Header Size | Inst Size | Load Times | Store Times |
+-------+-------+-------------+-----------+------------+-------------+
| RISCV | 13.1s | 0           | 241540415 | 43187911   | 70074347    |
+-------+-------+-------------+-----------+------------+-------------+
| BLOCK | 72.6s | 63491346    | 613359384 | 55638755   | 78975168    |
+-------+-------+-------------+-----------+------------+-------------+
|  B/R  | 5.5   | N/A         | 2.5       | 1.2        | 1.1         |
+-------+-------+-------------+-----------+------------+-------------+

plugin 会在每条指令前插入一条 helper IR，每次执行一条指令就会调用一次这个 helper，
相应的时间就会变慢。输出的数据，header size 代表块头指令的数量，Inst Size 代表
块头和微指令的数量总和。

由上可以看出，当前运行时指令数量的差距在 2.5 倍左右，但是当前时间比却差了 11.9。
呵呵，QEMU 优化的空间挺大的，不过感觉最多优化成比 RISCV 慢 2.5 倍。想不出来还有什么优
化点。先用 perf 来看下 QEMU 中运行函数的调用次数。perf 命令为：``sudo perf record
-e cpu-clock -g $QEMU_RUNSCRIPT``。下面数据是调用次数最多的几个:

+-------------+--------------------------------+
| Event count | 33963250000                    |
+-------------+--------------------------------+
| Overhead    | Symbol                         |
+-------------+--------------------------------+
| 7.64%       | tb_lookup                      |
+-------------+--------------------------------+
| 4.43%       | check_regs                     |
+-------------+--------------------------------+
| 4.26%       | cpu_handle_interrupt           |
+-------------+--------------------------------+
| 3.93%       | cpu_get_tb_cpu_state           |
+-------------+--------------------------------+
| 3.34%       | cpu_tb_exec                    |
+-------------+--------------------------------+
| 3.23%       | helper_handle_exec_and_baranch |
+-------------+--------------------------------+
| 3.02%       | deposit32                      |
+-------------+--------------------------------+
| 2.67%       | cpu_exec                       |
+-------------+--------------------------------+
| 2.48%       | helper_blk_do_recovery         |
+-------------+--------------------------------+
| 2.36%       | riscv_has_ext                  |
+-------------+--------------------------------+
| 1.98%       | qemu_xxhash7                   |
+-------------+--------------------------------+

耗时最大的就属查找 TB 了，而 QEMU 可以用 chained TB 的方式减少查找 TB 的次数。也就是
用 goto_tb，下面先分析下它为啥能减少查找 TB 的次数。

QEMU 中有三种退出 TB，goto_tb、exit_tb、lookup_and_goto_ptr。

goto_tb 是最快的，TB 会有两个 slot 分别挂载一个 TB，这可以对应着 taken 和 no-taken
两个跳转方向，goto_tb 只有一个参数，用来指定将要跳转到哪个 slot 去。第一次的时候，对
应 slot 暂时还未挂载 TB，就会往下一个 IR 走，所以 goto_tb 一般会搭配更新 pc 操作和
exit_tb 使用。

exit_tb 是退出当前 TB 进入到 main loop 去。它有两个参数，一个是 tb，另一个是 tb 的返
回值 tb_exit。当退出 tb 的时候，exit_tb 会把 tb 和 tb_exit 作为返回值传到 main
loop 去(tb 的最后两 bit 为零，这两位就作为 tb 的返回值，其中 0, 1 是 slot 编号，其他
都是异常情况。详情见 TB_EXIT_MASK 上面的注释。)那么 main loop 会根据 pc 查找 next
tb，然后把 next tb 挂载到 tb 对应 slot 去。当然 exit_tb 中的 tb 可以传一个 NULL，那
就代表着直接退出 TB，不去链接。

lookup_and_goto_ptr 会尽量避免返回到 main loop，它会根据 pc 去查找下一个 tb，然后直
接跳过去。

基于以上的分析，去改一下当前的实现，优先使用 goto_tb，其次 lookup_and_goto_ptr。修改
后，运行数据如下:

+-------+-------+
|       | Time  |
+-------+-------+
| RISCV | 4.0s  |
+-------+-------+
| BLOCK | 36.6s |
+-------+-------+
|  B/R  | 9.1   |
+-------+-------+

perf 耗时次大的就属中断处理，先统计了下 rv 和 block 之间的异常和中断出现的次数:

+---------------------+-----+------+
| Exception/Interrupt | RV  | BLK  |
+---------------------+-----+------+
| s_timer             | 700 | 5098 |
+---------------------+-----+------+
| m_timer             | 700 | 5099 |
+---------------------+-----+------+
| s_external          | 29  | 20   |
+---------------------+-----+------+
| illegal_instruction | 1   | 1    |
+---------------------+-----+------+
| supervisor_ecall    | 727 | 5115 |
+---------------------+-----+------+
| exec_page_fault     | 1   | 1    |
+---------------------+-----+------+
| store_page_fault    | 2   | 2    |
+---------------------+-----+------+

时间中断好说，因为它跑的慢，自然触发的就多，但是不清楚为什么 secall 也会有这么多次的。
查了下在 kernel 以下地方产生了 secall 异常:

1. __sbi_set_timer_v02   -->  5101次

    设置下一个时钟事件，感觉像是时钟中断触发后，调整下次中断产生时间。

2. sbi_probe_extension   -->  4次

    用来判断是否支持某某 SBI 扩展，如：TIME、IPI、RFENCE 等。

3. __sbi_base_ecall      -->  3次

    SBI 协议的 base extension 中定义的，用来获取像 sbi spec version、machine
    vendor ID 之类的。

4. __sbi_rfence_v02_call -->  7次

    SBI rfence 扩展的实现。

secall 也主要是处理时间中断去了，没啥可说的。按照常规的方法，想不出来还有什么地方可以优
化。在汇报 QEMU 运行速度慢的时候，周博提出了个疑问::

    为什么BlockISA的块体也有自己的IR？BlockISA里面微指令不是真正的指令，块头才是真正的指令。

    所以BlockISA的一个Block对应QEMU里面的一个Translation Block。QEMU仿真BlockISA的速度应该比RISC-V快才对。。

QEMU 中有 ibpc、掩码检查等的存在，在同等运行指令数量上仿真 Block 应该是不会有 RV 快
的。然后李飘提出我们可不可以实现在解码 header 时，也把 body 解码，让 header 和 body
处在一个 TB 中，减少 header 跳转到 body 的步骤，这样或许会有比较大的性能提升。

但是这个改动不是短时间能搞定的，涉及到方方面面，例如：把一条微指令变成了 header 中的一
条条 IR 后，当产生异常，然后异常处理完成返回时，它怎么从中间码中的某条开始在执行呢？而
且这样也没有了块内中断。


20221207
=========

根据之前 perf 数据所生成的火焰图来看，查找 TB 的时间占比很大，之前的优化尝试使用了
chained TB 的方式来优化速度，但是提升不高。从 QEMU 日志来看，并没有使用到 goto_tb
的中间码。

通过分析 QEMU 的文档以及 gen_goto_tb 的实现，发现 goto_tb 使用有两个限制: 1. CPU
的状态是固定的，例如，跳转的地址固定(直接跳转)，还有特权级不会发生切换(其他架构不清楚，
RV 有这个限制，sret/mret 都是用的 exit_tb 来退出。猜测着主要原因是因为 QEMU 为每个
特权级分配了一个 mmu，也就是 load/store 用的 mem_idx，怀疑是为了避免虚拟地址重名问
题？)；2. 从 Target 角度来看，当前 TB 和下一个 TB 需要处在同一个页上，即::

    ((db->pc_first ^ dest) & TARGET_PAGE_MASK) == 0

如果能把 tb_lookup 的时间缩短，handle_interrupt 的也会缩短。所以，当前先看看 QEMU 怎
么让 BlockISA 支持 chained TB。目前大概是有几个方向:

1. header 和 body 放在一个 TB 内，这个国柱在 20211210 的日志中的第二步策略提到将整
   个 Block 一块儿解码，也是周博提出的想法。
2. 提高 tb_lookup 的速度，这个函数里 tb_htable_lookup 占比比较大，看看能不能扩大
   tb_jmp_cache 来提升命中率。
3. 分析为什么有同一个页的限制，看看是否能修改 QEMU，使其适应 BlockISA 这种情况。
4. 会上周博提出，找编译器配合，使生成出来的文件的 header 和 body 放在一块儿。

方案二，统计了查找 TB 的次数(find_times)、TB 不在 cache 中的次数(find_miss)、TB 在
cache 中，但不符要求(如：TB flags、cflags 不符或者 hash 冲突，被其他 TB 所占等)
(find_no_need)。之后的统计数据为::

    find_miss/find_times = 403380/109715877 = 0.3%
    find_no_need/find_times = 11671548/109715877 = 10.6%
    命中率: 1 - 10.6% - 0.3% = 89.1%

试了下将 cache 扩大后的情况::

    find_miss/find_times = 592796/108892375 = 0.5%
    find_no_need/find_times = 1478785/108892375 = 1.3%
    命中率: 1 - 0.5% - 1.3% = 98.2%

miss 的次数会有点儿提升，但是 no_need 的会降低很多，命中率会有一定提升。重新跑 perf
数据，tb_lookup 由原来的 28.9% 降低为 18.2%。基于 lookup_and_goto_ptr 优化，扩大
TB cache 后，性能提升情况如下: 

+-------+-------------+-------------------------------+--------------------+
|       | Un-Optimize | Optimized-lookup_and_goto_ptr | Optimized-TB_Cache |
+-------+-------------+-------------------------------+--------------------+
| RISCV | 4.0s        | 4.0s                          | 4.0s               |
+-------+-------------+-------------------------------+--------------------+
| BLOCK | 47.6s       | 36.6s                         | 32.6s              |
+-------+-------------+-------------------------------+--------------------+
|  B/R  | 11.9        | 9.1                           | 8.2                |
+-------+-------------+-------------------------------+--------------------+

方案三，打算从 rv 生成 goto_tb 的地方去分析为什么会有同页的限制。

方案二和三应该是要改 QMEU 的架构，这样我们的 QEMU 大概是没法合到主线。

方案四，如果将 header 和 body 放置在一起，这样的话，首先 fall through 的规则就变了，
不再是 bpc + header_size。这个的改动至少涉及了编译器、QEMU、内核(因为内核有
pushsection 的操作)


20221215
=========

一般来说，BlockISA 编译器会将块头和块体放在不同的段中，这样做大概是有两个原因，第一点，
我们有个块头分支类型叫 fall through，它的意思是说，当一个块执行完后，将要执行的下一个
块的块头的 BPC 是当前块头 BPC 往下移动当前块头大小。所以从体系结构上说这里就限制了，
要用 fall through 块头跟块头就要放在一起，块体和块体放在一起。函数调用用的是 call 类
型的块头，那其实可以每个函数内部存着各自的块头和块体，但是这样块体就没法共享，会导致生
成出来的文件过大，这应该就是第二点。

因为块头和块体是分属着不同段，如果文件大的时候，块头和块体就分属到了不同的页中。这样会
使 QEMU 对于 BlockISA 没法使用 chained TB 的方式来加快 TB 的执行，导致每次执行完 TB
都需要去查找下一个 TB 的位置。如果能将 header 和 body 放到同一个 TB 块内，对于那些目
标地址明确的块(fall、direct、call)，它们就有机会使用 chained TB。

目前初步想法是，对于正常情况下，块头和块体放在一个 TB 内，但这样 PC 不连贯，不确定会不
会对 cpu_restore_state() 有影响。异常情况(如块被中断)就按照现在的逻辑，块头和块体分属
不同的 TB。

QEMU 为了避免重复翻译会翻译好的 TB 缓存起来，之后执行前会通过 PC 作为键值去索引。索引
到之后，还会对 TB 的一些标志位对比。现在假设缓存里有一个正常情况的 TB，这次执行这个 TB
过程中发生了异常，就需要走异常情况分支的 TB 生成行为，但这两个 PC 是相同的，那就设置不
同的标志位。当异常返回并需要恢复块状态时，设置标志，当块提交时，清标志。在解码头时，判
断这个标志，决定是否要连着块体一起翻译。


20230103
=========

TB 在运行的过程中如果产生异常，那么得把 CPU 状态更新到出现异常的那条指令时的状态。大部
分 CPU 状态是 TB 执行时实时更新的，但像 PC 这种，如果为每条 target 指令翻译时加上一个
更新 PC 的操作，这会拖慢执行速度，而且收益不大，毕竟异常发生的占比是少数的，大部分的时
候 TB 是完整执行的。那么就得异常产生的时候，将 PC 更新成出异常那条指令。对于一些进行简
单操作的指令，在 raise exception 前加上个更新 PC 的操作就行，但是对于像 load/store
这类需要使用 helper 函数来处理地址翻译操作的复杂指令，在 helper 执行过程中需要产生异常
时得更新下 PC，但 helper 并不知道 target PC 是多少，那要么将 target PC 以参数的形式
传进去，要么另想办法。使用参数有个数的限制，如果需要更新的状态多了，就会出现参数不够用
的情况，QEMU 现在用的是 cpu_restore_state 来更新状态。

每条 target 指令翻译都会有一条 insn_start 的中间码，这个中间码的参数就是需要更新的状
态，参数的个数通过宏 TARGET_INSN_START_WORDS 来决定。 (RISCV 中这个值是 1，只有 PC
需要更新。) 这个中间码所做的事是生成一张 sleb128 格式的表，表中存储的就是这些状态。例
如，现在有两个状态需要保存 PC 以及 BPC，有两条指令，那这个的样子大概为::

    PC1->BPC1->insn1_code_size->PC2->BPC2->insn2_code_size
    (insnx_code_size 是指这条指令所翻译出来的 host 指令大小。)

大概是为了节省内存，或者优化编码/解码 sleb128 的速度，优化成如下::

    (PC1-tb->pc，一般这里就是 0)->BPC1->insn1_code_size->(PC2-PC1)->(BPC2-BPC1)->insn2_code_size

helper 在产生异常的时候，查表更新到对应指令的状态。通过 searched_pc (GETPC() -
GETPC_ADJ)，GETPC() 获取到的是 helper 函数正常执行完时的返回地址，一般是 TB 中 call
这个 helper 的下一条 host 指令。GETPC_ADJ 请看 :ref:`getpc_adj<getpc_adj>` ，最后这个
searched_pc 值大概是 call helper 指令所占地址区间中的中间位置。) 以及 host_pc (初始
值为当前 TB 的开始地址，每取出一条 target 指令的状态就加上一个 insnx_code_size，意思
就是 target 指令对应 host 指令组的起始位置。)来判断是否找到了那条指令，判断条件为:
host_pc > searched_pc，简单来说就是，一条 target 指令生成出来的 host 指令组的起始和
结束位置分别是 host_start 以及 host_end，如果 searched_pc 在 host_start 和 host_end
之间，那么就是找到了这条target 指令，找到了就用这条指令的状态更新，用的是
restore_state_to_opc。

那么将块体作为块头的数据时，每解码一条块体指令在前面加上个 insn_start，这样可以用
restore state 更新状态了，这样当块体执行出错时，可以将 pc 更新为该指令的 pc，也就是
tpc。块内异常的判断条件是 bpc 或 tpc 有效，其中，bpc 有效的时候是在块内产生的异常，
这时候会将 tpc 更新为 pc，sepc 为 bpc。当执行到某条微指令的中间码并出错时，bpc 已经被
赋值，restore state 也会将 pc 更新成这个微指令的 tpc。

.. _getpc_adj:

getpc_adj
---------

GETPC_ADJ 是一个宏，全称大概是叫 GETPC adjust，该值为 2。因为 GETPC() 获取的是调用
该函数的函数的返回地址:)，函数 A 调用了这个 GETPC()，那返回的值是函数 A 的返回地址。
这个返回地址通常情况下是 call A 指令的下一条指令，那么通过这个 GETPC_ADJ 将这个值
调整成 call A 指令中的某个位置，假设说这个 call 指令是 4 字节的，它的起始地址是 0x0，
下一条指令的地址是 0x4，经过调整后的地址是 0x2，指向的是 call 这个指令中间的位置。

它这个值的解释如下::

    The true return address will often point to a host insn that is part of
    the next translated guest insn.  Adjust the address backward to point to
    the middle of the call insn.  Subtracting one would do the job except for
    several compressed mode architectures (arm, mips) which set the low bit
    to indicate the compressed mode; subtracting two works around that.  It
    is also the case that there are no host isas that contain a call insn
    smaller than 4 bytes, so we don't worry about special-casing this.


20230104
=========

v0.13beta 新添加了几条指令，修改了几条指令，也删了两条指令。

新添加的指令有以下几种::

    1. 字操作的，ADDIW, ADDW, ANDIW, ANDW, ORIW, ORW, SLLIW, SLLW, SRAIW, SRAW,
       SRLW, SRLIW, SUBIW, SUBW, XORIW, XORW, DIVUW, DIVW, MULW, REMUW, REMW,
       SGETW, SGETWU
    2. 除法操作的，MULH, MULHSU, MULHU
    3. 条件 trap 指令，ctrap
    4. Scratch 寄存器操作，ADDSPI, ADDSR, Stack-Realted Load, Stack-Realted Store
       sgetw, sgetwu
    5. 等待事件指令，wfe
    6. 线程操作类型的，get cpuid, get tid

新增的算术类型指令都与 riscv 相同，QEMU 实现可以直接参考 riscv 的。


20230201
=========

623 测例分析放在了 issues 里。


20230202
=========

接着昨天的分析。


20230203
=========

现在手头上有以下几个任务:

1. 401 测例问题的排查
2. supertest 测例排查
3. 与编译器沟通，将块内跳转单独放到一种类型的块中，修改临时分支支持其他块
4. 删除 goto_tb 的跨页限制；关闭掩码检查；块头到块体用 goto_tb 连接。

supertest 是从编译器那边拿过来的二进制，这个测例要简单点，相比 SpecInt 测例应该更好去
查问题，没准 supertest 的问题都定位出来后，SpecInt 的也就解决了一部分。

上午先把 QEMU 性能分支的东西先做了，下午再排查 SpecInt 和 supertest 的问题。

第三点这个可能没法那么快，还得找编译器一起定下来。第四点这个简单些，可以弄得快点。

因为 user mode 不用去处理 recovery，user 没有中断，redo ecall 也不会走这个 recovery。
所以 user 其实可以直接将块头和块体链接起来。这样子看来，这种改动其实是可以合到主线上的。

下午还是在弄性能，没看 supertest 测例。

20230204
=========

supertest 目前看有 71 个测例无输出，4 个测例出现非法指令，1 个测例失败，2 个测例陷入
死循环。

非法指令测例是因为有 trap 指令，现在 QEMU user 模式遇到 trap 指令会报非法指令。一般编译
器也不会编译出 trap 指令，这个还得和编译器沟通下。

测例失败的问题是程序往 a0 里设值为 0xd，然后，就会跳到 -1 的地址上去并结束。QEMU 就会
判断 a0 是否为 0，不为 0 就会导致测例失败。

没有任何输出的测例，挑了个 suite/iso14882/15/0/t218-111.C/t218-111 看了下，它执行的
情况。函数调用过程是这样的::

    _start -> main -> _Z10TEST__MAINv -> __cxa_allocate_exception

__cxa_allocate_exception 这个有点奇怪，这个是不是走到异常了。得找编译器的同事问问他们
这个测例本身的逻辑是啥。不会这个测例就是用来测异常的吧。

没输出中的一个测例 tprintfreturnerror 有点奇怪。QEMU 里输出 test pass 会有两次，一次
是 printf，一次是 qemu_log。但现在 qemu 日志里有这个输出，控制台却没有。目前怀疑的点是
user 里模拟系统调用的时候，修改了 stdout。通过 gdb 调了下，这个测例会运行个 close，而
close 的参数是 1，也就是对应的 stdout。看起来是程序运行着就把 stdout 给关了。

20230211
=========

在跑 Supertest 测例写了个脚本，其中有些 shell 命令是直接从网上抄来的，之前没时间弄懂，
现在这里写一写。

第一个命令查找 run_log 目录及其子目录下的所有可执行文件(不包括目录文件)::

    find ./run_log -executable -type f -print

executable 用来匹配可执行文件以及目录。这个功能也能通过 -perm 来实现，perm 的全称就是
permission，-perm /u=x 就相当于 -executable。perm 有三种格式::

    -perm mode   文件的权限正好是 mode 就匹配
    -perm -mode  文件的权限包括 mode 就匹配
    -perm /mode  文件的权限部分满足 mode 就匹配

例如，'-perm g=w' 只有 group 可写的文件才会匹配，哪怕是 group 可读可写都匹配不上。
'-perm -u=w,g=w' 只要 user 以及 group 可写，文件就会匹配上，也就是说，当权限为 user
可读可写，group 可读可写的文件也会匹配上。
'-perm /g=w，u=w' 只要 user 或 group 可写，该文件就会匹配上，就是当文件只有 user 可读
可写，group 或 others 都不可写也会匹配上。

type 就是文件类型，有这么几种参数::

    b 块设备
    c 字符设备
    d 目录
    p 管道
    f 常规文件
    l 链接文件
    s 套接字
    D door(这个只有 Solaris 系统有，不太了解)

print 就是将文件的完整名字输出到标准输出中，就是路径名以及文件名。

第二个命令是用来将文件中 'test pass!' 这行及其上一行删除::

    sed -i '$!N;/test pass!/!P;D' run.log

sed 命令全称叫过滤和转换文本的流编辑器，这个有点复杂。。

浩富和继钦他们将 header->body 以及 body->header 之间的 TB 跳转优化加到主线后，测试了
下这个东西跟之前没有优化的差距，得出来的数据是，原先跑 1min 13s的数据，优化后需要 1min 3s，
总的优化只有 13%，提升不大。看来最耗时的还得是动态索引 T 寄存器以及掩码检查。

掩码检查这个东西好搞，跑性能数据的时候将它关掉就好了。但是，动态索引这个就莫得办法了。
跑块内跳转指令不可能不用动态索引。所以还是得想办法优化下动态索引机制。目前能想到是的将
helper 用中间码来实现，因为本身逻辑并不复杂。或者在解码头的时候，就遍历一遍块体，看看有
没有块内跳转指令，有才用动态索引，但这个提升就得依赖这个 TB 会被多次执行。

用中间码的话，QEMU 有一个可以读写 Host 内存的中间码。每次索引 Treg 的时候，计算出要用
哪个 Treg，然后拿到对应 Treg 在 env->blk_t 里的偏移，然后就往这块内存去存取。这样就可
以不用 helper 了。

中间码实现的这种用李飘的测例跑了下，只需要 21.5s，相比 helper 的 1m3s，提升了 64.8%。
用 SpecInt 的 429 测例跑了个 4m11s，helper 跑了 6m51s，提升了38.9%。这样看来提升还是
挺大的。

另一个方案，在解码块头时，遍历块体。这个也实现了，也有一定的提升，但不大。但对于 0.16 前
的性能分支，应该帮助还是挺大的。

20230213
=========

今天主要是把之前的 0.13beta 剩下的三条指令添加上，添加完后，要打上 0.14 的 tag。剩下的
三条指令是 lconst、sysget 和 sysset。lconst 现在改了名字，叫 lw/lwu/ld.ip，它的汇编
语法是::

    block_text_begin xxx
    ...
    ld.ip <label>
    ...
    block_text_end xxx

    label:
        0xXXXXX

指令编码就只有 opcode、function code 以及 imm 组成。imm 用于 label 的索引，label 的
地址的计算方式为 tpc2 + zext(imm,XLEN) << 1。

现在不再是把立即数硬编码的指令里面，作为指令编码的一部分。而是将立即数放到内存上，从内存
上去读取立即数的值，那么这条指令本质上就是一个 load 类型指令。这样做是为了将指令都变成
一个定长指令，降低微架构的复杂度来提升解码速度。

这个指令实现相对简单些，可以基于之前的 load 指令实现，但是在做 QEMU 反汇编就不太友好了。
因为现在立即数是放在块外面的，那么对应内存可能在页表里没有对应的映射，那反汇编就没法去到
立即数。看起来只能显示立即数所在的内存地址。

sysset 和 sysget 原先是将系统寄存器编号硬编码在指令中，用了 16 位来表示系统寄存器编号，
这样总的指令长度就到了 32 位。现在 0.14 是将系统寄存器编号放到一个 t 寄存器，然后，
sysset/sysget 就跟 add 指令一样，索引 t 寄存器就行。

修改完后，微指令的长度就都为两字节。

20230214
=========

用了 0.14 的 gcc 编译器编了我们的测例，会跑出段错误，从 QEMU 的日志上看是一个 call 块，
call 到了一个 0 地址，QEMU 去读这块内存就出现了段错误，而且这个错误是在进入 main 函数前
就产生了。以 newlibc 的为例，出问题的 call 块在函数 frame_dummy 中::

    0000000000021d20 <frame_dummy>:
    21d20:   00000000 00008000 00501000 ee49ac0b     bstart  b.std, bnext.cond, battr:atomic.none, battr:none, bget:0x00000000, bset:0x00008000, ptr:0x101ba size:0x4, bnext:0x21d70,
    21d30:   00000004 00000004 00000c00 ee4a000b     bstart  b.std, bnext.fall, battr:atomic.none, battr:none, bget:0x00000004, bset:0x00000004, ptr:0x101d0 size:0x3, bnext:0x21d40,
    21d40:   00000006 00000c02 e2c02400 ee497c0b     bstart  b.std, bnext.concat, battr:atomic.none, battr:none, bget:0x00000006, bset:0x00000c02, ptr:------ size:0x9, bnext:------,
    21d50:   ffffffff fffffffd 00000000 00000800     bstart.concat    bnext.call ptr:0x101d6 bnext:0x0  ===> 出问题的块
    21d60:   00000004 00000006 00001400 ee49000b     bstart  b.std, bnext.fall, battr:atomic.none, battr:none, bget:0x00000004, bset:0x00000006, ptr:0x101f0 size:0x5, bnext:0x21d70

这个问题找了邹将将，将将说在 gcc 编译器目录下面去搜一下 frame_dummy，得出来的相关结果::

    grep: lib64/gcc/linx64-unknown-elf/11.0.0/crtbegin.o: binary file matches
    grep: lib/gcc/linx64-unknown-elf/11.0.0/crtbegin.o: binary file matches

这两个都有这个函数，然后把它们俩反汇编，去找到 call 块是要 call 啥，反汇编的时候加上 -r
参数，-r 看 man 手册里说是显示文件的重定位入口。lib64 反汇编出来是这样的::

    278:   00000006 00000c02 00002400 ffeabc0b     bstart  b.std, bnext.concat, battr:atomic.none, battr:none, bget:0x00000006, bset:0x00000c02, ptr:------ size:0x9, bnext:------,
        278: R_LINX_START   .Ltmp18.bstart
        278: R_RISCV_RELAX  *ABS*
        278: R_LINX_STOP    .Ltmp18.bstop
        278: R_LINX_BNEXT   __register_frame_info
        278: R_RISCV_RELAX  *ABS*
    288:   ffffffff 00000000 00000000 00000800     bstart.concat    bnext.call ptr:0x122 bnext:0x278
        288: R_LINX_CONCAT  __register_frame_info

lib 的反汇编::

    278:   00000006 00000c02 00002400 ffeabc0b     bstart  b.std, bnext.concat, battr:atomic.none, battr:none, bget:0x00000006, bset:0x00000c02, ptr:------ size:0x9, bnext:------,
        278: R_LINX_START   .Ltmp18.bstart
        278: R_RISCV_RELAX  *ABS*
        278: R_LINX_STOP    .Ltmp18.bstop
        278: R_LINX_BNEXT   __register_frame_info
        278: R_RISCV_RELAX  *ABS*
    288:   ffffffff 00000000 00000000 00000800     bstart.concat    bnext.call ptr:0x122 bnext:0x278
        288: R_LINX_CONCAT  __register_frame_info

这两个都是要 call __register_frame_info，用 readelf 可执行文件没有搜出来这个符号。将
将说没有符号编译的时候应该会报符号未找到的错误。

20230215
=========

现在 0.14 相比之前就只是加了那三条指令，然后现在跑的是用户态程序，那只有 lconst 指令有
问题了。直接编 lconst 那个汇编测例，用 QEMU 跑，再看看去的值对不对。编译的命令是这样的::

    ~/block_toolchain/linx64-unknown-elf-20230210/bin/linx64-unknown-elf-gcc -e ld_ip -nostartfiles block_asm/ld_ip.S -static -o test_ld_ip

-e 参数来决定入口函数，让它将入口设到 ld_ip，这样 QEMU 一开始就会跑这个函数，除了这样外，
还要加上个 -nostartfiles 让它链接的时候不要使用标准系统启动文件，大概就是 _start 那些，
而这些会调用 main，如果不加 nostartfiles 会报这样的错误::

    /home/wenjie/block_toolchain/linx64-unknown-elf-20230210/bin/../lib64/gcc/linx64-unknown-elf/11.0.0/../../../../linx64-unknown-elf/bin/ld: /home/wenjie/block_toolchain/linx64-unknown-elf-20230210/bin/../lib64/gcc/linx64-unknown-elf/11.0.0/../../../../linx64-unknown-elf/lib/crt0.o: in function `_start':
    (.text+0x30): undefined reference to `main'
    /home/wenjie/block_toolchain/linx64-unknown-elf-20230210/bin/../lib64/gcc/linx64-unknown-elf/11.0.0/../../../../linx64-unknown-elf/bin/ld: (.text+0x40): undefined reference to `main'
    /tmp/ccLSJgXE.o: in function `__ld_ip__.bstart':
    (.text.body+0x0): relocation truncated to fit: R_LINX_BODY_STOP against `__ld_ip__.bstop'
    collect2: error: ld returned 1 exit status

一开始编出来的 lconst 测例还有点问题，lconst 的偏移为 0，计算出来的地址是块头的，不是立
即数的。问了下将将，在块结束的位置需要加上 bend 伪指令。这样编译器才可以知道块体结束的位
置，然后再计算 lconst 指令的偏移。修改后，重新跑下，看日志里，它取值跟预期是一样的。

昨天那个符号 __register_frame_info 是一个弱符号，该符号不存在也不会报错。那这样看，也
没有问题了。接下来就是比较下 0.13beta 以及 0.14 同个程序的执行流，比较了下，出现差异的
地方是在一个叫 frame_dummy 的函数，它的样子是这样的::

    0x0000000000021d20:  00000000 00008000 00501000 ee49ac0b std_block.conditional next:0x21d70, ptr:0x101ba, attr:none, out_reg(a5), in_reg()
    0x00000000000101ba:  032a                  lw.ip           [101c8]
    0x00000000000101bc:  0120                  addtpc          t#1
    0x00000000000101be:  0f32                  set             a5, t#1
    0x00000000000101c0:  2011                  setc.eqi        t#2, 0

0.13beta 这个条件块的条件是成立的，也就是 lconst 的立即数与下面的 addtpc 算出来是等于
0 的，但是 0.14 这个算出来不等于 0。然后，这个 lconst 和 addtpc 组合大概率是去取一个符
号的地址，用昨天学到的反汇编技巧，反汇编一下那个 crtbegin.o 看看这个是要读取什么样的符
号::

    0000000000000104 <.Ltmp16.bstart>:
    104:   002a                    lw.ip   0x0 # 0
               104: R_LINX_LCONST_DATA .Ltemp_lconst_9  --> lconst 要读取的符号
               104: R_RISCV_RELAX  *ABS*
               104: R_LINX_BODY_STOP   .Ltmp16.bstop
               104: R_LINX_TPCREL_LO   .L1^B10

   0000000000000106 <.L1^B10>:
    106:   0120                    addtpc  t#1 # 106 <.L1^B10>
    108:   0f32                    set a5,t#1
    10a:   2011                    setc.eqi    t#2,0

   000000000000010c <.Ltmp16.bstop>:
    10c:   0009                    const   0x0 # 0
               10c: R_RISCV_ALIGN  *ABS*+0x6
    10e:   0009                    const   0x0 # 0
    110:   0009                    const   0x0 # 0

   0000000000000112 <.Ltemp_lconst_9>:
    112:   0000                    add t#1,t#1
               112: R_RISCV_32 __register_frame_info  --> lconst 与 addtpc 组合要读取的符号
    114:   0000                    add t#1,t#1

查出来还是读取的这个 __register_frame_info 符号，但这个是弱符号，编译器对于这个符号的
处理是将地址设为 0，但现在看 0.14 对于弱符号还存在些问题，之后就是等编译器排查了。

20230218
=========

编译器将问题修复了，之前的测例可以跑通。用这个编译器跑 SpecInt 之后，大部分测例都出现了
执行逻辑紊乱，或者输出乱码。例如说，在 401 测例中的 main 函数中加上一些打印::

    int main (int argc, char *argv[]) {
        int i, level;
        int input_size=64, compressed_size;
        char *input_name="input.combined";
        unsigned char *validate_array;
        seedi = 10;

        if (argc > 1) input_name=argv[1];
        if (argc > 2) input_size=atoi(argv[2]);
        if (argc > 3)
        compressed_size=atoi(argv[3]);
        else
        compressed_size=input_size;

        printf("input name: %s \n", input_name);
        printf("input size: %d\n", input_size);

        exit(1);
        ...
    }

它的输出如下::

    input name: %s
    input size: %d
    spec_init: Error mallocing memory!spec_initFilling input fileCreating Chunkslll,,,,

                                                                                       LLLLLlll,,,,,,l,,,l,ll

                                                                                                             ,,,lll,LLlllllll,,,
              LL,,l,l,linput size: %d
    spec_init: Error mallocing memory!spec_initFilling input fileCreating Chunkslll,,,,

                                                                                       LLLLLlll,,,,,,l,,,l,ll

                                                                                                             ,,,lll,LLlllllll,,,
              LL,,l,l,l

打印内容中，input_size 后面那些都是 exit 后面的内容，却都会打印出来。同时，input_name
和 input_size 的打印都存在问题，它把未经过格式化的字符串打印了出来。其他人跑的测例也会
出现这样的问题。

401 的文件比较多，反汇编出来文件会比较大，无关的内容又比较多，不太好定位，就把出问题的部
分提取了出来，写了个测例::

    #include <stdio.h>
    #include <stdlib.h>

    int main (int argc, char *argv[])
    {
        int input_size = 64;
        char *input_name = "input.combined";
        char *format = "input name: %s \n";
        printf(format, input_name);
        input_size = atoi(argv[2]);
    }

这个测例使用 gcc glibc 编译，编译后运行情况如下::

    wenjie@kl-dev:~/test$ ~/BlockQEMU/linx-dev-performance/build/qemu-linx run_fail
    input name: %s  --> 字符串直接输出，没有进行格式化

用了其他编译器（gcc newlib 和 llvm）都不会有这个问题，将 atoi 那句注释掉后，也没有问题。
之后，将 atoi 未注释的测例与注释后的测例运行，通过 QEMU 的日志来记录两边的执行流，比较
两边的日志，发现第一个明显的差异点，下面是打印异常的执行情况::

    IN: __strchrnul
    0x0000000000102b50:  00021840 0001a000 ffe03c00 35d6dc0b std_block.concat next:0x102b30, ptr:0x388bc, ...
    0x0000000000102b60:  ffffffff ffffffff 00000000 00000c00 std_block.conditional
        0x00000000000388bc:  0c12           get             a2
        0x00000000000388be:  1112           get             a7
        0x00000000000388c0:  0480           xor             t#1, t#2
        0x00000000000388c2:  1032           set             a6, t#1
        0x00000000000388c4:  0b12           get             a1
        0x00000000000388c6:  8000           add             t#5, t#1
        0x00000000000388c8:  6400           add             t#4, t#2
        0x00000000000388ca:  3880           xor             t#2, t#7
        0x00000000000388cc:  3480           xor             t#2, t#6
        0x00000000000388ce:  0d32           set             a3, t#1
        0x00000000000388d0:  4440           and             t#3, t#2
        0x00000000000388d2:  1860           or              t#1, t#7
        0x00000000000388d4:  0f32           set             a5, t#1
        0x00000000000388d6:  0612           get             t1
        0x00000000000388d8:  4010           setc.eq         t#3, t#1

    Trace 0: 0x7f7830109680 [0000000000000000/0000000000102b50/00004200/00000200] __strchrnul
     pc       0000000000102b50
     x0/zero  0000000000000000 x1/ra    00000000000af990 x2/sp    00000040007ffcc0 x3/gp    0000000000216f68
     x4/tp    00000000002187a0 x5/t0    00000000001f2320 x6/t1    ffffffffffffffff x7/t2    0000000000000009
     x8/s0    00000000002151c8 x9/s1    0000000000008000 x10/a0   00000000001f2328 x11/a1   7fefffffffffffff
     x12/a2   616e207475706e69 x13/a3   0000000000002525 x14/a4   0000000000000025 x15/a5   0000000025252525
     x16/a6   0000000000217638 x17/a7   2525252525252525 x18/s2   fffffffffbad2084 x19/s3   0000000000000000
     x20/s4   0000000000000000 x21/s5   0000000000000000 x22/s6   0000000000000000 x23/s7   0000000000000000
     x24/s8   0000000000000000 x25/s9   0000000000000000 x26/s10  0000000000000000 x27/s11  0000000000000000
     x28/t3   0000000000000000 x29/t4   0000000000000000 x30/t5   0000000000000000 x31/t6   0000000000000000
    ----------------
    IN:
    (block body) 0x000388bc+30 in (0x000388bc-0x000388da)

    Trace 0: 0x7f7830109800 [0000000000000000/00000000000388bc/00004200/00000200]
     pc       00000000000388bc
     x0/zero  0000000000000000 x1/ra    00000000000af990 x2/sp    00000040007ffcc0 x3/gp    0000000000216f68
     x4/tp    00000000002187a0 x5/t0    00000000001f2320 x6/t1    ffffffffffffffff x7/t2    0000000000000009
     x8/s0    00000000002151c8 x9/s1    0000000000008000 x10/a0   00000000001f2328 x11/a1   7fefffffffffffff
     x12/a2   616e207475706e69 x13/a3   0000000000002525 x14/a4   0000000000000025 x15/a5   0000000025252525
     x16/a6   0000000000217638 x17/a7   2525252525252525 x18/s2   fffffffffbad2084 x19/s3   0000000000000000
     x20/s4   0000000000000000 x21/s5   0000000000000000 x22/s6   0000000000000000 x23/s7   0000000000000000
     x24/s8   0000000000000000 x25/s9   0000000000000000 x26/s10  0000000000000000 x27/s11  0000000000000000
     x28/t3   0000000000000000 x29/t4   0000000000000000 x30/t5   0000000000000000 x31/t6   0000000000000000
    Trace 0: 0x7f7830109200 [0000000000000000/0000000000102b30/00004200/00000200] __strchrnul  --> 异常测例，条件头条件成立走 bnext

以下是打印正常的执行流::

    IN: __strchrnul
    0x0000000000101470:  00021840 0001a000 ffe03c00 36e05c0b std_block.concat next:0x101450, ptr:0x38274, ...
    0x0000000000101480:  ffffffff ffffffff 00000000 00000c00 std_block.conditional
        0x0000000000038274:  0c12           get             a2
        0x0000000000038276:  1112           get             a7
        0x0000000000038278:  0480           xor             t#1, t#2
        0x000000000003827a:  1032           set             a6, t#1
        0x000000000003827c:  0b12           get             a1
        0x000000000003827e:  8000           add             t#5, t#1
        0x0000000000038280:  6400           add             t#4, t#2
        0x0000000000038282:  3880           xor             t#2, t#7
        0x0000000000038284:  3480           xor             t#2, t#6
        0x0000000000038286:  0d32           set             a3, t#1
        0x0000000000038288:  4440           and             t#3, t#2
        0x000000000003828a:  1860           or              t#1, t#7
        0x000000000003828c:  0f32           set             a5, t#1
        0x000000000003828e:  0612           get             t1
        0x0000000000038290:  4010           setc.eq         t#3, t#1

    Trace 0: 0x7ff5a0109540 [0000000000000000/0000000000101470/00004200/00000200] __strchrnul
     pc       0000000000101470
     x0/zero  0000000000000000 x1/ra    00000000000ae2b0 x2/sp    00000040007ffcc0 x3/gp    0000000000216f68
     x4/tp    00000000002187a0 x5/t0    00000000001f20c0 x6/t1    ffffffffffffffff x7/t2    0000000000000009
     x8/s0    00000000002151c8 x9/s1    0000000000008000 x10/a0   00000000001f20c8 x11/a1   1999999999999999
     x12/a2   616e207475706e69 x13/a3   0000000000002525 x14/a4   0000000000000025 x15/a5   0000000025252525
     x16/a6   0000000000217638 x17/a7   2525252525252525 x18/s2   fffffffffbad2084 x19/s3   0000000000000000
     x20/s4   0000000000000000 x21/s5   0000000000000000 x22/s6   0000000000000000 x23/s7   0000000000000000
     x24/s8   0000000000000000 x25/s9   0000000000000000 x26/s10  0000000000000000 x27/s11  0000000000000000
     x28/t3   0000000000000000 x29/t4   0000000000000000 x30/t5   0000000000000000 x31/t6   0000000000000000
    ----------------
    IN:
    (block body) 0x00038274+30 in (0x00038274-0x00038292)

    Trace 0: 0x7ff5a01096c0 [0000000000000000/0000000000038274/00004200/00000200]
     pc       0000000000038274
     x0/zero  0000000000000000 x1/ra    00000000000ae2b0 x2/sp    00000040007ffcc0 x3/gp    0000000000216f68
     x4/tp    00000000002187a0 x5/t0    00000000001f20c0 x6/t1    ffffffffffffffff x7/t2    0000000000000009
     x8/s0    00000000002151c8 x9/s1    0000000000008000 x10/a0   00000000001f20c8 x11/a1   1999999999999999
     x12/a2   616e207475706e69 x13/a3   0000000000002525 x14/a4   0000000000000025 x15/a5   0000000025252525
     x16/a6   0000000000217638 x17/a7   2525252525252525 x18/s2   fffffffffbad2084 x19/s3   0000000000000000
     x20/s4   0000000000000000 x21/s5   0000000000000000 x22/s6   0000000000000000 x23/s7   0000000000000000
     x24/s8   0000000000000000 x25/s9   0000000000000000 x26/s10  0000000000000000 x27/s11  0000000000000000
     x28/t3   0000000000000000 x29/t4   0000000000000000 x30/t5   0000000000000000 x31/t6   0000000000000000
    ----------------
    IN: __strchrnul  ===>  正常测例，上面的条件块，条件不成立，fall through
    0x0000000000101490:  00000000 00000000 eb700000 36e03c0b std_block.concat next:0x1014b0, ptr:0x38292, ...
    0x00000000001014a0:  ffffffff ffffffef 00000000 00000000 std_block.fall_through

打印异常的条件块判断成立，另一个则相反。判断条件是最后一条微指令 setc.eq 判断 t#3 和
t#1 是否相同，因为里面计算比较复杂，直接比较块的输入也行。条件块的输入是 a1、a2、a7 以
及 t1，这四个寄存器中只有 a1 寄存器的值不同。再从程序的执行流往上找，找 a1 设值的地方，
找到这一条语句::

    IN: __strchrnul
    0x0000000000102b10:  00000000 00000840 d4f01800 35d8bc0b std_block.concat next:0x102b30, ptr:0x3889a, ...
    0x0000000000102b20:  ffffffff ffffffef 00000000 00000000 std_block.fall_through
        0x000000000003889a:  012a           lw.ip           [388a8] ---> 这条语句是要在地址为 0x388a8 的地方加载数据，数据为：0x1ddefc
        0x000000000003889c:  0120           addtpc          t#1     ---> 要加载符号的地址在 0x3889c + 0x1ddefc = 0x216798
        0x000000000003889e:  0064           ld              [t#1, 0]
        0x00000000000388a0:  0b32           set             a1, t#1
        0x00000000000388a2:  ffe9           const           -1
        0x00000000000388a4:  0632           set             t1, t#1

从汇编上看，它是加载一个符号所对应内存中的数据给 a1，而这个 __strchrnul 是在库函数中，
在编译器目录下搜索了下这个符号，相关的地方只有 sysroot/usr/lib/libc.a。

通过反汇编(objdump -dr，r 用来显示重定位入口)该文件去找它要加载的符号::

    0000000000000052 <.Ltmp14.bstart>:
      52:   002a                    lw.ip   0x0 # 0
                52: R_LINX_LCONST_DATA  .Ltemp_lconst_0  --> 这个是 lconst 立即数所在位置
                52: R_RISCV_RELAX   *ABS*
                52: R_LINX_BODY_STOP    .Ltmp14.bstop
                52: R_LINX_TPCREL_LO    .L1^B1

    0000000000000054 <.L1^B1>:
      54:   0120                    addtpc  t#1 # 54 <.L1^B1>
      56:   0064                    ld  [t#1,0]
      58:   0b32                    set a1,t#1
      5a:   ffe9                    const   0xffffffffffffffff # -1
      5c:   0632                    set t1,t#1

    000000000000005e <.Ltmp14.bstop>:
      5e:   0009                    const   0x0 # 0
                5e: R_RISCV_ALIGN   *ABS*+0x6
      60:   0009                    const   0x0 # 0
      62:   0009                    const   0x0 # 0

    0000000000000064 <.Ltemp_lconst_0>:
      64:   0000                    add t#1,t#1
                64: R_RISCV_32  .LC0  --> 这个是 lconst 和 addtpc 指令组合所要加载的符号
      66:   0000                    add t#1,t#1

从上面可以看出它要加载的符号是 .LC0。

之后，再通过反汇编打印异常的可执行文件，去看看这个符号对应的值。符号所在的地址可以通过上
面日志中的的两条指令算出来，算出来的地址是 0x216798，在反汇编中(objdump -D)对应数据::

    0000000000216798 <.LC10>:

    0000000000216798 <.LC11>:
      216798:     7fefffff ffffffff     0x7fefffffffffffff

这个数据跟 a1 寄存器的值是能对的上的，也就是说 lconst 的功能是正确的，但是符号对不上，
从库文件里看，它是要加载 .LC0 符号，但这里是 .LC10 或 .LC11。正常那个测例，也按照上面
的步骤可以看到它要加载的符号是 .LC0。这个之后得编译器一起看下。

20230222
=========

符号地址计算问题链接器已经修复，编译器团队也给出了新的一版。周兵他们现在在跑，还没有测例
是跑挂的，但是有些通过 SpecInt 的比较程序，有些测例虽然能跑完，输出日志也相同，但运行的
结果确实不一样的，这种问题不好查，先创建个 issues 目录来记录问题排查日志。

现在的 glibc 合入了 memcpy/memset 优化版本(用块内跳转实现的)，QEMU 没法直接上之前的优
化，将块内跳转单独放到一个头还得 blockisa v0.16 才有，当前 0.14 还无法通过这个来判断是
否使用静态索引。

王州之前说没有跨块索引 T 寄存器的话，应该是可以直接使用静态索引，从 memcpy 和 memset 的
实现上看也没有跨块索引，直接使用静态索引方式后，出现了段错误，这个还得再分析下，但切片那
边还得跑，QEMU 还得上优化。

现在想的是在解码块头的时候，就遍历一下块体，看看是否有块内跳转指令，如果有的话，那这个块
体才会用动态索引，否则，用静态索引。

实现完后，测试了下，效果不太行。401 bzip2 原先优化后跑 3m48s，现在得要 22m16s，429 mcf
原先是 19s，现在是 1m17s。不知道是因为 memset/memcpy 占比太大，还是说重复 load 耗时多，
但重复 load 只会翻译的时候触发，也只会触发一次，应该影响不会这么大才对。

（20230223：更新下，差的没这么多，之前忘记把 goto_tb 的跨页限制的 patch 加上了。）

或许可以用个 hashmap 来把在解码块头时，读到的块体指令放到 map 中，pc 作为 key，opcode
作为 value，这样解码块体就不用再一次去 load。通过这样的方式来消除重复 load 的影响，剩下
的就是 memcpy/memset 占比对性能的影响。

这个明天过来搞一下，可以试试用 glib 的 hash table。

20230223
=========

之前在验证 0.13beta gcc 的时候，编译器给出了一个有块内跳转指令的 glibc 版本，这个 glibc
合入了用块内跳转指令实现的 memcpy 和 memset，除了这两个函数外，编译器不会主动生成块内
跳转指令。

使用这个编译器编译 429 mcf 测例，QEMU 用之前的静态索引优化运行测例，出现了段错误问题，
在编译器未使用这个版本的 glibc 之前，QEMU 静态索引没有问题。通过分析 memset 和 memcpy，
里面没有见到有跨块索引 T 寄存器的操作，按理说，是可以用 QEMU 的优化。这个得分析下，看看
QEMU 能不能修改下实现，使得可以继续使用静态索引优化。

尝试自己构建了 memset 的测例，但是没有复现问题，程序正常运行。目前似乎只能通过 SpecInt
测例里来复现问题。可以快速复现的测例是 429 测例，运行起来后，几秒钟即可复现。

测例可以从 56 机器上 /home/wenjie/ca_spec_test/spec06_Qemu_Test/429.mcf/build 下获
取，源码、测例输入文件以及编译脚本都在 build 下面。编译的命令为::

    make -f linx.mk clean && make -f linx.mk -j

运行的命令如下::

    qemu-linx -enable-force-tb-chained mcf_linx inp.in

编译器的版本是 20230208_B004，QEMU 的分支是 linx-trace-v0_13-beta。
memset 和 memcpy 的实现在 codehub 的 glibc-linx，分支是 feature-support-linx-target，
这两个的实现都在 sysdeps/linx 下面，分别是 memset.S 和 memcpy.S。

周兵在跑切片的时候，提了个需求，看看 QEMU 这边能不能只打印进入函数时寄存器状态，而不是
每个 TB 块执行都打印一次，通过这样的方式来减少日志的输出。一开始，李飘和志林所想的是当块
为 call 块或 direct 块时，将寄存器输出，但是 direct 块中只有部分是属于函数调用，这样子
日志中会有很多不相关的内容。并且周兵他们在使用这个功能的时候，日志中也会将库函数的状态显
示出来，他们只想先关注测例本身函数执行流。

因为之前在排查 SpecInt 测例时，经常是自己手动插桩，搞的很烦。在网上搜了下，gcc 和 llvm
都支持一种自动插桩的功能，-finstrument-functions，细节可以看 gcc 文档中的 Program
Instrumentation Options 章节。

这个选项开启后，gcc 会在每个函数进入的位置和退出的位置加上桩函数，每次函数被调用的时候，
就会调用 `__cyg_profile_func_enter`，每次函数退出的时候，就会调用
`__cyg_profile_func_exit`。这两个函数的原型如下::

    void __cyg_profile_func_enter (void *this_fn, void *call_site);
    void __cyg_profile_func_exit  (void *this_fn, void *call_site);

this_fn 是被调函数的函数指针，call_site 即被调函数的返回地址，也就是主调函数。

用了下这个方法，在 enter 函数中加了个 linx_debug 指令，只是用来输出函数的名字，也就是
this_fn 在符号表中对应的复活，这样，原先只要 22s(user：21s，sys：1s) 的程序，现在都需
要 7min49s(user：3m17s，sys：4m31s)，看起来打印耗时要大点，user 的时间也差的挺大的，
感觉最主要还是函数返回是间接跳转，QEMU 没法优化，但这个函数还不能设置内联。这差的有 21
倍了，没得搞。

在 gcc 文档里看到了 -finstrument-functions-once 参数，让每个函数中的桩函数只会调用一
次，但现在 blockisa 的编译器，不管是 gcc 还是 llvm 都不支持，这个插桩怕是得搁置了。

静态索引不支持跨块索引的没看出来啥，感觉晚上状态不对，先溜了。

20230227
=========

之前的 memcpy 和 memset 分析出了一点问题，它里面是有跨跳转指令索引 T 寄存器的操作，一
开始以为这两个实现只要不存在循环，或有循环，但是这个循环的输入来自于 scratch 寄存器，那
它 T 寄存器索引也不会有问题，但对于下面这样的测例，t 寄存器的索引就会出现问题::

    get a0
    bci t#1, 1f

    addi t#2, 1
    const -4
    jr t#1

    1:
    get a1
    bci t#1, 1f

    addi t#2, 1
    const -4
    jr t#1

因为有块内跳转指令，QEMU 会以块内跳转指令为分界线，将上面的代码翻译出四个 TB 块，按顺序
分别叫 TB1、TB2、TB3 以及 TB4。假设有这么一种情况，第一次进入这块代码时，a0 为 0，a1
为 1，生成出来的 TB 大概如下::

    TB1:
    T0 = get a0
    T1 = bci T0, 1f
    IBPC += 2

    TB2:
    T2 = addi T0, 1
    T3 = const -4
    T4 = jr T3
    IBPC += 3

    TB3:
    T5 = get a1
    T6 = bci T5, 1f
    IBPC += 2

    TB4:
    不翻译

当再次进入这块代码段，假设 ibpc 还是从 0 开始，a0 为 1，a1 为 0，它的执行顺序是::

    TB1->TB3->TB4

当要执行 TB4 时，因为之前未执行到这个块，所以先翻译，翻译会依据之前 TB 执行的 ibpc 翻
译，生成出来的 TB 大概如下::

    TB1(已翻译):
    T0 = get a0
    T1 = bci T0, 1f
    IBPC += 2

    TB2(已翻译):
    T2 = addi T0, 1
    T3 = const -4
    T4 = jr T3
    IBPC += 3

    TB3(已翻译):
    T5 = get a1
    T6 = bci T5, 1f
    IBPC += 2

    TB4: IBPC = 4
    T4 = addi T2, 1
    T5 = const -4
    T6 = jr T5

可以看出来，TB4 中的微指令使用了错误的 T 寄存器，当这四个 TB 是顺序翻译时，也就是 a0 和
a1 都为 0 时，每个 TB 中的微指令都可以索引到正确的 T 寄存器，导致出现这种情况的原因主要
还是在于 ibpc 的实现有误，当前实现的 ibpc 糅杂在了翻译和执行两个阶段。

当使用静态索引时，ibpc 代表的是微指令在块体中的位置，这时候，ibpc 应该是一个只在翻译阶
段使用的变量；当使用动态索引时，ibpc 代表的是当前块体所执行的微指令数量，那它就是一个只
在执行阶段使用的变量。得益于微指令的长度统一为 2 字节，每个 TB 的起始 ibpc 可以通过 TB
中第一条微指令的 tpc 与块体第一条微指令的 tpc 计算出来。

修改后，还是存在问题，明日再看。


综合分析
============

.. _qemu_linux_startup:

QEMU 中 Linux 的启动
----------------------

BIOS 与 kernel 加载到内存
^^^^^^^^^^^^^^^^^^^^^^^^^^^

QEMU 这边的 boot 似乎于我了解的不一样。virt machine 里定义的 rom 在地址空间起始地址
为 0x1000，QEMU 的一级 BIOS 是烧写在 ROM 中的，相应烧写操作的函数栈如下::

    +-> virt_machine_init
      +-> riscv_setup_rom_reset_vec

``riscv_setup_rom_reset_vec`` 设置了 CPU reset 到 0x1000 时，如何去调用 firmware
的 BIOS。virt 的 reset vector 就使用的由宏 ``DEFAULT_RSTVEC`` 定义的默认值，其值为
0x1000。(sifive_u 用的是 0x1004)

这里有个问题，还没搞懂，先 mark 以下：

**todo**::

    CPURISCVState 里定义了 resetvec 为默认值 0x1000。cpu reset 时，应该调用的是
    ``riscv_cpu_reset``，这里也是把 pc 设为 resetvec。
    但是 sifive_u 在其 DeviceState 设置了个属性 resetvec 为 0x1004：
    
        qdev_prop_set_uint64(DEVICE(&s->e_cpus), "resetvec", 0x1004);
        qdev_prop_set_uint64(DEVICE(&s->u_cpus), "resetvec", 0x1004);

    那么当 cpu reset 后，用的是哪个？

``riscv_setup_rom_reset_vec`` 定义的 BIOS 如下:

.. code-block:: C

    /* reset vector */
    uint32_t reset_vec[10] = {
        0x00000297,                  /* 1:  auipc  t0, %pcrel_hi(fw_dyn) */
        0x02828613,                  /*     addi   a2, t0, %pcrel_lo(1b) */
        0xf1402573,                  /*     csrr   a0, mhartid  */
        0,
        0,
        0x00028067,                  /*     jr     t0 */
        start_addr,                  /* start: .dword */
        start_addr_hi32,
        fdt_load_addr,               /* fdt_laddr: .dword */
        0x00000000,
                                     /* fw_dyn: */
    };
    if (riscv_is_32bit(harts)) {
    } else {
        reset_vec[3] = 0x0202b583;   /*     ld     a1, 32(t0) load fdt_load_addr */
        reset_vec[4] = 0x0182b283;   /*     ld     t0, 24(t0) 这里 load start_addr */
    }

start_addr 是 DROM 的起始地址 0x80000000，二级 BIOS ``-bios`` 就是加载在这个地方，
默认使用的是 QEMU 自带的 opensbi。然后就是 kernel 了，QEMU 直接就把 kernel 加载到
内存了，按理说，不是二级 BIOS 把 kernel 从磁盘 0 盘 0 道 1 分区给它加载到内存的？

OpenSBI
^^^^^^^^^^^^^^^^^^^^^^^^^^^

待看。


Kernel（Linux 启动）
^^^^^^^^^^^^^^^^^^^^^^^^^^^

这里就是我们现在在弄的部分。

kernel 的入口在 head.S 中的 _start。_start 里一开始就跳到 _start_kernel。我们从
这开始看起。

这里我们先只看单核的，那么 Kconfig 里的 SMP 也就为 no，相应的 RISCV_BOOT_SPINWAIT
等一下多核相关的宏也就不会定义（也就是 depends on SMP 的一些配置。）其他，我们按默认情
况来说，MMU 默认是 yes，RISCV_M_MODE 的默认值因为是 ``default !MMU``，所以为 no。
XIP 的我们暂时也不看。

好嘞，该进入正戏了。先将剪切后的代码段贴上来::

    ENTRY(_start_kernel)
        /* Mask all interrupts */
        csrw CSR_IE, zero
        csrw CSR_IP, zero

    #ifdef CONFIG_RISCV_M_MODE
        ...
    #endif /* CONFIG_RISCV_M_MODE */

        /* Load the global pointer */
    .option push
    .option norelax
        la gp, __global_pointer$
    .option pop

        /*
         * Disable FPU to detect illegal usage of
         * floating point in kernel space
         */
        li t0, SR_FS
        csrc CSR_STATUS, t0

    #ifdef CONFIG_RISCV_BOOT_SPINWAIT
        ...
    #endif /* CONFIG_XIP */
    #endif /* CONFIG_RISCV_BOOT_SPINWAIT */

    #ifdef CONFIG_XIP_KERNEL
        ...
    #endif

    #ifndef CONFIG_XIP_KERNEL
        /* Clear BSS for flat non-ELF images */
        la a3, __bss_start
        la a4, __bss_stop
        ble a4, a3, clear_bss_done
    clear_bss:
        REG_S zero, (a3)
        add a3, a3, RISCV_SZPTR
        blt a3, a4, clear_bss
    clear_bss_done:
    #endif
        /* Save hart ID and DTB physical address */
        mv s0, a0
        mv s1, a1

        la a2, boot_cpu_hartid
        XIP_FIXUP_OFFSET a2
        REG_S a0, (a2)

        /* Initialize page tables and relocate to virtual addresses */
        la sp, init_thread_union + THREAD_SIZE
        XIP_FIXUP_OFFSET sp
    #ifdef CONFIG_BUILTIN_DTB
        ...
    #else
        mv a0, s1
    #endif /* CONFIG_BUILTIN_DTB */
        call setup_vm
    #ifdef CONFIG_MMU
        la a0, early_pg_dir
        XIP_FIXUP_OFFSET a0
        call relocate_enable_mmu
    #endif /* CONFIG_MMU */

        call setup_trap_vector
        /* Restore C environment */
        la tp, init_task
        la sp, init_thread_union + THREAD_SIZE

    #ifdef CONFIG_KASAN
        ...
    #endif
        /* Start the kernel */
        call soc_early_init
        tail start_kernel

    #if CONFIG_RISCV_BOOT_SPINWAIT
        ...
    #endif /* CONFIG_RISCV_BOOT_SPINWAIT */

    END(_start_kernel)

上面这些代码主要做了一下事情：

1. 失能所有中断，也就是让 CPU 不响应中断。这个时候都没有设置 mtvec 寄存器
   （设置异常向量基地址的）。话说，opensbi 里也会失能中断不？

2. 初始化 gp 寄存器。

3. 


.. _tcg_goto_tb:

TCG GOTO_TB 分析
----------------------

代码分析：

前端（生成中间码），先检查 tb 的 cflags 有没有 CF_NO_GOTO_TB 标志，如果有将会报错。
goto_tb 只支持两个跳转地址，一般用于分支跳转的 taken 和 not。之后生成一个
INDEX_op_goto_tb 的中间码。

.. code-block:: C

    void tcg_gen_goto_tb(unsigned idx)
    {
        /* We tested CF_NO_GOTO_TB in translator_use_goto_tb. */
        tcg_debug_assert(!(tcg_ctx->tb_cflags & CF_NO_GOTO_TB));
        /* We only support two chained exits.  */
        tcg_debug_assert(idx <= TB_EXIT_IDXMAX);
    #ifdef CONFIG_DEBUG_TCG
        ...
    #endif
        plugin_gen_disable_mem_helpers();
        tcg_gen_op1i(INDEX_op_goto_tb, idx);
    }

后端（生成 host 代码），这里以 riscv 的来看。

.. code-block:: C

    static void tcg_out_op(TCGContext *s, TCGOpcode opc,
                           const TCGArg args[TCG_MAX_OP_ARGS],
                           const int const_args[TCG_MAX_OP_ARGS])
    {
        TCGArg a0 = args[0];
        TCGArg a1 = args[1];
        TCGArg a2 = args[2];
        int c2 = const_args[2];

        switch (opc) {
        case INDEX_op_goto_tb:
            assert(s->tb_jmp_insn_offset == 0);
            /* indirect jump method */
            tcg_out_ld(s, TCG_TYPE_PTR, TCG_REG_TMP0, TCG_REG_ZERO,
                       (uintptr_t)(s->tb_jmp_target_addr + a0));
            tcg_out_opc_imm(s, OPC_JALR, TCG_REG_ZERO, TCG_REG_TMP0, 0);
            set_jmp_reset_offset(s, a0);
            break;
        }
    }

tb_jmp_target_addr 在 tb_gen_code 中对其赋值。

QEMU 有对 goto_tb 和 goto_ptr 有进行封装，名叫：``translator_use_goto_tb``。
这样更安全些。首先，要用 goto_tb 需要 TB 没有 CF_NO_GOTO_TB 这个标志，否则，
会报错。其次，要求目标跳转地址要与当前 TB 处在同一个页里。

.. code-block:: C

    bool translator_use_goto_tb(DisasContextBase *db, target_ulong dest)
    {
        /* Suppress goto_tb if requested. */
        if (tb_cflags(db->tb) & CF_NO_GOTO_TB) {
            return false;
        }

        /* Check for the dest on the same page as the start of the TB.  */
        return ((db->pc_first ^ dest) & TARGET_PAGE_MASK) == 0;
    }


