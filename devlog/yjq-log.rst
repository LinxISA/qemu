yjq开发日志
***********
2023/04/10
==========
更新一下v0.20的掩码检查规则。

之前v0.16的检查规则为：

1.get类指令不检查掩码（允许获得所有local gpr的值）

set类指令包括SET，SET.G，SET.GL三种

set local gpr时不进行掩码的检查，set GPR的时候，进行如下的掩码检查：

 1. set GPR检查对应的set bit是否置1，如果没有则报LINX_EXCP_SET_REGS异常
 2. 不允许一个块内对同一GPR进行重复set，报LINX_EXCP_DUP_SET异常
 3. CALL和INDIRECT_CALL类型的块会在commit的时候进行RA的set，所以不允许这种块
    有set ra的情形，报LINX_EXCP_DUP_SET异常

当前v0.20的掩码检查规则改为：

get 指令，从local gpr获取值，需要进行相应的get bit检查，看是否置1，否则报
LINX_EXCP_GET_REGS异常

set指令，set local gpr的时候不进行掩码检查，set GPR的时候，进行如下的掩码检查：

 1. set GPR检查对应的set bit是否置1，如果没有则报LINX_EXCP_SET_REGS异常
 2. 不允许一个块内对同一GPR进行重复set，报LINX_EXCP_DUP_SET异常
 3. CALL和INDIRECT_CALL类型的块会在commit的时候进行RA的set，所以不允许这种块有set ra
    的情形，报LINX_EXCP_DUP_SET异常

这里用的掩码不完全是head里面的掩码，set mask先从haed中获取，如果翻译的时候遇到set
GPR的操作之后，把对应的bit置0(不允许重复set GPR),get mask也先从head获取，get的时候
先检查这个get mask，如果遇到set r1的操作，可以把get mask中对应的bit置为1，允许后面
的微指令进行r1的get。 


2023/04/07
==========
考虑一下怎么重构一下v.20的微指令trans函数，之前看到的问题有这些：

 1. 类似MXL_RV32的用法不对(而且不应    该还起rv这个名字)，这个是说支持的寄存器长度，
    LINX上面寄存器长度都是64.
 2. 所有的指令输出都要新创建一个temp寄存器，这个是不必要的，T寄存器本来就是一个TCG寄存器。
    这个可以后续再搞。
 3. 类似rev16要考虑有没有中间码直接可以做，比如bswap之类。
 4. LINX32/LINX64这个定义不对，linx只有64。
 5. 最好不要做代码复制拷贝，比如又新加一个linx-test/new_test_020的目录出来。
 6. bxu之类的指令考虑直接用中间码，比如，deposite。建议大家把tcg/README的所有中间码
    都看一遍，再写代码。
 7. trap目前为hack设计，后续需要和acr融合。
 8. 类似addw的指令，在计算长度、输入扩展和输出扩展需要有一个小设计，目前的语义错乱的。

    对于输入输出是64bit的情况，我们直接使用对应的TCG寄存器就好，目前v0.20中有很多
    后缀是w的指令，这类指令的特点是输出只取低32bit，做符号扩展后写入64bit的输出寄存器
    中，对于输入参数一般而言是64bit参与计算的，但是也有32bit参与计算的情况，对于
    输入的情况，我们很难做统一的概括，那就针对具体情况具体写代码，把可以公共的代码
    抽出来。所以，这样看，只需要定义一个公共参数，表示一个指令是否带W。

问题1：已经改了，这里就不重复了。

问题2：这个问题，可以拿随便拿一个函数看看：

.. code-block:: c

 static bool gen_blk_arith_a(DisasContext *ctx, arg_arg_a *a, DisasExtend ext,
                       void (*func)(TCGv, TCGv, TCGv))
 {
     TCGv dest = temp_new(ctx);
     TCGv src1 = get_blk_t_ext(ctx, a->LinkL, ext);
     TCGv src2 = get_blk_t_ext(ctx, a->LinkR, ext);

     func(dest, src1, src2);
     set_blk_dest(ctx, dest);
     return true;
 }

这里就没有必要新建一个dest变量来进行计算了，这样写就成了先把计算结果报存到dest这个
临时寄存器中，后面还因为这样加了一条mov的中间码，没有必要。set_blk_dest函数可以
考虑直接干掉，直接用t寄存器进行计算，这几乎涉及了所有微指令，改动地方不少，需要小心

问题3：其实就是rev类指令能不能用bswap类中间码进行转换，看block的定义，考虑rev8指令
用bswap64_i64这个中间码，rev16用bswap16_i64，rev32用bswap32_i64.

目前的实现，rev8已经改了，rev16和rev32需要改一下，验证看这个中间码是否符合。

测试发现，一个0x1122334455667788的立即数，bswap16_i64的结果是8877，bswap32_i64的
结果是88776655，不符合预期。

查了一下资料，例如说bswap16_i64的定义是：

tcg_gen_bswap16_tl(ret, arg1);  ret = ((arg1 & 0xff00) >> 8) | ((arg1 & 0xff) << 8)

也是就是字节交换一个 16 位寄存器,也就是bswap和rev的定义不一致，bswap的三种类型，
其实都是以字节为单位进行交换的，不同的是操作数的长度,有16，32，64三种长度，而rev
指令的操作数长度都是64bit，只是交换单位的改变,有8，16，32三种交换单位。两者不一致，
不能进行简单的替换。也没有别的中间码合适，先放一下这个问题。

问题4：LINX32/LINX64这个定义可能要看看用个什么名字合适，最好能体现位数以及拓展，
所以考虑用b,h,w,d作为长度，加上SIZE,ZERO等表示符号拓展的内容，也就是全局考虑一下，
把所有微指令的长度，符号拓展等统一用一套linx的风格。这个事情可能需要把所有微指令
都过一遍。。。


2023/02/25  
==========
先帮文杰实现一下，V0.20 System Register Access/System Call/Memory Model这三部分的
指令。

1. System Register Access指令，也就是SYSGET/SYSSET这两条指令，0.20的版本没有变动
含义，实际上还是用来进行ssr的读写，只是SYSSET编码上进行了调整。

所以SYSGET/SYSSET指令trans函数不需要改动。

2. System Call类型指令，这个版本的变动，TRAP，WFI没有定义上的变化，原来的ecall，
eret指令不在以微指令的形式体现（直接包含在块头里面了，指令的行为没有变动），除此
之外就是wfe指令，这条指令的行为定义是wait for event，0.16的版本就定义了，但还没有
实现，0.20是否需要实现还待确认，除这些之外，还有个变动就是之前的操作数是reg，现在
改完用imm，这样减少了指令数。

所以TRAP，WFI的trans函数需要改动操作数，ecall，eret的实现挪到head的decode代码部分
里面，并改动操作数，wfe待确认。

3. Memory Model类型指令，这类型的指令定义没有变化，但是0.20没有SFENCE.VMA了？这个
事情还得确认。

所以FENCE.I/FENCE/LR.D/LR.W/SC.D/SC.W这部分指令的trans函数不需要改动。

总的来说，这三部分指令大概率还会有变动，后续acr的设计融合上来之后，但如果光看0.20
的版本，基本上没有变化，影响不大,主要是后面根据acr设计以及相应的larm，这部分的设计
可能还会有所改动。

2023/02/20
==========
确认一下v0.20版本Load,Stack Load,Constant Load这三类指令的改动：

1. 首先看Load类指令，之前load类指令是用[link0] + signed imm索引内存读数，现在这部分
指令不变，多了RegSrcL + imm的索引方式，所以这部分的指令都新增一条就行，操作数改成
用local gpr的，然后load就没什么其他变动了。

2. Stack Load指令，指令的定义跟v0.16一致，沿用就行，忽略

3. Constant Load四条指令，跟v0.16一致，沿用

2023/02/15
==========
考虑一下怎么实现v0.16缩减head长度，首先要改一下insn32.decode，因为编码有变更，head
解码出来的低两位不是11了，原本的在riscv解码流程，把head当成一条32bit的rv指令的方案，
就不适用了，得考虑去掉rv解码的流程，改完rv的部分改完解码块头的部分，这涉及的是 .decode 
文件的修改，以及对应riscv_tr_translate_insn函数中，如何区分head，rv指令，块内
微指令的代码。

考虑一下怎么实现v0.16缩减head长度，首先要能够把块指令解码，之前的head作为一条32bit
的rv指令，用低位0001011来作为识别。块头缩减了，用低三位来区分块类型。

所以首先要做以下的事情：
1. decode_opc中低位为11会走到rv32bit的指令逻辑，其余走到rv16bit的逻辑,现在可以都
走到16bit或者32bit的逻辑，然后先去掉跟块头指令编码冲突的rv指令编码，把各个类型的
head在 .decode文件中写好。

2. 然后是trans_blk_head函数的改动，可以把它作为一个接口，然后让trans_blk_fall_head,
trans_blk_call_head等等这些trans函数调用。

然后到整个块的解析部分，或者说trans_head的内容，之前处理head的内容是：
1. BlockType, 之前每个head都带有BlockType表示块指令的种类，现在例如说64bit的CALL/
CONDITIONAL/INDIRECT/RETURN的块指令的种类是隐藏的，默认为standrad类型，修改不大。

2. BranchType, 还是块的跳转类型，不同的是BranchType代表的块大小变了, pc需要做相应
的变化，之前变动16字节或者32字节，现在是8或者16字节的变化。

3. BNext Offset 和 BText Offset属性，这个没定义的变化，只是长度有改变。还有FALL/
INDIRECT/INDCALL/RETURN类型的块不需要BNext Offset属性了。

4. BGET Mask 和 BSET Mask属性，位置和个数的修改，其余部分在缩减gpr的时候说明。

5. BlockSize值，定义没有发生变化，只有从原本的10bit，变成不同BranchType中，域值
长度有所差别。

6. attribute属性, 原本每种branch的head都有，现在只有concat的时候才会用，要加一层
区分。

新版的head还有以下属性：

1. TP 属性，计算Btext Offset的方式，预留位，当前版本用不上

2. trap , qemu怎么实现这个特性还需要考虑一下,先实现其他的，等确认了再动手

3. hyp属性，标识是否有块内跳转，可以用于性能优化，这个可以留着性能优化用.

然后是寄存器缩减的部分，这部分的变动应该不大

所以需要代码的改动目前有：
1. 新建个.decode文件，加入块头指令的解码流程，之前的decode_opc中会根据条件解码成
rv16bit指令，rv32bit指令以及块头指令，decode_block翻译block的微指令，分成两层，
现在decode_block这一层还是解码微指令就行，然后decode_opc这一层，改成只处理head。

在进入decode_opc之前，bpc1和bpc是无效的，只有执行了decode_opc，解析了head里面的信息
进行初始化之后，它们才有效，这时我们才能进入decode_block进行微指令的翻译。

2. 然后是head的处理，也就是trans_blk函数，BlockType，attribute，BNext Offset这个的处理，
只有部分head才带这三个域，所以使用这三个属性需要根据head进行初始化，head中没有的时候，
默认值为0，这时BlockType默认为standard类型，attribute默认没有，只有concat情况下，
才考虑对应的原子属性，BNext Offset为0，也就是不需要根据它计算下一个块指令。

不过特别的是，v0.16只实现了部分的BlockType，也就是标准块和SEC块，v0.20才跟0.14的块
种类差不多，我的理解的可以先保留其他类型的块指令，等到0.20自然会用上，先保留其他
的block type，新增标准块和SEC块的block type，并且在分支处理的时候，新增它们的处理。
还有个问题，原本只运行标准块出现块内跳转，现在改完新增标准块和SEC块可以有块内跳转。

3. BranchType则表示块的branch类型，这个没有变化，改动一下初始化，然后movi到env里面
的指令执行使用。BlockSize和btext_offset变化了长度，也是初始化，然后movi到env里面

4. BGET Mask 和 BSET Mask属性, 这部分根据gpr的缩减，在其他块中都各自占16bit，call
类型变成只用10个gpr，也是初始化写入env，指令翻译的时候使用

5. trans_blk的改动大概也就2-4点涉及的变量修改，然后head加码完就可以进入微指令的执行，
整个流程是这样，然后就是寄存器修改的部分，我们要修改cpu里面，gpr和local gpr的定义，
没有zero,gp,tp,以及临时t寄存器,以及浮点寄存器，是否同步

这个寄存器除了xT0，其余都没有用到，xT0会用于LINX_EXCP_U_ECALL_32这个异常进行系统
调用的情况下，有可能作为参数来赋值，具体改到哪个寄存器，这个要确认

6. kernel 有用到target_sigcontext这个变量来进行发送信号，打印31个寄存器的值（zero除外），
所以缩减寄存器后，这个变量的gpr个数以及相应打印个数要做出修改

7. riscv64_user_regs这个变量，用来做user状态下包含pc以及gpr，这里要修改，相应的判断
大小以及打印要修改,一般会打印31个寄存器的内容，zero排除，现在的话，按照如今的逻辑，
16个寄存器都是需要的。

8. cpu里面gpr和local gpr个数的修改，tranlate.c文件里面的初始化也要相应修改，
riscv_int_regnames这个按照表格里面的内容重新给寄存器别名，方便识别打印信息

9. debug 调试的代码，helper_linx_debug函数会输出a0，a1，a2寄存器的内容，这三个
寄存器的映射已经改成了gpr2，3，4，所以这部分代码需要修改

2023/02/13
==========
动手完成v0.15的local gpr特性，我的理解主要是增加local gpr来方便set，get指令，达到
缩减指令数目的目的，set指令就变成了类似sget指令。

主要涉及的指令为set，sset，set.g，get，sget，getw，getwu这部分指令，改动是寄存器
的变化，set和sset，get和sget功能重复，融合成一条。（sgetw，sgetwu也一样）

指令的改动主要涉及的是几个trans_blk的改动，需要把src从gpr改成local gpr，反汇编的
改动，主要是编码变化以及指令数目也减少了，print_block_minsn_set和print_block_minsn_get
这些打印需要根据指令码的变动进行调整

然后是块内私有寄存器SR0-SR7的扩充，v0.15先扩充到32个，等0.16缩减块头的时候再改回
16个，相应代码重命名即可。

之前SR0-SR7这部分寄存器，在sp类型的指令有用到，用的是SR0，这个事情孙文博确认是要
改成local gpr中的sp，还有就是分支跳转中用到的函数linx_reset_bstate，之前是直接全部
重置为0，现在我的理解是直接再次从gpr中copy到local gpr.

然后是寄存器的保存和恢复，之前是保存SR0-SR7到ebstate到r10到r17，现在确认了需要保存
到r10到r41，r42保存set_mask（0.16版本再改完16个local gpr）。

关于寄存器的变动，主要涉及了几点：

1.用到了SR0-SR7的指令，主要是sp类型的指令，有用到s[0]，这个事情跟孙文博确认了，改成
直接用gpr中的sp寄存器。

2.这个寄存器的保存以及恢复，在块指令初始化的时候，按照块头中bitmask的寄存器列表，
从第一层GPR复制到块内的SGPR。在块指令提交时, 从块内的sGPR提交到第一层GPR.

那么我的理解是，在trans blk head里面加上gpr值拷贝的操作(按照bitmask)，然后在
helper_mask_check函数后面再加个中间码函数，完成local gpr的提交，进行先检查后
提交的操作。

3.中断异常处理的情况，中间态的local gpr我的理解是需要保存，然后恢复，所以这可以跟
T寄存器一样，用bstate保存，然后再恢复。

还有相关的异常，get命令，会出现两种异常情形：
1. get访问的时候，发现不符合head里面的get_mask，报LINX_EXCP_GET_REGS异常，这个可以
去掉了

2. 块内set Rx后不能再get Rx的的限制，报LINX_EXCP_GET_REGS异常，这个要去掉

set指令会出现这四种异常：
1. set访问的时候，发现不符合head里面的set_mask，报LINX_EXCP_GET_REGS异常，可以去掉

2. 不能出现重复set一个寄存器的情形，报LINX_EXCP_DUP_SET异常，要去除

3. call/incall不能出现set ra寄存器的情形，报LINX_EXCP_DUP_SET异常，这个要保留

4. 实际的set寄存器跟块的set_mask数目和目标对不上，LINX_EXCP_SR_MASK异常，要去除

还有set.g的场景，这个指令作用也就是原本的set，但是要考虑异常情况:

1. set.g访问的时候，发现不符合head里面的set_mask，报LINX_EXCP_GET_REGS异常

2. 不能出现重复set.g 一个寄存器的情形，报LINX_EXCP_DUP_SET异常

3. call/incall不能出现set.g ra寄存器的情形，报LINX_EXCP_DUP_SET异常

然后还有个set_mask的改动，之前这个set_mask的作用是：

1. 用来进行重复set的检查
2. 用来进行get后set的检查
3. 用来保存执行了的set.g信息用于bstate的保存以及恢复
4. 校验是否跟head里面的smask一致

所以这四个特性，根据之前的设计，我们需要保留3，其余的三点限制，可以放开。暂时先保留
set_mask以供第三个特性，不过第三个特性，后续可以用head里面的掩码smask代替，如果已经
set.g 那么可以clear掉smask里面对应寄存器的bit，块commit的时候就不会重复提交到一个gpr了，
这也是一种方式，这样set_mask就可以去掉了

2023/02/04
==========
今天分析了一下之前进行性能优化的大体框架，考虑把相应能复用的代码合入主线。

框架中的修改点主要有以下部分：

1. T寄存器绝对寻址：这依赖无块内中断和块内跳转不跨越跳转点访问T寄存器。这只能成
   为一种约定，通过qemu参数决定用这种hacking的方法访问。

2. 去掉对g/smask的检查：这违反语义，但不影响正常的程序工作。也只能成为一种约定，
   通过qemu参数关闭这种检查。

3. 把无body Header和下一个Header Chain起来：通用。

4. 把Header和Body Chain起来：仅适用无块内中断的情形（User模式除了系统调用天然无
   块内中断）。

5. 把Body和下一个Header Chain起来：通用。

6. 去掉goto_tb的检查：只能用于无自修改代码的情形，包括多进程的时候无重新分配的
   进程，也只能作为一种约定，通过qemu参数决定。

7. 用中间代码代替helper_handle_exec_and_branch的行为：通用。

所以综上，1，2，6的相应代码也就是-enable-force-tb-chained这个参数的对应代码我的理解
是暂时不考虑加入主线，主线没必要用到这部分代码，暂时没有这个需求。（不需要这样约定来加速）

所以其他的修改点，可以过一遍代码把通用的加入主线以提高主线性能。

1. 21caff82 这个patch是user模式没有中断，不需要中断的恢复，可以直接把块头跟
   块body chain起来
2. 3aeafeae 这个patch用于把body跟下一个块的块头chained起来，可以复用
3. 3b380775 减少分支中过多的exit_tb的操作，可以复用于提高主线性能
4. c131bcc1 helper_handle_exec_and_branch的优化,减少helper用中间码代替。
5. 70cbc726 清除不必要的块内寄存器reset，有利于主线的性能提高

其中1已经合入主线，2,3,4,5还未合入

2023/01/03
==========
要动手写一下中断处理cpu一侧的代码，首先要确定一下中断的类型，编号，这个跟LXIC的人对齐了一下，
目前LXIC的定义是分成三种中断类型，体现在acr0和acr1上面，一共六种。类型的分类跟RV的一样，
优先级定义也是一样,逻辑没有很大变动。

然后是寄存器，目前的LXIC设计了四种寄存器，EOIEI,TOPEI,IENABLE,IPENDING。IENABLE,IPENDING
已经加到寄存器高层设计的文档里面，qemu也已经完成相应的定义,EOIEI,TOPEI这两个还未完成。
我们要完成cpu一侧中断关于这四个寄存器的代码。寄存器掩码的定义，关于mip这些的掩码可以替换。

LXIC对于cpu一侧的要求，主要是体现在linx_cpu_set_irq，linx_cpu_do_interrupt，linx_cpu_exec_interrupt
这三个函数的处理逻辑，这里按照LXIC的设计实现相应函数就行，主体逻辑跟RV很接近，我的理解是主要是涉及
特权级的部分，以及相应的寄存器的设计，相关的代码需要改动，大部分逻辑应该不会有太大改动。

整体梳理一下需要修改的内容，首先是添加中断类型的编号以及定义，这个可以跟LXIC的人确定。
然后是linx_cpu_set_irq这个中断触发时的回调函数，这里需要判断中断类型是否是LXIC的六种
中断，并且调用linx_cpu_update_mip函数判断，这里可以将名字改为linx_cpu_update_ipending，
其实也就是根据中断类型，把ipending寄存器对应的中断位置为1，指示这种中断正在传入。
所以原本这部分的代码是根据RV的12种中断进行mip的置位，现在改成根据LXIC的六种中断，
先区分是ACR0/ACR1，然后对ACR0/ACR1的ipending置位。然后这个函数应该就完成了，这点可以到时候
代码出来了让LXIC的人确认。

然后是linx_cpu_exec_interrupt函数，先是判断是否打开中断的条件，然后调用linx_cpu_local_irq_pending，
根据是否有中断pending，以及mie寄存器是否运行中断等，返回一个要处理的中断num，然后调用linx_cpu_do_interrupt
处理中断，这个函数就修改完了，我们把linx_cpu_local_irq_pending里面的逻辑改成ienable就行。

最后就是linx_cpu_do_interrupt函数，这个函数rv的流程就是，先获取中断的相关信息，根据是否
委托选择trap到委托的特权级处理，或者trap到M模式进行中断处理，设置中断基址等信息，
现在是按照LXIC的设计，我们根据domain确认要跳转到的目标acr，设置好对应的寄存器，并且
进行跳转就行，不过跳转的逻辑要按照LXIC的规则增加一些判断。

这样IENABLE,IPENDING这两寄存器要求的内容，cpu一侧就没了，这也是LXIC给出的方案中要求的内容，
http://wenote.huawei.com/wapp/pages/view/share/s/0P4g6w1ENh7G2vGq_m1McRlE01nz_024Lx7J2Nz7Z92b3dNo，
然后EOIEI,TOPEI的用法，这个可以先放一放，等待LXIC的方案出来再动手，我的理解是miclaim原本cpu一侧就
提供了一个riscv_cpu_claim_interrupts的接口，进行一个寄存器的检查，topei这样就大概完成了，
如果eoiei也只是类似接口，可以先实现一版，让LXIC的人review一下。

2022/12/30
==========
分析了一下LXIC对于cpu及硬件的需求，具体的实现还待分析对齐，主要有下面的一些疑问:

目前的LXIC设计了四种寄存器，EOIEI,TOPEI,IENABLE,IPENDING。IENABLE,IPENDING已经加到
寄存器高层设计的文档里面，qemu也已经完成相应的定义,EOIEI,TOPEI这两个还未完成。

然后是中断编号，这个需要确认一下，看看是否需要修改：

.. code-block:: c

 /* Interrupt causes */
 #define IRQ_U_SOFT                         0
 #define IRQ_S_SOFT                         1
 #define IRQ_VS_SOFT                        2
 #define IRQ_M_SOFT                         3
 #define IRQ_U_TIMER                        4
 #define IRQ_S_TIMER                        5
 #define IRQ_VS_TIMER                       6
 #define IRQ_M_TIMER                        7
 #define IRQ_U_EXT                          8
 #define IRQ_S_EXT                          9
 #define IRQ_VS_EXT                         10
 #define IRQ_M_EXT                          11

寄存器掩码的定义，关于mip这些的掩码可以替换。这是一些先期的工作，然后就主要是回调
函数的处理逻辑改成LXIC。主要是linx_cpu_set_irq，linx_cpu_do_interrupt，linx_cpu_exec_interrupt
这三个函数的处理逻辑，细节还需要对齐一下应该就可以动手了。

2022/12/19
==========
重新梳理一下RISCVHartArrayState初始化流程以及如何把特权级模型树添加进去。

总体来说RISCVHartArrayState变量，直接在里面添加ACRTree acr[16]特权级模型就行，它是
这样的一个数据结构:

.. code-block:: c

 struct RISCVHartArrayState {
     /*< private >*/
     SysBusDevice parent_obj;
 
     /*< public >*/
     uint32_t num_harts;
     uint32_t hartid_base;
     char *cpu_type;
     uint64_t resetvec;
     RISCVCPU *harts;
 };

num_harts就是用来保存cpu的个数，而用harts指针来指向RISCVCPU数组，如果单核可能就只有
一个变量，每个RISCVCPU代表一个cpu，每个cpu里面还有CPURISCVState变量保存寄存器等信息。
想要把特权级模型直接当成一个硬件配置，所有hart共享一份的话，可以考虑这种形式。

首先riscv_hart.h里面OBJECT_DECLARE_SIMPLE_TYPE(RISCVHartArrayState, RISCV_HART_ARRAY)
应该是初始化这个类型，并且会调用对应的初始化函数。

看看初始化流程，riscv_hart.c直接调用初始化函数type_init(riscv_harts_register_types)
进行类型注册。然后调用下层注册函数type_register_static(&riscv_harts_info);
然后riscv_harts_info这个结构体，把类型大小，name等，初始化函数属性进行保存，让
type_register_static函数用这个信息初始化:

.. code-block:: c

 static const TypeInfo riscv_harts_info = {
     .name          = TYPE_RISCV_HART_ARRAY,
     .parent        = TYPE_SYS_BUS_DEVICE,
     .instance_size = sizeof(RISCVHartArrayState),
     .class_init    = riscv_harts_class_init,
 };

初始化各个属性的地方在这里：

.. code-block:: c

 static TypeImpl *type_register_internal(const TypeInfo *info)
 {
     TypeImpl *ti;
     ti = type_new(info);
 
     type_table_add(ti);
     return ti;
 }

函数type_new根据输入的info进行初始化，赋值类初始化函数，然后type_table_add把这个类型加到
一个全局哈希类型表里面，调用 ``g_hash_table_insert(type_table_get(), (void *)ti->name, ti);`` 进行哈希表插入。

所以直接在对应的类初始化函数riscv_harts_class_init增加初始化，然后看看是否可以提供
接口给target文件夹下的代码就行。

接下来看看初始化函数流程riscv_harts_class_init底下调用device_class_set_props函数进行
各个成员变量的赋值。用到传参是Property类型的结构体，riscv_harts_props[]数组已经带了
初始化信息，其实只要在这里加一条就行:

.. code-block:: c

 static Property riscv_harts_props[] = {
     DEFINE_PROP_UINT32("num-harts", RISCVHartArrayState, num_harts, 1),
     DEFINE_PROP_UINT32("hartid-base", RISCVHartArrayState, hartid_base, 0),
     DEFINE_PROP_STRING("cpu-type", RISCVHartArrayState, cpu_type),
     DEFINE_PROP_UINT64("resetvec", RISCVHartArrayState, resetvec,
                        DEFAULT_RSTVEC),
     DEFINE_PROP_END_OF_LIST(),
 };

后续函数qdev_class_add_legacy_property，qdev_class_add_property在这里分别是用来初始化父类
的property，和TYPE_RISCV_HART_ARRAY的property。

然后这样初始化就完成了，我的理解是，后续在target中，看看能不能像boot.c一样，直接
引用 ``RISCVHartArrayState *harts`` 就行。

2022/12/16
==========
今天考虑一下把ACRTree acr[16]这个特权级模型，添加到RISCVHartArrayState变量里面，作为
一个硬件配置，之所以考虑这样添加，我的理解是这应该作为一个固定配置，用公用的流程会
相对规范。首先看看这个变量:

.. code-block:: c

 struct RISCVHartArrayState {
     /*< private >*/
     SysBusDevice parent_obj;
 
     /*< public >*/
     uint32_t num_harts;
     uint32_t hartid_base;
     char *cpu_type;
     uint64_t resetvec;
     RISCVCPU *harts;
 };

分析一下这个变量的话，可以看出来num_harts就是用来保存cpu的个数，而用harts指针来指向
RISCVCPU数组，如果单核可能就只有一个变量，每个RISCVCPU代表一个cpu，每个cpu里面还有
CPURISCVState变量保存寄存器等信息。

所以接下来看看怎么初始化这个RISCVHartArrayState。

首先riscv_hart.h里面OBJECT_DECLARE_SIMPLE_TYPE(RISCVHartArrayState, RISCV_HART_ARRAY)
应该是初始化这个类型，并且会调用对应的初始化函数。

.. code-block:: c

 static const TypeInfo riscv_harts_info = {
     .name          = TYPE_RISCV_HART_ARRAY,
     .parent        = TYPE_SYS_BUS_DEVICE,
     .instance_size = sizeof(RISCVHartArrayState),
     .class_init    = riscv_harts_class_init,
 };

instance_size代表了RISCVHartArrayState这个实例的大小，TYPE_RISCV_HART_ARRAY用来作为
变量名，所以我的理解是，把变量加到RISCVHartArrayState里面，然后在riscv_harts_class_init
中，加上ACRTree acr[16]的初始化,后续就可以根据这个来作为一个模型树的部件了。

device_class_set_props这里应该会调用riscv_harts_props来给RISCVHartArrayState里面的变量
赋值，所以可以在riscv_harts_props这里，把特权级模型对应的信息进行初始化。

可以加到riscv_harts_props这里把ACRTree acr[16]进行初始化，作为这个类型的默认值。

.. code-block:: c

 static Property riscv_harts_props[] = {
     DEFINE_PROP_UINT32("num-harts", RISCVHartArrayState, num_harts, 1),
     DEFINE_PROP_UINT32("hartid-base", RISCVHartArrayState, hartid_base, 0),
     DEFINE_PROP_STRING("cpu-type", RISCVHartArrayState, cpu_type),
     DEFINE_PROP_UINT64("resetvec", RISCVHartArrayState, resetvec,
                        DEFAULT_RSTVEC),
     DEFINE_PROP_END_OF_LIST(),
 };

这样后面直接用就行了，具体以哪种形式使用，这个可能还需要跟志林对齐一下什么方式比较好，
以方便后续异常级信息获取以及切换的处理。

2022/12/13
==========
对齐了对于设计思路上的一些意见,把特权态模型对应的变量名按照命名规则重新定义。

首先是特权级模型的存储结构，先选用每个节点都保存直接父节点的特权级别和直接子节点的
特权级别的方式，以方便特权级间的切换。

暂定如下，ACRTree结构是链表头部，ppriv存储直接父节点特权级别，priv存储自己所在特权级，
childpriv指针用来指向直接子节点的链表:

.. code-block:: c

 ACRTree acr[16];

 typedef struct ACRTreeNode
 {
    int cpriv;
    struct ACRTreeNode *next;
 } ChildACR;

 typedef struct
 {
    int priv;
    int ppriv;
    bool valid;
    ChildACR * childpriv;
 } ACRTree;

根据 “ACR特权态切换设计实现” 文档，特权态切换分为acr_request和acr_enter两类，acr_request
是低特权级到高特权级的请求，acr_enter是高特权级进入低特权级，可以提供两个接口校验这两种
切换请求的合理性，由于acr_request从文档可以得到只要是祖先节点就能进行acr_request请求，
acr_enter则由于目前中断/异常的委托机制是逐级委托，只要在直接子节点即可，先提供以下接口:

.. code-block:: c

 bool check_acr_request(int aacr, int target_acr);
 bool check_acr_enter(int aacr, int target_acr);

都定义完之后，可以对特权级模型进行初始化，可以在csr.c或者cpu.c里面进行初始化，只初始0，1，2这
三个特权级以及相应父子关系，其余更多的特权级可以根据方案后续添加。


2022/12/12
==========
考虑如何实现特权态树模型的定义，并思考对应的代码思路。

ACR特权态模型
----------------------
我们使用树来定义特权态的关系。ACR特权态树是以ACR0特权态为根节点以其他特权态为
普通节点组成的一棵树。ACR0又称为ACR root。

ACR特权态树定义了各个特权态的权限关系、资源关系和切换关系。

**权限关系**

* 父节点的权限高于子节点，可以直接访问和控制子节点。
* 兄弟节点之间/子树之间没有权限关系，相互之间不能访问和控制。

**资源关系**

* 资源包含系统寄存器、指令和物理地址空间（包括内存和IO）。
* 每个特权态都有自己专属的资源，包括系统寄存器和指令（对应特权态的功能配置）。
* 根节点（ACR root）管理全部的物理地址空间，父节点管理子节点的物理地址空间。

**切换关系**

* 从根节点到叶子节点的路径上的特权态之间可以任意相互直接切换，在此路径之外的特权态之间不可以相互直接切换。
* 兄弟节点之间/子树之间的特权态切换，可以通过他们共同的祖先节点中转实现切换。

ACR特权态的定义不同于RISC-V，当前中断/异常委托处理等需要考虑到特权态之间的关系是怎样的，因此
我们需要将当前特权态关系在QEMU中体现出来，对于这样的一个树状特权关系，在qemu中最简单的实现方法
可以单独使用一个数据结构存储，用一片连续的空间来表示这个树::

 1.双亲表示法：例如说一个大小为16的int型数组，存储的是其父亲的索引（求父节点方便）
 2.孩子表示法：对应的是自己的特权级，存储的是其子节点的索引链表（求子节点方便）。

想查父节点和子节点都快，可以使用双亲表示法，也就是双亲表示法+孩子表示法的结合，用的
空间更多，但是每个节点直接保存了父节点和子节点的信息，当前给出建议存储结构如下:

.. code-block:: c

 ACRTree acr[16];

 typedef struct ACRTreeNode
 {
    int cpriv;
    struct ACRTreeNode *next;
 } ChildACR;

 typedef struct
 {
    int priv;
    int ppriv;
    bool valid;
    ChildACR * c_priv;
 } ACRTree;

根据 “ACR特权态切换设计实现” 文档，特权态切换分为acr_request和acr_enter两类，特权态树模型在qemu
中主要用于检查该两类请求合法性检查，即acr_enter时只能进入到自己或者在以本特权态为根的特权态树，
acr_request只能直接进入到当前特权态所在分支上的其它特权态，这里只做简单陈述，详细请参考中断/异常
委托设计文档，根据以上场景，可以提供这样的接口，供特权级切换时进行检查：

.. code-block:: c

 bool check_act_request(int aacr, int target_acr);
 bool check_act_enter(int aacr, int target_acr);

注：当前仅看到从子节点向上查父节点一个需求，但并未确定从父节点到子节点查询需求是怎样的, 待确认。

所以综上，我的想法是，先把这个树模型的数据类型，在linx-dev分支定义下来，可以写在cpu.h里面，
然后类型定义完之后，可以在初始化的时候，进行特权级树的填充。可以在riscv_cpu_reset函数里面进行初始化
(应该是忘记改名了？到时候可以修改函数的时候顺便修改名字),或者是目前存放寄存器代码的地方csr.c,
由目前确定的模型可以这样：

.. code-block:: c

 #ifndef CONFIG_USER_ONLY
     for(int i=0;i<15;i++){
        acr[i]->ppriv = -1;
        acr[i]->valid = false;
     }
     acr[0]->valid = true; //初始化特权级0
     acr[0]->priv = 0;     //特权级0的value
     acr[0].c_priv=(ChildACR*)malloc(sizeof(ChildACR));
     acr[0].c_priv->cpriv=1; //特权级0的第一个子节点特权级
     acr[0].c_priv->next=NULL; //特权级0的无第二个子节点
     
     acr[1]->valid = true;
     acr[1]->priv = 1;
     acr[1]->ppriv = 0;
     acr[1].c_priv=(ChildACR*)malloc(sizeof(ChildACR));
     acr[1].c_priv->cpriv=2;
     acr[1].c_priv->next=(ChildACR*)malloc(sizeof(ChildACR));
     acr[1].c_priv->next->cpriv=3;
     acr[1].c_priv->next->next=NULL;
     
     acr[2]->valid = true;
     acr[2]->priv = 2;
     acr[2]->ppriv = 1;
     acr[2].c_priv=NULL;
     
     acr[3]->valid = true;
     acr[3]->priv = 3;
     acr[3]->ppriv = 1;
     acr[3].c_priv=(ChildACR*)malloc(sizeof(ChildACR));
     acr[3].c_priv->cpriv=4;
     acr[3].c_priv->next=NULL;
     
     acr[4]->valid = true;
     acr[4]->priv = 4;
     acr[4]->ppriv = 3;
     acr[4].c_priv=NULL;
 #endif
 
然后是check_act_request和check_act_enter函数，可以初步实现来提供检查访问的两个接口:

.. code-block:: c

 bool check_act_request(int aacr, int target_acr)
 {
    if(acr[aacr]->c_priv == target_acr){
        return true;
    }
    else{
        return false;
    }
 }
 bool check_act_enter(int aacr, int target_acr)
 {
    bool res = false;
    ChildACR * temp = acr[aacr]->c_priv;
    while(temp != null){
        if(temp->c_priv = target_acr){
            res = true;
            break;
        }
    }
    return res;
 }



2022/12/02
==========
分析ACR切换文档，编写开发实施方案。

ACR切换文档分析内容
----------------------

ACR特权态模型
^^^^^^^^^^^^^^^^^^^^^^^^^^^

我们使用树来定义特权态的关系。ACR特权态树是以ACR0特权态为根节点以其他特权态为
普通节点组成的一棵树。ACR0又称为ACR root。

ACR特权态树定义了各个特权态的权限关系、资源关系和切换关系。

**权限关系**

* 父节点的权限高于子节点，可以直接访问和控制子节点。
* 兄弟节点之间/子树之间没有权限关系，相互之间不能访问和控制。

**资源关系**

* 资源包含系统寄存器、指令和物理地址空间（包括内存和IO）。
* 每个特权态都有自己专属的资源，包括系统寄存器和指令（对应特权态的功能配置）。
* 根节点（ACR root）管理全部的物理地址空间，父节点管理子节点的物理地址空间。

**切换关系**

* 从根节点到叶子节点的路径上的特权态之间可以任意相互直接切换，在此路径之外的特权态之间不可以相互直接切换。
* 兄弟节点之间/子树之间的特权态切换，可以通过他们共同的祖先节点中转实现切换。

对于这样的一个树状特权关系，目前qemu里是没有的，那么就需要考虑如何来保存这样的优先级，
要在qemu中实现，最简单的方法可以直接用一片连续的空间来表示这个树：
1.双亲表示法：例如说一个大小为16的int型数组，存储的是其父亲的索引（求父节点方便）
2.孩子表示法：对应的是自己的特权级，存储的是其子节点的索引链表（求子节点方便）。

这两种方法都可以，不过就是会造成查找父节点和子节点的时间复杂度，一个是O(n)，一个是
O(1)的区别。

想查父节点和子节点都快，可以使用双亲表示法，也就是双亲表示法+孩子表示法的结合，用的
空间更多，但是每个节点直接保存了父节点和子节点的信息。

所以树总体如下::

        struct CPURISCVState {
            ......
            ACRTree acr_tree[16];
            ......
        }
        typedef struct ACRTreeNode
        {
            int child_acr_level;
            struct ACRTreeNode *next;
        }*ChildACR;
        typedef struct
        {
            int acr_level;
            int parent;
            ChildACR firstchild;
        }ACRTree;

ACR特权态切换
^^^^^^^^^^^^^^^^^^^^^^^^^^^

ACR特权态切换是指CPU从当前特权态运行的程序位置切换到另一个特权态的特定的程序位置继续运行。
ACR模型提供了两种ACR特权态切换路径，一种是从祖先节点切换到子孙节点（从高特权态切换到低特权态），
称为acr_enter，一种是从子孙节点切换到祖先节点（从低特权态切换到高特权态），称为acr_request。

所以在实现了特权级模型的情况下，acr_enter会进入子孙节点，也就是低特权级，判断acr_enter跳转的
目标是否符号特权级要求，只要在当前节点遍历子树即可，如果查询不到，即跳转异常；
acr_request的情况下，逐级查找祖先节点，查询不到表示切换异常，只有查询到才能切换。

例如说acr_request和acr_enter可以这样校验：

.. code-block:: c

        bool check_act_request(int aacr,int int target_acr)
        {
            while(acr_tree[aacr]->parent != -1) //层层搜索父节点，搜寻是否目标acr
            {
                if(acr_tree[aacr]->parent == target_acr){
                    return true;
                }
                aacr = acr_tree[aacr]->parent;
            }
            return false;
        }

        bool check_act_enter(int aacr,int int target_acr)
        {
            ChildACR *temp = acr_tree[aacr]->firstchild;
            while(temp != null) //搜索所有子孙节点，搜寻是否目标acr
            {
                if(temp->child_acr_level == target_acr){
                    return true;
                }
                temp = temp->next;
            }
            return false;
        }

别名
^^^^^^^^^^^^^^^^^^^^^^^^^^^

别名的方式是：例如我们可以使用USER表示用户程序的特权态，在非虚拟化环境下，USER被
解释为ACR2，在虚拟化环境下，USER被解释为ACR4，这样，我们就可以统一使用acr_enter(USER)启动用户程序。

从目前的寄存器定义方式就是，我ssr读写的时候，一条sysget ELINK指令，ELINK代表alias寄存器，
然后根据当前所处的特权级，返回对应的寄存器内容，如果当前处于ACR3，那么就返回ELINK_A3的内容，
实际上就会执行成sysget ELINK_A3.

.. code-block:: c

        target_ulong get_mapped_cstate(CPULINXState *env)
        {
            target_ulong cstate = env->cstate;
            int ssrno = get_field(cstate, CSTATE_AACR);
            target_ulong mcstate = 0;
            linx_ssrrw_do64(env, (ssrno << ALIAS_OFFSET) + SSR_BASE, &mcstate, 0, 0);
            return mcstate;
        }

上面就是一个alias使用的例子，qemu的实现可以根据alias的寄存器，以及对应的的特权级，
返回对应的mapping register编号，得到对应映射寄存器。

异常和中断委托机制
^^^^^^^^^^^^^^^^^^^^^^^^^^^

目前的中断委托如下：

+------------+-----------------+----------------+--------------+----------+
|            | E0(ROOTCALL)    | E1(PAGEFAULT)  | E2(ECALL)    | Et(BRK)  |
+------------+-----------------+----------------+--------------+----------+
| ACR0.EDEL  |  0              | 1              | 1            | 1        |
+------------+-----------------+----------------+--------------+----------+
| ACR1.EDEL  |  0              | 1              | 1            | 1        |
+------------+-----------------+----------------+--------------+----------+
| ACR3.EDEL  |  0              | 0              | 0            | 0        |
+------------+-----------------+----------------+--------------+----------+

异常委托机制：

#. CPU为每个处理异常的特权态都定义一个异常委托寄存器，该寄存器是64位的系统寄存器，默认值为0。
#. 异常委托寄存器每个位的位置，对应异常类型的编码。
#. 每个特权态的异常委托寄存器被置0的位，表示该位对应的异常不委托给下一个特权态，对应异常在本特权态处理。
#. 每个特权态的异常委托寄存器被置1的位，表示将该位对应的异常委托给下一个特权态（即目标特权态），对应异常在目标特权态处理。
#. 当前特权态发生异常后，CPU从根节点到当前特权态的父节点的路径上的各个特权态的异常委托寄存器查找处理该异常的目标特权态，直到找到目标特权态。
#. 所有异常默认都在根节点处理，在根节点发生的异常，都在根节点处理。

所以这个机制，例如说ACR4产生的ECALL异常，这个特权级首先到ACR0,因为需要ACR root来管理，
然后ACR0配置了它把这个异常委托出去，所以向下一级，到ACR1，然后ACR1接着进行委托，
ACR虽然有两个委托路径，但是异常是ACR4产生的，它只能委托到ACR3（所以这还需要根据尝试异常的特权级确定路径），
然后ACR3不再进行委托，直接进行异常处理。

所以ACR4产生异常的时候，这个产生的异常级QEMU还需要保存，会根据这个异常级，确认根据
特权级树委托的时候，QEMU的跳转路径。不仅要检查check_act_enter委托的特权级是否是当前
特权级的子树，还必须检查check_act_enter是否是ACR0到ACR4路径上的节点。

ACR特权态切换相关的指令
^^^^^^^^^^^^^^^^^^^^^^^^^^^

要实现目前的切换，也就是enter和request的要求，需要支持以下的定义：

acr_enter指令：

* ERET

acr_trap指令：

* ETRAP
* EBRK




