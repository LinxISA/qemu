Wood的开发日志
*******************

20211220
========

1. 看了陈娟娟关于linx的add指令和load指令的实现，添加8个临时寄存器来记录linx指令的
   执行结果，后面的指令根据索引值来拿到寄存器中的值进行运算，没有考虑这个块的状态
   存储，从bstart识别出linx指令后就切换解码器进行顺序解码

2. 考虑用一个结构体来存储某个块的状态，主要缓存load和store的值，块的起始位置和末尾
   位置，记录执行到了哪条指令，考虑的出发点是如果产生中断，重新执行这个块时从那里
   去恢复

   .. code-block:: c

       block_state struct{
         char *Subfix;
         char *property;
         int b_size;
         TCGv t[8];
         TCGv block_gpr[32];
         ull dest[8];
         TCGv s[32];
         TCGv l[32];
         ull bpc1;
         ull bpc2;
         int offset;
         //int set[32];
       };

   对这个结构体在哪个位置使用和更改还在考虑，需要进一步考虑这个问题

3. 看了国柱工关于Qemu TCG模拟器的cpu_gpu变量部分的介绍，对于具体的结构不是很了解，
   读了几遍之后只是有个大致概念，在考虑某个块的状态是不是也用到TCGtemp的空间，
   不只是这种简单的结构体来描述

20211221
========

1. 添加指令然后编译测试，报如下错误，暂时没找到错误原因::

     gen_blk_bstart xxx  pc_next=10492
     gen_blk_bstart xxx  pc_next=10494

     Segmentation fault (core dumped)

2. 添加指令时模仿了陈娟娟的流程，加上通过国柱工对于TCGv的分析，对于涉及的变量有
   了一个初步了解，当前的考虑是总觉得添加指令不会是这个简单流程，但测例能出现正
   确结果。

20211222
========

1. riscv的压缩指令长16bit，以高3bit和最低2bit来区分指令，将所有压缩指令排序后发
   现以100起始，11为结尾的指令编码空间未被使用，以此作为bstart指令

2. 关于tcg中tb块的使用，tb块的结尾通常是分支指令，每个tb块执行前/后要执行PROLOG
   UE和EPILOGUE，用来保存tb块的栈指针、基址针，便于tb块的恢复和查找，类似于函数
   调用，从一个tb块跳转到另一个tb块效率较低，所以将多个tb链起来，多个tb块只有一
   个PROLOGUE和EPILOGUE区域
   类似于QEMU -> prologue -> code cache -> epilogue -> QEMU的流程
   一个tb内可容纳多个linx指令块，强制每一个块都从新的tb块起始位置开始翻译执行，
   空间上和效率感觉有所浪费

20211227
========

1. 按照工作任务尝试添加分支指令beq和bnext_cond，卡在TCGv变量的内容比较难获取，怎
   么进行内容的比较还要进一步了解一下
2. 了解了一下rv分支指令的产生方式，主要是gen_branch函数，明天继续看一下这部分内容

20211228
========

1. 添加beq和bnext_cond分支指令并进行测试，写的测试还未通过，使用qemu的中间码进行分
   支跳转时提示是非法指令，还未定位到问题，初步判断是中间码未正确使用
2. 容易忽略使用中间码来完成指令定义的操作，例如：比较两个t寄存器可以使用中间码存在
   的cond比较指令，而不是用c语言来获取值进行大小比较等诸如此类

20211229
========

1. 确定分支指令定义跳转以B.xx为真时，bnext_cond进行跳转
2. 添加store相关指令 sb、sh、sw、st并通过测试
3. 分支指令的添加设置了标志位，使用riscv的接口来实现跳转指令，目前仍提示为非法指令
4. 错误总结:
   (1) 非法指令：测例汇编程序宏定义的linx指令中传递参数不正确(寄存器编号、参数顺序)
   (2) temp_idx: Assertion n >= 0 && n < tcg_ctx->nb_temps failed 由于间接索引值引起
5. git提交时命令顺序
   git pull --rebase
   git diff  //检查空行、空格、注释，无关代码等
   git add
   git commit -s
   git rebase -i //调整提交内容
   git push

20211231
========

1. 添加函数调用bl指令，将当前下一条指令的地址保存到riscv调用返回地址存储的ra寄存器，
   使用riscv的ret指令不能正常返回，使用自己添加的ret指令返回后位置不正确，后续继续修改

20220104
========

1. 函数调用指令bl添加完成并通过测试

20220106
========

1. 按照文档更新了block.decode文件中的指令编码，删除了之前添加实现但在文档中不存在的指令，
   需要其他人按照这个版本修改对应指令的trans函数

20220107
========

1. 修改store指令实现，还未通过测试

20220110
========

1. 添加store和compare类指令实现及测试

20220111
========

1. 修改添加影子寄存器sgpr后的set指令，测试不通过

20220114
========

1. 修改head宏定义，使head的宏能够在各自的测例中修改对应数值，算数和读写指令通过测试
2. 测试setbpc.cond指令，还在调整分支跳转的宏

20220117
========

1. 添加setbpc.cond测试用例
2. 编译linux内核

20220118
========

1. 添加branch类型call和ret，没有走到commit函数，可能是添加的测例里head对应了0条
   微指令，还在测试

20220121
========

sfence.vma指令的描述是虚拟内存屏障，根据后续的虚拟地址翻译对之前的页表存入进行
排序。fence指令是为了保证程序在运行时内存实际的访问顺序和程序代码编写的访问顺序
一致，也就是避免内存乱序访问。内存乱序主要发生在两个阶段，在编译时由于编译器的
优化导致，如在编译时使用优化选项O2（或O3）等，这种情况可以将变量声明为volatile
类型，或者是在需要保持顺序的位置添加barrier() ``__asm__ __volatile__("" ::: "me
mory")``, 告诉编译器这里对内存保持顺序访问，不要优化，本质上还是添加了一个volat
ile，volatile变量要求在更新了缓存之后立即写入到系统内存，而非volatile变量，在适
当的时候将缓存数据写入内存；另一个阶段是运行时，多线程时对同一个变量进行读写，
多cpu间交互引起的。

sfence.vma需要要两个参数，一个参数指示tlb中哪个页表的虚拟地址被修改了，另一个参
数是被修改表的进程的地址空间描述符（ASID）。

了解一下这个ASID，为了提高TLB的性能，将TLB分成Global和process-specific。global
是指常驻在tlb中不会被刷出的，例如内核空间的翻译，process-specific 是指每个进程
独有的地址空间，当发生进程切换的时候，这部分tlb可以被刷出，ASID就是为了指示进程
独有的tlb，这样TLB就可以识别出这个 TLB 页表项是属于哪一个进程的。

这个ASID在RV中是存储在satp（Supervisor Address Translation and Protection，监管
者地址转换和保护）的 S 模式控制状态寄存器，来控制分页系统。satp 有三个域。MODE
域可以开启分页并选择页表级数，ASID（Address Space Identifier，地址空间标识符）
域和 PPN 字段保存根页表的物理地址，它以 4 KB 的页面大小为单位。

20220128
========

今天国柱提到rv和block要用同一个返回地址寄存器。在test.c函数中调用汇编函数，会产
生一条rv的jal指令，该指令保存下一条指令的地址；block的call分支类型也需要保存下一
条指令的地址，如果还继续使用ra来存储，ra的值就会被覆盖，rv的ret指令读取ra寄存器
返回时就会出错；之前是用gpr[5]来临时充当ra寄存器的角色，来避免冲突；反汇编函数后
可以看到rv在jal指令之前会将s0和ra进行入栈，在调用函数结束返回在ret指令前再将s0和
ra出栈，在嵌套调用时就可以避免冲突，汇编函数如下：::

  addi    sp,sp,-32
  sd      ra,24(sp)
  sd      s0,16(sp)
  addi    s0,sp,32
  jal ra, xx <xxx>
  ...
  ld      ra,24(sp)
  ld      s0,16(sp)
  addi    sp,sp,32
  ret

借鉴这种方式来验证block的call和ret分支类型，将上面的rv指令转换为block指令：::

  #define ra 1
  #define sp 2
  #define s0 8

  JUMP_CALL_HEAD
  JUMP_RET_HEAD

      CONST(0, -32)
      GET(sp)
      ADD(1, 0)
      SET(sp, 0)
      CONST(0, 24)
      ADD(2, 0)
      GET(ra)
      SD(1, 0, 0)
      CONST(0, 16)
      ADD(5, 0)
      GET(s0)
      SD(0, 1, 0)
      GET(sp)
      SET(s0, 0)

      CONST(0, 24)
      GET(sp)
      ADD(0, 1)
      CONST(0, 0)
      LD(0, 1)
      SET(ra, 0)
      CONST(0, 16)
      ADD(0, 5)
      LD(0, 4)
      SET(S0, 0)
      CONST(32)
      GET(sp)
      ADD(0, 1)
      SET(sp)

用上面的block指令集合来进行汇编测例里的函数调用，还需进一步测试

20220129
========

1. 按照国柱提示的思路，使用rv的指令来完成返回地址寄存器的入栈和出栈，在使用同一
   个ra寄存器时，分支类型call和ret测试通过，好像还有问题
2. 与王州经过确认，间接跳转和条件跳转不用修改，sbpc在间接跳转时存储地址，在条件
   跳转时做为标志寄存器flag
3. qemu中没有指令的乱序执行，fence指令暂时什么都不做，对应的trans函数直接返回true
4. wfi测试时异常退出了，进入了qemu_main_loop循环，之前以为是wfi导致产生的，发现
   是wfi异常后就切换了线程，该循环时qemu softmmu的主循环，继续再看一下

20220211
========

1. 确定setbpc传递的参数为绝对地址

20220215
========

riscv的特权指令集文档中对wfi指令的描述如下：

等待中断指令（WFI）为实现提供了一个建议，即当前hart可以暂停，直到出现可能处理的
中断。WFI指令的执行也可用于通知硬件平台，适当的中断应优先路由到此hart。WFI可在所
有特权模式下使用，也可在U模式下使用。当mstatus中的TW=1时，此指令可能会引发非法指
令异常。

mstatus寄存器的TW（timeout wait）位是一个支持拦截WFI指令的WARL字段。当TW=0时，如
果由于其他原因而未阻止，WFI指令可以在较低权限模式下执行。当TW=1时，如果WFI在任何
特权较低的模式下执行，并且它没有在特定于实现的有界时间限制内完成，则WFI指令会导
致非法指令异常。时间限制可能始终为0，在这种情况下，当TW=1时，WFI总是在特权较低的
模式下导致非法指令异常。当没有比M低特权的模式时，TW硬连线到0。

当实现S模式时，在U模式下执行WFI会导致非法指令异常，除非它在特定于实现的有界时间
限制内完成。本规范的未来修订可能会添加一个功能，允许S模式选择性地允许U模式下的
WFI。只有当TW=0时，此功能才会激活。

Hypervisor模式下hstatus寄存器的VTW字段类似于机器模式下mstatus寄存器的TW字段。当
VTW=1（并假设mstatus.TW=0）时，如果WFI未在特定于实现的有界时间限制内完成，则在
Virtual supervisor模式下尝试执行WFI会引发虚拟指令异常。

RV的实现中使用helper_wfi函数处理wfi指令，在该函数中会判断当前运行的特权级，检查
是否支持Supervisor module和Hypervisor module（和虚拟化有关）的扩展，并读取mstatus.TW、
hstatus.VTW字段进行判断是否满足等待中断的条件。

经过测试：softmmu中特权级为u模式，mstatus.TW字段为0（根据文档定义TW为0时，wfi可
以在低特权级运行），并且支持Supervisor module(RVS)的扩展（根据文档定义，当实现S
模式时，在U模式下执行WFI会导致非法指令异常），不支持Hypervisor module（RVH）的
扩展，当前添加wfi指令后非法指令报错，所以如果延用rv的实现，测试wfi指令需要改变特
权级为s模式或更高的m模式

20220216
========

1. 更新指令测试中的边界测试方案
2. 王州建议wfi指令的测试方法可以尝试使用添加内核驱动模块的方式，将wfi指令使用内
   联汇编的方式添加到驱动函数中进入到内核态进行测试，还未尝试

20220221
========

1. review异常时bstate寄存器的恢复和保存，进入块执行时，bstate.in.en为0时对gpr保
   存到bstate寄存器及初始化的操作未添加
2. review原子操作的代码，自己关于原子操作执行流程还没梳理清楚，还得再看看

20220222
========

1. 补充比较指令测试用例，校验比较指令的正确性
2. 在测试无符号比较指令时，犯了个低级错误，在无符号比较中，负数是可以大于正数的，
   比如-1>1，因为-1对应的无符号数为32位最大值（-1的补码为1111 1111 1111 1111）在
   计算机底层，数据只有0或1的表示，没有类型可言，数据类型只有在上层的解释时才会
   不一样，对于无符号而言，1111 1111 1111 1111为INT_MAX，而有符号中为-1，它们在
   底层的存储是一样的

20220223
========

1. 补充条件跳转和store指令的测试用例
2. 看了下基础指令设计文档0.12版本，与0.11版本的差异娟姐大致已经列出来了，比较指
   令划分为辅助块指令并添加了与立即数的比较，条件跳转指令setbpc.cond也增加了和立
   即数的比较，下一步按0.12版本添加指令

20220224
========

关于最新0.12版本的指令设计文档，存在如下差异和疑问：

1. 标准辅助块指令bstart.a包含所有的标准标量块指令，是否意为着要实现两套标准指令，
   还是只实现辅助的几条指令？
2. 系统块指令bstart.sys中无标量运算，下面第14条从文中摘抄说系统指令能完成所有标
   量计算，互相矛盾
3. bstart.ptr指针块指令组成256bit块头，未实现
4. bstate.out和in改为bstate.ext，bstate.in.en改为bstate.en
5. 每个块指令执行前，处理器将共享寄存器GPR复制到影子寄存器SGPR，在块指令执行过程
   中，块内微指令只能去修改影子寄存器，不能修改共享寄存器，复制过程没有实现，需
   要添加，以及是否意味着set指令要改为从sgpr读取？
6. 块内T寄存器T0保存TPC，1~8指向指令，当前是0~7做为T寄存器；发生异常时，bstate
   保存到bstate.ext，移动完成后块指令内部bstate状态清空。无论正常退出与否，bstate
   都需要被清空，清空操作没有实现
7. 块属性MO，访存提交模型——待扩展
8. head标识删去bstart.p、bstart.pf、bstart.c，增加了bstart.a
9. 如果内部get/set与bget/bset声明的不匹配，产生异常，未实现
10. 块头汇编格式，bstop指向块最后一条微指令的地址，更改bnext为bnext.type，并且已
    经有块头指令域段的定义，这个块头汇编格式怎么用？
11. 增加了算数逻辑与立即数操作微指令，例如srli
12. 增加了辅助块指令：长立即数，复杂整数（乘法、除法、取余），比较微指令（添加
    与立即数的比较），条件选择微指令
13. 指令编码发生了变化，需要对block.decode做修改
14. 单独的系统块指令是完备的，可以完成所有的标量运算。系统块指令拥有单独定义的微
    指令+辅助标量块指令的所有定义，不是很理解，是系统块内可以直接使用辅助指令的
    意思吗？
15. bstart标准标量块指令无整数乘除余数，无浮点运算，除法和取余移动到辅助块指令
16. const加载11bit宽立即数，lconst加载64bit宽超长立即数，目前的const需要修改
17. 分支类型ret和ind相同，额外给硬件有ret提示，有提示是什么意思，函数调用后如何
    读取链接寄存器返回？
18. 分支类型ind，删除了“setbpc只在块提交时生效”这句话，是否立即跳转（setbpc指令的
    定义是提交后生效）？
19. 分支类型cond，添加了“setbpc.cond只在块提交时生效”这句话，在提交时才将比较结果
    写到sbpc，目前是执行setbpc.cond指令后直接将结果写到sbpc
20. 所有指令最多两输入，当前const指令三输入，按新版改动后，const只有一个输入
21. load指令中的立即数参数改为有符号，store指令也类似，之前对此没有区分

20220228
========

辅助块指令包括：乘除取余指令、超长立即数指令比较指令、条件选择指令。辅助块指令的
编码可以添加到block.decode中，辅助块指令对应的trans函数放到insn_trans下新的文件
trans_block_aux.c.inc中，超长立即数指令lconst（长80bit）需要特殊处理。

函数调用返回的跳转类型bnext.ret和间接跳转bnext.indirect相同，都通过setbpc来获取跳
转地址，函数调用时依旧将跳转的下一条地址写到ra寄存器，函数调用返回时依然要获取ra
寄存器中的值才能返回到调用位置，所以首先获取函数调用时链接寄存器里的值，然后通过
setbpc写到sbpc中，在提交时根据sbpc中的值来返回到函数调用的位置

20220301
========

1. 修改调用返回函数ret
2. 添加与立即数相关的比较指令

20220302
========

1. 添加与立即数相关的条件跳转setbpc指令
2. TEST_VERIFY该宏定义只返回调用函数的函数名，条件跳转指令的测试封装一层后，测试
   结果只打印输出封装的函数名，考虑使用宏函数来替代封装函数

20220303
========

1. 添加lconst指令
2. 测试条件跳转指令

目前变长指令只有系统指令sysset和sysget（32bit）以及辅助指令lconst（80bit，16bit
指令码加64bit立即数），变长指令可以采用解析128bit head头部的方式，如果指令码解析
后发现是变长指令，则连读读取后续连续的空间，解析其中的内容。lconst将有符号长立即
数移动到输出寄存器，它的指令码中包含sign（1bit）和size（2bit）字段。sign字段用来
表示这个立即数为正数还是负数，在写入输出寄存器时未定义的字段用符号位填充，暂定
sign为0时为正数，为1时为负数；size字段的取值为（0、1、2、3）分别表示有几个16bit
的数据被定义

20220307
========

1. 与立即数相关的条件跳转指令已添加
2. 最近更新的文档中ret指令依旧使用linkreg寄存器，当前实现中使用检查是否有setbpc
   指令，如果有即用sbpc中的值做为返回地址，否则使用linkreg返回。目前先不做修改，
   对该指令的定义还不明确，后面确定后再做变动

20220324
========

qemu的log日志输出时添加的指令被认定是非法，指令码对应的指令名为illegal，在文件
qemu/disas/linx.c中包含有关log文件打印信息相关的代码。在枚举变量 rv_op 中，包含
所有指令对应的一个枚举值

结构体rv_opcode_data中说明了指令对应的数据段信息，声明了一个rv_opcode_data类型的
数组 opcode_data ，其中列举了所有的指令的具体信息，指令参数集的名称和指令格式来源
为qemu/target/linx/block.decode文件中指令码的定义形式::

  /* 每条指令实际名称、指令参数集、指令格式的具体信息 */
  typedef struct {
      const char * const name;     // 指令名称
      const rv_codec codec;        // 指令参数集定义名称（枚举值）

      /*
       * 指令格式定义名称  根据format定义的一个字符串，比如add指令有2个源寄存器，
       * 1个目的寄存器，则定义为  #define rv_fmt_rd_rs1_rs2  "O\t0,1,2"
       */
      const char * const format;

      /* 伪指令相关*/
      const rv_comp_data *pseudo;  // 一些行为稍复杂指令的指令的动作由多条指令来完成

      /* 压缩指令相关，当指令为压缩指令时会处理下面的字段 */
      const short decomp_rv32;
      const short decomp_rv64;
      const short decomp_rv128;
      const short decomp_data;
  } rv_opcode_data;

其中riscv的add指令示例如下，指令名为add、指令的参数集名称为rv_codec_r、指令的参
数形式为rv_fmt_rd_rs1_rs2，因为add为普通指令，剩余的字段赋值为NULL或0::

  { "add", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 }

  /* 一条指令输出log时的信息 */
  typedef struct {
      uint64_t  pc;     // pc值
      uint64_t  inst;   // 指令码
      int32_t   imm;    // 指令操作立即数
      uint16_t  op;     // 指令类型名称
      uint8_t   codec;  // 指令编码中的参数集名称
      uint8_t   rd;     // 指令目的操作寄存器
      uint8_t   rs1;    // 指令源操作寄存器
      uint8_t   rs2;    // 指令源操作寄存器
      uint8_t   rs3;    // 指令源操作寄存器
      uint8_t   rm;
      uint8_t   pred;
      uint8_t   succ;
      uint8_t   aq;     // 原子aq属性
      uint8_t   rl;     // 原子rl属性
  } rv_decode;

输出qemu log日志时的处理函数有如下几个::

  disasm_inst(buf, sizeof(buf), isa, memaddr, inst)
  {
    decode_inst_opcode(&dec, isa)；// 通过对指令解码，获得了rv_op中对应的一个映射值

    /*
     * 解码指令操作数，根据上一步获得的op值，查表opcode_data，找到指令集参数集定义
     *（不同的指令可以共用相同的参数集），根据不同的参数集对目的寄存器rd，源寄存器rs，
     * 立即数imm进行填充
     */
    decode_inst_operands(&dec);
    decode_inst_decompress(&dec, isa); // 处理压缩指令
    decode_inst_lift_pseudo(&dec); // 处理伪指令

    /*
     * log输出格式，根据指令定义的格式，对格式类型进行解析，将指令携带的信息输入
     * 到buff里，包括指令名称，寄存器（包括csr）名称等
     */
    format_inst(buf, buflen, 16, &dec);
  }

这里数组opcode_data中指令的条数和枚举变量rv_op中的枚举值是一致的，他们一一对应，
所以我们添加block指令时，先在枚举变量rv_op的尾部按照格式添加指令，比如blk_head指
令，rv_op_blk_head = 362, 这个值会索引到opcode_data数组中下标为362的内容，在
opcode_data数组尾部按照格式添加 { "blk_head", rv_codec_r, rv_fmt_rd_rs1_rs2, NULL, 0, 0, 0 },
指令参数集和指令格式需要按照block.decode中的定义自行添加，然后在函数
decode_inst_opcode中根据blk_head的指令码添加解码流程，让函数认识这是一条blk_head
指令，并能够找到rv_op中对应的枚举值，索引到opcode_data中的信息，按照其中的指令参
数集和指令格式解析指令字段的信息，去读取相应的寄存器的值

但这样做存在的问题是，跟我们在指令解码时做的工作类似，需要从众多riscv指令中找出
block指令。在指令译码时有bstart指示，通过bpc1和bpc2划分界限，在输出qemu log时同样
需要进行区分

关于qemu log日志输出时指令显示illegal的问题，国柱说之后把pc的最低位设置为1，方便
解码。日志输出时指令码对应指令名的工作可以之后再做

20220325
========

用当前版本的block编译器编译出的执行文件，在qemu中用-d exec,cpu -D run.log输出日志
时，符号表没有打印出来，也没有其他相关的内容，梳理一下流程

qemu中加载符号表的流程如下::

  main (./linux-user/main.c)
    loader_exec
      load_elf_binary
        load_elf_image
          load_symbols

之后在翻译执行时通过函数lookup_symbol根据pc值来查找对应的符号名称，流程如下，在::

  cpu_exec
    cpu_loop_exec_tb
      cpu_tb_exec
        log_cpu_exec
          qemu_log_mask
            qemu_log
            lookup_symbol
              lookup_symbolxx

在load_symbols函数中会对syminfos链表进行初始化，查找符号的过程是在一个syminfos链
表中进行二分查找，链表内容和结构如下,其中的lookup_symbol_t是一个函数指针，最终指
向lookup_symbolxx ::

  struct syminfo {
      lookup_symbol_t lookup_symbol;
      unsigned int disas_num_syms;
      union {
        struct elf32_sym *elf32;
        struct elf64_sym *elf64;
      } disas_symtab;
      const char *disas_strtab;
      struct syminfo *next;
  };

符号表加载不成功的问题还没找到，继续看一下
