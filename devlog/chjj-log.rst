..  版权所有 2021

:Authors: chenjuanjuan
:Version: 1.0


陈娟娟的开发日志
******************

2021/12/17
==========

开发策略：

        1. 先大家一起实现cpu-block结构设计，实现基础指令：bstart、get、ld、st、add、const
        然后分工实现后续的其他30几条指令。

        2. bstart 从16bit的RV中找到一个不冲突的编码
                100 1 11 ... 10 ... 01 保留的编码

        3. 李飘进行分工设计


2021/12/20
==========

1. 今天测试了几个小程序，可以set、get、load、const、add正确执行，store指令还有
   些问题。pass的测试程序在linxtest下的三个test1/test2/load运行

        qemu-linx -singlestep -d in_asm,cpu,nochain,guest_errors,int -D run.log test1/test后，

   能在run.log中看到全局寄存器和block寄存器的值变化

2. block_cpu的设计思路如下（目前只考虑一个cpu一个block，不支持多个block），block的T寄存器与全局cpu位置一样
        1. 在struct CPURISCVState 结构体中增加：blk_cpu_t表示T寄存器和
           blk_cpu_alloc表示每次新使用T寄存器的下标。:: 

            target_ulong blk_cpu_t[MAX_BLK_T_NUM];
	        target_ulong blk_cpu_alloc;

        2. t寄存器使用，模仿gen_set_gpr,dest_gpr函数，设计以下三个函数::

                static TCGv get_blk_dest_reg_t(DisasContext *ctx)
                static TCGv get_blk_reg_t(DisasContext *ctx, int reg_num)
                static void gen_set_blk_reg_t(DisasContext *ctx, TCGv value)

	3. 在riscv_cpu_dump_state增加t寄存器的打印::

                for (i = 0; i<8; i++){
                        qemu_fprintf(f, " %-8s " TARGET_FMT_lx, riscv_blk_regnames[i], env->blk_cpu_t[i]);
                        if ((i & 3) == 3) {
                                qemu_fprintf(f, "\n");
                        }
                }

	4. 如此：set和get、加法可以简单的实现如下：:: 

                static bool gen_blk_get(DisasContext*ctx, arg_blk_get*a)
                {
                        TCGv result=get_blk_dest_reg_t(ctx);

                        tcg_gen_mov_tl(result, cpu_gpr[a->gpr]);
                        return true;
                }

                static bool gen_blk_set(DisasContext*ctx, arg_blk_set*a)
                {
                        TCGv src1;
                        src1 = get_blk_reg_t(ctx, a->link0);
                        tcg_gen_mov_tl(cpu_gpr[a->gpr],src1);
                        return true;
                }

                static bool gen_blk_add(DisasContext*ctx, arg_blk_add*a, void(*func)(TCGv,TCGv, TCGv))
                {
                        TCGv src1, src2;
                        src1 = get_blk_reg_t(ctx, a->link0);
                        src2 = get_blk_reg_t(ctx, a->link1);

                        (*func)(src1, src1, src2);
                        gen_set_blk_reg_t(ctx, src1);
                        return true;
                }

2021/12/22
==========
1. 根据评审意见修改代码-上传到linx-dev-chjj-pr2

               a)名字定义
               b)删除不必要的函数调用
               c)删除文件translate_block.c

2. 讨论bstart和设计cpu状态

     1)看到设计databuf和addrbuff想到block中不能支持循环
     2)跳转中由于imm的size只有7bit，超出跳转范围的需要liac帮助

3. 测试方案初始思考

        1.启动qemu-system-linx
        2.运行测试程序脚本，显示pass了多少个指令，failed多少个指令
        3.测试用例：::

           base用例：
           用例1：base: set/get/const
           用例2：两个const拼接
           用例4：ADD
           用例5：SUB
           用例6：AND
           用例7：OR
           用例8：NOT
           用例9：XOR
           用例10：SRL
           用例11：SRA
           用例12：DIV
           用例13：DIVU
           用例14：REM
           用例15：REMU
           用例16：MUL
           用例17：SLL
           load测试用例
           store测试用例
           跳转测试用例
           cmp测试用例
           函数调用跳转和ret测试用例
           复杂的测试用例：copy、trap、跳转地址越过imm的范围

2021/12/23
==========
1. 讨论了t_reg的个数设计。8个够用，128空闲太多。在tb中修改和读取寄存器的数据。

2. 测试方案，补充了综合测试项目。具体如下：

   .. toctree::

      test

2021/12/24
==========
1. 根据评审意见 修改了test方案。添加目的和策略。给开发者一个清晰的不要有歧义的目标。

2. 讨论了op图偏移的概念定义.弄清楚了。

2021/12/27
==========
1. qemu-linx:增加sub/or/not/xor/srl/sra/div/divu指令解析功能

2. test.rst 修改了link0 op link1的关系，以前是link1 op link0,参考了RV的设计，t1 op t2

2021/12/28
==========
1. qemu-linx:为div/divu增加除数为0的处理。否则会报coredump错误。

2. 验证RV的溢出验证，无异常。


2021/12/30
==========
1. 今天实现了比较跳转BLT+BNEXTCOND。考虑了两种方案 ::
   第一种: BLT/BEQ/BGE等比较结果是0或1保存在一个寄存器里面T，然后bnextcond根据上一条寄存器
   的结果比较，若是1则跳转，否则不跳转。限定了bnextcond必须在比较指令之后。不能乱序运行。

   第二种: 方案是参考arm的设计方式，加一个状态寄存器保存比较结果。然后根据状态寄存器的结果是1则跳转，否则不跳转。
   这种方法要求每次比较结果都写入固定的状态寄存器。在复杂条件比较的时候有些问题：例如if(a<b||c>d) 还是得用
   T寄存器，一个状态寄存器不够用，若是比较用状态寄存器，那么所有的逻辑的计算结果(与或非异或)都得同时更新
   状态寄存器，比较复杂。

   比较：第一种方案简单,比较和跳转得一起。第二种方案复杂，可以不一起。
   群里讨论了跳转的要求：分支跳转、函数跳转、ret都不能发生在块中间，必须是块结尾。

2022/1/4
=========
1. 今天实现了getw指令，看了下符号扩展相关的函数 ::
   tcg_gen_ext16s_tl
   tcg_gen_ext8s_tl
   tcg_gen_ext32s_tl

2. 看了下wangzhou的log，看看BLOCKISA新版本指令。
   START头变化比较大，长度变大（长度是变长还是固定需继续看看文档）。
   跳转指令明确规定只能跳转到START。跳转和store只能在commit后发生，则需要在在CPU中记录一些状态信息才能实现。

3. 走读代码bstart.cond.

2022/1/5
========
1. 看BLOCKISA文档 ::
   load/store变化：多了imm参数。有些不对称：load [link0+imm]->T, 而store是[link1]<-link0+imm ?可能是我理解错了。
   Store变化：存储指令影响当前T寄存器。
   关于溢出：没有涉及到
   CONST指令变化比较大。可以多条合成一个大的立即数。
   BLOCKhead固定128，可能256bit，concat粘合。


2022/1/6
========
1. BLOCKHEAD中的BTEXT含义是指向该块的微指令的开头，这个变化了，BLOCKHEAD和块内容的分离.Fall顺延的计算是NEXTBPC=BPC+sizeof(block_head)
   FENCE在C语言中有概念，是为了防止乱序执行，过度优化。这里的FENCE也跟执行顺序相关。
   FENCE：内存屏障指令，理解应该是要求内存操作指令按照先后顺序执行完，不能乱序。？当前没有乱序执行，不好模拟。
   FENCE.I  不太懂。是前面的没有写完，后面的读指令可以用吗？
   ECALL、EBREAK不太懂什么场景下用。
   SYSGET：读取系统寄存器的值，写到架构寄存器中。SYSGET #GPR,#SYSREG GPR是架构寄存器，SYSREG是系统寄存器。
   系统寄存器是对外可见的寄存器。架构寄存器是块内部寄存器。？
   CMP.EQ/LT/LTU/GE/GEU比较结果修改T寄存器，不涉及跳转，用于比较计算。
   SETBPC.EQ/NE/LT/LTU/GE/GEU 涉及跳转，比较结果修改SBPC寄存器。
   ADDBPC：用途在什么地方？
   ADDTPC：用途在跳转？
   SETBPC：设置BPC，使用场景选择性跳转，跳转在HEAD中的BNEXT中设置地址。
   BPC的含义原本是当前块的PC，现在多了一个身份，可能是跳转的BPC地址。

2. BNEXT.TYPE 应该是写错了吧，应该是BranchType：3bit
	000Fall Through: 顺延，相邻的下个块指令。NEXTBPC=BPC+sizeof(block_head)
	001Direct Link:直接跳转， NEXTBPC=BPC+BNEXT
	010Call：顺延的地址是返回地址，LinkRegister中，类似RA(return address)。
	011Conditional 条件跳转。 SETBPC.COND计算为TRUE，则执行BPC+BNEXT头。否则执行顺延地址
	100Indirect Link ：间接跳转。SETBPC计算下一个跳转地址。
	101Indirect Call：间接call
	110Ret：NEXTBPC=LINKREG寄存器
	111Concat ：粘合

3. qemu的ld/st的异常处理还没有看明白。

2022/1/7
========
1. review裕业的block.decode的修改，用别名替换，简化结构体中的名字imm
2. test宏修改，add/sub/and/div/divu/rem/remu/sll/sra/srl等简单的用例pass。跳转类别没有通过

2022/1/10
=========
1. 修改测试用例的test的blockISA v0.9的head 128bit，验证通过add/sub测试用例
2. 学习FENCE，类似barrier的使用

2022/1/11
=========
1. 修改test用例，通过大部分单个block的用例
2. qemulinx代码，增加headtype/branchtype和branchtype的某些处理
   对于direct_link的处理有分歧 ::
   1. 直接修改pc_succ_insn
   2. 用jmp实现,可能导致后续循环处理时，通过PC查到缓存，不再解析，一系列状态不会变。

2022/1/12
=========
今天看qemu-riscv的exception
	* 定义cpu_bits.h
	* 回调函数定义 ::

            .do_unaligned_access = riscv_cpu_do_unaligned_access,
		.do_transaction_failed = riscv_cpu_do_transaction_failed
		.tlb_fill = riscv_cpu_tlb_fill

	* 异常触发 ::

                tlb_fill()->cc->tcg_ops->tlb_fill()->riscv_cpu_tlb_fill->raise_mmu_exception()
		atomic_mmu_lookup -> cpu_unaligned_access->do_unaligned_access()
		load_helper->cpu_unaligned_access->do_unaligned_access()
		store_helper->cpu_unaligned_access->do_unaligned_access()
		cpu_transaction_failed()->do_transaction_failed()->riscv_cpu_do_transaction_failed()
		helper_tlb_flush()->riscv_raise_exception
		helper_raise_exception（）->helper_raise_exception
		helper_sret() -> riscv_raise_exception() RISCV_EXCP_INST_ADDR_MIS
		helper_mret()->riscv_raise_exception()
		helper_wfi()->riscv_raise_exception
		helper_hyp_tlb_flush()->riscv_raise_exception
		helper_hyp_gvma_tlb_flush()->helper_hyp_gvma_tlb_flush
		gen_helper_raise_exception()->tcg_gen_callN()->helper_raise_exception()

	* 处理中断target/linx/cpu_helper.c
		riscv_cpu_do_interrupt
		设置模式为S或M

	* 恢复中断helper_mret/helper_sret ::
		riscv_cpu_set_mode(env, prev_priv);


2022/1/14
=========
编译linux-riscv64
-----------------

1. cd linux
2. cp arch/riscv/configs/defconfig .config
3. make CROSS_COMPILE=riscv64-linux-gnu- ARCH=riscv  menuconfig
   打开debug
4. make CROSS_COMPILE=riscv64-linux-gnu- ARCH=riscv

debug vxlinux
-------------
1. 必须添加nokaslr，否则vmlinux符号会对不上，无法正常显示debug信息和下断点
   qemu-system-riscv64 -append  nokaslr
2. gdb-multiarch vmlinux
3. set architecture riscv
4. target remote :1234

2022/1/17
=========
1. add MTR/MRT
   因为x0-x9中有pc/sp等特殊的寄存器，故r0--10寄存器

2022/1/18
=========
1. MTR test 通过GET/SET检查每一个R寄存器是不是正确
2. SRET/MRET 指令参考riscv的sret/mret,去掉pmp和cpu_virt
3. BRK指令参考riscv的ebreak指令，调用do_common_semihosting进入gdb模式
4. Trap指令参考ecall指令，进入内核模式

2022/1/19
=========
1. MRT test 通过SET检查每一个T寄存器是不是正确
2. CONST 多种情况验证 多个test不通过：64bit/单独一个const
3. BRK/TRAP的验证不知道方法

2022/1/20
=========
CONST
-------

   若根据文档，一个const指令只占一个槽位，那么跟现在的计算方法冲突，暂时占用多个槽位。

ebreak
-------

   The EBREAK instruction is used by debuggers to cause control
   to be transferred back to a debugging environment. It generates a 
   breakpoint exception and performs no other operation.
   EBRAK指令由调试器用来引起控制传输回调试环境。它生成一个断点异常，不执行其他操作。
   riscv-ebreak:演示例子 ::
   https://webcache.googleusercontent.com/search?q=cache:GYY7gzIof5EJ:https://sourceware.org/pipermail/gdb/2021-January/049125.html+&cd=2&hl=zh-CN&ct=clnk

   riscv_ebreak.S ::

        .text
        .align 4
        riscv_ebreak:
        .global riscv_ebreak
            nop
            nop
            ebreak
            nop
            nop

    运行结果 ::

        gdb test
        Program received signal SIGTRAP, Trace/breakpoint trap.
        riscv_ebreak () at asm/riscv_ebreak.S:15
        15          ebreak
        (gdb)

    程序中的流程generate_exception(ctx, RISCV_EXCP_BREAKPOINT);


ecall用户模式调用系统函数
-------------------------

* 规则 ::

   系统调用号传入 a7
   系统调用参数传入 a0至 a5
   未使用的参数设置为 0
   返回值在 a0 中返回

* 代码 ::

           case RISCV_EXCP_U_ECALL:
           env->pc += 4;
           if (env->gpr[xA7] == TARGET_NR_arch_specific_syscall + 15) {
                /* riscv_flush_icache_syscall is a no-op in QEMU as
                   self-modifying code is automatically detected */
                ret = 0;
            } else {
                ret = do_syscall(env,
                                 env->gpr[(env->elf_flags & EF_RISCV_RVE)
                                    ? xT0 : xA7],
                                 env->gpr[xA0],
                                 env->gpr[xA1],
                                 env->gpr[xA2],
                                 env->gpr[xA3],
                                 env->gpr[xA4],
                                 env->gpr[xA5],
                                 0, 0);
            }
            if (ret == -TARGET_ERESTARTSYS) {
                env->pc -= 4;
            } else if (ret != -TARGET_QEMU_ESIGRETURN) {
                env->gpr[xA0] = ret;
            }
            if (cs->singlestep_enabled) {
                goto gdbstep;
            }

TRAP修改内核状态
----------------
  本命令用于U-mode下需要变化成内核模式S-MODE，调用内核指令。完成后，通过SRET返回U-mode

2022/1/24
=========
ecall/ebreak 处理流程涉及到操作系统的处理代码，exception编号暂时采用riscv的。后续再修改。
trap的流程还需要继续查找资料。

ecall的系统调用num定义在${linux}/include/uapi/asm-generic/unistd.h中
handle_syscall符号处理syscall，代码在arch/riscv/kernel/entry.S:312

2022/1/25
=========
1. SYSGET/SYSGET 是将Rx系统寄存器设置到架构寄存器:mcause、mtval、mepc等

    对于sysget/sysset可以用4bit表示系统寄存器，用13bit表示架构寄存器
    指令可以这样设计(参见文档) ::

        #source t_reg:
        %rs1  27:5
        %rs2 22:5
        %rd 11:5
        %csr0_6 20:7
        %csr7_13 9:7
        %csr0_11 20:12
        %csr12_13 9:2
        # Argument sets:
        &sysget   rs1 csr0_6 csr7_13
        &sysset csr0_11 rd csr12_13
        # Formats linx:
        #@sys    ....... ......................... &bsys %instr
        @sysget ..... ....... .... ....... . ........ &sysget %rs1 %csr0_6 %csr7_13
        @sysset ............ .... ..... .. . ........ &sysset %csr0_11 %rd %csr12_13
        blk_sysget        ..... ....... 0000 ....... 0 01111100 @sysget
        blk_sysset        ............ 0001 ..... .. 0 01111100 @sysset

2.  sysget/sysset指令类似riscv的csrrw指令
    The CSRRW (Atomic Read/Write CSR) instruction atomically swaps values
    in the CSRs and integer registers.

    在trans_csrrw中调用gen_helper_csrr读取csr寄存器，
    gen_set_gpr(ctx, rd, dest);#设置系统寄存器值
    tcg_gen_exit_tb(NULL,0); #退出当前loop到main循环中，理由是可能改变了重要的状态寄存器

3.  BLOCKISA的csr编号还没有设计出来，暂时参考riscv的。

4.  系统指令的原子性：？ csrrw定义是原子的，现在的riscv的实现本身就是原子的吗?

2022/1/27
=========
1. BRK-ebreak功能上一致即可
   TRAP需要考虑快内的状态保存和恢复
   ecall不用考虑状态的保存和恢复，linx的ecall用riscv的exception-num，这样就对pc得特殊处理。
   因为riscv的do_syscall完成后，pc+=4，这样linx的处理blk_ecall时需要保存pc的地址是
   当前的pc_next+16-4.这样的pc_next+16表示下一个head，-4是因为do_syscall后自动加4.与os连调
   需要注意这点。

2022/1/28
=========
1. BRK/TRAP功能考虑到exception完成后，pc会加4，那么产生异常时，设pc为pc_next-2
   TRAP会调用do_syscall，那么需要设置a7,a0-a5寄存器，而块中set是不会起作用，故test程序只能
   将TRAP指令和设置a7，a0-a5寄存器设置到不同的块中。之前讨论的保存状态不会发生。

2022/1/29
=========
1. 今天验证测试了ind_call，发现setbpc指令实现不符合v0.11版的要求。修改。
   setbpc 实现： sbpc=T[link0]
2. 发现bnext需要设置成有符号整形，而不是无符号数。这样跳转才能实现向前间接跳转。
    bpc+bnext

2022/2/7
========
1. 今天修改了符号的扩展
2. 确定系统寄存器的结构
    struct LINXBstate{
    struct LINXBstate in,out;

2022/2/8
========
1. 编码的符号解析可以通过设置decode文件s标志进行自动解析
2. head block-转tcg中间码：head->no_return
   assert(tb->size!=0)导致的原因可能是btext为0.
   tb->size=tb->pc_next-tb->pc_first
   tb->pc_next相当于ctx->base.pc_next

2022/2/9
=========
1. BSTATE.in BSTATE.out的41*2个寄存器编号采用riscv-privileged.pdf中的
   Supervisor CSRs中的 Custom read/write 两组 0x5C0-0x5FF 0x9C0-0x9FF 权限是read/write
   0xDC0-0xDFF  这组权限是readonly
   BSTATE.in 是只写，可用用read/write编号； BSTATE.out是只读用readonly的编码即可。
2. target/linx/cpu_bits.h 定义CSR_IN/OUT_XX编码
3. csr_ops表增加处理

异常处理流程
============
1. 错误编号
   RISCV_EXCP_LOAD_ACCESS_FAULT  5
   RISCV_EXCP_STORE_AMO_ACCESS_FAULT 6
   RISCV_EXCP_INST_ACCESS_FAULT  7

   产生异常流程 ::

    #0  io_readx (env=0x10000005, iotlbentry=0x555555ceee81 <tlb_index+32>, mmu_idx=32767, addr=93825014062672, retaddr=93825000205775, access_type=(unknown: 601860640), op=MO_8)
    at ../accel/tcg/cputlb.c:1338
    #1  0x0000555555cf3c67 in load_helper (env=0x555556a25e50, addr=268435461, oi=3715, retaddr=140734871505404, op=MO_8, code_read=false, full_load=0x555555cf3e4a <full_ldub_mmu>)
    at ../accel/tcg/cputlb.c:1957
    #2  0x0000555555cf3ea3 in full_ldub_mmu (env=0x555556a25e50, addr=268435461, oi=3715, retaddr=140734871505404) at ../accel/tcg/cputlb.c:2016
    #3  0x0000555555cf3edb in helper_ret_ldub_mmu (env=0x555556a25e50, addr=268435461, oi=3715, retaddr=140734871505404) at ../accel/tcg/cputlb.c:2022
    #4  0x00007fff64060a80 in code_gen_buffer ()
    #5  0x0000555555cdfc68 in cpu_tb_exec (cpu=0x555556a1ccc0, itb=0x7fffa4060900, tb_exit=0x7fff23dfb124) at ../accel/tcg/cpu-exec.c:357
    #6  0x0000555555ce0a81 in cpu_loop_exec_tb (cpu=0x555556a1ccc0, tb=0x7fffa4060900, last_tb=0x7fff23dfb130, tb_exit=0x7fff23dfb124) at ../accel/tcg/cpu-exec.c:842
    #7  0x0000555555ce0e3f in cpu_exec (cpu=0x555556a1ccc0) at ../accel/tcg/cpu-exec.c:1001
    #8  0x0000555555d02b16 in tcg_cpus_exec (cpu=0x555556a1ccc0) at ../accel/tcg/tcg-accel-ops.c:67
    #9  0x0000555555d02ea6 in mttcg_cpu_thread_fn (arg=0x555556a1ccc0) at ../accel/tcg/tcg-accel-ops-mttcg.c:95
    #10 0x0000555555ed5c81 in qemu_thread_start (args=0x555556a44ec0) at ../util/qemu-thread-posix.c:556
    #11 0x00007ffff5bb8609 in start_thread (arg=<optimized out>) at pthread_create.c:477
    #12 0x00007ffff5adf293 in clone () at ../sysdeps/unix/sysv/linux/x86_64/clone.S:95

2. riscv_cpu_do_interrupt在异常处理程序中记录pc到sepc中 env->scause=0x8000000000000005

3. 内核处理完成后，调用helper_sret.可以看到 env->scause=0x8000000000000005和其他的错误

4. 异常线程保存在riscv_cpu_do_interrupt函数中增加代码。恢复在helper_sret

5. 代码修改
    1) 发生异常的处理函数?
        这个地方是否要限制，对env->pc与env->bpc1/bpc2的关系是否要限制。运行块程序期间，调用底层指令导致异常出现，
        BSTATE 赋值到BSTATE.out

    2) 异常恢复的地方 BSTATE.out 赋值到BSTATE.in

    3) trans_blk_head解析的地方，目的不用重复执行
        动态判断BSTATE.in.en是否为True,若为True,恢复BSTATE.in->BSTATE，跳转到tpc

    4) commit时候 BSTATE.in/out设置为false

6. 验证except的测试用例设计1需要修改qemu内部代码
   用TRAP指令会触发异常，进行现场的保存
   在helper_sret触发恢复，执行
   需要注意的是，trap指令异常后执行下一条指令，而tlb需要执行当前指令。这里还需要区别一下

7. 验证except的测试方案2：不需要内部代码修改
   1) 增加一个新的SAVE微指令，在块中，保存BSTATE->BSTATE.out tpc=env->pc+2
   2) 第一个块是根据某个寄存器值进行条件跳转,等于0跳转到另外一个块进行拷贝bstate.out->in
   3) 第一个块，判断非0 到结束块
   4) 拷贝块指令fallthrough到下个块，下个块是普通块，跳转到第一个块，重新执行。

2022/2/21
=========
1. 今天 检查了sysget/sysset的测试出错问题:由于添加异常保存和恢复功能，
   清零了in.tpc/in.sbpc导致。

2. 根据王州意见修改了except的保存和恢复代码。BSTATE.out到BSTATE.in 不好在test代码中完成
    a) BSTATE.out -> BSTATE.in 不能在qemu中拷贝，只能user中
    b) 判断pc的范围,少了第一条指令和最后一条指令
    c) 保存函数中，不能有特殊代码

2022/2/22
=========
1. 今天讨论了一下王州的except验证方案，可行，见异常处理流程。编码完成。待调测。

2022/2/23
=========
1. 今天调测了except的异常保护恢复测试。这个in的数据判断失败不是该块不能清理。完成。

V0.12的BLOCKISA的变化
=====================
    1) get指令之前是从GPR获取数据，以后改为从SGPR获取数据
    2) BSTATE.in OUT 修改成BSTATE.EXT  没有in/out 拷贝了 ？？？
    3) 地址对齐 不用管？？
    4) BNEXtype  没有变？ P26 BNEXT.type 0 代表本块， 1代表下一块 -1代表上一块 imm代表
       这个比较影响test程序的修改。很多用到了直接跳转的，与riscv混合编译ret的要修改。
       实现方案：在head解析里面存储bnext值就存储bnext*16，目前会影响branch_direct_link
       branch_call/branch_conditional三种场景
       检查16B对齐地址，可以通过 (addr&0xf) != 0 即为未对齐。
       参考riscv的对齐逻辑：gen_jal函数，判断next_pc&0x3!=0 触发gen_exception_insn_addr_mis
    5) BSIZE表示的含义变了  P27页  指示多少个2B
    6) 增加了xxxi计算 ： ADDI、ANDI、ORI、SRAI SRLI SLLI SUBI 、
        SETBPC.EQI、SETBPCGEI、SETBPCGEUI、SETBPC.LTI SETBPC.LTUI SETBPC.NEI  XORI
    7) 编码文档修改 block.decode
    8) 长编码块  超长立即数 、三操作指数、
    9) 系统块微指令 编码. 与标准块编码不重复应该可以在同一个文件进行decode
       这里需要注意系统编码有部分是32bit，有的是16bit，这样需要注意pc的变化

2022/2/25
=========
1. 讨论一下v0.12的版本差异，工作。
2. 后续工作开展
   1) block.decode修改
   2) test基本的验证
   3) 算术指令带I的实现
   4) BSTATE.in/out 切换成 BSTATE.EXT 测试代码也需要修改
   5) .....

2022/2/28
=========
1. 后续工作计划
   1) decode / test 是基础
   2) 旧的测试用例保证通过
   3) 新的算术imm指令

2022/3/1
========
1. 修改bodysize=2B单位，qemu中只需要修改head解析即可。
   test中需要修改设置。
2. 修改bnext的值为16单位的，qemu只需要修改head即可，bnext值
   而test中需要修改测试用例比较多的地方。
3. head 16字节对齐问题  用.align 4实现

2022/3/2
========
1. 修改系统编码定义与标准块的定义一并在一个文件中。
2. 注意pc的变化。 sysget/sysset/lconst(32bit变长) 其他指令是16bit
    sysget/sysset pc=pc_next+4
3. 需要注意系统块的结束commit处理 若是刚好最后一条指令是sysget/sysset不需要特殊处理
   需要测试用例多测试边界情况

原子交换amoswap
===============
1. amoswap_w/amoswap_d
   原子交换，内存中的w/d数据
2. lr/sc指令 load  store
   成对出现
3. riscv中均有实现

2022/3/10
=========
1. ebreak需要在commit的时候进行提交影响
    1) 记录是否有ebreak,需要动态设置
    2) 记录ebreak的tpc，需要动态设置
    3) riscv_cpu_do_interrupt 保存上下文时用env->pc到env->ext.tpc不正确
       要区别处理
       若是ebreak，则用ebreak_tpc,否则用env->pc

head.concat实现
===============
1. 第一个head中的btext/bnext是低位
2. 第二个head中的前64bit无用，接着32bit是bnext的高位，
   最后的32bit是btext的高位

2022/3/15
=========
1. lconst的功能修改。添加多个测试用例。找到几个bug。测试用例需要全面一些。
   size=0/1/2/3 sign=0/1 至少8个用例
2. 验证compile编译器的功能。编译通过。运行失败。
   blockhead：16B对齐问题
   2B对齐btext*2
   用ra/setbpc指令多次返回

2022/3/21
=========
1. 今天走读了下王州上周修改的代码
   a) 对于align 5的hack写法 修改了一部分。
   b) 验证了在sys模式下 ecall测试用例通过，user模式失败，看代码找到了原因。interrupt函数中有hack处理
2. 验证新版本编译器
   a) mask 检查失败 bsetmask检查失败，多了一个ra或者是少了一条指令
   b) 主体程序main/add 运行完毕，在exit失败

2022/3/22
=========
1. 联调编译器的-qemu的代码，16B未对齐，main运行完毕，在exit返回失败 ::
   Thread 1 "qemu-linx" received signal SIGSEGV, Segmentation fault.
   0x00007fffe8009457 in code_gen_buffer ()
   10a26:   0109                    const   0x8
   10a28:   0912                    get s1
   10a2a:   0400                    add t#1,t#2
   10a2c:   0044                    lw  [t#1,0]
   10a2e:   0109                    const   0x8
   10a30:   0812                    get s0
   10a32:   0400                    add t#1,t#2
   10a34:   0064                    ld  [t#1,0]
   10a36:   0e92                    set a4,t#1
   10a38:   ffe9                    const   0xffffffffffffffff
   10a3a:   c000                    add t#7,t#1
   10a3c:   0409                    const   0x20
   10a3e:   20e0                    sll t#2,t#1
   10a40:   04c0                    sra t#1,t#2
   10a42:   0f92                    set a5,t#1
   10a44:   1212                    get s2
   10a46:   4030                    setbpc.ne   t#3,t#1
   10a48:   0109                    const   0x8
   10a4a:   0912                    get s1
   10a4c:   0400                    add t#1,t#2
   10a4e:   1212                    get s2
   10a50:   200e                    sw  t#1,[t#2,0]
   10a52:   0e12                    get a4
   10a54:   0011                    setbpc.eqi  [t#1,0]
   10a56:   6209                    const   0x310
   10a58:   0912                    get s1
   10a5a:   0400                    add t#1,t#2
   10a5c:   0044                    lw  [t#1,0]
   10a5e:   0409                    const   0x20
   10a60:   1612                    get s6
   10a62:   04e0                    sll t#1,t#2
   10a64:   1212                    get s2
   10a66:   20e0                    sll t#2,t#1
   10a68:   e001                    addi    [t#8,0]
   110bc:   00040300 0000c000 00804400 ff96ac0b     bstart  b.std, bnext.cond, bget:0x00040300, bset:0x0000c000, ptr:0x10a26 size:0x11, bnext:0x1113c
2. 修改了测试的hack代码 'align 5' 运行成功
3. 测试的setbpc.S 用宏优化了一下，去掉重复的代码

2022/3/24
=========
1. 今天验证了os提供的单线程atomic_add/sub测试用例。通过。main返回值正确。
2. 看了下exit/__call_exitprocs发现里面有free，于是验证了malloc小测试用例，不过。
   对于编译器联调思路：看c代码，分离出小测试用例，验证，查找错误。明天继续看

2022/5/9
========
1. 看了lr/sc的实现和测试代码，看了王州log对其理解。smp设置为1时测试用例能pass
   理解了实现思路。cpu 1个和多个的情况。sc后要检查是否成功
   明天继续查看其它失败情况。

2022/5/12
=========
1. lr/sc功能从王州的分支rebase到最新的linx-dev,合并后建立新分支debug_lr_sc_cj进行测试
2. atomic有5种。判断是否是atomic用incline函数去判断。
3. 多线程测试发现：lr/sc会触发中断，不合理进入一个分支if(in_block);
   env->pc=0xffff8xxxx,bpc1/bpc2/bpc为block中的值.
   了解到原本此分支设计是为了ecall/ebreak中断情况而写，现在没有ecall/ebreak进入的分支，与设计不符。
4. lr/sc大量数据测试时，触发许多LOAD_ACCESS_FAULT(5)
5. lr/sc多线程运行失败，由于中断处理程序错误导致。屏蔽部分程序可以运行成功，因为运行riscv-os，没用到t寄存器。
   这个需要解决，因为linx-os联调需要中断恢复。
6. 修改ecall/ebreak的中断处理流程。
   为ecall/ebreak选择LINX_EXCP_U_ECALL/LINX_EXCP_BREAKPOINT
   进入riscv_cpu_do_interrupt函数后，进入变化
   1) 若cs->exception_index 是LINX_EXCP_U_ECALL，修改cause为RISCV_EXCP_U_ECALL.与RISCV_ABI保持一致。
   2) 若cs->exception_index 是LINX_EXCP_BREAKPOINT,修改cause为RISCV_EXCP_BREAKPOINT.与RISCV_ABI保持一致。
   3) 若用户态变化为S态，当cs->exception_index 是LINX_EXCP_U_ECALL或LINX_EXCP_BREAKPOINT时，修改env->sepc为env->bpc

Load-Reserved/Store-Conditional(简称lr/sc)
==========================================
1. lr/sc 是成对使用，用于原子计算的。lr是特殊的load指令(设置一个标志)，sc是特殊的store指令（该指令可能成功，
   可能失败，有返回值，若前一个lr失败，则这个sc也会失败）。
   对于linx的块要求：可以在普通块中，也可以在特殊块中。
   lr指令在属性为common/atomic.aq/atomic.aq_rl的块中;
   sc指令在属性为common/atomic.rl/atomic.aq_rl的块中。
   lr/sc跟锁也不完全一样，lr指令不确定是否成功，只有sc指令结束时才能知道lr/sc整体是否成功，
   若失败时需要重新跳转到lr指令开始执行。
   SC只能按程序顺序与最近执行的LR配对，并且SC必须与最近执行的LR地址相同，数据大小相同(larm)。
   例子1：只有sc，无lr，存储失败
   例子2：先一个lr,后一个sc，存储成功
   例子3：先两个lr，后两个sc 第二个sc失败
   测试：通过多线程，验证原子性。这里的读取存储地址选用分配的大页地址，避免缺页导致失败。
   缺点：测试用例没有覆盖缺页场景。
   测试考虑：不测试lr/sc在同一个block中的场景，测试lr/sc在不同block中的场景;因为一个block中的原子测试已经测试过了。

2. riscv有lr.d/lr.w/sc.d/sc.w指令，参考用以实现linx的lr_d/lr_w/sc_d/sc_w指令
    lr指令实现：tcg_gen_qemu_ld_tl加载数据到load_val,保存数据地址到load_res，给数据分配一个寄存器
    sc指令实现：比较load_res与sc指令中的地址是否相等，若相等继续，不等失败(设置返回值1); 相等继续
    则保存数据到sc数据到内存。

    ```
    tcg_gen_atomic_cmpxchg_tl(dest, load_res, load_val, src2,  ctx->mem_idx, mop);
    ```
    sc写入的逻辑：如果load_res上数据和load_val相等，表示数据没有被其他写入,
    那么src2数据被写入load_res的位置，写0到dest表示sc成功;
    load_res上数据和load_val不等，写1到dest表示失败；最后将load_res设置成-1(清零).
    这种实现写入的好处是，不用barrier也可以保证atomic。若是一个sc写成功了，那么val值一般会变化，
    即使load_res没清零，下一个sc还是会写失败;若是val值没有变化，那么sc再写入一次没有坏的影响;当然laod_res还是要清零。

    riscv有辅助变量load_res和load_val,linx需要类型的linx_load_res/linx_load_val

    memory_barrier：riscv通过lr/sc的指令中携带参数，确认是否要是能TCG_BAR_LDAQ/TCG_BAR_STRL,非必须。
    linx:根据块的类型使能，atomic.aq使能TCG_BAR_LDAQ;atomic.rl使能TCG_BAR_STRL
    atomic.aq_rl使能TCG_BAR_SC,意思是No ops cross barrier; OR of the above(没有或以上两个)

3. 关于乱序限制memorybarrier的理解：riscv中的aq/rl是微指令级别的乱序限制;
   而linx的乱序限制的是块的顺序，block内部是不能乱序的，是图计算，是有序的。
   故可以设置lr所在块结束时设置TCG_BAR_LDAQ，在sc块结束时设置TCG_BAR_STRL
   不过验证过不设置memorybarrier的多线程，lr/sc的测试也是pass的。riscv的barrier也不是必须的。

5/17
=====
修改bug：日志中(block body)前的IN:后面缺函数名
----------------------------------------------

看函数代码riscv_tr_disas_log()：
qemu_log("IN: %s\n", lookup_symbol(dcbase->pc_first));
分析可以通过lookup_symbol(env->bpc)获取当前的函数名


-symfile符号不全
----------------
打印出所有的符号信息，发现问题可能是编译器type错误，需要协助定位.
保留的正确的type=2，st_shndx=2 st_shndx=2，被过滤掉的是不需要的。
但是被过滤的relocate/___start_kernel 应该保留


i=3 name=relocate value=0xffffffff80000060 st_size=0 st_shndx=1 st_shndx=1 type=0
i=22 name=___start_kernel value=0xffffffff80000040 st_size=0 st_shndx=1 st_shndx=1 type=0
i=4 name=.L1^B1 value=0xffffffff80000100 st_size=0 st_shndx=1 st_shndx=1 type=0

i=1675117 name=__memset value=0xffffffff8143e360 st_size=480 st_shndx=2 st_shndx=2 type=0
i=21 name=.L106 value=0xffffffff800013a0 st_size=0 st_shndx=2 st_shndx=2 type=0
i=22 name=.L80 value=0xffffffff800013e0 st_size=0 st_shndx=2 st_shndx=2 type=0


2022/5/18
=========
load_symbols的逻辑
------------------

1. 输入参数struct elfhdr  表示 elf段信息。
2. if (shdr[i].sh_type == SHT_SYMTAB) 查找存储符号表的段.
3. 过滤掉不需要的符号syms[i].st_shndx为SHN_UNDEF或大于SHN_LORESERVE，或type类型不是STT_FUNC.
4. 过滤掉一些标签符号后 --nsyms 总数缩少.
5. 将后来的符号覆盖不要的：syms[i] = syms[nsyms];
6. i++ ：判断是函数符号表，索引增加.


