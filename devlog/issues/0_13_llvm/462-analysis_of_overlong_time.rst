462 测例执行时间过长分析
************************

介绍
====
这个Issue 2023-2-14创建，目标是解决462.libquantum这个测例的性能明显比RISCV低的问题。

当前问题Owner：韩志林（h30043474）

qemu性能分析
============

qemu BlockISA仿真执行462.libquantum这个测例的用时是ARM的9倍。BlockISA的仿真时间
是99.2s，ARM的仿真时间是10.8s。我们在10.145.46.56机器上验证一下qemu的仿真速度
（这里和RISCV比较），下面是分析所用的环境等内容。::

    测例来源：/home/wenjie/ca_spec_test/spec06_Qemu_Test/
    测例路径：/home/hanzhilin/code/0218/462.libquantum/build
    编译器路径：/home/lipiao/workspace/perfor/linx_llvm_0208/bin/clang
    QEMU分支：linx-trace-force-tb-chained
    QEMU改动：blk_trap 用户模式直接 "return true;"
    QEMU当前分支已做的优化点（-enable-force-tb-chained）：

        1. 静态索引（块类型是非jump类型则静态索引T寄存器）、关掩码检查
        2. 块头到块身的chain，关掉recovery检查
        3. 块head到下个块head的chain（块body为空）
        4. 块body到下个块head的chain
        5. helper_handle_exec_and_branch 改成中间码的实现

统一记录一下下面数据的使用命令：

    1. time ~/code/LinxBlockModel/build/qemu-linx -enable-force-tb-chained 462_llvm 219 7
       time ~/code/LinxBlockModel/build/qemu-linx -d in_asm -D 462_llvm.log -enable-force-tb-chained 462_llvm 219 7

    2. time ~/code/LinxBlockModel/build/qemu-linx -enable-force-tb-chained -d plugin -D plugin.log -plugin ../libinsnCount.so 462_llvm 219 7
    3. time ~/code/LinxBlockModel/build/qemu-riscv64 libquantum_rv 219 7
    4. perf record -e cpu-clock ~/code/LinxBlockModel/build/qemu-linx -enable-force-tb-chained
       462_llvm 219 7

    5. perf report -i perf.data
    6. grep -nE "atomic.relaxed|acquire|release" 462_llvm.asm | wc -l （统计原子块个数）
    7. grep -n "atomic" 462_llvm.asfim | wc -l （统计块头个数）
    8. grep -nE "jr|bcond" 462.asm | wc -l
    9. /home/wenjie/block_toolchain/linx64-linux-gnu-20230118-B003/bin/linx64-linux-gnu-objdump -D 462_llvm >> 462_llvm.asm
       （用gcc的objdump即可，llvm的不能用）

462执行时间linx(即BlockISA)和RISCV的时间、TB exec对比：::

    riscv:  0m13.561s
    linx:   1m42.020s
    riscv:  TB exec = 29721415, trans = 1780
    linx:   TB exec = 138202142, trans = 9489
    倍数:   TB exec: 4.75倍，trans: 5.3倍

linx的仿真时间是RISCV的7.5倍，查看一下perf数据：::

   2.64%  qemu-linx  qemu-linx                [.] helper_lookup_tb_ptr
   2.59%  qemu-linx  qemu-linx                [.] cpu_get_tb_cpu_state
   2.40%  qemu-linx  qemu-linx                [.] tb_lookup
   1.45%  qemu-linx  qemu-linx                [.] deposit32
   0.87%  qemu-linx  qemu-linx                [.] check_for_breakpoints
   0.69%  qemu-linx  qemu-linx                [.] curr_cflags
   0.67%  qemu-linx  qemu-linx                [.] env_archcpu
   0.59%  qemu-linx  [JIT] tid 2028102        [.] 0x00007fb7942158ea
   0.57%  qemu-linx  [JIT] tid 2028102        [.] 0x00007fb794215ef6
   0.55%  qemu-linx  [JIT] tid 2028102        [.] 0x00007fb794215a6a
   0.54%  qemu-linx  [JIT] tid 2028102        [.] 0x00007fb794215caa
   0.54%  qemu-linx  [JIT] tid 2028102        [.] 0x00007fb794215912
   0.54%  qemu-linx  qemu-linx                [.] qemu_loglevel_mask
   0.52%  qemu-linx  [JIT] tid 2028102        [.] 0x00007fb794215a63
   0.51%  qemu-linx  [JIT] tid 2028102        [.] 0x00007fb794215e48

能入手的是 helper_lookup_tb_ptr 这个函数。cpu_get_tb_cpu_state、tb_lookup、
check_for_breakpoints、curr_cflags这些函数都会在helper_lookup_tb_ptr这里面被调用
（当然也会在其他函数内调用，但这样的热点函数的之间的关系特点也有一定的参考价值），
helper_lookup_tb_ptr 调用的地方有下面几种情况：

1. gen_goto_tb 在 tb_cflags 无 CF_NO_GOTO_TB 这个标志的情况下，会改成走
   lookup_tb_ptr 这条线。那么什么情况下有这个 CF_NO_GOTO_TB 呢，一般是打断点、
   单步执行、原子指令执行的情况下。打断点、单步执行我们的测试过程中不涉及，
   那就剩原子指令（原子块）的情况了。这里hack了一下原子块的改动，不置位
   CF_NO_GOTO_TB和CF_NO_GOTO_PTR发现基本无优化。另外，统计了一下反汇编文件
   462.asm中原子块的占比，发现占比也不高（301/55398）
2. 执行块头时，若body为空，且branch_type是 INDIRECT_LINK, INDIRECT_CALL, RET
   的时候，因为要跳转的地址是动态的，所以需要用lookup的方式，也无法优化。
3. 和第2点同理，在块提交的时候，branch_type 是 INDIRECT_LINK, INDIRECT_CALL,
   RET的时候，也要用lookup的方式，无法优化。
4. blk_jr, blk_bcond 这两条块内跳转的指令，正常情况下是一定会跳转的(执行lookup)，
   不然就会产生一个异常。因为是否跳转也是动态的，是不确定的，所以也无法优化。
   那么我们也可以统计一下这种情况的占比。首先统计了下462测例的静态汇编数据，
   这两条微指令有4992条，那么下一步可以统计一下这两条指令的执行次数(动态汇编数据?)，
   gdb 调试过程中，在 trans_blk_jr, trans_blk_bcond 这2个函数打断点，发现1条
   也未执行，说明426这个测例这两条指令是不会翻译，也不会执行到的，不是优化点所在。

这么分析下来，现有的qemu已经把能优化的点都已经优化了，发现并没有什么其他可以优化
的地方了。这里再确认一下原有的 gen_goto_tb 是否真的使用上了，或者说确认一下
helper_lookup_tb_ptr 的占比。通过在解码块头的函数中添加一个 helper 打印的中间码，
看看有关 INDIRECT_LINK(4)、INDIRECT_CALL(5)、RET(6)的占比有多少，结果如下：::

    kenny: TB exec = 138202147, trans = 9495
    lipiao: FALL_THROUGH branch:        3639094908
    lipiao: DIRECT_LINK branch:         116241665
    lipiao: CALL branch:                104967630
    lipiao: CONDITIONAL branch:         5714191076
    lipiao: INDIRECT_LINK branch:       33224107 (0.34)
    lipiao: INDIRECT_CALL branch:       52
    lipiao: RET branch:                 104967678 (1.08%)
    lipiao: CONCAT branch:              0
    lipiao: all branch:                 9712687116

发现总的TB exec的执行次数确实基本上是块类型为3，4，5（需要helper_lookup_tb_ptr）
的执行次数之和，也就是说，其他块类型我们基本上都已经chain了。
那么是不是说elf中的指令数量本身就比较高，确切的讲执行过程中我们执行的指令数本身
就比较多。（另外，我们直观地认为由于我们能chain的地方都chain了，是不是说每个TB块
指令数是不是比RISCV的要多？）我们来分析462，429，473这3个测例（选这几个测例是因
为这几个测例执行时间相对较少）的 "TB exec" 数和执行时间的关系；另外，也借助 mapeng
的 libinsnCount.so 插件统计指令数: ::

    462-riscv:  TB exec = 29721415, trans = 1780
    462-linx:   TB exec = 138202142(4.65倍), trans = 9489
    462-riscv:  0m13.561s
    462-linx:   1m42.020s（7.5倍）
    462-riscv:  inst = 24374910818,block = 0,tb = 4870529768
    462-linx:   inst = 80055713775(3.28倍),block = 10897845047,tb = 19650057050(4.03倍)

    429-riscv:  TB exec = 172849617, trans = 1989
    429-linx:   TB exec = 5940985(0.03倍), trans = 8209
    429-riscv:  0m25.782s
    429-linx:   0m13.904s（0.54倍）
    429-riscv:  inst = 7868527025,block = 0,tb = 1111976554
    429-linx:   inst = 59564576429(7.5倍),block = 6547109626,tb = 11869828935(10.67倍)

    473-riscv   TB exec = 268181931, trans = 2945
    473-linx:   TB exec = 454447984(1.69倍), trans = 12492
    473-riscv:  0m42.693s
    473-linx:   1m50.708s（2.6倍）
    473-riscv:  inst = 22171903443,block = 0,tb = 3883751833
    473-linx:   inst = 59564576429(2.69倍),block = 6547109626,tb = 11869828935(3.06倍)

根据这3个测例发现，qemu执行时间和指令数 inst 的多少并无直接关系。

这里有个疑问，429测例根据我自己的测试操作，发现执行时间比RISCV的还要少，不确定是
不是我的操作有问题，至少根据log输出以及TB执行数和执行时间的关系，可以认为实验数
据是有效的。

和RISCV比较，可以直观的发现，TB exec 数越高，所对应的执行时间越高。
INDIRECT_LINK(4)、INDIRECT_CALL(5)、RET(6)这3种需要用 helper_lookup_tb_ptr 这种
方式实现的块头执行数越高，所对应的 TB exec 数也就越高，所对应的执行时间也就越高。
或许对于462这个测例而言，执行过程中我们执行的指令数比RISCV的要多很多。

从exec的数据来看，这个测例的CALL branch的执行次数比较高，导致RET branch的值较高。
那么我们接下来可以看看为什么 CALL branch 的块执行数较多。

在高级语言的角度来看，BlockISA和RISCV的函数调用次数应该是一样多的，但是由于指令
级的限制（比如编译器不编译出浮点型指令），在汇编层面的函数调用次数在各个架构或者
不同的编译选项的时候是有区别的。接下来继续统计汇编层面的实际函数调用次数。

.. note::

   这个问题不影响切片工作，这个问题的分析不再以优化qemu执行速度为目的，可以
   确定一下这个问题的具体原因。我们先继续往下分析。

riscv call/ret的执行次数统计
----------------------------

riscv编程手册(riscv-card.pdf)中表示call实现是通过2条其他指令实现的，做的操作就是
跳转到 pc+offset[31:0] 的地方，然后ra寄存器赋值为 当前pc+4，以便函数返回；ret的
实现是通过jalr实现的：::

    call：
    auipc x1, offset[31:12]     #x[ra]=pc+sext(imm[31:12]<<12)
    jalr x1, x1, offset[11:0]   #t=pc+4; pc=(x[ra]+sext(imm[11:0]))&~1; x[ra]=t

    ret：
    jalr x0, x1, 0  #pc=x[ra]

在462测例中查看 main 函数中的函数调用，发现都是通过"jal ra, imm" 指令实现call功
能的，没有用jalr指令实现的情况；ret用objdump出的伪指令是"ret"，编码是"0x8082"，
"0x8082"的实际指令是"jalr"的其中的一个压缩指令 C.JR(rd=0,rs=ra)，在qemu中的翻译
流程中走的是 trans_jalr() 这个路径。在对应的指令翻译过程添加helper函数统计
"call", "ret" 指令的执行次数(通过rd,rs的值来判断是否为call,ret)，统计结果如下：::

    462-riscv:  rv_call = 14873995, rv_ret = 14874043

linx 的RET branch 执行次数有 104967678(是rv_ret的7.06倍)

接下来分析各个函数的调用次数统计，得到的数据如下：

462-Linx::

    145 func call
    0x00000000000afbc0, count: 16225304,  __muldf3
    0x00000000000994e0, count: 14718482,  quantum_objcode_put
    0x00000000000b7a60, count: 13189224,  __mulsf3
    0x00000000000abda0, count: 12491519,  __adddf3
    0x00000000000bc280, count: 7693664,  __clzdi2
    0x00000000000b0f40, count: 7513428,  __subdf3
    0x00000000000b40a0, count: 6060745,  __addsf3
    0x00000000000ba880, count: 4442740,  __unordsf2
    0x00000000000bb380, count: 3050396,  __extendsfdf2
    0x00000000000b5be0, count: 2495849,  __divsf3
    0x00000000000aed60, count: 2495842,  __nedf2
    0x00000000000b8c40, count: 2221373,  __subsf3
    0x00000000000af080, count: 1507413,  __gtdf2
    0x00000000000ad900, count: 1313675,  __divdf3
    0x00000000000bb960, count: 1248315,  __truncdfsf2
    0x00000000000b2ba0, count: 1247923,  __unorddf2
    0x00000000000a96c0, count: 1247921,  __feholdexcept
    0x00000000000a9640, count: 1247921,  __fesetround
    0x00000000000a5c80, count: 1247921,  sqrtf64
    0x00000000000a9600, count: 1247921,  __fegetround
    0x00000000000a9740, count: 1247921,  fesetenv
    0x00000000000b7480, count: 349524,  __ltsf2
    0x00000000000af620, count: 284326,  __ltdf2
    0x000000000009abc0, count: 38462,  quantum_qec_get_status
    0x0000000000096b40, count: 38390,  quantum_gate_counter
    0x000000000008fbf0, count: 38238,  quantum_decohere
    0x00000000000905a0, count: 22058,  quantum_toffoli
    ...

462-RISCV::

    125 func call
    0x0000000000013dfa, count: 14718482,  quantum_objcode_put
    0x0000000000014cd8, count: 38462,  quantum_qec_get_status
    0x0000000000012d46, count: 38390,  quantum_gate_counter
    0x0000000000010b24, count: 38238,  quantum_decohere
    0x0000000000010ed8, count: 23029,  quantum_toffoli
    0x00000000000110d0, count: 8924,  quantum_sigma_x
    0x0000000000010e1a, count: 5373,  quantum_cnot
    0x0000000000027d20, count: 254,  memcpy
    0x0000000000013758, count: 224,  muxha_inv
    0x0000000000013816, count: 224,  madd
    0x00000000000112f4, count: 224,  quantum_swaptheleads
    0x0000000000013bc8, count: 224,  add_mod_n
    0x000000000001398e, count: 224,  madd_inv
    0x000000000001326e, count: 224,  test_sum
    0x0000000000012d66, count: 163,  quantum_memman
    0x0000000000028138, count: 157,  __strchrnul
    0x0000000000012a88, count: 120,  quantum_cond_phase
    0x0000000000010ad2, count: 120,  quantum_cexp
    0x0000000000015f40, count: 120,  __sincos
    ...

发现Linx调用了很多次浮点数操作的函数，查看riscv编译出的可执行文件，里面确实存在
浮点指令。编译器那边建议如果要比较Linx和RISCV的执行速度的话，可以换个编译选项
（--with-arch=riscv64im），这样两者的比较才能对等。换了编译选项之后，重新执行，
得到的数据如下：

462-riscv64im::

    kenny: TB exec = 201059158, trans = 2789
    lipiao: count_call = 96640620, count_ret = 96640670
    155 func call in
    0x000000000001c3e0, count: 16225304,  __muldf3
    0x00000000000160f8, count: 14718482,  quantum_objcode_put
    0x000000000001b8b8, count: 12491478,  __adddf3
    0x000000000001da80, count: 10807996,  __mulsf3
    0x000000000001e6b4, count: 7693575,  __clzdi2
    0x000000000001c838, count: 7513339,  __subdf3
    0x000000000001cffc, count: 4870131,  __addsf3
    0x000000000001e494, count: 3107703,  __extendsfdf2
    0x000000000001d4bc, count: 2495850,  __divsf3
    0x000000000001c1c8, count: 2495842,  __eqdf2
    0x000000000001c23c, count: 1471203,  __gtdf2
    0x000000000001bd94, count: 1313677,  __divdf3
    0x000000000001e554, count: 1248316,  __truncdfsf2
    0x000000000001cd24, count: 1247923,  __unorddf2
    0x000000000001adfc, count: 1247921,  fegetround
    0x0000000000019fd0, count: 1247921,  sqrtf32x
    0x000000000001ae14, count: 1247921,  __fesetenv
    0x000000000001ae04, count: 1247921,  fesetround
    0x000000000001ae0c, count: 1247921,  __feholdexcept
    0x000000000001de38, count: 973452,  __subsf3
    0x000000000001e314, count: 973449,  __unordsf2
    0x000000000001d8bc, count: 349524,  __gtsf2
    0x000000000001c30c, count: 248110,  __ledf2
    0x00000000000174f8, count: 38462,  quantum_qec_get_status
    0x0000000000014714, count: 38390,  quantum_gate_counter
    0x0000000000010ed8, count: 38238,  quantum_decohere
    0x0000000000011400, count: 23029,  quantum_toffoli
    ...

去掉这些debug的调试代码之后（不然会影响执行速度），得到的riscv的462测例执行数据
如下：

    kenny: TB exec = 201059168, trans = 2467
    real    0m30.996s
    user    0m30.989s
    sys     0m0.004s

462测例所有数据对比整理如下：

    462-riscv:  TB exec = 201059168, trans = 2467
    462-linx:   TB exec = 138202142(0.69倍), trans = 9489
    462-riscv:  0m30.996s
    462-linx:   1m42.020s（3.29倍）
    462-riscv   rv_call = 96640620, rv_ret = 96640670
    462-linx:   CALL Branch = 104967678（1.09倍）

执行时间RISCV和Linx对比为 1:3.29，在可接受范围内，该问题分析可关闭。后续的SpecInt
的编译测试将会按照SpecInt的标准流程执行，对比试验后续也会规范对比。

