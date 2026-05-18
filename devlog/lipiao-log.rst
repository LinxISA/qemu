..  版权所有 2021

:Authors: lipiao
:Version: 1.0

lipiao的开发日志
^^^^^^^^^^^^^^^^^^^^^^



2020230512
===============

BlockISA v0.20 CARG寄存器组说明::

 CARG寄存器组包括以下寄存器：
 CARG.FLAG[4:0]
 CARG.TGT[63:0]
 CARG.BGET[15:0]
 CARG.BSET[15:0]
 CARG.TRAP[7:0]
 CARG.MSG[63:0] 暂定64位

注::

 CARG.FLAG 寄存器包含Predicate, Negative, Zero, Carry, Overflow五个标记， 用于
 微指令跳转和最终 条件跳转块 做判断，通过微指令进行置位；
 CARG.TGT寄存器用于 间接跳转块&indcall块 的目标地址；
 CARG.BGET初始化为块头get掩码，当使用了某个lgpr时将对应位清楚，提交阶段进行检查；
 CARG.BSET初始化为块头set掩码，但有set.g或set.gl时检查对应位并清除，提交阶段将
 剩余未提交的寄存器提交到全局状态
 CARG.TRAP用于延迟指令标记，该寄存器位数给了8位，由需要延迟执行的指令置位，所有
 块提交时检查。
 CARG.MSG寄存器，该寄存器被设置表示需要将local gpr寄存器提交到gpr同时转存到制定
 的目标内存中

综上：CARG.FLAG 条件跳转块检查，CARG.TGT只在indcall,indirect块进行检查使用，后
面的5个寄存器所有块提交的时候都要进行检查。

20230207
================

关于生成切片内容说明：
CA Model因功能不够完善，无法完整的执行一个应用程序进行性能仿真，因此采用了通过
功能模型执行一个应用程序，通过在执行程序中插桩方式选取其中某个程序片断，截取功
能模型的执行结果生成切片，用于CA模型执行，进行性能仿真，切片内容格式如下：::

 [{
    "Reg":[r0, r1, ..., r31],

     // 用于kernel执行完毕后寄存器校验
    "LastReg":[r0, r1, ..., r31],
    "SysReg":[{"id":123,"value":456}],
    "LastSysReg":[{"id":123,"value":456}],

     // checkpoint的起始执行PC
    "StartPc":0,

    // 指令段
    "Inst":[ {"pc":0,"opcode":0x9091},
             {"pc":2,"opcode":0x9091},...
         ],
    // 内存初始化
    "memAcc":[ {"addr":0x4000,"data":0x4000},
               {"addr":0x5000,"data":0x5000}, ...
         ],
    // 用于kernel执行后的内存校验
    "LastMemAcc":[ {"addr":0x4000,"data":0x4000} ,
                   {"addr":0x5000,"data":0x5000}, ...
         ],
    // 用于memory bank的创建
    "ISection":[ {"begin":0x10,"end”:0x30},
                 {"begin":0x50,"end":0x70},
         ],
    // 用于memory bank的创建
    "DSection":[ {"begin":0x10,"end”:0x30},
                 {"begin":0x50,"end":0x70},
    ],
 }]

Reg: 该字段保存需要该切片执行前所有通用寄存器的值信息，用于CA执行切片前设置
通用寄存器初始值。
LastReg：该字段用来存储该切片结束时的通用寄存器的值，用于CA执行切片后校验执行结果。
SysReg：该字段同Reg，只是寄存器换成了系统寄存器，用于系统模式。
LastSysReg：该字段同LastReg，只是寄存器换成了系统寄存器，用于系统模式。
StartPc：用于指示出切片开始的PC地址。
Inst：所有执行的PC的操作码，所有的操作码按照2Bytes对齐，也就是说块头也拆开成多
个两字节的opcode，后续仿真由CA再组合处理。
memAcc：该字段用于记录该切片中对某个内存地址的首次访问初始值，该记录用于CA
性能仿真时恢复该切片会使用到的初始内存信息
LastMemAcc：该字段用于记录该切片执行完成时修改后的内存信息，用于CA执行完切片后
执行结果的正确性校验。
ISection：该字段是将“Inst”字段中的PC排序后，从“Inst”中第一个PC开始，以1M为长度
去设置begin和end  PC，begin和end的pc值也在“Inst”字段的PC中,该字段在CA中
用于做访问的地址空间检查，即取指的PC一定要在该字段限制的这些地址范围
内。

DSection：该字段是将“memAcc”字段中的地址排序后，从第一个地址开始，以1M为长度
      去设置begin和end 地址，begin和end的地址也在“memAcc”字段地址中,该字段在CA中
	  用于做访问的地址空间检查，即该切片所有的数据访存地址一定要在该字段指定
	  的地址范围内。


20230201
==============
今天协助杨继钦看了下specint17 600 这个测例的问题，该测例执行会导致qemu段错误，
在分析了qemu输出日志，发现是对寄存器压栈过程中出错，写入的地址不对，更进一步
分析，整个程序栈空间使用了7M左右，栈空间耗尽导致，后面继续看了下函数调用，发现
600 这个测例里面函数调用关系是这样的，出现了以下的循环调用的情况，因此导致栈空间
耗尽。

Perl_sv_upgrade -->  Perl_safesyscalloc ---> Perl_croak ----> Perl_vcroak --->
Perl_newSVpvn_flags ----> Perl_sv_setpvn ---> Perl_sv_upgrade

当前仅分析到这，后面需要进一步看看c代码中的函数调用关系是怎样的，在里面加一些
打印看看为何会出现这样循环调用的情况。


20221220
===============
RV、ARM等架构定义的异常只有一级，用一个统一的编号定义了所有异常，Block中将异常
进行了分类，确定一个异常需要通过异常类型tarpnum和异常编号syndrome两个参数，因
此qemu原有的异常触发函数为一个参数，看似不满足于当前需求，实际上我们可以将trap
和syndrome进行组合，当前设计是trapnum占了6位，syndrome占了24位，除此之外还有
qemu自身定义的异常需要考虑，qemu原有异常存储变量为exception_index, 只有32位，
我们可以按如下方式进行融合：

qemu_excp[31:16] + trapnum[15:10]  +  syndrome[9:0]

注：划分如上，虽然syndrome设计为24位，但此处qemu且认为syndrome不会超过1024个，同时按照如下的异常定义，
qemu 异常编号是大于等于0x10000，因此可以极好的将三者融合在一块儿了。

.. code-block:: c

 typedef enum LINXException {
    LINX_EXCP_NONE = -1, /* sentinel value */

    /* INSNuction exception LINX_EXCP_INSN */
    LINX_EXCP_INSN = 0x0,
    LINX_EXCP_INSN_ACCESS = 0x0,
    LINX_EXCP_INSN_TRANSLATION,
    LINX_EXCP_INSN_MISALIGNED,
    LINX_EXCP_INSN_ILLEGAL,
    LINX_EXCP_INSN_PERMISSION,
    LINX_EXCP_INSN_PAGEFAULT,

    /* data exception LINX_EXCP_DATA */
    LINX_EXCP_DATA = 0x400,  // trapnum = 1, syndrome = 0
    LINX_EXCP_DATA_LD_ACCESS = 0x400,
    LINX_EXCP_DATA_LD_MISALIGNED,
    LINX_EXCP_DATA_LD_PAGEFAULT,
    LINX_EXCP_DATA_ST_ACCESS,
    LINX_EXCP_DATA_ST_MISALIGNED,
    LINX_EXCP_DATA_ST_PAGEFAULT,

    /* software exception LINX_EXCP_SOFTWARE1 */
    LINX_EXCP_SOFTWARE1 = 0x800, // trapnum = 2, syndrome = 0
    LINX_EXCP_SOFT1_TRAP = 0x800,

    /* software exception LINX_EXCP_SOFTWARE2 */
    LINX_EXCP_SOFTWARE2 = 0xC00, // trapnum = 3, syndrome = 0
    LINX_EXCP_SOFT1_TRAP = 0xC00,

    /* block exception */
    LINX_EXCP_BLOCK = 0x1000, // trapnum = 4, syndrome = 0
    LINX_EXCP_BLK_IVLD_SET = 0x1000,
    LINX_EXCP_BLK_IVLD_GET,
    LINX_EXCP_BLK_IVLD_PARM,
    LINX_EXCP_BLK_DUP_SET,
    LINX_EXCP_BLK_IVLD_FIXUP,

    LINX_EXCP_BREAKPOINT = 0x1400, // trapnum = 4, syndrome = 0
    LINX_EXCP_ILLSSR = 0xF800,
    LINX_EXCP_UNSUPPORTED = 0xFC00,
 } LINXException;




20221213
=============

系统寄存器&异常实现说明：
将16个特权态公共寄存器部分提出单独定义在SYSreg 结构体中，在CPURISCVState中定义添加具体定义，对于寄存器编号
宏定义命名按照设计文档An_XXREG（n表示ACR特权态编号）命名定义，关于对应寄存器当前仅定义0,1,2,3,4,所
属寄存器LINX_MAX_ACR_NUM 表示最大的ACR特权态编号值，LINX_EBSTATE_SIZE 指示ebstate中寄存器个数。对于异常定义，当前
异常设计包含两个层次，第一层LINXL1Exception指令异常、数据异常、软件提权异常、块异常、断点异常等，第二层
LINXL2Exception对每一类异异常进行细分，包含了第一层中所以异常对应类型的细分类型，两层分别定义各自的枚举类型。
对于所有寄存器读写函数实现，后续参照RV逐步给出,关于以上描述，详细的内容太多，单
独在群里给出文件。




中断控制器设计方案阅读记录
============================

挑选路由目标::

 1. 为何会说“中断路由配置”是收集中断模块的输出？？
 2. 挑选目标后的输出中“中断优先级” 和 “中断使能字” 从哪来？？

登记中断信息::

 1. 中断响应前被产生了多次，这个多次的区分是通过中断类型号区分还是中断类型号加中断地址以及其它信息呢，表项内容是什么？
 2. 多次提到MMIO这个组件，可以了解下。，还有DDR。。。

挑选中断::

 1. 本节中给出了登记的条目序号越小，上报优先级越高，那么也就是说在登记中断信息时就必须已经确定好了中断优先级？？
 2. 挑选中断并输出中断条目号

过滤中断::

 1. 好奇，这个私有pengding缓存是如何处理同一个核多多个特权态场景的。。。。。

应答中断::

 1. 该节中中断应答接口，软件读取该接口，，这个不是由硬件自己上报的吗？为何需要软件进行读取，，？
 2. 应答中断接口实现在MMIO中，如何由LinxCore的MMU来进行访问权限控制呢？这个不太理解

虚拟中断直通::

 1. 该节中描述的情况，中断控制器是能够看到进程的概念的吗？我理解中断控制器是仅仅能根据自身的配置信息将中断分配到各
    个hart，只能看到LinxCore的硬件啊，，为何可以看见进程呢。。。
 2. 每个核有一个存储vCPU编号的位置，要求软件（OS）去配置，那么OS就必须感知虚拟化的进程，这样理解不知道对不。。。，但
    我个人认为所有软件不管是模拟器还是其它普通应用程序，对于OS来说应该看到的都是一样的才对呀。。。


上一个版本国柱评审意见::

 1. 建议写到LinxTechAnalyse中，这样容易统一管理，这毕竟不是一个临时的设计（我知道王州已经让你放了一个版本过去了，不过我这个是对devlog上的原文的评审，这个意见我继续留着。你可以把我的整个意见放到你的devlog中）
 2. 需要设计一下当前配置库的tag和分支管理，原来的版本估计还要升级的，你一把修改过去了，中间砸了要退回去。或者中间要做一个特定的测试，你也需要原来的版本来支持。这个要分开处理。
 3. 需要设计一些阶段点，在这些点上，整个qemu可以跑起来，这样才能验证我们达成那个阶段点没有了。
 4. 尽量不要用“当前QEMU已经去除RV相关的代码”这种说法，你的版本和分支管理确定以后，你可以直接说那个分支如何如何，这样我们有明确的指示，否则都不知道你指什么。
 5. 我其实比较关心你的设计文档，一个很重要的原因是：我想知道你这一波修改打算支持哪些特性，因为若愚其实一直在更新他的定义，但我知道kernel团队有不少特性这次是不打算做的。编译器也有不少特性不可能理解提供，而你要正常到达每个阶段点，你需要所有人的特性和你配合，否则你跑都跑不起来。所以，我想知道你到底承认哪些特性，做什么功能。你这里没有给出这个定义。
 6. CPU状态定义，这个事情不是kernel组在做吗？你这里是打算说不等他们完成，你先定义一个自己的，做做再说？（就是我形容的“练兵”？）我倒不反对你要做这种练习，但麻烦再拉一个分支，别污染主线分支，否则后面只能回退，那你练习的东西也没了。
 7. 具体说这个特权树的设计：数据结构是为使用服务的，不是为了表明存在而实现的。你表达这个特权树模型是为了什么呢？我看不出理由，你让我说，我认为只有当前acr是必须的，其他东西，有什么寄存器，就定义什么就好了。写棵树在那里，谁要用它？如果你要实现发生异常或者中断的时候，判断向什么特权级来跳转，那你就要先给出这个判断算法，然后才定义数据结构。而且作为一个静态算法，这东西怎么会需要放在CPUState里面呢？

根据上述意见修改如下，请国柱审阅

ACR和BlockISA融合设计QEMU实施方案编写思路
=========================================

编写实施方案思路::
 1. 当前QEMU现状（已有可复用的、没有需要新增的部分）
    1. 中断异常处理（包含委托）
    2. MMU
 2. 当前融合设计文档包含了哪些部分

基于以上两点：：
 1. 分析当前QEMU有哪些模块，需要梳理一下
 2. 当前融合设计文档包含了哪些部分，对当前的设计进行划分
 3. 给出后续各个部分在QEMU中的实施方案

注：1. 文档参考LinxTechAnalyse文档
    2. 整体设计可参考王州10-18的QEMU设计

ACR和BlockISA融合设计QEMU实施方案
==================================
QEMU代码基于B002发布版本，BlockISA设计基于0.13版本，继续后续B100版本设设计实现；B100版本主要是要替换掉原有的RISC-V特权级架构，
这个和王州讨论了一下思路，在QEMU实现上替换的两种思路::

 1. 分步骤一个一个模块用BlockISA特权集去替换掉原有的内容。
 2. 先把RISC-V相关的代码分多个步骤去除，根据融合方案重写所有的模块。

最终我们选择先去除QEMU RISC-V相关代码（这个可以分步骤，多个Patch去完成），重新实现前融合设计，
同时在实现的过程中尽量不去修改原有代码的整体结构；原有B002版本QEMU在linx-dev分支已经给出tag，
我们后续的QEMU实现代码基于B002发布版本，代码仓仍旧使用linx-dev分支。

基于当前ACR与BlockISA的融合方案，QEMU计划实现以下特性::

  1. ACR 特权态树模型
  2. 指令中断/异常处理（包含中断/异常委托特性）
  3. 特权态相关指令实现


根据以上需要实现特性并参考王州10.8的分析文档，初步两个阶段，第一阶段将BIOS起来起来，
第二阶段启动内核进行联调，详细如下::

 第一阶段：
  1. 特权态树模型的定义。
  2. CPU状态寄存器、异常类型等QEMU数据结构定义实现。
  3. 相关特权态指令实现
 第二阶段：
  4. 内存管理单元修改适配
  5. 中断/异常处理
  6. Virt平台设备初始化 -- 这个貌似不用管
  7. 中断接收处理机制
  8. 机器启动停止

1. 特权态树模型的定义。
ACR特权态的定义不同于RISC-V，当前中断/异常委托处理等需要考虑到特权态之间的关系是怎样的，因此
我们需要将当前特权态关系在QEMU中体现出来，对于这样的一个树状特权关系，在qemu中最简单的实现方法
可以单独使用一个数据结构存储，用一片连续的空间来表示这个树::

 1.双亲表示法：用一个大小为16的int型数组，存储的是其父亲的索引（求父节点方便）
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

 bool check_act_request(int aacr,int int target_acr);
 bool check_act_enter(int aacr,int int target_acr);

注：当前仅看到从子节点向上查父节点一个需求，但并未确定从父节点到子节点查询需求是怎样的, 待确认。



2. CPU状态寄存器、异常类型等QEMU数据结构定义实现

当前定义了0-4五个特权级，不过仅0,1,3三个特权级包含以下系统寄存器::

 异常处理类：An_ECSTATE,An_EVBASE,An_ECAUSE,An_EARG0,An_ELINK,An_ETEMP,An_FUTO,An_IENABLE,An_IPENDING,An_IPRIORITY,An_EDEL0
            An_EBSTATE_ST,An_EBSTATE_R0...An_EBSTATE_R63
 内存管理  ：An_MMTBASE
 公共寄存器：CSTATE

异常类型定义::

 0: Instruction exception
  1.Instruction access fault
  2.Instruction translation fault
  3.Instruction misaligned
  4.Instruction illegal
  5.Instruction permission fault
 1: Data exception
  1.Load access fault
  2.Store/A_op access fault
  3.Load misaligned
  4.Store/A_op misaligned
  5.Load permissions fault
  6.Store/A_op misaligned
 2：Software escalate exception
  TODO, 软件提权，包括ecall，设计中, 块内的ecall异常也报到这里
 3: Block exception
  1.Invalid set_regs detected
  2.Invalid get_regs detected
  3.Invalid Parameter
  4.Duplicated set to the same GPR
  5.Invalid fixup block
 4：Breakpoint exception, 块内的ebreak异常报到这里

 4~61: Reserved

 62: Illegal SSR，非法SSR ID

 63: Unsupported exception, 不支持的异常

说明：以上寄存器中的n取值0,1,3，代表三个特权态分别有这样一套寄存器定义，地址编码
以及字段内容参考 “系统寄存器高层设计” 文档，详细的编码方案由具体实施时确定。除了
上述的状态寄存器外，QEMU中实现还应该包含一个当前特权级priv定义;另外关于异常类型，
当前设计对异常进行了分类，我们在QEMU中定义的时候应该考虑分别定义两个层次的异常枚举
值，第一层指指令异常、数据异常、软件提权异常、块异常、断点异常等，第二层指每一类
异常下细分的详细异常号，这两层分别对应于An_ECAUSE寄存器中的TRAPNUM/SYNDROME域。

3. 系统指令的实现
当前定义的系统指令：ETRAP, ERET, FENCE(考虑沿用RV定义), FENCE.I, ATMKB,  WFI, EBRK, SYSGET, SYSSET
说明：FENCE.I 该指令用于刷Cache可以考虑不用实现或者用RV的，ATMKB指令和RV SFENCE.VMA指令相同，RV是一把刷掉TLB，照抄实现即可，EBRK 指令内核用于调试，需要产生一个异常。
因此当前必要实现是 ETRAP/ERET/FENCE/SFENCE.VMA/EBRK，WFI用于等待中断？可选。


4. 内存管理实现
内存管理沿用了RV的设计，QEMU代码基本可以复用，去掉two stage翻译机制，不考虑虚拟化。


5. 指令中断/异常处理

这部分包含了中断异常委托过程，异常处理具体设计实现包含了上下文保存，过程如下::
进入异常/中断::

  1.硬件将CSTATE的值备份到An_ECSTATE(n表示目标特权态)。
  2.将块内状态备份到An_EBSTATE(n表示目标特权态，如果是块内被打断)。
  3.清零CSTATE.I（关中断）。
  4.异常原因写入An_ECAUSE(n表示目标特权态)。
  5.异常发生地址写入An_ELINK(n表示目标特权态)。
  6.根据具体的异常类型设置An_EARG0(n表示目标特权态，如果是page fault的话，将page fault的地址写入)。
  7.进入目标特权态A<n>。

以上几个步骤在硬件上是一个原子操作，不可被打断。

中断/异常返回::

  1.将An_ECSTATE(n表示目标特权态)恢复到CSTATE寄存器。
  2.硬件**不对**EBSTATE_A<r>做恢复，软件需要根据实际情况决定是否恢复。
  3.返回到特权态ECSTATE.ACR的An_ELINK(n表示目标特权态)位置继续执行。

以上几个步骤在硬件上是一个原子操作，不可被打断。

以上参考“ACR特权态切换方案设计”文档做了简单修改，给出了中断/异常进入和退出过程,
关于中断异常的处理部分，异常进入我理解QEMU实现仅仅需要提供切换合法性检查、现场的保存、关中断、
填写异常原因即可，即设置上述每个步骤涉及的寄存器到对应的状态即可，而不需关心何时会
进行acr_request, acr_enter。

6.virt平台外设的定义
根据QEMU riscv virt平台外设分析文档，当前必要设备包含rtc，串口，virtio-mmio，中断控制器,
这一部分王州给出建议是基本可以不用做修改。

7. 中断接收处理机制
当前由内核团队负责设计和实现？-- 待确认


8. 机器启动停止
当前virt平台提供了sifive_test设备，该设备提供关机功能，开机我估计不会要，能正常启动就好。



LinxInit支持多核启动、向低特权级提供服务

 LinxInit 启动流程梳理::

  1. 只启动一个核, 直接跳到os所在的特权级地址就行了.
     (只启动一个核, 唯一的服务cpu_start也不需要了)
  2. 启动多个核 (支持cpu_start服务, bios发起服务)
     主核: 初始化(?) -> 设置异常向量 -> 发起服务唤醒其他核 -> eret返回 \
     从核: 等待被唤醒 -> 设置异常向量  ->                          /

  3. 支持更多的服务
     可以考虑用一个表(数据段内存)来保存服务的函数指针, 然后根据异常进来时的参数,
     去查这个表去执行, 执行过后eret.

     异常可能是内核发起的, 所以处理服务 要保存和恢复寄存器.
     可能需要类似 mscratch作用的SSR寄存器, 分配私有空间, 去做这个保存和恢复(可以顺便支持C语言)

     这样启动变成这样:

     主核: 初始化(为每个核分配私有空间) -> 设置异常向量 -> 发起服务唤醒其他核 -> eret返回 \
                                                                                         跳到os所在地址
     从核: 等待被唤醒 -> 去认领这个私有空间 -> 设置异常向量  ->                          /

     计划全部用汇编实现.

  LinxIinit任务划分::

  代码分为两部分, bios

    1. qemu 实现cpu的 start 和 halt功能, 通过一个SSR实现, 用于实现cpu_start/stop服务
    2. 实现bios的单核启动, 统一设计代码逻辑
    3. 实现多核启动, 编写 中断异常委托, 异常入口, 服务入口. 区分主核和从核的启动代码. 实现cpu_start服务.
    4. 支持 C语言 和 来自OS的服务请求, 主核需要 配置所有核的私有空间, 需要类似mscratch的SSR寄存器
    5. 移植opensbi的fdt代码
    6. 添加驱动框架, 考虑添加串口驱动
    7. 添加更多服务

    LinxInit 简单验证测试
       bios 从0x80000000开始.
       test 从0x80200000开始, 目前作用是向串口输出一个字符, 可以判断主核或从核是否执行到这里, test 可能会添加更多代码 用来验证 bios的更多特性.
  说明：中断异常委托同时也要在1、2两个任务中完成。

当前因一次QEMU删掉太多并且无序，我们希望按步骤一块一块去删除RV相关的代码，因此当前比较
紧急的事情是分多个patch完成去代码RV化；而后按照上述分析流程，按上述整体8个任务来说
1.特权态树模型 和 2.cpu状态定义 可同步做，2很快可完成后开始 3. 特权指令定义实现，
4. 内存这个和其它依赖不大，和前面3点同步去做，5.中断异常部分等前面 1, 2完成后即可开始，
第7点同步分析下，第8点关系不大。另外，LinxInit 的具体实现估计也是有我们qemu组同事去完成，
因此在上述中晓强给出了初步的实施步骤。

遗留问题
系统寄存器高层设计::

 1. 异常委托寄存器 An_EDEL0， 名称有误多了个0？
 2. 中断委托寄存器未定义
 3. An_EBSTATE_ST 中 rmax和sz字段宽度过宽，是否重新定义。
 4. CSTATE寄存器中P位沿用RV的设计，还是Supervisor指示高于Umode的特权态？ ACR中都没有Supervisor概念。。。
 5. 关于编址规则中限定最低访问特权态的设计是指只能在当前分支上？
 6. 禁止将异常路由到比实施修改的特权态更高的特权态，也就是，实施修改的特权态不能比目标特权态低。
 7. 允许每个特权态有自己独立的页表（这点不太理解）
 8. 关于中断委托在遇到分支情况是如何进行处理的？中断类型是否被固定在某个特权级，或者按照当前被中断的特权级计算？




20221026
================

更新任务安排::

 1.cpu状态寄存器。                                --杨继钦 10-27
 2.qemu启动/停止部分汇编。                        --贾文杰 10-27
 3.异常处理移植。(需要eret指令)                   --石晓强(测例)、韩志林(开发) 10-28
 4.异常委托。                                     --晓强(测例)、韩志林(开发)
 5.(和3并行)特权级指令移植。                      --文杰(开发、测例)  10-28
 6.RV特权级手册结合qemu实现讲解。(划定范围)       --待划分
 7.ACR手册讲解。(from BlockISA ACR特权级切换方案设计 + Wiki)  --待划分
 8.命令行/配置文件 控制特定ACR开启(ACRCFG 是否功能类似) --继钦(开发)、浩富(测例)
 9.中断处理移植。                                 --文杰(开发)、晓强(测例)
 10.中断委托。                                    --文杰(测例)、晓强(开发)
 11.内存管理--地址翻译。                          --李飘(开发、测例 暂定一层)  10-27

注::

 1. 当前我们做这个事情，要求按照后面正式的开发流程去走 要求也是和后面的要求是一
    致的，当前我们做的虽然最终结果还未定，但大体上不会变化特别大，后面或许很多
    只需要简单改下可以复用。
 2. 另外在于大家要加深对rv & acr 以及对应qemu实现的理解，为后期做正式做准备

20221024
================

本周任务安排::

 当前能提前做的一些事情
 1. 状态寄存器以及对应的操作 从linx-lxr分支移植到 linx-b100 -- 贾文杰  10-27
 2. 中断异常处理部分代码移植工作                            -- 石晓强  10-27
 3. 内存管理部分代码移植工作                                -- 李飘    10-27
 4. qemu中的启动和停止部分代码修改                          -- 贾文杰  10-27
 5. 减掉rv中用户态不必要代码                                -- 石晓强  10-27

 当前需要深入分析的点
 1. 陈立福提供的初步特权级架构定义方案 -- 王州 李飘
 2. rv M、S、H模式在qemu中的详细实现  -- 王州 贾文杰 石晓强 李飘  10-28
 3. 异常中断的触发和委托机制 -- 贾文杰
 4. ACR设计方案 
 5. RV转换到ACR的替换方案串讲 -- 王州 贾文杰 石晓强 李飘


20220902
================
昨天完成了Fixup在qemu上的实现问题，问题如下::

 1. 中断异常处理函数在检查Fixup处理之后直接return未将异常号清除，导致一直在中断
    异常函数循环
 2. 检查异常时未对中断和异常做区分，并且指定了下个fixup异常处理的时候未将bpc设置
    为一个有效的值，可能会出问题
 3. 检查异常号时超出指定的异常号之外的异常未排除掉

后面重新分析了下，Fixup异常处理时只是设置了env->pc 没有对bpc进行设置，在pc没去
掉时是多余的，即使立刻来了一个中断，中断的epc用的是env->pc的值（中断返回来是会
返回到正确的Fixup处理块），不会存在什么问题。
今天检查了一下LARM中部分指令的描述，patch已发给王州合入



20220824
================
项目进展::

 1.  支持打开块内中断，刷新取消SGPR的方案，LARM已更新，Qemu编码完成，编译器和手
     写汇编要保证 set后不能get同一个寄存器，编译器保证sp先开栈，后续才能用栈 -- 石晓强
 2.  块内跳转方案 在设计中 方案待定， 方案已更新到larm，块内跳转指令   -- 贾文杰 石晓强
 3.  Fixup 块功能 方案已定，LARM 已更新，Qemu已实现, 待评审-- 石晓强 李飘


20220822
================
项目进展::

 1.  支持打开块内中断，刷新取消SGPR的方案，LARM已更新，qemu已实现，编译器和手写汇编要保证 set后不能get同一个寄存器，编译器保证sp先开栈，后续才能用栈 -- 石晓强
 2.  GPR local Index方案  LARM 在V0.13-local-index分支上 可进入编码阶段，qemu已编码完成，待review  -- 编译器验证 后续不跟踪
 3.  块内跳转方案 在设计中 方案待定， 方案已更新到larm   -- 贾文杰 石晓强
 4.  BlockISA Local Storage特性 方案挂起  -- 后续不跟踪
 5.  fixup 块功能 方案已定，LARM 待更新，qemu这边实现有些疑问 -- 石晓强 李飘
   5.1 fixup larm没更新，委托寄存器地址未确定 bedeleg  U0x800  S0x9C0
   5.2 fixup 有没有规定某些异常不能走fixup异常流程的？-- 没有，由用户确定



20220819
================

关于fixup特性在qemu上的实现分析
根据立福给出的分析文档，第一 fixup异常由软件确定，第二 fixup代码地址编码在块头


20220818
================
项目进展::

 1.  支持打开块内中断，刷新取消SGPR的方案，LARM已更新  -- 石晓强
 2.  GPR local Index方案  LARM 在V0.13-local-index分支上 可进入编码阶段  -- 编译器验证 后续不跟踪
 3.  块内跳转方案 在设计中 方案待定  -- 贾文杰 石晓强
 4.  BlockISA Local Storage特性 方案挂起  -- 后续不跟踪
 5.  fixup 块功能 方案已定，LARM 待更新 -- 李飘

20220810
================
项目进展::

 1. 支持块内中断，刷新取消sgpr方案, 方案确定，待larm更新 --石晓强 贾文杰
 2. 支持块内跳转 方案讨论中 -- 王州
 3. 支持fixup块功能，方案讨论中-- 李飘
 4. GRP local Index 方案实现,方案讨论中  -- 石晓强
 5. BlockISA Local Storage 实现, 方案讨论中 -- 贾文杰  王州

20220809
================
项目进展::
 1. 支持块内中断，刷新取消sgpr方案 --石晓强 贾文杰
 2. 支持块内跳转 方案讨论中 -- 王州
 3. 支持fixup块功能，方案讨论中-- 李飘
 4. GRP local Index 方案 gpr初步实现  -- 石晓强
 5. BlockISA Local Storage 实现, 方案讨论中 -- 贾文杰  王州

今天主要加深了fixup异常处理流程两种方案的理解，立福给的两种方案中，不同点在于
第一个方案将fixup处理异常固定由硬件做出判断，直接跳转到fixup异常处理流程，而第
二个方案则是提供一个可配置寄存器，由软件指定某些异常处理走fixup流程。共同点在于
都对异常做了划分，并非都先走中断向量表去跳转到传统中断处理，处理不了再走fixup。
针对以上两个方案给出了两种跳转到fixup处理流程的实现方案::

 1. 在块头定义fixup属性（fall，direct，indirect三种），并要求必须是bnext.concat块
 (因为在扩展字段中要放fixup_addr 的地址)
 2. 在块头添加fixup属性，但相较于上面不要求bnext.concat跳转属性，但需要新增一条
 微指令用于设置fixup异常处理流程的地址，当块内发生异常的时候能够跳过去处理。





20220714
================

项目进展::

 1. 调整执行时get/set掩码检查，修改异常实现 代码完成，待review  -- 石晓强

讲解交流::

 1. rv_entry.S rv_head.S  -- 王州
 2. rv特权级总结 -- 王州
 3. qemu tcg 分析 -- 王州
 4. lr/sc 在开关块内中断情况下出错的问题定位流程  讲解完成 -- 贾文杰
 5. 中断恢复地址出错问题，sebstate被意外修改问题，定位流程分享 -- 李飘
 6. monitor 使用讲解  -- 石晓强

今日完成lr/sc指令功能以及实现讲解，完成了get/set掩码检查部分代码，后期主要协助内核
OS联调，同时需要确定下qemu团队下一步需要更加深入了解的方向。



20220710
================
记录一次修复一个bug引入另一个bug的问题：
起因源于当前qemu的中断异常处理函数riscv_cpu_do_interrupt，会进入qemu中断异常处
理函数进行处理。riscv_cpu_do_interrupt函数说明，在函数开始时根据当前的模式获取
了保存现场需要用到的ebstate（当然从U模式产生的异常也获取的sebstate随后便设置这
个sebstate），即当S模式下产生了一个m_timer中断，进入中断处理函数后，由于模式尚
未切换，那么通过当前模式去获取ebstate就只能获取sebstate，即应该在m模式下处理的
中断将S模式的ebstate修改了，这就会出错。简单的描述一个场景：
启动内核，执行用户态程序时产生了一个ecall异常，此时sebstate.st.cause 被设置，
内核切换到中断异常处理地址，先进行现场保存（主要是通过st.cause计算该ecall返回
地址并保存），但在现场保存前出现一个m_timer或是该在m模式处理的其他异常，将尚
未使用的sebstate修改了,因此导致后续ecall返回地址计算错误。
处理该问题，我给的解决方法是先定义一个ebstate_st变量，当异常进来之后直接设置该
变量的值，等确定该中断在何种模式下处理时才将写好的ebstate_st设置到对应模式的
ebstate中。


存在的问题::

 1.在修改这个ebstate_st时仅仅检查了riscv_cpu_do_interrupt中对ebstate_st有设置过
   的地方，漏掉了在linx_save_bstate()函数中对ebstate.st的修改。
   对st.cause的设置。
 2.在与OS联调处理排查当时那个问题时未能详细检查当前的qemu版本，与后续是否有对qemu做
   问题修复相关提交。

后续改进措施::

 1. 在修改代码中原有逻辑时，应先梳理对ebstate.st修改的模型，以确定修改不会漏掉,
    而不是简单的修改对应函数中能看到的部分。
 2. 后续检查问题时，应先检查当前的qemu版本是否最新，或是当前版本存在什么缺陷，后续
    是否有相应的修复。

代码提交的一点点建议::

 1. 当需求有变化并且涉及到多方需要同时更新才能使用的情况（qemu和内核），qemu这边
    应该将已经实现的需求推送到一个临时分支上，而不应该直接推送到主分支，这会导致
    后续如果有qemu代码修复的提交，在内核尚未使能之前的需求的情况下OS是没法使用当
    前的已修复bug了的qemu版本。

 2. 后续联调过程中若有重大bug修复，在提交后应该在群里通知到联调的各方及时更新版本
    重新测试验证。

riscv_cpu_do_interrupt::

  void riscv_cpu_do_interrupt() {
      ...

     if (is_valid_linx_addr(env->bpc) || is_valid_linx_addr(env->tpc)) {
         /* bpc is valid, so tpc should be sync-ed with pc, ret address is this block */
         if (is_valid_linx_addr(env->bpc)) {
             env->sepc = env->bpc;
             env->tpc = env->pc;
         }

         save_state = true;
         linx_save_bstate(env);
         linx_reset_bstate(env);
     } else {
        assert(!is_valid_linx_addr(env->bpc));
        assert(!is_valid_linx_addr(env->tpc1));
        assert(!is_valid_linx_addr(env->tpc2));
        env->sepc = env->pc;
    }

    ...
  }

linx_save_bstate::

 void linx_save_bstate(CPURISCVState *env)
 {
    struct LINXEbstate *ebstate = get_ebstate(env);

    ebstate->reg[EBSTATE_REG_TPC] = env->tpc;
    ebstate->reg[EBSTATE_REG_CARG] = env->carg;
    ebstate->reg[EBSTATE_REG_SR_MASK] = env->sr_mask;

    if ((env->bpc + 16) == env->next_bpc) {
        ebstate->st = set_field(ebstate->st, EBSTATE_ST_SZ,
                                EBSTATE_ST_SZ_SZ128);
    } else if ((env->bpc + 32) == env->next_bpc) {
        ebstate->st = set_field(ebstate->st, EBSTATE_ST_SZ,
                                EBSTATE_ST_SZ_SZ256);
    }

    ebstate->reg[EBSTATE_REG_IBPC] = env->ibpc;
    memcpy(&ebstate->reg[EBSTATE_REG_T_REG_START], env->blk_t,
           8 * sizeof(uint64_t));
    memcpy(&ebstate->reg[EBSTATE_REG_SGPR_START], env->sgpr, sizeof(env->sgpr));

    ebstate->st = set_field(ebstate->st, EBSTATE_ST_RMAX, EBSTATE_MAX_VALID_REG);
    ebstate->st = set_field(ebstate->st, EBSTATE_ST_VLD, 1);
 }

20220708
================
项目进展::

 1. os 遇到对一个变量自增1时，输出增加后的结果不正确，定位到在第一次读取该变量
    的值时已经不正确，现怀疑是内核在处理缺页中断，Qemu的锅，而且还是我引入的问题，待复盘问题  -- 李飘
 2. bget/bset 掩码检查larm 上已更新，qemu代码已实现，待review，进入shell掩码检查出错，
    内核待实现sr_mask 保存，后续不跟踪    -- 石晓强
 3. 执行到执行pc时打开exec（临时确定，后面根据需要加）日志，代码已实现,已合入主分支  -- 石晓强
 4. 原子指令在关块内中断情况下错误排查,当前分析结论是sc写入成功的次数与预期相同，但将
    某一个数值重复写入，待继续分析原因 -- 王州 贾文杰
 5. 调试过程中发现了m模式在返回的时候出现CS_IN(0) Addr (800005f4=>800005f4) Priv(1=>1) 错误 国柱已处理

当前qemu lr/sc 问题处理后会有些时间，晓强将qemu中todo和fixme整理下，看看能否处理，
同时后期可能会替换掉rv特权架构，各位继续深入了解qemu各个部分以及riscv特权架构,
为后期能更好更快处理做好准备,后续讨论下方案。

20220707
=================
项目进展::

 1. os 遇到对一个变量自增1时，输出增加后的结果不正确，定位到在第一次读取该变量的值时已经不正确，待继续定位  -- 李飘
 2. bget/bset 掩码检查larm 上已更新，qemu代码已实现，待review，进入shell掩码检查出错    -- 石晓强
 3. 执行到执行pc时打开exec（临时确定，后面根据需要加）日志，代码已实现，待review  -- 石晓强
 4. 原子指令在关块内中断情况下错误排查,继续排查 -- 王州 贾文杰

20220706
=================

项目进展::

 1. os 遇到对一个变量自增1时，输出增加后的结果不正确  待定位  -- 李飘
 2. bget/bset 掩码检查larm 上已更新，qemu代码已实现，待review，进入shell掩码检查出错    -- 石晓强
 3. 执行到执行pc时打开exec（临时确定，后面根据需要加）日志，代码已实现，待review  -- 石晓强
 4. 原子指令在关块内中断情况下错误排查 -- 王州 贾文杰
 5. 在指定pc地址上打开单步指令执行 -- 李飘
 6. fence.i 指令的实现，如果该指令不是块最后一条指令，可能存在问题

20220705
=================

项目进展::

 1. os 遇到对一个变量自增1时，输出增加后的结果不正确  待定位  -- 李飘
 2. bget/bset 掩码检查larm 上已更新，qemu代码已实现，待review    -- 石晓强
 3. 执行到执行pc时打开exec（临时确定，后面根据需要加）日志  -- 石晓强
 4. 定位中断异常时ibpc 出错，内核虚拟地址错误问题，大致定位栈保存，按照国柱的方案（动态计算保存寄存器个数）修改， 代码已提交-- 王州 贾文杰
 5. ebreak指令产生异常时，内核无法启动，system模式模式代码已提，user模式暂不支持 -- 贾文杰
 6. fence.i 指令的实现，如果该指令不是块最后一条指令，可能存在问题
 7. 原子指令的排查

20220704
=================
项目进展::

 1. 定位中断异常时ibpc 出错，内核虚拟地址错误问题，大致定位栈保存，按照国柱的方案（动态计算保存寄存器个数）修改，代码review中 -- 王州 贾文杰
 2. ebreak指令产生异常时，内核无法启动 代码已实现，待review -- 贾文杰
 3. bget/bset 掩码检查larm 上已更新，代码已实现 待review-- 石晓强
 4. 执行到执行pc时打开exec（临时确定，后面根据需要加）日志  -- 石晓强
 5. fence.i 指令的实现，如果该指令不是块最后一条指令，可能存在问题


20220701
=================
项目跟踪::

 1. 定位中断异常时ibpc 出错，内核虚拟地址错误问题，大致定位栈保存，按照国柱的方案（动态计算保存寄存器个数）修改-- 王州 贾文杰  
 2. ebreak指令产生异常时，内核无法启动 -- 贾文杰
 3. bget/bset 掩码检查larm 上已更新，待实现   -- 石晓强
 4. fence.i 指令的实现，如果该指令不是块最后一条指令，可能存在问题
 5. 添加 linx_debug 16位调试指令

昨天看了一个OS那边的问题，先描述下问题吧，在关闭块内中断进入内核后，执行
'cat fi | grep 2' 这个指令，文件fi不存在，程序会先提示一个文件不存在的错误，然后
内核会打印出未处理的信号11的一些错误日志，异常号为12，访存异常，这个初步和春华看
了下，像是busybox在执行命令时申请内存并计数申请次数，命令执行完后依次释放，在执行
上述命令时出现申请内存计数错误，而后对内存进行重复释放，因此出现访存问题。在上述
命令后多加一个或多个'| grep'都不会有问题。一条指令也不会出错，就在执行上述指令时
才必现。
异常现象如下::

 / # cat 1 | grep 2                    -- 执行上述命令
 In parent shell: child = 39
 forkparent pre jp->nprocs = 0
 forkparent back jp->nprocs = 2        -- 异常时这个nprocs加得不正确
 [   28.021916] random: fast init done
 forkparent ps->ps_cmd = 0x86e840 jp->nprocs = 2
 In parent shell: child = 40
 NO Job hack
 forkparent pre jp->nprocs = 2
 forkparent back jp->nprocs = 4       -- 从上面的2 ++ 然后变成了 4
 forkparent ps->ps_cmd = 0x86fa70 jp->nprocs = 4
 Frame 0: PC=0x3cc860
 freejob jp->nprocs=0
 freejob jp->ps = 0x86d1a0 
 -/bin/sh: can't set tty process group: No such file or directory
 cat: can't open '1': No such file or directory
 Frame 0: PC=0x3ccdc0
 freejob jp->nprocs=4
 freejob ps->ps_cmd = 0x0 ; nullstr = 0x86b3a4
 freejob ps->ps_cmd = 0x86e840 ; nullstr = 0x86b3a4
 freejob ps->ps_cmd = 0x21 ; nullstr = 0x86b3a4
 [   28.200931] sh[38]: unhandled signal 11 code 0x1 at 0x0000000000000019 in busybox[10000+854000]
 [   28.202855] CPU: 0 PID: 38 Comm: sh Not tainted 5.16.0-rc3-gbe29e7f228fe-dirty #400
 [   28.203724] Hardware name: riscv-virtio,qemu (DT)
 [   28.204395] epc : 00000000005d5560 ra : 00000000003c83c0 sp : 0000003fdd5a6940
 [   28.205172]  gp : 0000000000867290 tp : 000000000086a7a0 t0 : 0000000000000020
 [   28.205862]  t1 : 0000000000000000 t2 : 0000000000000010 s0 : 0000003fdd5a6980
 [   28.206786]  s1 : 000000000086c430 a0 : 0000000000000021 a1 : 000000000086e930
 [   28.207487]  a2 : 000000000000002f a3 : 0000000000000000 a4 : 0000000000000021
 [   28.208175]  a5 : 0000000000000021 a6 : 0000000000000000 a7 : 0000000000000040
 [   28.208864]  s2 : 0000000000000001 s3 : 0000003fdd5a6dc8 s4 : 0000000000867410
 [   28.209553]  s5 : 0000003fdd5a6dd8 s6 : 00000000002bbc00 s7 : 0000003fdd5a6cc8
 [   28.210480]  s8 : 0000000000000001 s9 : 0000000000000000 s10: 000000000000000a
 [   28.211201]  s11: 0000000000000000 t3 : 0000000000000000 t4 : 000000000086b3a4
 [   28.211888]  t5 : 0000000000000000 t6 : 0000000000000000
 [   28.212429] status: 0000000000000020 badaddr: 0000000000000019 cause: 000000000000000d
 [   28.213154] ebstate.st: 0000000000000000

正常命令执行::

 / # cat 1 | grep 2 | grep 3
 In parent shell: child = 42
 forkparent pre jp->nprocs = 0
 forkparent back jp->nprocs = 1
 forkparent ps->ps_cmd = 0x86e950 jp->nprocs = 1
 In parent shell: child = 43
 forkparent pre jp->nprocs = 1
 forkparent back jp->nprocs = 2
 forkparent ps->ps_cmd = 0x86fb90 jp->nprocs = 2
 NO Job hack
 In parent shell: child = 44
 forkparent pre jp->nprocs = 2
 Frame 0: PC=0x3cc860
 freejob jp->nprocs=0
 freejob jp->ps = 0x86fb20 
 forkparent back jp->nprocs = 3
 forkparent ps->ps_cmd = 0x86fbb0 jp->nprocs = 3
 NO Job hack
 Frame 0: PC=0x3cc860
 freejob jp->nprocs=1
 freejob ps->ps_cmd = 0x86e950 ; nullstr = 0x86b3a4
 NO Job hack
 freejob jp->ps = 0x86fb20 
 Frame 0: PC=0x3cc860
 freejob jp->nprocs=2
 freejob ps->ps_cmd = 0x86e950 ; nullstr = 0x86b3a4
 freejob ps->ps_cmd = 0x86fb90 ; nullstr = 0x86b3a4
 freejob jp->ps = 0x86fb20 
 cat: can't open '1': No such file or directory
 Frame 0: PC=0x3ccdc0
 freejob jp->nprocs=3
 freejob ps->ps_cmd = 0x86e950 ; nullstr = 0x86b3a4
 freejob ps->ps_cmd = 0x86fb90 ; nullstr = 0x86b3a4
 freejob ps->ps_cmd = 0x86fbb0 ; nullstr = 0x86b3a4
 freejob jp->ps = 0x86fb20
 

20220630
==================
项目跟踪::

 1. 定位中断异常时ibpc 出错，内核虚拟地址错误问题，大致定位栈保存  待确认 -- 王州 贾文杰
 2. ebreak指令产生异常时，内核无法启动 -- 贾文杰
 3. 修改m模式下日志控制参数，添加到外部接口 -d 参数  -- 石晓强
 4. 添加一个从指定pc开始打印日志的功能 -- 石晓强
 5.  添加 linx_debug 16位调试指令
 6. bget/bset 掩码检查在架构上待定义，qemu尚待实现
 7. fence.i 指令的实现，如果该指令不是块最后一条指令，可能存在问题


20220629
==================
项目跟踪::

 1. 定位bodysize为0时跳转出错问题，问题已定位，m模式下修改了sebstate -- 李飘
 2. 定位中断异常时ibpc 出错，内核虚拟地址错误问题，大致定位栈保存  待确认 -- 王州 贾文杰
 3. ebreak指令产生异常时，内核无法启动 -- 贾文杰
 4. 修改m模式下日志控制参数，添加到外部接口 -d 参数  -- 石晓强
 5.  添加 linx_debug 16位调试指令
 6. bget/bset 掩码检查在架构上待定义，qemu尚待实现
 7. fence.i 指令的实现，如果该指令不是块最后一条指令，可能存在问题

第一点问题在中断处理函数中用了当前的模式去获取对应的ebstate，而后reset st寄存器，
也就是说即使在s模式下发生了中断异常，本应该修改mebstate却修改了sebstate，因此导致
内核尚未读取sebstate.st寄存器值就被m模式中断意外修改了。

20220627
==================
项目跟踪::

 1. 定位bodysize为0时跳转出错问题 -- 李飘
 2. 定位tlb_flush 问题  -- 石晓强
 3. 定位中断异常时ibpc 出错，内核虚拟地址错误问题 -- 王州 贾文杰
 4. 添加 linx_debug 16位调试指令 -- 李飘
 5. bget/bset 掩码检查在架构上待定义，qemu尚待实现 -- 石晓强
 6. fence.i 指令的实现，如果该指令不是块最后一条指令，可能存在问

开发日志::

 定位了一下OS那边执行命令时出现非法指令错误的问题，确认了下是u_ecall执行完成后返回
 的地址不对导致非法指令错误，这个问题和yipeng确认了下，根本原因在于uecall异常产生后
 内核需要用到st的值来计算返回地址，但在使用st之前产生了一个s_timer中断，在s_timer
 中断处理之后最终会调用一个s_ecall 进入qemu do_interrupt函数时将st重置了,因此内核
 使用了一个错误的st的值来计算uecall返的地址，因此导致非法指令错误。



20220622
===================

项目跟踪:: 

 1. 当一个body翻译到两个tb中后，中间产生中断后ibpc值的更新存在问题（参照王州意见改一版）  -- 石晓强
 2. 添加 linx_debug 16位调试指令 -- 李飘
 3. bget/bset 掩码检查在架构上待定义，qemu尚待实现 -- 石晓强 
 4. fence.i 指令的实现，如果该指令不是块最后一条指令，可能存在问
 5. get_ebstate逻辑整理


20220615
===================
项目跟踪::

 1. 当前atomic 原子实现整理 代码已完成 -- 王州
 2. sfence.vma 指令待实现,和当前larm实现不符合  -- 李飘
 3. bget/bset 掩码检查在架构上待定义，qemu尚待实现 -- 石晓强
 4. sret 指令实现恢复现场是存在问题 -- 李飘。
 5. m模式中断处理需要清理bpc，保留tpc -- 石晓强。
 6. 实现qemu代码中的todo 断言换成异常实现 -- 石晓强。
 7. 测试用例在多线程情况下是否cpu在同一进程 -- 贾文杰。
 8. atomic 原子属性更新  -- 贾文杰

20220614
===================
项目进展::

 1. 解决第一个问题后跑yipeng测例 原子功能不正常   -- 贾文杰
 2. 当前atomic 原子实现整理 -- 王州
 3. sfence.vma 指令待实现，larm待添加  -- 李飘 
 4. bget/bset 掩码检查在架构上待定义，qemu尚待实现 -- 石晓强   

当前已知待处理问题共计4点，其余情况已处理完成。关于原子块测试出的问题，初步判定
是因测试模型不对，在用户程序中用fork启动另一个进程时qemu在user模式同时也会fork一
个进程执行cpu，因此原有原子实现的临界区不成立，所以出了问题，另外便是qemu代码中
部分代码实现需要调整，今天继续梳理中断异常逻辑顺便看下。

20220613
===================
项目进展::
 1. user mode 运行Kernel原子测例无法正常 -- segment falt    --贾文杰
 2. 解决第一个问题后跑yipeng测例 原子功能不正常   -- 贾文杰
 3. assert 问题 helper_blk_do_recovery assert(env->tpc >= env->tpc1 && env->tpc < env->tpc2) 异常  该问题已定位，中断现场恢复后，再次发生中断保存现场的tpc值有错，待修改 -- 李飘
 4. 当前atomic 原子实现整理 -- 王州
 5. sfence.vma 指令待实现，larm待添加  -- 李飘
 6. sbpc 寄存器名称修改  -- 石晓强
 7. bget/bset 掩码检查在架构上待定义，qemu尚待实现 -- 石晓强
 8. srli 指令日志输出，两个参数之间没有逗号, sysset 日志输出错误  -- 石晓强

日志记录
梳理了下qemu代码流程，本地状态bstate清空这块后面需要抽时间调整下，正常情况应该
在blk提交时统一清理，但目前好多地方都有在清，继续再看看,问了下，当时这么写的缘由
是sbpc还需要再用，只是这个实现可以改为将sbpc作为sret指令的参数。另一个记录一下一
个小小bug，larm上m/sret指令描述时用于恢复的epc应该写反了:) 。第三点是感觉sbpc这个
寄存器的使用不应该带到中断处理函数里面去使用，我看了下实现，这个主要用来判断异常
情况，但我觉着这个实现可以改为通过st.cause 去判断才对。



20220611
===================
项目进展::
 1. user mode 运行Kernel原子测例无法正常 -- segment falt    --贾文杰
 2. 解决第一个问题后跑yipeng测例 原子功能不正常   -- 贾文杰
 3. assert 问题 helper_blk_do_recovery assert(env->tpc >= env->tpc1 && env->tpc < env->tpc2) 异常 -- 李飘
 4. assert riscv_cpu_do_interrupt 函数   assert(env->pc >= env->tpc1 && env->pc < env->tpc2);  -- 李飘 石晓强
 5. assert 问题 assert(env->bpc  == LINX_ILLEGAL_INSTR_ADDR)；出错  -- 石晓强
 6. riscv_cpu_do_interrupt 前两个分支判断 存在问题
 7. 当前atomic 原子实现整理 -- 王州
 8. sfence.vma 指令待实现，larm待添加  -- 李飘
 9. sbpc 寄存器名称修改，larm上待修改  -- 石晓强
 10. bget/bset 掩码检查在架构上待定义，qemu尚待实现 -- 石晓强
 11. ecall-redo实现，当前计划在head中产生异常 已完成  -- 贾文杰


20220610
====================
项目进展::
 1. 配合Kernel检查出现的断言错误 -- 石晓强
 2. bget/bset 掩码检查在架构上待定义，qemu尚待实现 -- 石晓强
 3. ecall-redo实现，当前计划在head中产生异常，代码已完成，待测试上库 -- 贾文杰
 4. sbpc 寄存器名称修改，larm上待修改  -- 石晓强
 5. sfence.vma 指令待实现，larm待添加  -- 李飘
 6. 原子块指令实现问题，待调整 -- 王州
 7. 延迟以及其它指令epc设置问题，待梳理检查逻辑  -- 石晓强 贾文杰 
 8. 梳理qemu 整个执行流  -- 李飘



20220609
=====================

项目进展::
 1. ebreak 代码完成，已上库
 2. bget/bset 掩码检查在架构上待定义，qemu尚待实现 
 3. ecall-redo实现，当前计划在head中产生异常，代码已完成，待测试上库
 4. sbpc 寄存器名称修改，larm上待修改
 5. fence.vma 指令待实现

今天在做检查的时候发现当前的延迟指令实现在设置epc的时候传进去了一个pc_next的值，
这个值直接设置进了pc，最终给到epc，该epc用于中断返回则有问题，后面石晓强和文杰
一起梳理下这块内容。



20220608
=====================
记录一下当前看的larm可能需要更新的问题::
 1. sbpc 寄存器名称尚未修改。
 2. ebreak指令是否还需要延迟执行？4.2.3.3.2 描述是需要延迟执行

当前项目进展::
 1. ebreak 待实现
 2. bget/bset 掩码检查在架构上待定义，qemu尚待实现
 3. ecall-redo实现，当前计划在head中产生异常，待完成
 4. bstate 本地状态 清空/初始化 部分已完成
 5. lr/sc  lr.aq/sc.rl 已实现，代码已上库
 6. sbpc 寄存器名称修改，larm上待修改
 7. 所有应延迟执行指令实现修改为运行时检查执行（即用helper函数实现）已完成
 



20220601
=========
在这之前分析过ibpc的问题，也提过一次patch，但在之前的patch中，少考虑了一种情况，
即一个body正常翻译到一个tb中，但在这中间产生了异常，该情况下ibpc是不可信的，再用
该ibpc进行t寄存器的滚动计算（保存现场的时候）则会出错，该问题的解决方式有以下两
种::

 1. 实时滚动t寄存器，ibpc则不需要用，该方式对性能影响较大
 2. 在之前提的patch基础上实时去增加ibpc值，但ibpc这会有点奇怪，既翻译的时候用了，
    也在执行的时候去更新，，，

关于对ibpc的说明::

 1. 仅考虑具体指令执行，硬件是执行一条微指令，滚动一次t寄存器，在qemu实际实现上
    没有做实际的滚动操作,因此需要有一个寄存器去记录当前指令指向了哪个t寄存器，
    并实时更新，这便保证了,在中断或是异常时正常将ibpc现场保存和恢复即可,个人觉
    着这仅仅是个实现方式问题。当前qemu的实现是实时更新了这一寄存器，而中断现场
    保存和恢复对t寄存器根据实际ibpc的值滚动一轮，将ibpc调整到从0开始。

分两点去说明该实现在qemu中是没毛病的::

 1. 一个body正常翻译到一个tb块中的情况，该情况下tb翻译连续的，在翻译阶段能正常将
    每条指令和对应的寄存器联系起来。该情况下tb的执行，中间不会被中断打断，但可能
    会出现异常情况(若没有，那万事大吉)，因ibpc在微指令执行时实时更新，ibpc指向了
    异常处理的那个微指令 ，当前的异常滚动保存没问题，该情况下中断返回时将ibpc清0，
    qemu进行继续进行后半段body翻译，ibpc值正常。
 2. 一个body翻译到两个tb的情况，两个tb之间如果没有发生中断上半部分翻译不会有问题，
    上半部分body执行完后ibpc指向下半部分body的第一条微指令，因此接续翻译也不会有
    问题。另一种情况，上个tb执行后产生了中断，当前现场滚动保存了t寄存器，恢复时
    ibpc清零，因此翻译后半部分body从0开始索引，也不会有问题。


20220528
=========

关于ibpc的分析(仅用于翻译)：
ibpc初始化：在翻译body第一条指令的时候初始化为 0
ibpc更新::

 1. 正常翻译整个body在一个tb中，ibpc正常自增即可
 2. 一个body翻译到两个tb中(跨页的情况),若中间有中断进入，保存现场时需要同时保存
    ibpc，中断返回时进行恢复(因存在变长指令，不能用(tpc-bpc1)/2 进行计算)
 3. 在body 和head翻译中间进入中断的情况，根据以上初始化位置，无需保存ibpc

关于ibpc不清零的情况，对程序执行而言也没啥毛病，无非就是不从t0寄存器开始用，只是
在单步调的时候会很奇怪。





20220526
======================
1. 今天和os联调那边链条时出现非法指令错误，细看了下是因concat头标记仅仅在翻译的
   的时候被设置了，而执行的时候未被设置，因此当中间执行过非concat类型头部后，再
   次执行之前翻译过的concat块，

20220524
=========
1. 关于qemu  -symfile 参数为将部分label标签打印出来的问题，我检查了下问题是qemu在
加载vmlinux符号表的时候未将NOTYPE类型的label加载到qemu进行查找，因此qemu在加了-d in_asm
参数时部分地址找不到对应的符号,因此仅需要把该类型加入进行查找即可，代码如下::

 load_symbols(){
 ...

 for (i = 0; i < nsyms; ) {
        bswap_sym(syms + i);
        /* Throw away entries which we do not need.  */
        if (syms[i].st_shndx == SHN_UNDEF
            || syms[i].st_shndx >= SHN_LORESERVE
            || ((ELF_ST_TYPE(syms[i].st_info) != STT_FUNC)
            && (ELF_ST_TYPE(syms[i].st_info) != STT_NOTYPE))) { //新增这个
            if (i < --nsyms) {
                syms[i] = syms[nsyms];
            }
        } else {
            i++;
        }
 ...
 }

上面代码中if中是选择需要清除的label，把NOTYPE类型排除在外。我看下qemu查找label使
用了二分查找，并用内核验证了一把修改后的结果，性能影响不大。


20220504
======================

今天主要是排查内核启动时卡住不动的问题，后面确定是内核在初始化4GB空间上运行时间比较长。



整体进展 20211225更新
======================
1. 统一指令码以及解码架构，指令码实现参照李国柱提供的文档，部分重叠指令做了调整。
2. 整体 add  get set 指令并初步调测通过，李飘这边ld指令尚待理解与实现。
3. 就当前测试结果看，t寄存器貌似不需要同步操作。
4. 指令实现以block.decode文件为准，指令的行为定义由具体的实现人员给出，参照王州意见，指令行为也在
   block.decode 文件中进行说明。
5. 测试方案待进一步更新。
6. 初始协作版本更新中。。。。


20211210
========

1. 了解qemu 指令翻译执行流程 -- 就blockISA而言，其指令解码执行方式与riscv有较大不同，需要重新设计blockISA指令解码。



20211213
========
1. 参考陈娟娟与王州提交版本，重新拉取分支进行copy编译，在这过程中遇到不少问题，该版本尚未完成编译。
2. 同时也了解到新增target目标平台时可能会涉及到得一些问题。
3. 在复制编译过程有遇到部分问题，虽复制或是解决了，但不太清楚为什么。。。后面再继续看看吧。

20211214
========
1. 王州copy的linx平台中关于结构体，函数定义部分名称未修改
2. 参考已有的修改，重新编译copy的linx 出现了一堆问题。。。
3. 了解了国柱提交的代码，对linx与riscv指令混合程序中区分bstart指令存疑，
        riscv指令结构与linx指令结构不一样，没法区分开  此问题待定。
4. 关于块指令的处理方式（一次性翻译执行一个块，单独一条指令一条指令执行）考虑，感觉这个涉及异常以及中断部分 存疑。


20211215
========
1.copy 一份linx平台，编译问题尚未解决
2.参照国柱提交代码流程，新增指令，并引入block块概念，但尚未完工。
3.新增加一个分支 linx-dev-lp1 该分支基于可编译运行代码，后期在该分支上测试更新代码。
4.保留原有分支，看看编译问题是否能解决（修改了代码中的各结构体函数宏定义等名称）。


20211216-20211217
=================
1. 重新了解了新指令集的设计文档，记录了部分需要注意的点
   1  中断异常处理部分，中断异常根据文档描述其实不需要保存什么状态，因为中断处理返回后直接重新开始执行块，因此无需缓冲之类的。
   2. get与set指令，主要是set,应该是所有对全局寄存器的修改应该是在块指令执行完成时才能提交
   3. 块内程序状态有部分flag，但具体待确认

2. 看了国柱大佬写的关于gpr全局变量的问题，有个大致的概念，但细节处还得再西看看。
3. 关于每条指令为何需要那么去编写，这个问题还没搞定，继续看看吧。

20211220
========
1.任务安排

 +---------+------------------------+--------------------------------------+--------------------------+
 |  姓名   |       实现指令         |            20211220 - 20211224       |   20211227-20211230      |
 +---------+------------------------+--------------------------------------+--------------------------+
 | 陈娟娟  | ADD SUB AND OR NOT     |                                      |                          |
 |         | XOR SRL SRA DIV DIVU   |                                      |                          |
 |         |                        |                                      |                          |
 +---------+------------------------+                                      |                          |
 |  王州   | REM REMU MUL SLL LB    |                                      |                          |
 |         | LH LW LD LBU LHU LWU   | 1.理解riscv指令转换为tcg中间码机制,  |                          |
 |         |                        |  即弄清楚变量分配原理以及如何生成以及|  完成剩余指令并完成测试  |
 +---------+------------------------+  使用tcg中间码并初步实现bstart get   |  至少一天完成3条指令     |
 | 樊裕业  | SB SH   SW   ST    BR  |  add const ld sd 指令                |                          |
 |         | BL B.EQ B.LT B.LTU L.GE|                                      |                          |
 +---------+------------------------+                                      |                          |
 |  李飘   | B.GEU RET BSTART BATTR |                                      |                          |
 |         | BNEXT.T BNEXT.F GET SET|                                      |                          |
 |         | CONST COPY BNEXT       |                                      |                          |
 +---------+------------------------+--------------------------------------+--------------------------+

说明:

1. 给出一个初步版本，在前期大家要对tcg中间码转换变量分配有一个较深的理解并完成
   选取出的指令，不懂的可以相互学习。相同类型指令由同一人完成应该会快些，后面有
   异议可根据需要修改，前面若是快得话感觉要不了那么长时间。

2. 查看tcg指令转换为中间码变量分配原因，ts分配的变量使用了TCGContext 结构体中的
   temps数组的空间，相对应的有计数器nb_temps计算已经使用的空间大小,变量类型为
   TCGTemp。关于后面国柱工提得那个cpu_env的问题，这个了解了为何分配了x19寄存器
   ，可能是对tcg中间指令如何使用寄存器了解不够，暂时还不太理解中间码如何访问到
   对应cpu中的gpr的，明天继续看看

3. 关于Block内部状态寄存器，待上传。

20211221
========
1. bstart指令定义:

	100 1 11 ... 10 ... 01

注: 该指令定义是riscv16位压缩指令中保留其一，只有6位立即数可用，但我想暂时足够

2. blk_state本地状态定义，给定一个初始版本: 

   .. code-block:: c

        struct blk_state{
            /* BlockISA state */
            uint64_t bpc1;
            uint64_t bpc2;
            /* Block inner index*/
            /* In-block pc.That is, index of the instruction within the block. */
            uint32_t ibpc;            
            /*block type,for example, Standard block，Floatpoint block，System block 
            * The suffix of the opcode defines the type of the block, which restrict the range of
            * mini-instructions that can be used in this block */
            uint32_t  blk_subfix;
            /* block attribute,0: note specified, 1:atomic, 2:swap ,3:afr(atomic fail report) */    
            uint32_t blk_prop;
            /* block size, bstart immediate. */
            uint32_t blk_size;
            /* t registers. */
            uint64_t blk_t[8];
            /*  local state, used to save the cache of modified global registers. */
            uint64_t blk_gpr[32];   
            /*store instruction address buffer.*/
            uint32_t addr_buf[32];
            /* store instruction data buffer.*/
            uint32_t data_buf[32];
            /*  Indicates the count of storage instructions. */
            uint32_t st_count;
        };

3. 计划安排:

2021-12-22:
	1.确认bstart指令定义，blk局部状态定义。
	2.提交关于局部状态操作各个接口定义（）。
	3.对齐各位对tcg指令翻译接口，变量定义的理解--国柱工已提供文档供参考。
	4.对齐指令名称以及指令码定义（供测试使用）。
	
2021-12-23:
	1.确定协同开发版本并review。
	2.实现bstart add ld 指令翻译函数。
	3.制定统一验证测试方案

2021-12-24:
	陈娟娟:实现 ADD SUB AND 指令并完成验证。
	王  州:实现 REM REMU MUL 指令并完成验证。
	樊裕业:实现 SB SH SW  指令并完成验证。
	李  飘:实现 B.GEU RET BSTART BATTR 指令并完成验证。
2021-12-27:
	陈娟娟:实现 OR NOT XOR 指令并完成验证。
	王  州:实现 SLL LB LH 指令并完成验证。
	樊裕业:实现 ST BR BL  指令并完成验证。
	李  飘:实现 BNEXT.T BNEXT.F GET 指令并完成验证。

2021-12-28:
        陈娟娟:实现  SRL SRA DIV  指令并完成验证。
	王  州:实现 LW LD LBU 指令并完成验证。
	樊裕业:实现 B.EQ B.LT B.LTU 指令并完成验证。
	李  飘:实现 SET  CONST COPY 指令并完成验证。

2021-12-29:
	1.  陈娟娟:实现 DIVU  指令并完成验证。
	    王  州:实现 LHU LWU 指令并完成验证。
	    樊裕业:实现 L.GE 指令并完成验证。
	    李  飘:实现 BNEXT 指令并完成验证。

	2.联合测试

2021-12-30:
        todo

2021-12-31:
        todo

20211222
========

1. bstart指令定义 ::

	reserveed instr：
	100 1 11 ... 10 ... 01
	100 1 11 ... 11 ... 01
	100 1 11 ... 1 .... 01

注：以上上面两条指令码是riscv 16位压缩指令预留可用的编码，该编码可定义在riscv本
身提供的编码文件insn16.decode中而不会导致冲突，我们可将两条指令都用上，仅需在解
码时读取第3条指令留白处组合为理解数即可，那么此时可用立即数为7位,分2位作为块类
型剩余5位作为块大小立即数，块大小限制为32，block其它指令可采用原有已定的编码。
当然还可以考虑其它，若image实际有未使用其它编码，可以再考虑下。

2. bstart指令之外的编码参照原有blockISA指令码定义即可，指令命名统一为 ”blk_指令
   名“。

3. 联合测试方案

   1. 测试方式参考国柱工实现::

        .text
        #define a0 10
        #define a1 11

        #define BLK_INST(BINST) .2byte (BINST)
        #define BSTART(bsize) BLK_INST(0x4e3 << 5 | bsize << 2 | 1)

        #define BLK_INST_C(op, t1, t2)  BLK_INST((op)<<8 | (t1)<<3 | (t2))
        #define BGET(t)                 BLK_INST_C(0x93, t, 1)
        #define BADD(t1, t2)            BLK_INST_C(0x00, t1, t2)
        #define BSET(t)                 BLK_INST_C(0x97, t, 1)
        .align 4
        t1:
        .global t1
                BSTART(4)
                BGET(a0)
                BGET(a1)
                BADD(1, 2)
                BSET(a0)
                ret

   2. 测试方案设计 -- 陈娟娟设计 20211222 给出一个初稿。

4. block state 状态更新说明如下：::
	/*用于表示解码状态*/
	uint64_t bpc1;
	uint64_t bpc2;
	bpc1=0xffffffff 的时候表示RV解码

  导致bpc1状态切换刺激如下： ::
	// kenny.comment: 这里我不知道要写什么格式，请李飘自行整理

	* RV解码状态刺激
	  * 解码到bstart，切换到Block, 即bpc1设置为下一条指令pc，bpc2设置为块指令最后一条指令pc
	  * 任何其他情形，状态不变
	* 块解码状态刺激
	   * 解码失败，异常，状态不变
	   * pc_next超过当前块地址范围，bpc1设置为全f

  int ibpc;            
  导致ibpc状态切换刺激如下：::

	* rv解码状态刺激
	  * 解码到bstart，ibpc设置为-1.
	  * 其它情形不变
	* 块解码状态刺激
	  * 解码一条指令ipbc加1
	/*block type,for example, Standard block，Floatpoint block，System block 
	* The suffix of the opcode defines the type of the block, which restrict the range of
	* mini-instructions that can be used in this block */
	uint32_t  blk_subfix; //该变量暂时不可用，subfix应指示该块是属性是怎样的
    /* block attribute,0: note specified, 1:atomic, 2:swap ,3:afr(atomic fail report) */    
    uint32_t blk_prop;//该变量存储当前块的属性，感觉暂时不涉及。
    uint64_t blk_t[128];/* t registers.当前bstart指令容纳128个微指令，根据微指令执行进行修改。 */
	/*  local state, used to save the cache of modified global registers. */
    uint64_t blk_gpr[32];//当前set指令要求修改gpr，但应在块指令执行完成commit阶段才能提交对gpr修改，
	因此提供一个gpr缓存，在commit时提交修改。
    uint32_t addr_buf[128];/*store instruction address buffer.*/
	uint32_t data_buf[128];/* store instruction data buffer.*/
    uint32_t st_count;//用于统计sd指令缓存数量。

一个块内部指令数限制，sd指令数有限制，暂定128，addr_buf用于存储写内存地址，
data_buf用于储存相对应地址的数据，再由commit时统一提交修改。
注：若是以下由一个块实现，感觉这个设计不是那么合理，以下例子超出store缓存。::

	for(int i=0;i<1000;i++){
	  a[i]=i+30;
	}

注：块状态的定义是参照blockISA的说明文档给出，关于运行级别，块类型等标记目前
还得更进一步去理解。

计划安排
===============

2021-12-22：
	1.确认bstart指令定义，blk局部状态定义。--李飘
	2.提交关于局部状态操作各个接口定义。  --李飘
	3.对齐各位对tcg指令翻译接口，变量定义的理解--国柱工已提供文档供参考。
	4.对齐指令名称以及指令码定义（供测试使用）。
	5.陈娟娟给出初步测试方案
	
2021-12-23：
	1.确定协同开发版本并review。
	2.实现bstart add ld 指令翻译函数。
	3.确定验证测试方案

20211224-20211229：
    1.实现以下指令：
        陈娟娟：ADD SUB AND OR NOT XOR SRL SRA DIV DIVU
        王州：REM REMU MUL SLL LB LH LW LD LBU LHU LWU
        樊裕业：SB SH SW ST BR BL B.EQ B.LT B.LTU L.GE
        李飘：B.GEU RET BSTART BATTR BNEXT.T BNEXT.F GET SET CONST COPY BNEXT

    2.联合测试 -- 根据陈娟娟给出测试方案

2021-12-30：
2021-12-31：

更新bstart指令定义
===========================

::

        bstart <imm>    100 000 ......... 00
        bstart.f <imm>  100 001 ......... 00
        bstart.p <imm>  100 010 ......... 00
        bstart.pf <imm> 100 011 ......... 00
        bstart.c <imm>  100 100 ......... 00
        bstart.sys      100 101 ......... 00
        bstart结构为： 100 btype:3 c:1 bsize:7 00
        注：考虑到bsize和btype情况

20211223
============

工作日志
------------
1. 初步实现了整体块解码解析执行提交流程，代码已上传，linx-dev-lp-mr分支。
2. 初步完成指令码定义，包括bstart指令。完成t寄存器以及gpr buffer操作定义，
   后续针对t寄存器、data_buf、blk_size等有操作需要定义公共接口再行提出修正。
3. 整体测试方案陈娟娟继续完善，要为一个整体的，全面的测试方案。
4. 关于单个测试例程，请王州设计一个测试用例的框架，后面新增单条指令在此框架内添加。

项目进展
------------
1. 统一指令码以及解码架构，指令码实现参照李国柱提供的文档，部分重叠指令做了调整。
2. 整体 add  get set const ld 指令并初步调测通过。
3. 就当前测试结果看，t寄存器貌似不需要同步操作。
4. 指令实现以block.decode文件为准，指令的行为定义由具体的实现人员给出，参照王州意见，指令行为也在
   block.decode 文件中进行说明。
5. 测试方案待进一步更新。
6. 初始协作版本已合入linx-master,后续新增指令在此基础上进行。


20211225
==========
工作日志
------------
1. 参照王州意见，修改初始协作版本，更新完成，后续指令增加基于该版本。
2. 新增add  get set const ld 指令实现以及对应的测试例程。

20211227
============

工作日志
------------
1. 今日对齐部分问题
2. 更进一步了解tcg中间码转换以及riscv ld,跳转等指令实现，暂时没进展。

项目进展
------------
1. 目前提供了一个可协作开发的初始版本，统一指令码定义以及语义添加 在block.decode 文件中，对应的
    trans指令实现函数在 trans_block.c.inc 文件中提供了一个具体接口，t寄存器操作接口已在 
	translate.c 文件中提供，待评审。
2. 团队成员已实现完成基本指令，后续在统一的版本上继续新增各自指令，由于
   上周进度稍慢，原本上周五应完成的3条指令尚未完成，后面需要各位加快进度。
3. 王州需要对齐下 “跳转” 类指令的实现。
4. 单独指令测试的测试用例各自实现，已提供了样例，在初始代码linx-test文件夹下。
5. 综合测试方案尚待完成。
6. 初始版本代码链接 https://codehub-y.huawei.com/LinxISA/LinxBlockModel/files?ref=linx-dev-lp-mr


20211228
==========

工作日志
------------
1. 理解riscv指令转换为tcg中间码机制，暂无进展。

项目进展
------------
1. 初始版本协作开发版本已定，王州给出新分支linx-dev，之后在该分支上开发提交即可（包括日志以及代码）。
2. 需要注意问题：
	1. 在新分支上不要做reverse，有需要更改，重新修改提交即可，提交细节参考王州开发日志。
	2. 关于指令行为定义，当前如若不确定可参考riscv实现或是在群里提问，确定后将最终实现行为写入到
	   block.decode文件中。
	3. 在设计指令实现同时需要考虑下异常情况处理。


20211229
==========

项目进展
------------
1. 王州指令基本实现，但尚未完成测试。
2. 樊裕业分支跳转基本实现，但尚待测试，尚待实现指令包含了部分存储指令以及比较指令，进度稍慢。
3. 陈娟娟算术指令中除法指令异常处理尚待实现，关于整体测试方案，测试用例进度待更新。
4. 李飘关于跳转以及返回指令待实现，目前实现一半指令。
5. 各自在完成各自指令后，建议了解下其它指令实现，以加深整体理解。

工作日志
-------------
1. 分析了下riscv 跳转指令，与其它指令一样也是转换为中间码，对处理块内跳转以及跨块跳转时机制以及为何会
   有一个块退出指令不太理解，待细看。

20211230
==========

工作日志
-------------
1. 指令实现在bnext指令以及ret处遇到疑问，虽已初步实现正常跳转（复用已有接口），但对tcg tb机制了解不
   够，感觉有些不可靠。



项目进展
------------
1. 樊裕业存储类指令测试通过，尚未提交，跳转指令待完成。
2. 王州指令已完成。
3. 陈娟娟指令已完成。
4. 我这分支跳转待实现。
5. 关于跳转指令暂定
   1. bl作为函数调用，保存下一条指令的地址
   2. bnext和br 跳转到指定的bstart地址（指令格式不一样）
   3. 去掉原有的BNEXT.T BNEXT.F， 改为bnextc,指令根据falg标志是否为true进行跳转。
6. 各位已经实现的指令，请及时将指令行为更新到block.decode。


20211231
=============

项目进展
-------------
1. 指令设计实现基本完成，李飘这边目前在跳转类型指令（bl，bnext，ret）遇到问题，对tb以及中间码机制理解不够。
2. 指令综合测试测试方案以及测试用例待完成。


20220104
=============
1. 测试getw,getwu指令。


项目进展
--------------
1. 在上一阶段剩余了 br, bl, ret指令尚未完成，比较标志位设置指令测试用例待实现。
2. 20211231给了新BlockISA v0.9 较大块修改主要有 1. bstart，2. 跳转，3. 原子，4. 异常中断，5.
   系统指令，可参考王州提供20211231日志。
3. 关于指令的定义，请各自check下当前的定义和v0.9差异。
4. 一起商讨下，制定下一阶段工作计划。

20220105
=============

工作日志
-------------
1. 重新详细看了下新BlockISA v0.9文档，了解了下当前可能需要修改的方向。

整体进展
--------------
1. 上一阶段指令任务基本完成，开启后续新spec v0.9迭代更新。
2. 初步得实施方向在下面给了一个初步版本。

初步任务方向
--------------
1. 各位根据BlockISA v0.9版本，检查前期指令实现语义更新，并提出对新版本疑问。
2. 块头指令指令码定义解析以及新增指令（包含系统指令）实现。
3. 关于微指令指令码定义，如若不是参数个数法生变化，可暂时不调整编码。
4. 关于影子寄存器得问题，在该版本中需要去实现。
5. 关于异常和原子相关 -- 待定


20220106
============

任务划分
-------------
1. 各位根据BlockISA v0.9版本，检查前期指令实现语义更新，并提出对新版本疑问。
2. 整体的BlockISA opcode 更新 -- 樊裕业。
3. 块头指令指令码定义解析以及新增指令（包含系统指令）实现。
   王州: 原子块指令 LoadxxB StorexxB AMOAWAP AMOADD   ADDBPC ADDTPC
   李飘: 系统状态维护指令 SYSGET SYSSET SYSSET.CONST ECALL EBREAK WFI BlockHeader
   陈娟娟: 系统块指令 MTR MRT SCMT SKPT MRET SRET  BRK TRAP
   樊裕业: 比较指令 CMP.XXX  分支指令 SETBPC SETBPC.COND FENCE SFENCE.VMA

4. 关于影子寄存器得问题，在该版本中需要去实现 -- 李飘。
5. 关于异常和中断需要保存当前现场，数据结构定义 -- 王州
6. fence 指令实现
7. 原子指令实现
8. 内核联调 --


工作日志
--------------

1. 比较指令中的 B.EQ B.LT B.LTU L.GE B.GEU 更换为 CMP.EQ CMP.GE CMP.GEU CMP.LT CMP.LTU,且
   比较结果不写入标志寄存器而写入对应T寄存器。
2. 去掉了RET指令。


李飘： GET SET CONST COPY 
	

20220110
==============
工作日志
--------------
1. branch type,bnext.fall, bnext.direct <label>, bnext.call <label>, bnext.cond, bnext.ind,
   bnext.indcall, bnext.ret, bnext.concat
2. block type, bstart, bstart.f, bstart.p, bstart.pf, bstart.c, bstart.ptr
3. 添加了头部解析部分。

20220111
==============
工作日志
--------------
1. RET BNEXT.F BNEXT.T BNEXT BATTR 删除, BSTART 更新为128bit头,剩余GET SET CONST COPY 需要实现。


20220112
==============

工作日志
--------------


20220114
==============

项目进展
--------------
1. BlockISA v0.9版本解析执行框架修改待调整完成 -- 李飘
2. 原有基础指令集更新为v0.9版本完成。


当前问题
--------------
1. header头部解码方案存在问题，会导致跳转时不会将头部信息写入cpu。
2. 跳转实现有问题，存在重复解码问题。
3. header解析与块提交时需要实现。
4. 系统指令实现。

20220117
==============

项目进展
--------------
1. BlockISA 解析执行框架待确认调整。
2. 中断和异常部分正在了解中，关于
3. 系统指令码未定义。


20220118
==============

项目进展
--------------
1. 基本指令已更新完成，跳转指令待实现（李飘）
2. 系统指令SYSGET SYSSET SYSSET.CONST ECALL EBREAK WFI 待实现（李飘）
3. 原子指令LoadxxB StorexxB AMOAWAP AMOADD ADDBPC ADDTPC 方案和demo已实现，但
   功能还存在问题，还在debug（王州）
4. 系统块指令 MTR MRT SCMT SKPT 已完成，MRET SRET  BRK TRAP待实现（陈娟娟）
5. 比较指令 CMP.XXX 分支指令 SETBPC SETBPC.COND 已完成，FENCE SFENCE.VMA 指令
   待实现（樊裕业）
6. 近期任务进度较慢，请各位抓紧时间。


20220120
==============

项目进展
--------------
1. 基本指令已更新完成，跳转指令待实现（李飘）
2. 系统指令SYSGET SYSSET SYSSET.CONST ECALL EBREAK WFI 待实现（李飘）
3. 原子指令LoadxxB StorexxB AMOAWAP AMOADD ADDBPC ADDTPC 方案和demo已实现，但
   功能还存在问题，还在debug（王州）
4. 系统块指令 MTR MRT SCMT SKPT 已完成，MRET SRET  BRK TRAP待实现（陈娟娟）
5. 比较指令 CMP.XXX 分支指令 SETBPC SETBPC.COND 已完成，FENCE SFENCE.VMA 指令
   待实现（樊裕业）


20220121
==============

项目进展
--------------

LinxBlockISA
*************
1. 人员安排：陈娟娟 樊裕 李飘
2. 任务进度
   1. 系统指令 ECALL  EBREAK WFI 待实现（李飘）。
   2. 原子指令LoadxxB StorexxB AMOAWAP AMOADD ADDBPC ADDTPC 方案和demo已实现，但功能还存在问题，还在debug（王州）
   3. BRK TRAP 测试中（陈娟娟）
   4. FENCE SFENCE.VMA 指令待实现（樊裕业）

20220127
===============

项目进展
---------------
1. 关于BRK，TRAP，在system模式下无法确定具体执行效果，是怎样的。（陈娟娟）
   解决方式：trap指令暂时不做（与系统指令冲突，待定），ecall,ebreak 实现与测试方案
   同riscv中实现效果一致即可。
2. BRK只能使用riscv编码，已确认，暂时可不用做，年后考虑。
3. fence 指令进入mmu中，对mmu不太了解，同时fence与原子相关。（樊裕业）
   fence设计交由王州，具体实现（可参考riscv实现），先做成空指令。
4. WFI已实现，cpu进入死循环，待了解并测试。
5. SYSGET SYSSET SYSSET.CONST ECALL  EBREAK 指令待实现。（李飘）

年前提交版本实现
-----------------
1. ECALL EBREAK 指令实现，陈娟娟。
2. SYSGET， SYSSET SYSSET.CONST对系统寄存器的修改借用riscv实现，李飘。
3. fence实现为空。
4. WFI 指令实现，樊裕业。
5. 检查更新 BlockISA v0.11 指令变动在qemu中的实现，各自检查。
6. 关于块内异常离开本地状态的保存以及返回时的现场恢复对应的数据结构定义实现
   （bstate.out, bstate.in, 王州日志中已提供初步设计思路）王州。
7. 修改头的解析实现，同时考虑jmp相关的实现问题，李飘。
8. 关于系统指令解析 待定。

20220128
==================
LinxBlock项目进度
------------------
1. ECALL EBREAK SYSGET，SYSSET 指令 已完成（陈娟娟）
2. SYSSET.CONST 待完成， indcall已实现，待测试 李飘。
3. WFI 指令已实现，樊裕业。
4. 关于块内异常离开本地状态的保存以及返回时的现场恢复对应的数据结构定义实现
   （bstate.out, bstate.in, 王州日志中已提供初步设计思路）待完成，王州。
5. 修改头的解析实现，同时考虑jmp相关的实现问题，检查完成，关于跳转部分不需要用
   中间码进行数据更新，李飘。
6. 当前内存数据加载指令ld，lh等在地址计算时存在立即数未移位情况（王州）。

按照年前规划交付版本，系统指令 ecall ebreak sysget sysset wfi已实现，块头解析检
查完成，不需要转换为中间码更新env状态。当前sysset.const，indcall调用待完成（李飘），
关于函数调用返回处理待测试（樊裕业），以及块内异常部分需要保存的数据对应数据结构定
义待实现（王州）。
关于sysget, sysset,sysset.const 当前bstate架构寄存器（bstate.out, bstate.in）暂未定义
任务量较大，具有一定挑战性，但可以尽力一试（李飘 王州）。关于函数调用与返回部分
年前能正常完成（陈娟娟 樊裕业）。
关于原子指令部分，目前实现难度较大，留在20220207后再定计划。
定版条件：
1. bstate 架构寄存器数据结构定义完成。
2. 块跳转（call与ret）实现并完成测试。
3. sysget, sysset, sysset.const 指令实现。
定版时间： 2022-01-30 12：00

LinxBlockISA 项目
版本号：v0.1
发布时间：2022-01-29 17:20
说明：该版本基于 BlockISA基础指令集涉及文档 v0.11
已实现: 
1. 标准标量块指令，对应基础指令集。
2. 系统指令中实现了 sysget/sysset，ebreak，mret，fence(空)，sret。
待实现：
1. 中断异常现场保存和恢复。
2. 块原子相关部分。
3. sysget/sysset功能实现，但bstate.in/out 数据结构未定义，暂时不可用
4. add_tpc, add_bpc, 未加测试用例
5. wfi指令待检查。

LinxRISC
-------------------
樊裕业：了解qemu中执行时tlb的翻译过程，添加等待中断wfi和内存屏障fence指令
贾文杰：本周在实现 decodetree脚本（70%），目前还剩下指令的实现，fields、argument
sets，formats已经实现了

杨继钦：熟悉LinxRISC指令编码格式以及设计文档，完成LinxRISC指令解码流程的初步版本
曹胜：查看Linx-RISC指令文档，jsondef文件夹的部分文件，目前已完成ASR,LSR,LSL,ROR,ROL
指令的decode文件的格式定义以及它们的trans实现函数编写，J,BCEQ,ADDI,SUBL,XOR,MVHK,
LDL,STL,WFI指令的decode文件的格式定义也已完成

马鹏：
1.学习了LinxRISC的指令文档
2.学习了如何添加LinxRISC指令
3.正在进行LinxRISC的AND和OR指令的添加




20220207
===================

工作日志
========

阅读BlockISA文档：

  1. 关于函数调用返回寄存器问题已确定由r8/ra保存。v0.11/p8
  2. 系统块指令与原子块指令只能修改第一层架构状态，gpr。
  3. 微指令根据块类型不同其编码空间不同，这个需要注意下。
  4. 块内异常时，块内状态会被打包复制到BSTATE.OUT中，且BSTATE.OUT.VLD被设置为TRUE。
  5. 块指令异常返回，如果BSTATE.IN.EN为TRUE,则从系统寄存器BSTATE.IN所记录的微指令TPC
     和上下文执行，如果BSTATE.IN.EN为FALSE,则从异常返回块的第一条微指令开始执行。
  6. 不同的块类型可定义自己的编码空间，当前需要考虑后面新增的别的类型如何兼容。
  
  
疑问：
  1. 标准块指令和标准条件块指令有啥不同，为何其暂存寄存器个数无法对应？？？v0.11/p8
     ans：多了tr10-tr17 8个scratch寄存器，用于指令的绝对索引。
  2. 基于上问，如何区分标准块与标准条件块？？？
  3. 除了之前定义的t0-t7外还有块内私有暂存寄存器tr0-tr8，这两者不一样？？？ v0.11/p15 
     ans：感觉不是同一套，难道是因为名字重了？？ p34再次提到了T1-T8...
  4. 不同块指令tpc重合，由bpc与tpc共同确定当前地址应该是哪个块哪条微指令。
  5. p34块内便签寄存器是什么？？
  6. 系统指令数固定为1？怎么理解？一个头呢还是一个头加一条微指令？
  

20220208
===================

项目进展
-------------------

LinxBlock
*******************
1. 基础指令
   访存类指令：sd, sw, sh, sb, ld, lw, lh, lb, lwu, lhu, lbu (已完成)
   算术逻辑指令：add, sub, and, or, not, xor, srl, sra, sll, const, copy, addbpc, addtpc
   （addbpc, addtpc待测试）.
   块控制微指令：setbpc, setbpc.eq, setbpc.ne, setbpc.lt, setbpc.ltu, setbpc.ge, setbpc.geu(已完成).
   BlockIO指令：get, set, getw, getwu（已完成）
   Trap微指令：trap, brk（trap相当于sys的ecall， brk相当于sys的ebreak 已完成）.
2. 条件块指令
   指令：spec文档待定义。   

3. 系统块指令
   说明：系统块通过操作系统寄存器维护整个系统状态，系统块指令拥有独立的编码空间，且
   自带Barrier和原子属性。
   系统块微指令：sysget, sysset, sysset_const, ecall, ebreak, wfi, mret, sret, sfence.vma
   (sfence.vma类同于arm的tlbi; wfi待确认; sysget, sysset, mret，sret借用riscv实现，但未测试;)
    
4. 原子块指令类型
   说明：标量块指令加atomic后即可成为原子块指令。
   无访存原子指令：指令块执行不能被中断，中间不能产生异常（无需实现，当前qemu实现、
   能保证原子性）。
   单访存原子指令：有访存的原子块不能被中断（待实现，主要考虑多线程情况下原子块实现）

20220210
===================

LinxRISC
------------------
1. linxrisc 在tcg debug模式下关于指令解释的相关文件，暂定通过脚本实现，但可推后。
2. linxrisc 在copyriscv时capstone 是否需要copy一份?

工作日志
--------------------
1. 梳理qemu指令解析执行流程（绘制流程图）。


20220214
===================

项目进展
-------------------

LinxBlockISA
*******************
1. BSTATE.out, BSTATE.IN 中断相关数据结构定义编码（寄存器编号映射）完成，进度正常，
   中断qemu无法模拟实现？仅可实现异常处理，待定（陈娟娟）。
   目标：于本周五（即 2022-02-18）完成异常处理部分编码。
2. 原子相关进度，了解了原子实现逻辑，可开始编码，进度正常（王州）。
   目标：于本周五（即 2022-02-18）完成原子块部分编码。
3. wfi已实现，了解其行为，user模式下，system模式下暂定通过驱动验证，
   进度正常（樊裕业）。
   目标：于本周五（即 2022-02-18）完成wfi，setbpc 测试，20220225前完成指令边界
   测试方案（对于未确定部分指令，可先自行定义）。
4. 关于代码bug问题，可在主分支上添加注释评论以做提示。

问题：
  1. 中断现场保存和恢复分软硬件实现，如何划分？ 待确定。
  2. 指令边界测试方案，ISA spec文档中指令边界未定义？
     ans：spec 未定义可暂时推后，可先自己考虑。
  3. load/store 功能不完整？未测试完成。 

项目管理：
  1. 每日了解项目进展并根据计划目标评估。
  2. 每日了解当前遇到的问题或异议等并提供解决方案（寻求帮助或拉会讨论）。
  3. 根据前面两点，控制项目进度按时保质保量交付项目。


20220216
===================

项目进展
-------------------

LinxBlockISA
*******************
1. 关于异常处理部分，编码完成，待测试（陈娟娟）。
   目标：于本周五（即 2022-02-18）完成异常处理部分编码。
2. 梳理逻辑，开始编码。（王州）
   目标：于本周五（即 2022-02-18）完成原子块部分编码。
3. setbpc 已完成测试，wfi已实现，了解驱动实现以解决wfi测试问题（樊裕业）。
   目标：于本周五（即 2022-02-18）完成wfi，setbpc 测试，20220225前完成指令边界
   测试方案（对于未确定部分指令，可先自行定义）。

LinxRISC
*******************
1. QEMU arch目录准备（韩志林）
   1. linx-lxr-hzl分支，复制riscv目录为lxr目录，替换arch名字，并进行裁剪。
   2. 进展：裁剪了一些*_helper文件，添加了文杰提供的risc.decode，trans_risc.c.inc
   3. 要求：整理一下QEMU用到的重点struct，调用到的函数。理清traget和QEMU中代码交互
   的脉络。
   4. 目标：本周四（2022-02-17）完成大部分裁剪。

2. 添加指令（贾文杰，韩志林）

   1. 贾文杰：
      1. 进展：完成J、BCEQ、ADDI、SUBL、XOR_RR、MVHK指令添加、测试（分支linx-risc-dev）
      2. 进展：了解 TCG 翻译机制，整理文档，为添加指令增加理解深度。
      3. 目标：本周四（2022-02-17）完成文档整理。
   2. 韩志林：
      1. "LSL", "LDL", "STL", "WFI"：待添加。
      2. 目标：本周五（2022-02-18）完成指令添加。

3. 测例环境准备（黄浩富）

   1. 测例环境准备
      1. 进展：完成。对每条指令进行简单的验证。后续随着所添加的指令增加，可考虑make实现。

4. 指令验证（黄浩富）
   1. 要求：准备基于sail模型的trace的验证环境（可对比qemu的结果和sail的结果）
   2. 进展：当前sail测试流程已经能正常使用，后续需要对sail模型的trace输出进行理解。其次，
   还需要对qemu中的trace进行理解（先了解riscv中的trace使用），以便后续比较 sail 和 qemu 
   的trace输出，是否一致。
   3. 目标：本周五（2022-02-18）完成指令验证。


20220217
===================

项目进展
-------------------

LinxBlockISA
*******************
1. 异常现场保存和恢复完成，待测试（陈娟娟）。
   问题：需要讨论下异常保存和恢复如何进行验证。
   目标：于本周五（即 2022-02-18）完成异常处理部分编码。
2. 原子指令编码中
   目标：于本周五（即 2022-02-18）完成原子块部分编码。
3. setbpc已完成；wfi已实现，待测试，指令边界测试方案更新中（樊裕业）
   问题：当前wfi测试有些搞不定，待确定。
   目标：于本周五（即 2022-02-18）完成wfi，setbpc 测试，20220225前完成指令边界
   测试方案（对于未确定部分指令，可先自行定义）。

问题：
 1. 异常现场保存和恢复测试方法未定。
 2. wfi 测试待定。


20220218
===================

项目进展
-------------------

LinxBlockISA
*******************
1. 异常现场保存和恢复完成编码，测试中（陈娟娟）。
   目标：于本周五（即 2022-02-18）完成异常处理部分编码。
2. 原子指令编码完成，代码提交到atomic_v2分支，测试中。
   目标：于本周五（即 2022-02-18）完成原子块部分编码。
3. setbpc已完成；指令边界测试方案更新中（樊裕业）
   目标：于本周五（即 2022-02-18）完成wfi，setbpc 测试，20220225前完成指令边界
   测试方案（对于未确定部分指令，可先自行定义）。

4. 当前遗留问题：
   1. 当前的系统指令是放在blk头部，但根据v0.11版本，系统指令包含微指令。
   2. wfi测试待定，fence指令待实现。
   3. 原子指令编码完成，测试出异常，待解决。

5. 各自检查指令测试用例设计，待讨论。

问题：
 1. 异常对现场的保存和恢复，分软件实现部分和硬件实现部分，当前qemu中指的是
 bstate.in,bstate.out的保存和恢复。
 2. 关于异常恢复到当前指令或是当前指令下一条指令？

   ans: 当前了解仅ecall指令恢复到当前指令的下一条指令，其它指令是恢复到当前指令执行。
   关于blockISA，异常恢复时在user模式下头解析时需要检查bstate.out.vld，在原子指令实现
   时不需要检查。

小结：陈娟娟异常处理编码测试基本完成；原子指令编码完成待测试，本周小目标基本完成
下一步需要做的事：

 1. 与编译器对齐（与编译器沟通提供给我们一个测试版本），编译程序进行测试。 todo
 2. 与内核对齐 todo



20220222
===================

项目进展
-------------------

LinxBlockISA
*******************
1. 关于异常现场保存恢复编码完成，测试方案已定，待测试。
   目标：在本周五（2022-02-25）完成测试。
2. 原子指令当前调试中。
   目标：在本周五（2022-02-25）完成原子块指令。
3. 指令边界测试方案待review。
   目标：在本周五（2022-02-25）完成边界测试方案。

20220223
===================

项目进展
-------------------

LinxBlockISA
*******************
1. 关于异常现场保存恢复 已完成。
   目标：在本周五（2022-02-25）完成测试。
2. 原子指令当前调试中。
   目标：在本周五（2022-02-25）完成原子块指令。
3. 指令边界测试方案待review。
   目标：在本周五（2022-02-25）完成边界测试方案。



20220224
===================

项目进展
-------------------

LinxBlockISA
*******************
1. 关于异常现场保存恢复 已完成。
2. 原子块指令实现 已完成。
3. 指令边界测试方案待review。

20220301
===================
工作日志
-------------------
 1. smask,gmask未做块指令合法性检查。

20220303
===================
项目进展
-------------------

陈娟娟：
   1. 标准/系统块指令码对齐v0.12版本(编码和test) finished。
   2. sys系统块指令结构更改（包含指令实现的调整） finished。
   3. 块头bsize字段单位为2b finished。
   4. BSTATE.IN/OUT 到BSTATE.EXT转换（数据结构转换，寄存器编码，对应系统指令sysget/
      sysset指令修改）异常保存和恢复实现 finished。
   5. 块指令执行时对本地状态的初始化实现(无需将gpr同步到sgpr) finished。
   6. 标准指令块中新增算术逻辑与立即数操作微指令 addi, subi, andi, ori, xori, srli,
      srai, slli
   7. bnext 单位为16B 问题，待修改（代码中要求16B对齐）。

   进度：1、2、3、4、5已完成，6、7待完成。
   目标：20220311前完成 1-7。

樊裕业：
   1. 添加标准辅助块指令解码实现。            -该项当前同标准块共用解码器。
   2. cmp立即数指令，setbpc立即数指令实现     - cmp已完成，setbpc待实现。 
   3. ret/call 不对称问题，ret用的是将ra寄存器放入到bpc中（setbpc实现）。
   4. const 指令在标准块和辅助块中的实现。
   5. sysset 将立即数写入寄存器的实现在该版本中应该实现  - 该项无需实现。

   进度：cmp指令，标准块中const指令已完成，lconst，setbpc待实现。
   目标：20220311前完成 1-5。

王州：
   1. call/ret指令实现问题确认。
   2. 块指令执行时的合法性检查（主要时掩码的实现）
   3. 考虑测例中各指令码如何设置对齐问题（2b/16b对齐 需要考虑下）
   4. 原子指令(campare and swap)实现，具体实现待定义（先参考riscv实现）。
   5. select 指令的实现

   进度：当前考虑 原子指令的实现，其他待实现。
   目标：20220311前完成 1-5。

李飘：
   1. 块类型有所减少？？--需要修改块宏实现
   2. fence, fence.i, fence.vma 指令的实现。
   3. 块头属性之 MO lr, sc,是否需要实现？ --可先提前做。

   进度：
   目标：20220311前完成 1-3。

20220310
===================

项目进展：
-------------------
任务：对齐BlockISA v0.12 版本

待完成：
 1. call/ret 问题存疑。
 2. 原子块-campare and swap 指令实现。
 3. fence.vma 指令的实现。
 4. 关于qemu log 日志打印。

已完成：
 1. 标准/系统块指令码对齐v0.12版本(编码和test)
 2. sys系统块指令结构更改（包含指令实现的调整）
 3. 块头字段调整
 4. BSTATE.IN/OUT 到BSTATE.EXT转换（数据结构转换，寄存器编码，对应系统指令sysget/
	sysset指令修改）异常保存和恢复实现

 5. 标准指令块中新增算术逻辑与立即数操作微指令
 7. 头部字段调整对齐。
 8. select, const,lconst, cmp/setbpc指令实现。

任务对齐20220311
---------------------
1. 两个块头拼接支持 v0.12 李飘 finished
2. 标准块中trap/brk 需要保存上下文 王州 finished
3. 系统块中包含ebreak,要等提交时ebreak才能生效产生异常，需要对齐v0.12 王州。
4. call/ret 确认 樊裕业  finished
5. get/set 指令掩码检查 樊裕业。
6. 原子块-campare and swap 指令实现 推后。
7. fence.vma 指令的实现 王州。  finished 


20220315
===================
1. 原子块-campare and swap 指令实现。王州
2. 补齐测试用例（粘贴头部，跳转指令） 李飘
3. 对齐编译器（能通过简单的测试程序） 陈娟娟 樊裕业
   1. 简单算术逻辑运算。
   2. 函数调用，跳转
4. 与内核联调（由内核提供部分验证函数）
5. lconst 有异议（变长问题），已修改完成。

注：关于第1点 了解wz Devlog，关于第4点 可以了解内核中 entry.S 和 head.S
arch/riscv/kernel


20220320
===================


LinxBlockISA
-------------------
待完成事项
*******************
1. 根据实现的指令集 设计对应测试用例（在大方向上考虑，C语言实现）樊裕业
   1. 简单算术逻辑运算。
   2. 函数调用，跳转
2. 补齐测试用例（粘贴头部，跳转指令） 李飘
3. 通过编译器编译程序，进行qemu验证 陈娟娟 王州。
4. 当前测试用例集待优化完善。
5. 代码检查优化
6. lr/sc 未实现，推后实现

整体：对齐v0.12，与编译器联调完成
时间：2022-03-25

当前状态
********************
1. 编译器在exit部分（？？）测试不通过 当前因系统指令未实现。
2. 编译器_start 段提供了特定代码用于编译测试，当前实现ra未对齐v0.12版本的call/ret
   实现。
3. 编译器bpc 16byte未对齐
4. concat 未实现
5. system 指令未实现 
6. 编译过程不使用提供的start.S 会出现bnext overflow。
7. 当前使用的start.S暂时未测过，gcc正常编译函数返回exit存在问题。


20220323
=====================

LinxBlock
---------------------
1. 编译器在_start 未做16bit对齐。
2. _call_exitprocs 错误，编译器查找为主。
3. 编写综合测试用例 李飘。
4. log实现（考虑先梳理实现逻辑）樊裕业。
5. 用原生的crt0.S测试qemu 陈娟娟 王州。


20220324
=====================

1. 验证原子加法实现（内核提供测例）正确性-陈娟娟。
   state：单个加法验证通过，多线程编译器未能编译成功，暂时未验证。
2. _call_exitprocs 错误，编译器查找为主 - 王州。
   state：了解 编译器源码，检查start段
3. 编写综合测试用例 李飘。
   state:待实现

20220325
======================

LinxBlockISA
--------------------
1. 根据实现的指令集 设计对应测试用例（在大方向上考虑，C语言实现）初步实现
2. 补齐测试用例（粘贴头部，跳转指令） 跳转待完成
3. 编译器联调，通过编译器编译程序，进行qemu验证 陈娟娟 王州。

   state: 使用提供的start.S 作为程序运行的start和退出处理时，程序运行正常，
   当使用编译器原生实现的start段代码时，程序执行完成在退出段出现问题，待排查。

待完成事项
---------------------
1. 当前测试用例集待优化完善。
2. 代码检查优化
3. qemu lr/sc 未实现，推后实现
4. 当前实现ra未对齐v0.12版本的call/ret 实现。
5. 编译器bpc 16byte未对齐，qemu待验证
6. 编译器 concat 未实现，qemu待验证
7. 编译器 system 指令未实现 ， qemu待验证
8. gcc正常编译函数返回exit存在问题， 待排查
   
LinxISA-DSA
---------------------
进展：
 1.实现Tracemanager工具的移植（从5.2版本到6.2，编译选项已经修改，tracemanager
   和qemu的部分代码已修改，后续还需修改相应代码）

 2. tracemanger工具加载、初始化、信息保存等流程梳理完成（文档形式输出）
 3. 实现trace抓取的代码控制 -- 待完成。


LinxRISC周进展
---------------------
已完成:
    1. 测试自动化框架，目录迁移至linxRISC。
    2. minimal当前已实现的指令 测试集实现（主要是store类型指令）
    3. minimal 未通过指令测试 bug report 跟踪: BCLO, BCLT, PUT_SP。
    4. lxr-softmmu 编译通过。

 待完成:

``
1. 测试脚本功能完善(进度:80%): ①通过参数控制输出内容；②异常处理需要考虑
（比如添加 timeout 机制）；③functionmodel和ref_sail_model环境变量问题，
需要在 make sanity 之前，要把环境变量设置了。::
2. 了解softmmu（进度:30%）:不依赖 image 启动 qemu system。blockISA已实现过，
待寻求一些资料 by 钟老师。
3. 新增的minimal指令功能实现(进度:0%):添加了①乘除指令, ②有关可信栈TSP,可信
帧TFP相关指令。(需理解LinxRISC指令集中栈相关内容: Dual-Stack Scheme to 
Enhance CFI Protection.pdf)
("MADD", "UMULH", "SMULH", "UDIV", "SDIV", "UREM", "SREM",)
("GET_TFP", "PUT_TFP", "GET_TSP", "PUT_TSP", "GET_TSOFFSET", "PUT_TSOFFSET", "TSALLOCI")
4. 新增的minimal指令测试集实现(进度:0%)。::
``

待改进问题:
    1. 未根据计划将具体任务分配到人。
    2. 项目跟踪缺少每日进展跟踪，及问题反馈和交流。

20220327
===================
1. 关于国柱提到commit阶段没有一个规整化定义问题，待修正？
2. 指令测试问题-指令宏中指令掩码设计有问题
3. qemu调试技术

20220329
====================
1. qemu-调试部分的实现
2. qemu_log 日志实现（当前在system模式太卡，无法使用）
3. qemu lr/sc 未实现，推后实现
4. 编译器bpc 16byte未对齐，qemu待验证
5. 编译器 concat 已实现，qemu待验证 
6. 编译器 system 指令未实现 ， qemu待验证
7. gcc正常编译函数返回exit存在问题，待排查（查看gcc源码）
8. 当前测试用例集待优化完善。
9. 代码检查优化

项目进展
======================

LinxBlockISA
----------------------
1. 项目现在联调中，当前阶段需要对qemu加深理解，主要有以下部分：
1.1 了解内核加载和运行
1.2 深入了解Qemu内存机制，tcg翻译机制等
2. 档前问题暂时在编译器和内核那边

linxrisc
----------------------

已完成：
1. 添加新增的 minimal 乘除、栈相关指令功能。
("MADD", "UMULH", "SMULH", "UDIV", "SDIV", "UREM", "SREM",)
("GET_TFP", "PUT_TFP", "GET_TSP", "PUT_TSP", "GET_TSOFFSET", "PUT_TSOFFSET", "TSALLOCI")
2. 测试新增的 minimal指令功能，区分问题归属者: lxr-trans、sail_model、编译器。并提交bug report 跟踪。
2.1 lxr 的编译器/汇编器生成的elf待重新测试。
3. 完善测试脚本，测试编译器。将测试文件的指令码格式替换成 syntax 格式，从而测试编译器功能。 
4. 梳理lxr softmmu 相关的结构。整理好了中断、mmu相关的函数，为移植virt-machine做准备。
5. 不依赖image启动softmmu, 可打印出"hello"。
6. functionmodel和ref_sail_model环境变量问题，默认环境变量用户已设置好，即脚本默认能够执行命令: clang、llvm、qemu-lxr

后续计划：
1. softmmu 框架功能测试
2. 后续需移植virt-machine

小结：
1. 整体基本上按照计划进行。

linxISA-DSA
----------------------
马鹏:
1. 完成了trace user模式的旧代码控制，新代码的trace控制未完成。
2. 完成了TM控制解析（参数加载、主要数据初始化）的文档，更新到了wiki上。
曹胜:
1. 分析tracemanager源码中的config文件解析，sift文件生成，数据写入
2. 分析新的tracemanager源码的qemu-plugin-install的主逻辑
3.了解protobuf的安装，编译，基本语法，proto文件的编写，对c++语言的序列化及反序列化
的支持，编写demo测试序列化与反序列化
杨继钦:
1. 实现trace manager工具的移植。
2. 整理trace manager monitor的方案。

20220407
======================
1. qemu中代码实现是否对set多次设置同一寄存器进行检查问题？

20220411
======================

LinxBlockISA
----------------------
1. 项目联调，目前卡在内核初始化部分setup_vm函数，该函数为实际编译器编译第一个C
   函数，执行结果出错，考虑如何验证其正确性。
2. 在内核函数setup_vm之前的指令执行完成，但其执行结果正确性待检查。
3. busybox和glibc库可能需要开始联调。

linxRISC
----------------------
已完成：

1. 对齐4月份有关特权级的任务分工。
2. 修正指令功能 put_tsp, put_tfp, put_tsoffset, 保证了tsp/tfp和tsoffset的联动。
3. 新增以TFP为基址的load/store指令。理解这些指令，功能待添加。
4. 修改部分跳转类的指令测例，因为测试发现bc*类指令语法格式理解有误，比较值和跳转offset整反了。
5. 修正跳转类指令pc+offset功能，因为之前理解的pc+4+offset，理解有误。修改是否正确，待搭建好 lxr-pass 的回归测试检验。
6. 实现system mode中所需的函数，（以无中断控制器方式）创建virt-machine，根据jsondef列出异常编码值。

后续计划：

1. 新增的以TFP为基址的load/store指令指令功能添加及测试（难点：测试）
2. 特权级相关理解。


LinxISA-DSA
-----------------------
1.完成arm64 qemu system的启动。
2.了解protobuf语法，编写demo测试InsnFatRecord结构数据序列化和反序列化。
3.确认protobuf对结构内vector的支持。
4.完成telnet链接


LinxBlockISA任务分配
-----------------------
1. 内核主流程调试 - 甜根 石晓强 李飘
2. 原子调试       - 郑曾凯 王州
3. qemu in_asm 增加反汇编  贾文杰
4. 验证setup_vm之前执行结果正确性 -- 李飘
5. busybox & glibc 待定

1. setup_vm 编译器采用相对地址寻址 --编译器已确认 周五提供版本（20220415）。

20220413
===================
1. 当前进入setup_vm函数之前执行结果已确认

20220414
===================
王州更新了下change-log中的todolist，任务安排如下：
1. -d in_asm 反汇编 - 贾文杰。
2. lr/sc支持 - 王州。
3. -d cpu里 bpc/tpc为0，需要修正  - 石晓强。
4. qemu在某个地址停止运行 - 李飘。
5. 加临时调试指令，支持输出当前进程虚拟地址上的数据 - 李飘。

20220415
===================

整体：对齐v0.12，与编译器内核联调
LinxBlock状态（周进展）
-----------------------
1. qemu lr/sc 实现 <待完成> 王州
2. 实现 disas log in_asm 对 block 指令的支持 贾文杰

	2.1. block 指令长度支持 <待完成，微指令的长度获取不正确，排查中>
	2.2. block head 指令操作数提取及汇编打印 <待完成，汇编打印未实现>
	2.3. block 微指令操作数提取及汇编打印 <未开始>

3. 内核启动主流程调试  石晓强。

	3.1. setup_vm 前执行结果正确性确认 <已完成>
	3.2. qemu日志中 bpc 和 tpc 打印错误 <待处理>

4. 加临时调试指令，支持输出当前进程虚拟地址上的数据以及挂起程序 <完成> 李飘
5. 检查bpc/tpc 实现 李飘

下一步大致任务：  EQ  E EEDW 	发v多少
  1. 了解内核初始化流程。
  2. 加深对qemu翻译机制，内存管理等的理解。
  3. 与编译器内核联调排查问题

待处理事项：
1. trans_blk_head代码规整。
2. gcc正常编译函数返回exit存在问题， 待排查


工作日志
----------------------

今天看了下qemu -d参数日志打印的实现，添加了-stop参数，代码提交在linx-stop-p分支上
该实现是让cpu停在-stop参数指定的地址上面，具体是在翻译阶段调用了 gen_helper_wfi宏，
实现停机，此时可正常进入monitor进行操作。但具体wfi指令实现还不太了解，后面具体分析
下。

20220416
======================
工作日志
----------------------

调试了一把周五新发布的编译器，发现qemu中在处理分支类型为concat并且body_size 为零
时分支类型未更新，代码王州已修改完成。后续调试发现qemu读取异常地址，感觉像是编译
器在计算跳转地址时偏移量不太对，已找王州和编译器那边对齐，实际为偏移量未做符号扩
展。


20220418
=======================

LinxBlockISA
-----------------------

1. qemu log日志in_asm粘头的实现 -- 贾文杰
2. 以下微指令的解码实现::

	贾文杰：
	lbu, lhu, lwu cmp.eq, cmp.ne, cmp.lt, cmp.ltu, cmp.ge, cmp.geu
	cmp.eqi, cmp.nei, cmp.lti, cmp.ltui, cmp.gei, cmp.geui
	const, lconst copy sb, sh, sw, sd
	石晓强::
	setbpc.eq, setbpc.ne, setbpc.lt, setbpc.ltu, setbpc.ge, setbpc.geu
	setbpc.eqi, setbpc.nei, setbpc.lti, setbpc.ltui, setbpc.gei, setbpc.geui
	save getw, getwu setbpc  brk, trap, ecall, ebreak, mret, sret, wfi, cbrk
	sysget, sysset  fencei, fence, sfencevma sc.d, lr.d, sc.w, lr.w

工作日志
-----------------------

调试了下qemu 加载内核执行到relocate最后一个返回块指令后无法正常返回的问题，实际上
是最终写到pc的结果不对，即返回地址出错，检查了下qemu的实现也是直接一条mv指令将sbpc
内容写入到pc，但不知道为何一直写不进去，得到的结果一直在触发发缺页中断，死循环调
用栈如下::

#0  riscv_raise_exception (env=0x555556a20080, exception=12, pc=0) at ../LinxBlockModel/target/linx/op_helper.c:33
#1  0x0000555555b0c73f in riscv_cpu_tlb_fill (cs=0x555556a16ef0, address=3223322880, size=0, access_type=MMU_INST_FETCH, mmu_idx=1, probe=false, retaddr=0) at ../LinxBlockModel/target/linx/cpu_helper.c:972
#2  0x0000555555cf44be in tlb_fill (cpu=0x555556a16ef0, addr=3223322880, size=0, access_type=MMU_INST_FETCH, mmu_idx=1, retaddr=0) at ../LinxBlockModel/accel/tcg/cputlb.c:1304
#3  0x0000555555cf4c57 in get_page_addr_code_hostp (env=0x555556a20080, addr=3223322880, hostp=0x0) at ../LinxBlockModel/accel/tcg/cputlb.c:1508
#4  0x0000555555cf4d88 in get_page_addr_code (env=0x555556a20080, addr=3223322880) at ../LinxBlockModel/accel/tcg/cputlb.c:1540
#5  0x0000555555ce2412 in tb_htable_lookup (cpu=0x555556a16ef0, pc=3223322880, cs_base=0, flags=16897, cflags=4278190080) at ../LinxBlockModel/accel/tcg/cpu-exec.c:610
#6  0x0000555555ce1841 in tb_lookup (cpu=0x555556a16ef0, pc=3223322880, cs_base=0, flags=16897, cflags=4278190080) at ../LinxBlockModel/accel/tcg/cpu-exec.c:196
#7  0x0000555555ce2f78 in cpu_exec (cpu=0x555556a16ef0) at ../LinxBlockModel/accel/tcg/cpu-exec.c:1042

riscv_raise_exception  异常号为 0xc 对应于缺页异常，一直缺页一直循环，没法办法处理这个，因此一直卡在这。

好奇怪，上面描述的是第一次调的时候的问题，但后面调单步进入tcg_qemu_tb_exec该函数时便卡住了。。。。


20220419
===================

工作日志
------------------

调整完成qemu log 头部信息解析以及输出格式

20220422
===================

当前存在问题：
 1. qemu in_asm 日志在同一个body中打印两次tb块信息。
 2. qemu -linx_debug 在user模式下不可用 --文杰处理 
 3. 编译器编译程序生成符号表，但qemu中无法识别。  
 4. 在monitor中添加断点设置功能  -- 石晓强
   




TODO事项
===================

LinxBlock
-------------------
1. 原有基础指令集实现在有无符号情况下的处理检查（陈娟娟，樊裕业，王州，李飘）。finished
2. 原子操作:单访存原子块指令实现（主要考虑多线程v0.11） 王州 finished

3. 异常中断在qemu中的实现
   1. 块内异常中断现场保存相关数据结构定义（BSTATE.IN 与BSTATE.OUT 数据结构定义） 陈娟娟 finished。
   2. 异常现场保存与恢复实现。 finished
   3. 异常类型确定 暂定，暂时先用一种类型调通异常和中断保存恢复流程。
4. addbpc, addtpc 待测试  王州 Finished

5. 系统块指令：
   1. sysset  (立即数写入，暂不实现可用别的指令代替，操作系统和内核编译有诉求再行考虑添加)
   2. ecall  （暂不做，待定）Finished
   3. ebreak （暂不做，待定）Finished
   4. 验证wfi正确性  todo
   5. 确认系统块指令是放在头中还是在body部分 todo, 当前在blk header中
   6. 验证sysget, sysset实现正确性 Finished
6. 块指令头部转换为中间码实现 王州 Finished
7. 了解内核驱动编写 todo
8. 关于qemu log 日志打印 todo
9. 关于指令边界测试方案 finished
10. qemu实现对齐v0.12版本 todo
11. 具体事项在 ``LinxBlockISA v0.3`` 计划中进行状态更新。

LinxRISC
--------------------
XXX


LinxBlockISA v0.2
====================

执行计划
--------------------
计划时间：20220208 - 20220225

王州:
   1. 头部解析转为中间码实现。
   2. 原有基础指令集实现在有无符号情况下的处理检查
   3. 关于原子部分单访存原子块指令实现。
   
陈娟娟：
   1. 原有基础指令集实现在有无符号情况下的处理检查
   2. 块内异常中断现场保存相关数据结构定义（BSTATE.IN 与BSTATE.OUT 数据结构定义）
   3. 异常现场保存与恢复实现。

李飘：
   1. 原有基础指令集实现在有无符号情况下的处理检查
   2. 异常现场保存与恢复实现。
   3. 了解关于原子部分单访存原子块指令实现。

樊裕业：
   1. 验证wfi正确性
   2. 关于指令边界测试方案

版本发布
--------------------
定版时间：2022-02-25

定版条件:
  1. 原子块指令实现可用
  2. 中断异常现场保存和恢复实现可用

已定版

当前版本遗留问题：
--------------------
1. 关于异常处理部分对pc值得修改有问题。
2. 参考 ``change-log.rst``


LinxBlockISA v0.3
====================

执行计划
--------------------
计划时间：20220228-20220311

陈娟娟：
   1. 标准/系统块指令码对齐v0.12版本(编码和test) finished。
   2. sys系统块指令结构更改（包含指令实现的调整） finished。
   3. 块头bsize字段单位为2b finished。
   4. BSTATE.IN/OUT 到BSTATE.EXT转换（数据结构转换，寄存器编码，对应系统指令sysget/
      sysset指令修改）异常保存和恢复实现 finished。
   5. 块指令执行时对本地状态的初始化实现(无需将gpr同步到sgpr) finished。
   6. 标准指令块中新增算术逻辑与立即数操作微指令 addi, subi, andi, ori, xori, srli,
      srai, slli
   7. bnext 单位为16B 问题，待修改（代码中要求16B对齐）。

樊裕业：
   1. 添加标准辅助块指令解码实现 close。
   2. cmp立即数指令，setbpc立即数指令实现 
   3. ret/call 不对称问题，ret用的是将ra寄存器放入到bpc中（setbpc实现）。
   4. const 指令在标准块和辅助块中的实现（问题需要确认） finished。
   5. sysset 将立即数写入寄存器的实现在该版本中应该实现。

王州：
   1. call/ret指令实现问题。
   2. 块指令执行时的合法性检查（主要时掩码的实现） todo
   3. 考虑测例中各指令码如何设置对齐问题（2b/16b对齐 需要考虑下）finished
   4. 原子指令(campare and swap，lr, sc)实现，具体实现待定义（先参考riscv实现）。
   5. select 指令的实现 finished

李飘：
   1. 块类型有所减少？？--需要修改块宏实现
   2. fence, fence.i, fence.vma 指令的实现。

注：对齐v0.12版本

版本发布
--------------------
定版时间：2022-03-11

定版条件:
  1. 标准/系统块指令码对齐v0.12版本(编码和test)
  2. sys系统块指令结构更改以及指令实现
  3. 标准指令块中新增算术逻辑与立即数操作微指令
  4. 异常处理BSTATE.IN/OUT 到BSTATE.EXT转换
  5. 标准辅助块指令解码实现。

状态：完成
发布时间：2022-03-22

  
问题确认
--------------------
1. 头部解析的时候不需要gpr复制到sgpr（再次确认）。
2. 两个块头拼接不支持 v0.12
3. 标准块中trap/brk 需要保存上下文。
4. 系统块中包含ebreak,要等提交时ebreak才能生效产生异常，需要再次确认。
5. qemu中没法处理中断？？？

边界测试方案问题：
1. 关于内存load/store指令中立即数有符号扩展,对load数据本身有符号与无符号问题。
2. addtpc/addbpc 问题需要添加测试用例
3. 原子指令测试需要添加用例覆盖


LinxRISC
====================

第一阶段工作计划
--------------------
1. QEMU arch 目录准备

  1. 基于干净的qemu代码（xinhao准备）
  2. 复制riscv目录为 lxr 目录， 删除riscv大部分代码，替换arch名字 （韩志林）
  3. 目标： lxr框架完成，checkin， 可编译，可添加指令
  4. 完成时间：2022-02-11

2. 添加指令

  1. decode tree生成脚本实现（贾文杰，已完成）
  2. 添加 bare类指令（
     贾文杰："J", "BCEQ", "ADDI", "SUBL", "XOR_RR", "MVHK" 
     韩志林："LSL", "LDL", "STL", "WFI"）
  3. 目标： qemu可编译，可加载运行测例
  4. 完成时间：2022-02-16

3. 测例准备（黄浩富）

  1. 测例环境准备。 使得使用该环境可以快速写测例
  2. 充分测试bare类指令
  3. 目标： 完成几个测例能覆盖bare类指令测试
  4. 完成时间：2022-02-14

4. 指令验证（黄浩富）

  1. 准备基于 sail 模型的trace 的验证环境 （可以对比qemu的结果和sail的结果）
  2. 对比 qemu 的运行结果 和 sail 的运行结果 （每条指令执行后的寄存器状态）
  3. 目标： 完成 bare类 指令功能正确性验证
  4. 完成时间：2022-02-18


3月份计划
-------------------

任务：
支持所有minimal指令（可能栈操作有添加）（黄浩富 贾文杰 韩志林）

黄浩富：
  1. 测试自动化框架实现
  2. 指令测试集实现。

完成时间：20220311

韩志林：
  1. qemu riscv代码裁剪 
  2. LSL,LDL,STL,WFI,SBCAST,ADDL,MIN,MAX,BTZ,BTNZ,LDSW,LDL_RR,LDSW_RR,GET_SP
	UBCAST,BCNE,BCHI,BCLO,JL,LDSB,LDUW,LDUH_RR,PUT_SP,AND,OR,XOR,ASR,LSR,ROR,ROL,
	AND_RR,ANDN_RR 指令实现。

完成时间：20220311

贾文杰：
  1. tcg_gen_xxx 函数脚本实现
  2. 反汇编日志相关脚本实现  备注：脚本同时依赖于qemu与jsondef中的脚本。
  3. J,BCEQ,ADDI,SUBL,XOR_RR,MVHK,OR_RR,ORN_RR,XNOR_RR,LDUW_RR,LDSB_RR,RET
	MVHZ,MVHO,JR,LDUH,LDUB,LDUB_RR,STW,STH,STB,PUT_LINK,STL_RR,STW_RR,STH_RR,
	STB_RR,BCAST,MINU,MAXU,BCGT,BCLT,JRL,LDSH,LDSH_RR,GET_LINK 指令实现。

要求：
  1. 代码提交并测试通过。



任务：
  1. monitor接口调试了解（韩志林、黄浩富）。
  2. 能用TCG icount统计动态指令数，先了解当前riscv中如何做的icount（贾文杰）

完成时间：20220324

综合分析
===================

BlockISA记录
-------------------
1. 分全局状态与局部状态，块指令定义是对全局状态的一系列改变，中间可被打断。
2. 微指令是块内部定义操作，微指令之间不能跳转，只能按照块指令为粒度跳转。
3. 定义了影子gpr包括bpc，只有在块提交的时候才能更新到全局gpr。
4. 标准微指令格式定义：[link0:3  link1:3  opc:10]
5. 块内指令按序执行。
6. BState:块内状态寄存器 42个，即 TR0-TR7,TPC,SBPC,SGPR(SR0-SR31)
7. MTR,MRT,SCKPT,SCMT分别在TR0-TR9 与R0-R9之间 R0-R31 与SR0-SR31 之间传输数据，
   且该指令为系统指令不会被中断。
8. 标准块指令内部load/store指令数目不限，GET只能访问gpr，SET只能访问SGPR,只有在
   提交时才将SGPR写回GPR.
9. 块指令跳转只有在指令提交时才生效，块跳转目的地址可在BlockHeader前提前算出，
   间接跳转在块内微指令算出写入影子寄存器，跳转的目的地永远只是块头。
10. BlockHeader中的get/set bitmask需要和块内部的get/set相匹配否则非法指令错误，
    特定种类可能会限制get/set个数。
11. BSTATE 状态寄存器只本地的sbpc, tr0-tr7, sgpr0-sgpr31, tpc 寄存器，系统指令可
    访问状态寄存器

TRAP异常处理
-------------------


tcg翻译流程
-------------------
 qemu在执行指令时，先从使用tb_lookup()根据pc,cs_base等在缓存中查找，若是找到则读取
 该tb逐条解释执行其中的tcg中间码，若未找到则调用tb_gen_code启动tcg翻译。tb_gen_code
 翻译完成后随即将tb加入到缓存中。

.. code-block::c
 int cpu_exec(CPUState *cpu)
 {
   //此处省略部分代码
   while (!cpu_handle_exception(cpu, &ret)) {
          TranslationBlock *last_tb = NULL;
          int tb_exit = 0;

          while (!cpu_handle_interrupt(cpu, &last_tb)) {

			  //此处省略部分代码

			  tb = tb_lookup(cpu, pc, cs_base, flags, cflags);
              if (tb == NULL) {
                  mmap_lock();
                  tb = tb_gen_code(cpu, pc, cs_base, flags, cflags);
                  mmap_unlock();

                  qatomic_set(&cpu->tb_jmp_cache[tb_jmp_cache_hash_func(pc)], tb);
              }
			}

			...
    }
 }

在这提下tb缓存机制，缓存采用的键时pc值经过hash运算后的结果作为键，从以上tb翻译完
成后将该tb加入缓存的方式也可看出，因此在tb_lookup中找到tb后需要比较其pc，cs_base,
flag等是否相同，接下来继续看下tb_gen_code函数

相关数据结构
-------------------

 struct CPURISCVState 中主要包含各个cpu寄存器的定义，如gpr，fpr，pc，以及一些各特权级
 对应的csr寄存器。gpr即通用寄存器组，fpr则为浮点运算定义的浮点寄存器组，pc程序计数器
 等。该结构用于保存cpu执行时的状态信息，即每条指令的执行后cpu的状态都会更新到该结构中。
 因此根据cpu架构定义不同，需要新增具体的寄存器，则在该结构中进行添加即可。
 该结构中对gpr的映射可在translate.c 中见到，在该文件中定义了需要用到寄存器的映射变量，
 具体映射机制后面细说。

.. code-block::c
 struct CPURISCVState{
    target_ulong gpr[32];
    uint64_t fpr[32]; /* assume both F and D extensions */
    ...
    target_ulong pc;
    target_ulong load_res;
    target_ulong load_val;
    ...
 }

 struct RISCVCPU，该结构主要包含三个对象，env即上面所说的cpu执行状态，neg暂时不知
 有何用，看起来和tlb有关，但其定义又是空的，后面涉及到tlb时再细究下；CPUState 
.. code-block::c
 truct RISCVCPU {
    /*< private >*/
    CPUState parent_obj;
    /*< public >*/
    CPUNegativeOffsetState neg;
    CPURISCVState env;
	...
    /* Configuration Settings */
    struct {
        bool ext_i;
        ...
        char *priv_spec;
        char *user_sp  ec;
        ...
    } cfg;
 };
