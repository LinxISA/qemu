Linx Qemu性能问题分析
*********************


介绍
====

这个Issue 2023-2-2创建，目标是解决Linx Qemu的性能明显比RISCV低的问题。

当前问题Owner：李国柱。本文由Owner维护，但记录的是所有投入人员的结果。

Qemu-Linx性能不足问题在各种模拟中普遍存在，直接用李飘提供的一个例子程序，看到的
对比是这样的：::

  kenny@kl-dev:~/work/linx-qemu-dev/qemu_perf_test$ time ../LinxBlockModel/build/riscv64-linux-user/qemu-riscv64 test.rv
  
  real    0m0.455s
  user    0m0.447s
  sys     0m0.008s

  kenny@kl-dev:~/work/linx-qemu-dev/qemu_perf_test$ time ../LinxBlockModel/build/linx-linux-user/qemu-linx test.linx
  
  real    1m13.987s
  user    1m13.971s
  sys     0m0.016s

不到一秒和超过一分钟的区别。这个问题影响所有的测试，不解决这个问题，其他测试基
本上无法进行，特别是性能测试基本上都需要相当的时间，乘上这个系数完全无法定位问
题（比如大部分Spec测试发现的问题都是几天才能出来一次）。所以我们优先解决这个问
题，这样我们才有解决其他问题的基础。

之前有一个分析报告说这个问题主要是因为块头和块身不在同一个页中，产生大量的
unchain TB导致的，但我们还是先确认一下再说。


分析
====


20230201
--------

同一个程序看profile数据。

这是RV的：::

  Samples: 1K of event 'cycles', Event count (approx.): 1404716510
  Overhead  Command       Shared Object            Symbol
    10.03%  qemu-riscv64  qemu-riscv64             [.] helper_lookup_tb_ptr
     7.16%  qemu-riscv64  qemu-riscv64             [.] tcg_gen_code
     3.79%  qemu-riscv64  qemu-riscv64             [.] liveness_pass_1
     1.60%  qemu-riscv64  qemu-riscv64             [.] tcg_optimize
     1.26%  qemu-riscv64  qemu-riscv64             [.] check_for_breakpoints
     1.09%  qemu-riscv64  qemu-riscv64             [.] cpu_get_tb_cpu_state
     0.87%  qemu-riscv64  qemu-riscv64             [.] la_cross_call
     0.86%  qemu-riscv64  qemu-riscv64             [.] riscv_tr_translate_insn
     0.84%  qemu-riscv64  qemu-riscv64             [.] tcg_out_opc
     0.69%  qemu-riscv64  qemu-riscv64             [.] init_ts_info
     0.67%  qemu-riscv64  libglib-2.0.so.0.7200.1  [.] g_hash_table_insert
     0.58%  qemu-riscv64  qemu-riscv64             [.] tcg_out_op
     0.55%  qemu-riscv64  libglib-2.0.so.0.7200.1  [.] g_hash_table_lookup
     0.54%  qemu-riscv64  qemu-riscv64             [.] tb_gen_code
     0.49%  qemu-riscv64  qemu-riscv64             [.] tcg_reg_alloc
     0.44%  qemu-riscv64  qemu-riscv64             [.] copy_propagate.isra.0
     0.41%  qemu-riscv64  qemu-riscv64             [.] tb_tc_cmp
     0.38%  qemu-riscv64  qemu-riscv64             [.] tcg_out_sib_offset
     0.33%  qemu-riscv64  qemu-riscv64             [.] tcg_op_alloc
     0.32%  qemu-riscv64  qemu-riscv64             [.] tcg_emit_op
     0.30%  qemu-riscv64  [vdso]                   [.] 0x00000000000006e5
     0.29%  qemu-riscv64  [kernel.kallsyms]        [k] 0xffffffffa20a2517
     0.29%  qemu-riscv64  libc.so.6                [.] ____wcstold_l_internal
     0.27%  qemu-riscv64  qemu-riscv64             [.] tcg_temp_free_internal
     0.27%  qemu-riscv64  qemu-riscv64             [.] translator_lduw_swap
     0.27%  qemu-riscv64  qemu-riscv64             [.] finish_folding
     0.26%  qemu-riscv64  qemu-riscv64             [.] tcg_constant_internal
     0.22%  qemu-riscv64  [JIT] tid 5852           [.] 0x00007f10ec14b740
     0.22%  qemu-riscv64  qemu-riscv64             [.] tcg_out_st
     0.22%  qemu-riscv64  qemu-riscv64             [.] translator_loop
     0.22%  qemu-riscv64  qemu-riscv64             [.] cpu_exec
     0.22%  qemu-riscv64  libc.so.6                [.] ____wcstof_l_internal
     0.20%  qemu-riscv64  qemu-riscv64             [.] riscv_tr_insn_start
     0.19%  qemu-riscv64  qemu-riscv64             [.] fold_const2_commutative.constprop.0
     0.18%  qemu-riscv64  qemu-riscv64             [.] page_find_alloc
     0.16%  qemu-riscv64  qemu-riscv64             [.] tgen_arithi
     0.16%  qemu-riscv64  libglib-2.0.so.0.7200.1  [.] g_int64_hash
     ...

这是Linx的： ::

  Samples: 299K of event 'cycles', Event count (approx.): 231212848670
  Overhead  Command    Shared Object            Symbol
    27.57%  qemu-linx  qemu-linx                [.] helper_get_blk_t
    21.13%  qemu-linx  qemu-linx                [.] helper_set_blk_dest
     8.23%  qemu-linx  qemu-linx                [.] helper_mask_check
     8.22%  qemu-linx  qemu-linx                [.] cpu_exec
     4.51%  qemu-linx  qemu-linx                [.] helper_handle_exec_and_branch
     4.48%  qemu-linx  qemu-linx                [.] cpu_tb_exec
     2.39%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000000
     1.62%  qemu-linx  qemu-linx                [.] helper_blk_do_recovery
     0.59%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000018
     0.59%  qemu-linx  qemu-linx                [.] cpu_get_tb_cpu_state
     0.49%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000022
     0.28%  qemu-linx  qemu-linx                [.] tcg_bp_exec_tb
     0.27%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000001
     0.24%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc000001f
     0.17%  qemu-linx  qemu-linx                [.] tcg_gen_code
     0.15%  qemu-linx  qemu-linx                [.] liveness_pass_1
     0.13%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc000002a
     0.13%  qemu-linx  qemu-linx                [.] check_for_breakpoints
     0.11%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc000002c
     0.11%  qemu-linx  qemu-linx                [.] tcg_optimize
     0.09%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc000000d
     0.08%  qemu-linx  qemu-linx                [.] la_cross_call
     0.08%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000006
     0.07%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000004
     0.07%  qemu-linx  libglib-2.0.so.0.7200.1  [.] g_hash_table_lookup
     0.06%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000028
     0.04%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000002
     0.04%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000014
     0.04%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000026
     0.03%  qemu-linx  qemu-linx                [.] init_ts_info
     0.03%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000024
     0.03%  qemu-linx  [JIT] tid 16995          [.] 0x00007f3bc0000008

后者的占比非常集中，就是三个Helper函数占据了大量的时间，它们的用途如下：::

  helper_get_blk_t 
  helper_set_blk_dest
	这是读写temp寄存器用的helper函数，几乎每条指令都要用到，难怪它这么密。

  helper_mask_check
	这是校验指定的寄存器是否和块头一致。这个无论能否搞定，但校验本身肯定是
	可以暂时不做的。

现在看看为什么这个地方要用helper函数：helper是给JIT代码用的，因为正常用
tcg\_gen\_函数你只能给定TCGv让中间指令去访问，如果你有这个之外的变量要访问，你
就需要用其他代码来做，这些其他代码，就是helper。我们用gen\_helper\_xxxx来生成这
个代码，以便JIT代码可以调用这个helper函数。

T寄存器需要在运行阶段根据当前的ibpc来计算，这样我们就无法在生成JIT代码阶段就给
定一个TCGv给目标代码，而需要在运行阶段读出当前的ibpc，动态计算这个TCGv，然后再
投入计算，这样相比RV，我们几乎每条微指令，都多了至少一个动态读TCGv的过程（访问
了多少个T寄存器，就要多少个），这几乎是平白多出来的。这应该是造成我们性能低下的
主要原因。

下面是一个对比，在执行的时候，大家都做个加法，两个平台的JIT区是这个区别：::

              Linx               |             RISCV
                                 |
         TCGv rs0 = get_tcg(0)   |
         TCGv rs1 = get_tcg(1)   |
         TCGv rd = get_tcg(2)    |
         add rd, rs0, rs1        |         add rd, rs0, rs1

在RV中，生成这个代码的翻译器，一开始就算好了rd, rs0, rs1是什么了，一次在JIT里面
就填上了rd, rs0和rs1的内存地址，而我们翻译的时候根本就不知道这个位置在哪里，需要
在执行的时候一次次去计算那三个寄存器的TCGv。

所以，这就不是个什么Chained TB导致的问题，这个问题根本就是因为每条指令需要动态
计算寄存器引起的（判断依据：主要性能消耗都在helper中，而helper调用不涉及翻译）。
我们必须有个办法，在翻译阶段就给它确定输入输出寄存器，我们才有可能和RV的性能比
肩。

这可以做到吗？我想是可以的，如果没有块内跳转，那么我翻译的时候，每条指令的偏移
是固定的，那么理论上我可以得到一个静态的“当前指令-被访问寄存器的位置”的对应关系，
那么我就可以一开始就生成合适的代码。

而这个mask check，我一时想不到什么办法，我觉得在性能测试的时候可以临时关闭，等
功能测试的时候再打开。

那以后块内可以跳转怎么办呢？这好像一点办法没有啊，要不试试别搞什么中间代码了，
反正要翻译的指令不多，每个指令直接用一个helper，里面把相关的事情全部一起做了，
这样计算起来会不会反而会快一点？

get_blk_t的算法是这样的：::

  target_ulong helper_get_blk_t(CPURISCVState *env, int offset)
  {
      int index = (env->ibpc - offset - 1 + 8) % 8;
      if (index < 0) {
          riscv_raise_exception(env, RISCV_EXCP_ILLEGAL_INST, GETPC());
      }
      return env->blk_t[index];
  }
  
我简单这样优化了一下：::

  target_ulong helper_get_blk_t(CPURISCVState *env, int offset)
  {
      return env->blk_t[(env->ibpc - offset - 1) & 0x7 ];
  }

结果是这样的：::

  real    1m13.949s
  user    1m13.913s
  sys     0m0.028s

  Samples: 299K of event 'cycles', Event count (approx.): 231239709004
  Overhead  Command    Shared Object            Symbol
    27.40%  qemu-linx  qemu-linx                [.] helper_get_blk_t
    21.27%  qemu-linx  qemu-linx                [.] helper_set_blk_dest
     8.30%  qemu-linx  qemu-linx                [.] cpu_exec
     8.07%  qemu-linx  qemu-linx                [.] helper_mask_check
           
效果几乎可以忽略。所以，优化这个函数也没有什么意义，它占比高关键在于它的量，而
不是单个的执行效率。

所以，我觉得要不这样：如果我们知道这个块没有跳转，我们就用静态映射翻译，否则就
用动态映射翻译，这可能是现在能想到的最有可能选择的方案了。

这个报告写到这个位置，我发到群里，王州说贾文杰做过这个计算相对地址的修改，但当
时不知道怎么回事没有对比效果。直接就跳去分析chained-TB了。所以我让他出一个版本
给我，性能数据是这样的：::

  real    0m26.815s
  user    0m26.804s
  sys     0m0.012s

  Samples: 108K of event 'cycles', Event count (approx.): 83820894419
  Overhead  Command    Shared Object            Symbol
    22.18%  qemu-linx  qemu-linx                [.] helper_mask_check
    11.81%  qemu-linx  qemu-linx                [.] helper_handle_exec_and_branch
    11.58%  qemu-linx  qemu-linx                [.] helper_lookup_tb_ptr
    11.03%  qemu-linx  qemu-linx                [.] cpu_exec
     6.14%  qemu-linx  qemu-linx                [.] cpu_tb_exec
 
性能翻倍。这还没有修改mask check。我让他再删掉这个check，这次结果是这样的：::

  real    0m18.293s
  user    0m18.274s
  sys     0m0.020s

  16.62%  qemu-linx  qemu-linx                [.] helper_lookup_tb_ptr
  16.23%  qemu-linx  qemu-linx                [.] cpu_exec
  15.02%  qemu-linx  qemu-linx                [.] helper_handle_exec_and_branch
   8.93%  qemu-linx  qemu-linx                [.] cpu_tb_exec
   6.39%  qemu-linx  qemu-linx                [.] helper_blk_do_recovery
   4.79%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000000
   1.76%  qemu-linx  qemu-linx                [.] cpu_get_tb_cpu_state
   1.69%  qemu-linx  qemu-linx                [.] check_for_breakpoints
   1.65%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000018
   0.83%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000022
   0.71%  qemu-linx  qemu-linx                [.] tcg_bp_exec_tb
   0.61%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000001
   0.47%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a5400001f
   0.46%  qemu-linx  qemu-linx                [.] tcg_gen_code
   0.39%  qemu-linx  qemu-linx                [.] liveness_pass_1
   0.28%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a5400002a
   0.25%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a5400002c
   0.21%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a5400000d
   0.16%  qemu-linx  qemu-linx                [.] tcg_optimize
   0.16%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000006
   0.16%  qemu-linx  libglib-2.0.so.0.7200.1  [.] g_hash_table_lookup
   0.14%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000004
   0.11%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000024
   0.11%  qemu-linx  qemu-linx                [.] init_ts_info
   0.09%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000028
   0.08%  qemu-linx  libglib-2.0.so.0.7200.1  [.] g_hash_table_insert
   0.07%  qemu-linx  qemu-linx                [.] la_cross_call
   0.06%  qemu-linx  qemu-linx                [.] riscv_tr_translate_insn
   0.06%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000002
   0.05%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000014
   0.04%  qemu-linx  qemu-linx                [.] copy_propagate.isra.0
   0.04%  qemu-linx  qemu-linx                [.] tb_gen_code
   0.04%  qemu-linx  [JIT] tid 234372         [.] 0x00007f7a54000008
   0.04%  qemu-linx  qemu-linx                [.] tcg_out_opc
   0.04%  qemu-linx  libglib-2.0.so.0.7200.1  [.] g_int64_hash
  
数据再下降30%，总计下降75%，从一分半钟下降到18秒，但对比RV不到1秒，这还是不够看。

我把两者对比贴在一起::

  16.62%  qemu-linx                [.] helper_lookup_tb_ptr             |     10.03%  qemu-riscv64             [.] helper_lookup_tb_ptr
  16.23%  qemu-linx                [.] cpu_exec                         |      7.16%  qemu-riscv64             [.] tcg_gen_code
  15.02%  qemu-linx                [.] helper_handle_exec_and_branch    |      3.79%  qemu-riscv64             [.] liveness_pass_1
   8.93%  qemu-linx                [.] cpu_tb_exec                      |      1.60%  qemu-riscv64             [.] tcg_optimize
   6.39%  qemu-linx                [.] helper_blk_do_recovery           |      1.26%  qemu-riscv64             [.] check_for_breakpoints
   4.79%  [JIT] tid 234372         [.] 0x00007f7a54000000               |      1.09%  qemu-riscv64             [.] cpu_get_tb_cpu_state
   1.76%  qemu-linx                [.] cpu_get_tb_cpu_state             |      0.87%  qemu-riscv64             [.] la_cross_call
   1.69%  qemu-linx                [.] check_for_breakpoints            |      0.86%  qemu-riscv64             [.] riscv_tr_translate_insn
   1.65%  [JIT] tid 234372         [.] 0x00007f7a54000018               |      0.84%  qemu-riscv64             [.] tcg_out_opc
   0.83%  [JIT] tid 234372         [.] 0x00007f7a54000022               |      0.69%  qemu-riscv64             [.] init_ts_info
   0.71%  qemu-linx                [.] tcg_bp_exec_tb                   |      0.67%  libglib-2.0.so.0.7200.1  [.] g_hash_table_insert
   0.61%  [JIT] tid 234372         [.] 0x00007f7a54000001               |      0.58%  qemu-riscv64             [.] tcg_out_op
   0.47%  [JIT] tid 234372         [.] 0x00007f7a5400001f               |      0.55%  libglib-2.0.so.0.7200.1  [.] g_hash_table_lookup
   0.46%  qemu-linx                [.] tcg_gen_code                     |      0.54%  qemu-riscv64             [.] tb_gen_code
   0.39%  qemu-linx                [.] liveness_pass_1                  |      0.49%  qemu-riscv64             [.] tcg_reg_alloc
   0.28%  [JIT] tid 234372         [.] 0x00007f7a5400002a               |      0.44%  qemu-riscv64             [.] copy_propagate.isra.0
   0.25%  [JIT] tid 234372         [.] 0x00007f7a5400002c               |      0.41%  qemu-riscv64             [.] tb_tc_cmp
   0.21%  [JIT] tid 234372         [.] 0x00007f7a5400000d               |      0.38%  qemu-riscv64             [.] tcg_out_sib_offset
   0.16%  qemu-linx                [.] tcg_optimize                     |      0.33%  qemu-riscv64             [.] tcg_op_alloc
   0.16%  [JIT] tid 234372         [.] 0x00007f7a54000006               |      0.32%  qemu-riscv64             [.] tcg_emit_op
   0.16%  libglib-2.0.so.0.7200.1  [.] g_hash_table_lookup              |      0.30%  [vdso]                   [.] 0x00000000000006e5
   0.14%  [JIT] tid 234372         [.] 0x00007f7a54000004               |      0.29%  [kernel.kallsyms]        [k] 0xffffffffa20a2517
   0.11%  [JIT] tid 234372         [.] 0x00007f7a54000024               |      0.29%  libc.so.6                [.] ____wcstold_l_internal
   0.11%  qemu-linx                [.] init_ts_info                     |      0.27%  qemu-riscv64             [.] tcg_temp_free_internal
   0.09%  [JIT] tid 234372         [.] 0x00007f7a54000028               |      0.27%  qemu-riscv64             [.] translator_lduw_swap
   0.08%  libglib-2.0.so.0.7200.1  [.] g_hash_table_insert              |      0.27%  qemu-riscv64             [.] finish_folding
   0.07%  qemu-linx                [.] la_cross_call                    |      0.26%  qemu-riscv64             [.] tcg_constant_internal
   0.06%  qemu-linx                [.] riscv_tr_translate_insn          |      0.22%  [JIT] tid 5852           [.] 0x00007f10ec14b740
   0.06%  [JIT] tid 234372         [.] 0x00007f7a54000002               |      0.22%  qemu-riscv64             [.] tcg_out_st
   0.05%  [JIT] tid 234372         [.] 0x00007f7a54000014               |      0.22%  qemu-riscv64             [.] translator_loop
   0.04%  qemu-linx                [.] copy_propagate.isra.0            |      0.22%  qemu-riscv64             [.] cpu_exec
   0.04%  qemu-linx                [.] tb_gen_code                      |      0.22%  libc.so.6                [.] ____wcstof_l_internal
   0.04%  [JIT] tid 234372         [.] 0x00007f7a54000008               |      0.20%  qemu-riscv64             [.] riscv_tr_insn_start
   0.04%  qemu-linx                [.] tcg_out_opc                      |      0.19%  qemu-riscv64             [.] fold_const2_commutative.constprop.0
   0.04%  libglib-2.0.so.0.7200.1  [.] g_int64_hash                     |      0.18%  qemu-riscv64             [.] page_find_alloc

我从这里看到这几个要点：

1. 无论是谁lookup_tb_ptr的成本都很高，而我们每个块头都要做一次，这有必要chained
   起来。这个地方王州正在改，不过暂时有segment fault，要细看看代码。

2. helper_handle_exec_and_branch占比很高，这是commit的处理，有很多复位的操作，
   我们原来觉得无所谓的，反正多复位一下东西无所谓，但既然它占比这么高，我们可以
   优化一下，很多不需要做的初始化都可以删除的。

3. helper_blk_do_recovery占比很高，这不应该啊，我们没有那么多的中断，怎么会有那
   么多的recovery呢？我看了一下代码，发现我们这个helper是每个块都要进入一次的，
   因为是否recover，是要看当前的TPC，而TPC是个动态的值，静态没法决定是不是recover。
   但这个就会造成每次TB都chained不起来，我猜性能影响是很大的。

4. 我们的跟踪里面很多地方都集中了JIT的代码，这似乎可以认为我们的中间代码时间占
   比很高，这个还想不出原因，明天再说。

5. 我们的cpu_exec的占比也很高，这个比较奇怪，大家都是一样的函数，为什么会这里占
   比高呢？我对比了一下两个实现，发现唯一可能的是我们多了一个inline的
   linx_debug_check_tb，这东西影响性能吗？今天我还没有贾文杰手上的Patch，明天修
   改看看吧。

20220202
--------

今天我们先来看看JIT是什么意思，这个模块是怎么计算出来的。这些JIT的地址都在
0x00007f7a54000000附近，而qemu-linx的map是这样的：::

  00010000-002f6000 r--p 00000000 fd:01 11609171                           /home/kenny/work/linx-qemu-dev/qemu_perf_test/test.linx
  002f6000-002f7000 ---p 00000000 00:00 0 
  002f7000-002f8000 r--p 002e6000 fd:01 11609171                           /home/kenny/work/linx-qemu-dev/qemu_perf_test/test.linx
  002f8000-002fa000 rw-p 002e7000 fd:01 11609171                           /home/kenny/work/linx-qemu-dev/qemu_perf_test/test.linx
  002fa000-0031d000 rw-p 00000000 00:00 0 
  4000000000-4000001000 ---p 00000000 00:00 0 
  4000001000-4000801000 rw-p 00000000 00:00 0 
  4000801000-4000802000 r--p 00000000 00:00 0 
  557be2ce6000-557be2d42000 r--p 00000000 fd:01 125572920                  /home/wenjie/BlockQEMU/LinxBlockModel/build/qemu-linx
  557be2d42000-557be2ed5000 r-xp 0005c000 fd:01 125572920                  /home/wenjie/BlockQEMU/LinxBlockModel/build/qemu-linx
  557be2ed5000-557be3195000 r--p 001ef000 fd:01 125572920                  /home/wenjie/BlockQEMU/LinxBlockModel/build/qemu-linx
  557be3195000-557be31d4000 r--p 004ae000 fd:01 125572920                  /home/wenjie/BlockQEMU/LinxBlockModel/build/qemu-linx
  557be31d4000-557be3208000 rw-p 004ed000 fd:01 125572920                  /home/wenjie/BlockQEMU/LinxBlockModel/build/qemu-linx
  557be3208000-557be3225000 rw-p 00000000 00:00 0 
  557be323d000-557be34b1000 rw-p 00000000 00:00 0                          [heap]
  7f1af0000000-7f1af7fff000 rwxp 00000000 00:00 0 
  7f1af7fff000-7f1af8000000 ---p 00000000 00:00 0 
  7f1af8000000-7f1af8021000 rw-p 00000000 00:00 0 
  7f1af8021000-7f1afc000000 ---p 00000000 00:00 0 
  7f1afcfc3000-7f1afd145000 rw-p 00000000 00:00 0 
  7f1afd145000-7f1afd146000 ---p 00000000 00:00 0 
  7f1afd146000-7f1afd94b000 rw-p 00000000 00:00 0 
  7f1afd94b000-7f1afd94d000 r--p 00000000 fd:00 6166638                    /usr/lib/x86_64-linux-gnu/libffi.so.8.1.0
  7f1afd94d000-7f1afd954000 r-xp 00002000 fd:00 6166638                    /usr/lib/x86_64-linux-gnu/libffi.so.8.1.0
  7f1afd954000-7f1afd955000 r--p 00009000 fd:00 6166638                    /usr/lib/x86_64-linux-gnu/libffi.so.8.1.0
  7f1afd955000-7f1afd956000 ---p 0000a000 fd:00 6166638                    /usr/lib/x86_64-linux-gnu/libffi.so.8.1.0
  7f1afd956000-7f1afd957000 r--p 0000a000 fd:00 6166638                    /usr/lib/x86_64-linux-gnu/libffi.so.8.1.0
  7f1afd957000-7f1afd958000 rw-p 0000b000 fd:00 6166638                    /usr/lib/x86_64-linux-gnu/libffi.so.8.1.0
  ...                                                                      动态库的空间忽略
  7f1afe4ac000-7f1afe4ae000 rw-p 00039000 fd:00 6179972                    /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
  7f7a54000000                                                             这个JIT的位置在这里，但空间没有映射，不知道怎么回事。
  7ffd7dcac000-7ffd7dccd000 rw-p 00000000 00:00 0                          [stack]
  7ffd7dcd6000-7ffd7dcda000 r--p 00000000 00:00 0                          [vvar]
  7ffd7dcda000-7ffd7dcdc000 r-xp 00000000 00:00 0                          [vdso]
  ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0                  [vsyscall]

perf不知道从什么地方得到这个JIT的标记，我猜要不是perf自己加的，凡是代码段在数据
段里面，一概认为是JIT。

然后我们打开几个函数看看内部：

其中helper_lookup_tb_ptr的占比最高的是前面两条语句：::

  const void *HELPER(lookup_tb_ptr)(CPUArchState *env)
  {
     20.69 │       endbr64
     21.12 │       push    %r14
                   push    %r13
      0.65 │       push    %r12
     ...
  }

endbr64是End Branch 64bit，可以当做不存在，本意是在这里放一个标记，如果没有这个
标记就不让跳过来，这里占比这么高，我只能怀疑是Intel做这种检查挺花时间的。这说明
helper函数的成本是比较高的，轻易不应该用。

第一个push占比高，后面的都比较低，只能认为是cacheline操作时间长，所以，这个函数
本身成本不高，真正高的应该是数量。我们可以在函数里面统计一下两个平台中调用这个
函数的个数区别。如此类推，我们还可以统计一下执行的TB的数量差别（但Chained以后就
统计不了了，不过也有参考价值）。

cpu_exec的占比主要是几个内存操作（不是我们增加的调试函数），这些RV都有，所以只
能也认为，我们调用了更多的cpu_exec()，也就是我们被打断的次数很多。这也是我们要
统计的。这个定位也顺带说明了cpu_tb_exec()的成本是哪里来的。

最后就是这JIT的占比是怎么上来的，我猜做同一个行为，我们不见得更花时间，有没有可
能是我们的编译器就产生了这么多代码？这个地方又需要一个统计项。

另外就是今天团队和编译器确认了如下信息：

1. 当前的qemu和编译已经支持块内跳转的指令了。
2. 但编译器不会自动生成块内跳转的代码。
3. 但手写汇编可以写块内跳转代码。

所以静态映射T寄存器这个优化无法直接实施。作为一个取巧的解决方案，我建议现阶段和
编译器团队在块头里面约定一个bit，用于表示块中是否使用了跳转。

综合一下，前面的信息，我觉得重点还是要看我们的运行数据。我想到这样一些统计：

1. 和RV对比，RV完成测试用了执行了多少指令，我们执行了多少头，多少微指令。但这个
   东西一旦chain好像就没法统计了。
2. RV进行了多少次TB翻译，我们进行了多少次。这个容易统计。
3. RV退出了多少次TB chain我们退出了多少次。

第一个比较难搞，就先不搞了，我先统计了一下后面两个，结果如下：::

  RV: TB exec = 2638636,   trans = 11608
  LX: TB exec = 272126052, trans = 34452
      trans header of atomic = 8, 0body = 298, other = 17023

所以，现在的问题是我们的翻译多了3倍，执行多了100倍（我猜是没有chain引起的）。其
中普通的头就翻译了17023个，如果每个非0body块头一个body算，应该还有17031个，总共
有34062个，实际翻译了34452次，说明只有少数（390）个body被拆开了。

真的需要翻译这么多头吗？我看了一下测试程序的header的个数，bstart的个数是68228
（concat的个数是62743个，说明几乎都是大头），这个量纲是可理解的。但翻译这么多的
头，都不是在做执行，也就意味着我们就这个部分的行为是比RV额外多出来的成本。

不过无论如何，翻译不是占比的大头，一次次退出TB才是我们的问题的关键。这样，问题又
回到能不能把尽量多的TB Chain起来。

首先块头和块身能不能直接chain？块头有块身的字节地址，如果没有动态修改代码，这应
该是可以直接chain的。但就怕这个头要recover，这样就不能chain了。但我们翻译块头的
时候是不知道这个块头要不要recover的。所以，这里是个动态的行为，没法优化，RV下
indirect跳转也没法chain，必须靠Lookup TB然后jump。这个我们一样。

但用户态模拟不处理中断，这个模式下我们可以直接chain。

然后是一个块执行完了，能不能直接chain下一个块？如果本块是直接跳转的，其实我们也
是可以直接chain过去的，但贾文杰提出一个问题：是否跳转是Header决定的，如果两个
Header共享同一个body，这个body就没法静态chain到下一个块上了。

要解决这个问题，要不让编译器暂时不要产生这样的代码，要不就是查找TB的时候要加上
块头的地址作为key，而且严肃写的时候要判断这个header所在的页有没有被更新过，如果有，
相关TB也要invalidate。

综合来说，我们可以做下面几个危险动作，用一个选项来选择，开启后只用于测试，这样
应该就是可以的：

1. 仅支持user模式。
2. 删除跨页不能使用goto_tb的判断条件
3. 全部使用无块内跳转的操作，对这些块使用静态索引访问寄存器。请编译把有块内跳转
   的块换一种类型。
4. 关掉寄存器掩码检查
5. 块头到块身使用goto_tb连起来
6. 让编译器不要复用body（我猜现在就没有），对于direct和fall through的块的commit
   和下一个块头也用goto_tb连起来
7. helper_handle_exec_and_branch实现优化。
8. 去掉user模式中的recovery行为。

编译器的事情后续李飘去沟通，其他工作安排如下：

1. 王州拉一个分支出来，增加功能选项
2. 文杰把原来的静态索引和关掩码检查修改一下合上去
3. 王州负责块头到块身的chain（同时关掉page和recovery检查）
4. 李国柱负责块身到下个块头的chain？
5. 李飘负责helper_handle_exec_and_branch的优化。

20220203
========

今天完成昨天的修改，逐步测试的数据是这样的：

（最初我们执行时间是73.987s，RV是0.455s。TB进入数，我们是272,126,052，RV是2,638,636）

1. 仅增加块头到块身的Chain，执行时间降低到56.810s，TB进入数是135,652,780次。
2. 再增加块内静态偏移的优化，执行时间降低到12.58s，TB进入数不变。
3. 再增加块身到下一个块的Chain，执行时间降低到9.829s，TB进入数下降到90,247,697次。

性能有了大幅的提升，但还是远远比不上RV，我们的执行时间是RV的21倍，RV执行一分钟
的的程序，我们需要执行21分钟——好像勉强还能用：比原来需要两个半小时好多了:）。

现在的压力分布是这样的：::

  28.64%  qemu-linx                [.] helper_handle_exec_and_branch
  20.17%  qemu-linx                [.] cpu_exec
  11.46%  qemu-linx                [.] cpu_tb_exec
   6.19%  [JIT] tid 890551         [.] 0x00007f92ec000000
   1.40%  [JIT] tid 890551         [.] 0x00007f92ec000018
   1.34%  qemu-linx                [.] cpu_get_tb_cpu_state
   1.12%  [JIT] tid 890551         [.] 0x00007f92ec000022
   0.95%  qemu-linx                [.] tcg_gen_code
   0.92%  [JIT] tid 890551         [.] 0x00007f92ec000001
   0.67%  qemu-linx                [.] liveness_pass_1
   0.62%  [JIT] tid 890551         [.] 0x00007f92ec00001f
   0.61%  qemu-linx                [.] tcg_bp_exec_tb
   0.33%  qemu-linx                [.] tcg_optimize
   0.32%  libglib-2.0.so.0.7200.1  [.] g_hash_table_lookup
   0.30%  [JIT] tid 890551         [.] 0x00007f92ec00002a
   0.29%  qemu-linx                [.] check_for_breakpoints
   0.28%  [JIT] tid 890551         [.] 0x00007f92ec00000d
   0.28%  [JIT] tid 890551         [.] 0x00007f92ec00002c
   0.20%  qemu-linx                [.] init_ts_info
   0.19%  [JIT] tid 890551         [.] 0x00007f92ec000028
   0.18%  [JIT] tid 890551         [.] 0x00007f92ec000024
   0.15%  libglib-2.0.so.0.7200.1  [.] g_hash_table_insert
   0.14%  [JIT] tid 890551         [.] 0x00007f92ec000006
   0.14%  [JIT] tid 890551         [.] 0x00007f92ec000004
   0.12%  qemu-linx                [.] riscv_tr_translate_insn

还有个高高在上的helper_handle_exec_and_branch()等李飘的合入。

其他的还有一个是为什么我们的JIT占比特别高，这是不算helper的时间的统计，这只能说，
我们生成的代码的效率就不够高。

我做了一些排查，注意到这样一些点：

1. Header生成了相当多的代码，几乎把Head的每个域都生成了一个mov语句了。这些都是
   为了跳到header的TB上初始化的一组值，但这些内容我们不一定用，有些其实可以不读
   （比如不做mask检查的时候，可以不用初始化smask/gmask），有些可以到需要的时候
   直接从内存读（比如块的类型）

2. 现在文杰的优化明显是图简单，原来需要动态决定T寄存器，所以计算需要先计算到一
   个temp寄存器中，然后再mov到目标寄存器中，现在完全可以一次计算到目标寄存器中
   啊。

第二个点应该是JIT占比高的原因，这个任务由王州来组织优化（涉及很多函数，需要设计
一下策略）。

还有一个问题是这个0x00007f92ec000000到底是什么？为什么某个JIT的占比这么高？这玩
意儿MB对齐，很像是整个TB空间的入口啊。那么这里就是Prologue的代码？我在代码中加
了一个打印：::

  tcg_region_prologue_set to 0x7f9420000000 code_ptr=0x7f942000002d

再对应新的perf report，这个就是最高占比那个JIT的位置。而对比TB，TB中prologue的
占比就根本排不上号。

prologue是为了让cpu_exec调用cpu_tb_exec离开的时候可以回到原来的上下文设计的，只
要你能chain在一起，就算不是goto_tb而是lookup_goto_ptr，都不需要调用。所以，这说
到底还是有太多exit tb了，我们应该还有很大的优化余地的。

我review了一次所有的退出TB的点，发现我原来的修改太保守了，所以我重新调整了一下，
这次结果是这样的：::

  kenny@kl-dev:~/work/linx-qemu-dev/qemu_perf_test$ time ../LinxBlockModel/build/linx-linux-user/qemu-linx -enable-force-tb-chained ./test.linx
  kenny: TB exec = 36571, trans = 34468
  kenny: TB invalidate = 0
  kenny: trans header of atomic = 8, 0body = 302, other = 17029
  
  real    0m5.213s
  user    0m5.202s
  sys     0m0.012s

TB进入数从90,247,697下降到36,571。时间接近腰斩。我觉得这个基本上就可用了。

最后记录一下perf数据：::

  57.94%  qemu-linx  qemu-linx                   [.] helper_handle_exec_and_branch
   1.83%  qemu-linx  qemu-linx                   [.] tcg_gen_code
   1.37%  qemu-linx  qemu-linx                   [.] liveness_pass_1
   0.71%  qemu-linx  qemu-linx                   [.] tcg_optimize
   0.53%  qemu-linx  libglib-2.0.so.0.7200.1     [.] g_hash_table_lookup
   0.34%  qemu-linx  qemu-linx                   [.] init_ts_info
   0.26%  qemu-linx  libglib-2.0.so.0.7200.1     [.] g_hash_table_insert
   0.24%  qemu-linx  libglib-2.0.so.0.7200.1     [.] g_int64_hash
   0.21%  qemu-linx  qemu-linx                   [.] tcg_opt_gen_mov
   0.17%  qemu-linx  qemu-linx                   [.] copy_propagate.isra.0
   0.17%  qemu-linx  qemu-linx                   [.] tcg_constant_internal
   0.17%  qemu-linx  qemu-linx                   [.] la_cross_call
   0.16%  qemu-linx  qemu-linx                   [.] riscv_tr_translate_insn
   0.14%  qemu-linx  qemu-linx                   [.] tcg_out_opc
   0.14%  qemu-linx  libc.so.6                   [.] ____wcstof_l_internal
   0.13%  qemu-linx  qemu-linx                   [.] tcg_op_alloc
   0.12%  qemu-linx  qemu-linx                   [.] tcg_out_op
   0.11%  qemu-linx  qemu-linx                   [.] decode_block
   0.11%  qemu-linx  libc.so.6                   [.] ____wcstold_l_internal
   0.10%  qemu-linx  qemu-linx                   [.] tb_gen_code
   0.10%  qemu-linx  qemu-linx                   [.] tcg_reg_alloc_bb_end.constprop.0
   0.10%  qemu-linx  qemu-linx                   [.] temp_sync
   0.09%  qemu-linx  qemu-linx                   [.] tcg_out_sib_offset
   0.09%  qemu-linx  qemu-linx                   [.] translator_loop
   0.09%  qemu-linx  qemu-linx                   [.] tcg_emit_op
   0.08%  qemu-linx  qemu-linx                   [.] finish_folding
 
这个数据就比较合理了。

20230206
========

周末李飘等完成了部分helper_handle_exec_and_branch的合入，这个数据是这样的：::

  kenny: TB exec = 36571, trans = 34468
  kenny: TB invalidate = 0
  kenny: trans header of atomic = 8, 0body = 302, other = 17029
  
  real    0m2.270s
  user    0m2.258s
  sys     0m0.013s

  4.21%  qemu-linx  qemu-linx                [.] tcg_gen_code
  3.08%  qemu-linx  qemu-linx                [.] liveness_pass_1
  1.37%  qemu-linx  libglib-2.0.so.0.7200.1  [.] g_hash_table_lookup
  1.25%  qemu-linx  qemu-linx                [.] tcg_optimize

这个补丁把那个helper的大部分行为都直接用中间码代替了，性能再上升一倍多。但这个
补丁会导致system模式崩溃，王州进行了一些排错，用了更保守的多余设置，但这样速度
会慢一些，所以这个问题今天没有关闭。

我在周末对之前的工作做了一下总结。这个总结让我意识到我们原来性能不足主要都来自
相对T寄存器查找和退出TB，不是因为goto_ptr和goto_tb的性能差异。所以今天我试着去
掉那个危险的goto_tb判断，再测试了一下。这次的结果是这样的：::

  kenny: TB exec = 272125335, trans = 34468
  kenny: TB invalidate = 0
  kenny: trans header of atomic = 8, 0body = 302, other = 17029
  
  real    0m13.906

  42.72%  qemu-linx  qemu-linx                [.] helper_lookup_tb_ptr
  20.78%  qemu-linx  qemu-linx                [.] helper_handle_exec_and_branch
   3.70%  qemu-linx  qemu-linx                [.] check_for_breakpoints
   2.16%  qemu-linx  qemu-linx                [.] cpu_get_tb_cpu_state
   0.70%  qemu-linx  qemu-linx                [.] tcg_gen_code
   0.54%  qemu-linx  qemu-linx                [.] liveness_pass_1
   0.27%  qemu-linx  qemu-linx                [.] tcg_optimize
   0.19%  qemu-linx  libglib-2.0.so.0.7200.1  [.] g_hash_table_lookup
   0.14%  qemu-linx  qemu-linx                [.] init_ts_info
   0.12%  qemu-linx  qemu-linx                [.] la_cross_call

这里看来虽然没有了很多的exit tb（Prologue占比不高），但TB exec又加上去了，而且
helper_lookup_tb_ptr也上去了，说明goto_ptr这种方法本身的成本也是很高的。我对
Helper的成本做了一个分析，放在本文的附录中了。


问题总结
========

本Issue在20230202打开，预期在20230210前关闭。我们有办法把原来需要73s完成的程序
优化到2s左右。比标杆RV的0.5s还是差一点，但基本可用了。

现在总结一下我们这次的修改的适用范围：

1. T寄存器绝对寻址：这依赖无块内中断和块内跳转不跨越跳转点访问T寄存器。这只能成
   为一种约定，通过qemu参数决定用这种hacking的方法访问。

2. 去掉对g/smask的检查：这违反语义，但不影响正常的程序工作。也只能成为一种约定，
   通过qemu参数关闭这种检查。

3. 把无body Header和下一个Header Chain起来：通用。

4. 把Header和Body Chain起来：仅适用无块内中断的情形（User模式除了系统调用天然无
   块内中断）。

   这个地方的系统模式可以优化：我们原来是在这个地方产生一个恢复检查，然后退出TB，
   但其实我们可以用中间代码做这个检查，如果需要恢复才退出TB，否则我们可以直连过
   去。

5. 把Body和下一个Header Chain起来：通用。

6. 去掉goto_tb的检查：只能用于无自修改代码的情形，包括多进程的时候无重新分配的
   进程，也只能作为一种约定，通过qemu参数决定。

7. 用中间代码代替helper_handle_exec_and_branch的行为：通用。

还有一个优化我们没有实施：

1. T寄存器绝对寻址的时候没有删除多余的操作（修改成本的原因）

这些修改还是Hacker成分为主，只能用于性能模型，大部分功能不能成为我们主流方案。
所以，在Qemu平台上，Linx的TCG模拟速度可以长期比其他平台差。这包括一个前提和几个
原因。

前提是：Qemu的速度来自它把译码和执行分开了。传统的模拟器是解释一条指令，执行这
条指令，这样需要花相当多的时间解释指令。而Qemu的方法是把译码的结果放在缓冲中，
通过复用这些缓冲获得性能。在理想的情况下，程序运行稳定后，基本就不需要译码了。

而Linx块指令的几个特点，让它在上面的假设下吃亏了：

1. T寄存器寻址导致TCG无法在译码阶段指定确切的寄存器，这样无法让翻译的结果没法被
   再次使用，只能动态计算运算寄存器，这没有对应的中间代码支持。这种情况要不使用
   成本高昂的helper函数（现在就是这样），要不要为Qemu增加中间指令，增加中间指令
   就要为大部分平台实现这些中间指令。而且即使这样，因为这毕竟是额外的寻址，天然
   还是会比传统指令慢。

   不过这一点，只要花点时间，我觉得还是可以在少数平台上优化好的，不过必须投入一
   个相当有经验的人才行。

2. 译码流在块头和块之间流转，而两者不一定在同一个页中。这导致TCG不敢假设目标代
   码还是原来的代码，遇到这种情况必须重新判断目标代码是否有效，这个每次都发生的
   判断（用goto_ptr代替goto_tb，后者不判断TB是否有效，前者需要进行TB关键字查
   找，这个Hash查找相当消耗时间），大大拖慢了执行的速度。

3. 每次访问寄存器都要判断smask和/gmask是否一致，这在硬件实现的时候可以做并行，
   但软件必须串行，这也大大拖慢了它的执行速度。

总的来说，块指令太多动态的内容无法在译码阶段决定了，和Native平台差距太大，导致
它用Native平台模拟的成本也相应提高了。

附录
====

Helper函数的调用成本
--------------------

gen_helper_xxx函数调用的是tcg_gen_call_N()，这个函数把Guest的参
数，转换为Host的参数（记录在op中），生成一个中间代码INDEX_op_call。
最后用tcg_reg_alloc_call完成整个翻译，在后面这个函数里面，你会看到
它的成本包括：

1. 准备和释放堆栈
2. 把内存中的参数变成寄存器中的参数
3. 同步内存中的寄存器和实际的寄存器
4. 保存和恢复所有caller save寄存器

这样说起来，一个helper的成本确实比你直接用几条指令把数据从一个内存中读出来，完
成计算然后写回去的成本要高得多。
