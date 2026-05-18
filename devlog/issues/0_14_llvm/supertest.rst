0.14 supertest 测例分析
*************************

介绍
=====

本篇主要是记录并分析 supertest 用 QEMU 运行时产生的错误，以供后来者了解情况。

记录人：贾文杰

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
问题了。直接编 lconst 那个汇编测例，用 QEMU 跑，再看看取的值对不对，来验证 QEMU 的
lconst 功能实现有没有问题。编译的命令是这样的::

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
置，然后再计算 lconst 指令的偏移。修改测例后，重新跑下，看日志里，它取值跟预期是一样的，
验证出来 lconst 功能实现没有问题。

昨天那个符号 __register_frame_info 是一个弱符号，该符号不存在也不会报错。那这样看，也
没有问题了。接下来就是比较下 0.13beta 以及 0.14 同个程序的执行流，出现差异的地方是在一
个叫 frame_dummy 的函数，它的样子是这样的::

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
