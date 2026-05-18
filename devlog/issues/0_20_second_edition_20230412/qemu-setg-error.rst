set.g 指令实现问题分析
**********************

介绍
=====

本篇主要是记录并分析 set.g 指令qemu实现问题，以供后来者了解情况。

记录人：韩志林 30043474

20230413
========

编译器版本(0413): http://10.175.104.61:8888/other/bisa_share/temp/llvm_v0.20.1_0413/
现象：执行所有的specint子项，qemu代码仓中的自测集均出现 segment fault。
定位过程：

1. 找到出错指令 set.g
   执行出现段错误，qemu执行添加 "-d in_asm,exec,cpu" 参数输出日志，看到是一条
   lbu 指令直接导致的段错误。显然，是a0的值不对，继续往上溯源一个块，a0的值是来
   源于a2寄存器，a2的值为 0x0000004000800330，a0的值执行完0x1a560这个TB块值依旧
   没有变成 0x0000004000800330，可见是set.g指令实现有误，无法完成赋值操作。

   ..code::

        0x000000000005fce8:  20e6              get             a0
        0x000000000005fcea:  0048              lbu             [t#1, 0]

   ..code::

        0x000000000001a560:  42e7              set.g           a2, a0
        0x000000000001a562:  53e7              set.g           s0, a1

2. 分析qemu指令的实现 set.g
   set.g指令的实现如下，借助helper函数打印执行结果发现，set.g指令的实现无误，
   确实可以将a2的值写到a0中去，但是在执行完整个TB块之后，a0的值又会被写回原来的
   值。猜测是提交阶段出的问题。继续看提交阶段把local gpr写回global gpr的实现，
   发现实现方式是根据 env->smask 的值判断将哪个local gpr写回global gpr。这种实现
   方式是有问题的，因为set.g指令实现中的更新smask的操作是通过中间码实现的，意味
   着 env->smask 的值是在执行阶段更新的，而在翻译阶段的 linx_gen_blk_commit
   函数中，因为当前在翻译阶段，所以 env->smask 的值还是原来解码块头得到的值，并
   没有因为执行了set.g指令而更新(当前处于翻译阶段)，(假设前面是set.g a1)，所以
   这里会再次set local gpr(如 a1)到global gpr。

   ..code::

        static bool trans_blk_set_g_d(DisasContext *ctx, arg_blk_set_g_d *a)
        {
            ...
            TCGv t = get_blk_s(ctx, a->RegSrcL);
            tcg_gen_mov_tl(cpu_gpr[a->RegSrcR], t);
            // 更新smask，防止commit阶段再次set global。
            tcg_gen_andi_i32(smask, smask, ~(1 << a->RegSrcR));
            return true;
        }

   ..code::

        static void linx_gen_blk_commit(CPURISCVState *env, DisasContext *ctx)
        {
            ...
            /* commit local state to gpr after mask check */
            for (int i = 0; i < bset_bit_num; i++) {
                if (env->smask & (1 << i)) {
                    tcg_gen_mov_tl(cpu_gpr[i], blk_lgpr[i]);
                }
            }
            ...
        }

3. 代码修改策略。
   linx_gen_blk_commit 中的 local gpr 写回的操作用helper函数实现。因为helper函数
   是在执行阶段执行的，生成的只是一条 "call func" 这种形式的中间码。修改后 qemu
   代码仓中的自测集测试通过，memcpy自测集测试通过。

   ..code::

        void helper_update_gpr(CPURISCVState *env, uint32_t bset_bit_num)
        {
            for (int i = 0; i < bset_bit_num; i++) {
                if (env->smask & (1 << i)) {
                    env->gpr[i] = env->blk_lgpr[i];
                }
            }
        }

4. 影响。
   因为每个块执行完所有的微指令后，也就是commit阶段，只要存在要写回的local gpr，
   那么就需要生成一个"call helper_update_gpr"这种中间码，qemu的执行效率会降低。
