hzl日志
*******

2022/12/01
==========

李飘：
    11/30：面试、整理月度进度/计划、分析B003设计方案文档至LinxInit。
    12/01：继续分析设计方案文档；制定开发实施方案
贾文杰：
    11/30：BlockISA 用一个helper函数解析块头；统计异常/中断调用次数
    12/01：继续分析性能差距原因，调试优化。
石晓强：
    11/30：分析LinxInit文档，讨论实施方案。
    12/01：继续分析LinxInit文档；B003设计方案文档
黄浩富：
    11/30：解决patch合并问题、mmu RISCV代码的还原
    12/01：解决mmu RISCV还原出现的编译问题
杨继钦：
    11/30：整理SSR的实施方案；分析B003设计方案文档
    12/01：分析B003设计方案文档。
韩志林：
    11/30：整理月度进度/计划、分析设计方案；分析设计方案文档至系统状态寄存器
    12/01：分析B003设计方案文档；制定开发实施方案

2022/12/02
==========

李飘：
    12/01：整理其他项目进度；分析ACR切换文档80%。
    12/02：项目进度计划回归；分享ACR切换文档，制定开发实施方案
贾文杰：
    12/01：采用clained TB解决方案，性能差距从11.9降到9.1
    12/02：部分解码block head；块body和块head同时解码；进行性能调优
石晓强：
    12/01：分析opensbi普通内存的申请、锁的实现
    12/02：分析opensbi服务注册接口
黄浩富：
    12/01：解决mmu RISCV还原出现的编译问题；sbi接口替换文档分析
    12/02：分析B003文档
杨继钦：
    12/01：编写特权态树模型方案、特权级切换方案
    12/02：完善QEMU实施方案
韩志林：
    12/01：编写QEMU实施方案至特权态树模型的分析20%
    12/02：完善QEMU实施方案

2022/12/05
==========

李飘：
    12/02：制定开发实施方案 40%
    12/05：制定开发实施方案，预计今晚完成初版
贾文杰：
    12/02：调试 clained TB 出现的bug; 块body和块head同时解码可行性不高
    12/05：clained TB bug调试，起内核按回车卡住
石晓强：
    12/02：分析sbi服务注册接口，方案制定50%
    12/05：sbi实施方案制定；单核启动功能实现。
黄浩富：
    12/02：分析 寄存器高层设计文档
    12/05：分析启动相关内容
杨继钦：
    12/02：制定开发实施方案至寄存器高层设计 50%
    12/05：制定开发实施方案
韩志林：
    12/02：制定开发实施方案 50%
    12/05：制定开发实施方案

综合分析
========

B003疑问整理
------------

- 异常类型是按照"系统寄存器高层设计"这个文档定义吗？如果是的话，一些异常当前不会
  有场景触发，比如 39.PS_OOB 这个异常。
- 异常委托。软件有没有这么一个需求，不开启虚拟化的时候(012)，acr1不想委托某个异
  常给下一个特权级(acr2)；但是开启虚拟化的时候(0134)，acr1想要委托某个异常给下一
  个特权级(acr3)，但是我们只有一个ACR1.EDEL寄存器，那该如何"既要不委托又要委托"
  呢？RISCV有没有这么一个需求？
- 异常委托。理解一下这句话 "如果父节点跳过子节点启动软件实例并且要将异常委托给目
  标特权态时，父节点软件需要同时将跳过的特权态都配置委托这些异常，否则被跳过的特
  权态没有对应的软件实例，无法处理异常。" 就是在实现过程当中，只实现了ACR0,ACR2,
  没有实现中间的ACR1(这是可以做到的，可以直接通过eret指令从0降到2)。如果软件想要
  将acr0向下委托异常时，希望配置acr0.EDEL寄存器即可，但是硬件只知道012这条路，会
  认为委托到了acr1，但是acr1是没有软件实例的，所以软件还需要多做一步：配置
  acr1.EDEL寄存器，让acr0将异常一直委托到了acr2才可。
- RV异常委托。RV手册Page31中"For example, if the supervisor timer interrupt (STI)
  is delegated to S-mode by setting mideleg[5], STIs will not be taken when
  executing in M-mode. "，为什么当前执行在M模式，低特权级还能产生中断？因为STI
  这个中断的"S"并不表示产生中断时的特权级，而是表示这个中断是被设计给S处理的(通
  过委托)，即STI是在M模式产生的，后续要交给S模式去处理。

QEMU实施方案-1205
-----------------

* 简介
  本文主要分析QEMU的具体实施方案，目的是为了后续QEMU开发可以有些许的参考。分析
  思路是：1. 分析当前QEMU有哪些模块；2. 分析当前B003设计文档包含了哪些内容；3. 
  基于前两点分析QEMU的具体实施方案。

* 分析过程

当前QEMU已有模块和B003设计文档所提到的内容基本一致，那么我们可以直接进入第3点，
分析QEMU的具体实施方案。首先列举下我们要实现的功能点：

    1. 特权态树模型的定义。
    2. SSR的定义。
    3. 特权态指令的定义。
    4. SSR别名机制的实现。
    5. virt平台外设的定义。
    6. LinxInit支持多核启动、向低特权级提供服务。
    7. 异常/中断处理。
    8. 异常委托。
    9. 中断委托。
    10. 中断源的定义。
    11. 地址翻译。(沿用RISCV)

特权态树模型的定义
^^^^^^^^^^^^^^^^^^

因为我们的特权态总共是有16个特权态的(ACR0~ACR15)，只不过当前阶段我们只实现其中
一个子集，为了有一定的扩展性，就需要考虑如何定义特权态树模型。当前设计的特权态
树模型基本和RISCV定义的特权级是一一对应的。列举如下：

    - ACR0(M)：用于运行FIRMWARE/BIOS
    - ACR1(HS/S)：用于运行HYPERVISOR/HOST KERNEL
    - ACR2(U)：用于运行HOST USER
    - ACR3(VS)：用于运行GUEST KERNEL
    - ACR4(VU)：用于运行GUEST USER

之前我们实现过一个ACRCONF配置特权态的功能，ACRCONF是一个SSR，记录当前使能的特权
态的编号，但是他无法体现特权节点之间的关系，比如父子节点，兄弟节点。为什么需要
考虑父子节点，这里举一个异常委托的例子，ACR4产生一个异常，默认交给ACR0处理，假设
ACR0配置成向下委托(简单来讲1表示委托，0表示不委托，这样描述主要是为了说明委托无
法指定委托的目的特权级，只能表明是否委托)，那么会委托到ACR1；ACR1继续配置成向下
委托，但是这里有两个分支，可以是委托到ACR2，或者委托到3，那么我们该如何确定呢？
答: 根据产生异常的特权级确定。因为我们知道ACR4到ACR0的路径是ACR0,1,3,4，所以正确
的委托顺序是0->1->3。所以这个ACR0,1,3,4(或ACR0,1,2)的结构，也就是特权态树模型是
需要被定义的。具体实现还需进一步考虑，只要能够体现出对应的树结构即可。

SSR的定义
^^^^^^^^^

由内核团队给出定义，QEMU参考RISCV的CSR的实现即可。最新SSR的定义在
https://onebox.huawei.com/v/b3160ab8c3f34b7fa1befefcf2d1b263 中(by chenlifu)，
也需参考"系统寄存器高层设计.html"。

特权态指令的定义
^^^^^^^^^^^^^^^^^^

王州已分析整理出文档--"Block ISA去rv化特权级指令梳理"，参考该文档即可。

SSR别名机制的实现
^^^^^^^^^^^^^^^^^^

当前在b100分支别名机制已实现，因为别名寄存器的编制在一个固定的范围，所以可以根据
SSR的编号判断该SSR是否属于别名寄存器，然后再根据当前特权级(from CSTATE)判断其具
体映射到哪一个特权态所对应的SSR。

不过12/02更新的系统寄存器高层设计貌似删除了别名寄存器，所以别名机制不用实现。

virt平台外设的定义
^^^^^^^^^^^^^^^^^^

王州已分析整理出文档--"QEMU riscv virt平台外设分析"，参考该文档即可。

LinxInit设计
^^^^^^^^^^^^

王州已分析整理出文档--"LinxInit设计细化"，参考该文档即可。

异常/中断处理
^^^^^^^^^^^^^

可参考RISCV的实现

异常委托
^^^^^^^^

RISCV的特权级和我们的ACR是一一对应的("特权态树模型的定义"一节已说明)。可以先看看
RISCV的异常委托是如何实现的。当系统为M,S,U的时候，可以通过配置medeleg(mideleg同理)
将U,S执行过程中产生的异常委托到S模式处理。当有H扩展(即可选虚拟化使能)，即系统有
M,HS,VS,VU时，可以通过配置medeleg,hedeleg将异常向下委托。medeleg和hedeleg分别对
应LinxPriv的ACR0.EDEL和ACR1.EDEL，LinxPriv不同的是ACR3,4虽然分别是host user和
guest user,但是他们也有对应的SSR，所以也是具备处理异常能力的，所以ACR3,ACR4他们
的父节点是可以有异常委托寄存器的--ACR2.EDEL, ACR3.EDEL.

异常委托当前我们有两条路径--ACR0,1,2; ACR0,1,3,4.所以委托顺序是有两条路的。比如
ACR1需要向下委托异常的时候，有H扩展且确实使能虚拟化的时候，ACR1是需要走0,1,3,4这
条路线的。如果硬件判断异常委托的时候，可以看触发异常的特权级(ACR2或ACR4)的时候，
就可以直接确定走到是哪一条委托路径。

2022/12/16
==========

cpu处理异常和处理中断的内容基本一致，包括代理机制，其中，代理机制截至目前的方案
打算采用静态配置的方式。体现在QEMU中的代码(RV)就是riscv_cpu_do_interrupt()这个函
数。这里统一描述一下1.进入异常/中断的行为；2.异常代理机制(静态配置)；3.中断代理机制。

进入异常/中断
-------------

1. 硬件将CSTATE的值备份到ECSTATE_A<r>。
2. 将块内状态备份到EBSTATE_A<r>（如果是块内被打断）。
3. 并清零CSTATE.I（关中断）。
4. 异常原因写入ECAUSE_A<r>。
5. 异常发生地址写入ELINK_A<r>。
6. 根据具体的异常类型设置EARG0_A<r>(如果是page fault的话，将page fault的地址写入)。
7. 进入目标特权态A<r>。

上面的<r>表示处理异常的特权级的编号，若无代理，则<r>为0，若有代理，比如异常A被
ACR0向下代理给ACR1，那么<r>为1。代码实现在这加入一个判断即可，类似 get_handle_acr(ecause)
来获取处理异常的特权级的编号。

- 第2点

  BSTATE的状态保存，这里提到的"如果是块内被打断"，其中的 "块内" 指的是区别于RV指
  令的Block块，还是指解析块body中的微指令而不是块head被打断？和文杰确认过这里指
  的是第二种情况，因为只有块body中产生的异常这种情况的bstate才有必要被保存和恢复。
  qemu中已有判断(bpc或tpc有效即为块内被打断)，保留原有实现就好。

  .. note::

     ECAUSE_A<r>.BI[62] 是块内/块间中断指示位。这个位可以怎么用上呢？还是说这个位
     本来就需要硬件(QEMU)来更新(bpc或tpc有效则将该bit置位)，提供给内核使用。

- 第4点

  异常原因写入ECAUSE_A<r>。因为异常类型分两层异常(TRAPNUM/SYNDROME)，
  这两个值都需要被记录下来，用于异常处理过程当中将异常原因记录到ECAUSE_A<r>.
  RV的实现是将仅有一层的 异常号 存储在和架构无关的中性结构体 CPUState 的
  int32_t exception_index当中，其中，异常号 有范围限制，需要小于 0x10000。因为
  0x10000之上的值用于定义QEMU自己的异常状态，如下：

  .. code::

    #define EXCP_INTERRUPT      0x10000 /* async interruption */
    #define EXCP_HLT        0x10001 /* hlt instruction reached */
    #define EXCP_DEBUG      0x10002 /* cpu stopped after a breakpoint or singlestep */
    ...
    #define EXCP_ATOMIC_BLK 0x10006 /* stop-the-world and emulate atomic block */

  那么如果继续通过 exception_index 来记录异常号的话，那么就需要同时将 TRAPNUM/SYNDROME
  的内容都记录下来。根据ECAUSE_A<r>的 TRAPNUM/SYNDROME 字段所占bit可知(TRAPNUM[5:0],
  SYNDROME[47:24])，对应的异常种类最多有 2^(6+24)，是有可能大于0x10000的。所以
  不能用 exception_index 同时记录两层异常的信息。那么，我们可以选择用 exception_index
  仅记录第一层异常TRAPNUM；而第二层异常SYNDROME的值则可以记录在 CPULINXState 中。

  .. note::

     RV当中m/stval(Machine/Supervisor Trap Value Register)他是记录异常的一些必要
     信息的一个CSR，比如产生的是 page-fault exception，那么对应的 bad address 会
     在处理该异常的时候将其存储在m/stval当中。这里的 bad address 就是 CPURISCVState
     当中的一个临时变量。所以必要的话，我们也可以在 CPULINXState 定义一个 临时变量
     SYNDROME。

- 第5点

  异常发生地址写入ELINK_A<r>。下面进行分类讨论。(保留原有实现)

    1. bpc有效，说明块head已解码完，在解码块body时触发了异常。bstate需要保存；
    ELINK_A<r>保存bpc的值；tpc保存pc的值，待软件处理完异常之后，通过ELINK_A<r>的
    值跳回原来的块head当中，块头解码完成之后，再通过状态恢复(helper_blk_do_recovery)
    动作，通过tpc的值跳转到产生异常对应的pc继续执行。
    2. bpc无效，但是tpc有效。按照文杰的描述，这种情况是，内核/bios处理完异常，
    恢复到原来的低特权级(跳转到产生异常所在的块head)继续执行的时候，在解码块head
    之前产生了一个中断，这个时候，tpc有效，bstate有效，这样也是需要保存bstate的
    状态的。
    3. 其他情况，表示在解码块head的时候产生了异常(note:有RV指令的时候，那么也包
    含了解码RV指令产生了异常)，这个时候，只需要保存块head的pc就好，因为块body还
    没有解码执行，bstate没有必要被保存。

- 第6点

  根据具体的异常类型设置EARG0_A<r>. EARG0_A<r> 这个寄存器和RV的 m/stval 的功能
  类似，我们可以根据具体的异常，按需要在 CPULINXState 定义临时变量，用来暂存
  临时值，比如非法指令的异常，可以将指令编码暂时存储在 bad_insn；页缺失的异常，
  可以将虚拟地址存储在 bad_address。

异常代理机制(静态配置)
----------------------

1. 根据具体异常和已有的静态配置，来判断处理异常的特权级。类似get_handle_acr(ecause)
这样的接口。

中断代理机制
------------

TODO

2022/12/20
==========

基于技术手册和 12/16 的分析，以及qemu现有实现，这里再梳理一下QEMU的实现逻辑。

异常处理异常/中断的逻辑主要在 linx_cpu_do_interrupt()。原有代码逻辑大概如下
(不考虑虚拟化)：

    1. 区分是异常还是中断，是中断跳到4
    2. 更新 tval 的值
    3. 根据ecause的值更新ST.cause，用于保存layer2的异常；并将异常编号更新为RV的异常。
    4. do_fixup的判断。若需要do_fixup，跳到x.
    5. 根据当前特权级、委托寄存器、异常编号判断当前 异常/中断 交由给 S 还是 M 模式处理。

        1. (S处理异常) 将tval赋值给stval；保留异常原因等信息；判断是否为块内trap，
        若是，则保存TPC等信息到Rx寄存器、更新ST.sz、更新ST.rmax、更新ST.vld
        2. (M处理异常) 将tval赋值给mtval；保留异常原因等信息；bpc暂存，并置为非法值。

    6. 将ebstate内容拷贝到 s/mbstate
    x. 结束

当前可尽量保持原有大的逻辑不变，接下来对每一步去分析

- 第1步

  逻辑不变。区分是异常还是中断，

- 第2步

  我们可定义一个临时的earg0的值，功能和 tval 一样，将指令解码期间暂存的一些
  必要信息，如badaddr, badinsn 拷贝到 earg0 中；后续确定处理异常的特权级为
  acr0, 则再赋值给A0_earg0；或者赋值给A1_earg0.

- 第3步

  有关ebstate_st的字段如下。
  ST.rmax: 不变。表示Rx寄存器中保存的值为有效值的最大索引。
  ST.sz:   不变。表示被异常/中断打断的块的大小。
  ST.cause:去除。融合到ECAUSE.TRAPNUM/SYNDROME中。
  ST.vld:  去除。功能由ECAUSE.BI承接, ECAUSE.BI表示是块内被打断时，EBSTATE中信息即有效。
  因为，ST.cause被去除了，所以第3步可直接跳过，对应的异常编号交给后面的第5步处理。

- 第4步

  逻辑不变。do_fixup的判断。

- 第5步

  因为异常代理和中断代理方案不一样，这里仅讨论异常代理。（TODO: 中断委托）
  根据一个静态异常代理路由表，可确定一个具体的异常交由哪个特权级处理，需添加一个
  类似 get_handle_acr(current_acr, ecause) 这样的函数判断处理异常的特权级.

    1. (acr1处理异常) 将earg0的值赋值给A1_earg0；A1_ecause保存TRAPNUM/SYNDROME；
    判断是否为块内trap，若是，则A1_ecause.BI置位、更新ST.sz、更新ST.rmax、保存
    TPC等信息到Rx寄存器。

    .. note::

        ST.vld的功能由ECAUSE.BI承接，所以原有代码中有关ST.vld的赋值、使用需修改
        成对ecause.BI的赋值、使用。

    2. (acr0处理异常) 将earg0的值赋值给A0_earg0；A0_ecause保存TRAPNUM/SYNDROME；
    判断是否为块内trap，若是，则A0_ecause.BI置位、更新ST.sz、更新ST.rmax、保存
    TPC等信息到Rx寄存器。

    .. note::

        v0.13版本的blockISA实现，异常全由S mode处理，现在我们的ACR0也需要处理
        LinxPriv的异常，所以这里的行为基本和ACR1处理异常一致。

- 第6步

  逻辑不变。将ebstate内容拷贝到 A0/1_bstate

- 第x步

  结束

2023/01/03
==========

非法指令异常处理变动
--------------------

RISCV手册中有关stval寄存器的作用有下面这么一段描述（见note），表示stval有这么一
个可选功能：当异常是"非法指令"时，stval可存储异常指令的编码。LinxBlockModel 这个
代码仓的QEMU版本是 6.1.93, 还未实现这个可选功能；QEMU版本为 6.2.50 的这个版本实
现了这个可选功能；我们可和最新的qemu-riscv的实现保持一致，LinxPriv也可以实现这个
功能。

..note::

    The stval register can optionally also be used to return the faulting
    instruction bits on an illegal instruction exception.

综上，分条总结下非法指令的异常处理变动：

1. 目前实现：产生一个"INSN_ILLEGAL"异常，
2. 接口改动：产生"INSN_ILLEGAL"的同时，将非法指令的指令编码存储在earg0_A<r>（r为
   处理INSN_ILLEGAL的特权级，为0或1）中；
3. qemu改动：产生异常的同时将指令编码存储在 CPULINXState 中的临时变量 bins 中；
   cpu处理该异常的时候再将 bins 的值存储在earg0_A<r>中。

LinxPriv异常和RISCV异常的对应关系
---------------------------------

+----------------------------------+---------------------------------+
| TRAPNUM{0..63}/SYNDROME组合      | RISCV异常                       |
+----------------------------------+---------------------------------+
| {0}:Instruction exception        |                                 |
+----------------------------------+---------------------------------+
| 0.Instruction access fault       | 1:Instruction access fault      |
+----------------------------------+---------------------------------+
| 1.Instruction translation fault  |                                 |
+----------------------------------+---------------------------------+
| 2.Instruction misaligned         | 0:Instruction address misaligned|
+----------------------------------+---------------------------------+
| 3:Instruction illegal            | 2:Illegal instruction           |
+----------------------------------+---------------------------------+
| 4:Instruction permission fault   |                                 |
+----------------------------------+---------------------------------+
| 5.Instruction page fault         | 12:Instruction page fault       |
+----------------------------------+---------------------------------+
| {1}: Data exception              |                                 |
+----------------------------------+---------------------------------+
| 0:Load access fault              | 5:Load access fault             |
+----------------------------------+---------------------------------+
| 1:Load misaligned                | 4:Load address misaligned       |
+----------------------------------+---------------------------------+
| 2.Load page fault                | 13:Load page fault              |
+----------------------------------+---------------------------------+
| 3:Store/A_op access fault        | 7:Store/AMO access fault        |
+----------------------------------+---------------------------------+
| 4.Store/A_op misaligned          | 6:Store/AMO address misaligned  |
+----------------------------------+---------------------------------+
| 5.Store/A_op page fault          | 15:Store/AMO page fault         |
+----------------------------------+---------------------------------+
| {2}:Software escalate exception 1|                                 |
+----------------------------------+---------------------------------+
| 0:Trap from ACR1                 | 9:Environment call from S-mode  |
+----------------------------------+---------------------------------+
| {3}:software escalate exception 2|                                 |
+----------------------------------+---------------------------------+
| 0:trap from ACR2                 | 8:Environment call from U-mode  |
+----------------------------------+---------------------------------+
| {4}:Block exception              |                                 |
+----------------------------------+---------------------------------+
| 0:Invalid set_regs detected      |                                 |
| 1:Invalid get_regs detected      |                                 |
| 2:Invalid Parameter              |                                 |
| 3:Duplicated set to the same GPR |                                 |
| 4:Invalid fixup block            |                                 |
+----------------------------------+---------------------------------+
| {5}:Breakpoint exception         | 3:Breakpoint                    |
+----------------------------------+---------------------------------+
| {6~61}:Reserved                  |                                 |
+----------------------------------+---------------------------------+
| {62}:Illegal SSR                 |                                 |
+----------------------------------+---------------------------------+
| {63}:Unsupported exception       |                                 |
+----------------------------------+---------------------------------+

每个RISCV的异常都有LinxPriv的异常一一对应，这样，我们既可以保留原有RV的地址翻译
逻辑，又可以将对应的RV异常替换成LinxPriv的异常。

另外，在替换异常的过程当中，发现RV有关"二级翻译"的内容还未去除，所以也顺带删除有
关二级翻译的内容。（当前ACR仅实现ACR012, 对应RV的M,S,U,所以目前没有对应于RV虚拟
化扩展的特权级，也就是无H扩展，对应的虚拟地址的二级翻译也没有。）二级翻译体现在
QEMU的改动如下：

1. two_stage==true的情况可不考虑，对应的代码可删除。因为qemu中的一级翻译和二级
   翻译代码是同一套，是通过two_stage这个参数来区分的，不考虑H扩展的时候，get_physical_address()
   这个函数的两个参数first_stage，two_stage的值分别为true和false。

   ..note::

        get_physical_address() 中通过两个参数区分翻译的阶段，参数为 fist_stage
        和 two_stage。①当无虚拟化扩展时，这两个参数的值为true,false。我们的ACR0,1,2
        的特权级模型就只有这种情况。②当有虚拟化扩展时，VU的虚拟地址需要做第一次
        翻译，这两个参数的值为true,true；得到的地址是VS的地址，VS还需要再做一次
        地址翻译，即第二次地址翻译，这两个参数的值为false,true。所以参考RV的实现
        来看，我们仅考虑two_stage==false的情况。

2. riscv_cpu_virt_enabled()==true的情况可不考虑，对应的代码可删除。因为结果为true
   的情况是有H扩展并且使能了虚拟化。

2023/01/04
==========

RV的ecall & ACR的trap
---------------------

LinxPriv的trap指令是和RISCV的ecall指令逻辑是一样的。这里描述一下RISCV的ecall
逻辑。RV的ecall指令对应的异常有 U_ECALL, S_ECALL, VS_ECALL, M_ECALL. QEMU中的实
现是不管执行ecall的当前特权级是什么，都会先产生一个 U_ECALL 异常，然后在异常处理
（cpu_do_interrupt）的时候再根据当前特权级和是否使能虚拟化这两个条件来修改异常类
型，确定具体ecall异常是 U_ECALL, S_ECALL, VS_ECALL, M_ECALL 中的具体哪一个。

"-d cs,cs_nom"输出log简要分析
-----------------------------

1. 原有实现: "-d cs"是我们用来打印特权级切换时一些有关bpc,tpc,特权级值变化的log
   选项；"-d cs,cs_nom"表示打印"cs"类的log之前判断一下如果特权级涉及到了M（特权
   级切换前后任何一个是M mode，则可认为特权级涉及到了M），则不打印"cs"类的log。
2. qemu改动: 将 "PRV_M" 替换成 "ACR0" 即可。

   ..code::

        if (!(qemu_loglevel_mask(CPU_LOG_CS_NO_M) && (old_env.priv==PRV_M || env->priv==PRV_M)))

2023/01/09
==========

RV mmu逻辑简要分析
------------------

我们先挑一个切入点: get_physical_address().这个函数的功能是将虚拟地址转换成物理
地址。这个函数除了会递归调用之外，其他主要调用的地方是下面这两个接口。get_phys_page_debug
主要用于debug，可忽略，所以需要分析的的地方主要是tlb_fill这个接口。

    ..code::

        .get_phys_page_debug = riscv_cpu_get_phys_page_debug,
        .tlb_fill = riscv_cpu_tlb_fill,

tlb_fill还会涉及到raise_mmu_exception这个函数，也需要分析其改动点。这里列举一下
qemu中tlb_fill，raise_mmu_exception和get_physical_address改动点。

1. 无H扩展。
   也就是无虚拟化使能(riscv_cpu_virt_enabled)，无两级翻译(two_stage)的情况。因为
   当前ACR只有0,1,2这三个特权级，不涉及RV的H扩展的内容。
2. 无pmp检查机制。
   PMP(Physical Memory Protection)检查的情况有①S,U-mode的fetch；②MPRV==0时，
   S,U-mode的数据访问(load/store)；③MPRV==1时，上一个特权级（mstatus.MPP）值为
   S或U-mode时的数据访问（只涉及到MPP?没有SPP?因为MPRV==1时表示当前特权级已经是
   M-mode了，查看上一个特权级自然看的就是MPP位了，而且数据访问也可使用地址翻译
   功能）。不过当前ACR设计无该特性，可直接删除。
3. 无MPRV(Modify PRiVilege)机制。
   MPRV的作用是M mode下的数据地址（无指令地址）也可使用地址翻译的功能。当MPRV==0
   时，load/store 地址翻译和保护机制按照当前特权级执行，当前的ACR仅需考虑MPRV==0
   的情况。
4. tlb_fill()中"Two stage lookup"的if分支可删除。(里面的逻辑暂时还未分析(TODO))
   因为对应的是H扩展的两级翻译内容。那么，传给raise_mmu_exception的参数也就仅考
   虑"Single stage lookup"的情况即可。
5. raise_mmu_exception()中 first_stage, two_stage, pmp_violation 的值在一级翻译
   的情况下值恒为 true, false, false.
   ①因为tlb_fill中传递给raise_mmu_exception的first_stage的值是 first_stage_error
   这个变量，这个变量在RV中表示第一级页表翻译失败的意思。那么我们现在没有第二级
   地址翻译，这时候如果走到产生异常这里，那肯定是第一级页表翻译失败，所以
   first_stage 恒为true；
   ②two_stage 的值表示虚拟化使能或者是H扩展对应的load/store指令，显然LinxPriv中
   的 two_stage 的值恒为false.
   ③pmp_violation表示pmp检查不通过的标志，LinxPriv无对应机制，所以恒为false.
6. 翻译模式固定为Sv39。目前仅沿用RV的地址翻译中的Sv39模式。
7. get_physical_address()中的 use_background 默认为false，可删除。该变量目前不清
   楚其作用，和mstatus.sum有联系(TODO)，但是可以确定的是其在tow_stage==true的if
   条件下，即用于两级翻译，可删除。
8. MXR(Make eXecutable Readable)存在mstatus中，当前设计的CSTATE寄存器无对应的
   设计，所以相关逻辑可删除。
9. mstatus.sum替换成cstate.p。功能一致。
#. Svnapot扩展保留(TODO:待分析，当前版本 6.1.93 qemu中未实现)。Svnapot扩展标志
   就是将PTE的N设置为1，对一个连续区域内并且PTE[5:0]相同属性的虚拟地址和物理地址
   进行连续映射。
#. Svpbmt扩展保留(TODO:待分析，当前版本 6.1.93 qemu中未实现)。PTE的第62-61位表示
   使用基于页面的内存类型，这些类型会覆盖相关内存页的PMA。

2023/03/06
==========

基于20230303 wangzhou的分析，这里分析一下用静态索引的方式去实现包含块内跳转的块
在qemu中的实现。qemu在翻译、查找tb(包括翻译时查找tb和lookup_tb_ptr时查找tb)的时
候会调用cpu相关的函数cpu_get_tb_cpu_state获得flags，我们只对这个函数做修改就可以。

在flags中添加 ibpc%8 之后的代码逻辑是这样的：

1. cpu_reset(), ibpc置为illegal_ibpc(0xFFFFFFFF)
2. 翻译第一个块head时，这个TB块的flags-ibpc为7(0xFFFFFFFF%8==7)，不更新ibpc
3. 翻译第一个的块body时(无块内跳转)，ibpc未更新，所以这个TB块的flags-ibpc为7，
   虽然块body微指令执行当中，每条微指令都会ibpc++，但是在commit阶段会reset_bstate，
   将ibpc置为illegal_ibpc(0xFFFFFFFF)，所以接下来的块head翻译flags-ibpc依旧是7.
4. 翻译第n个块head
5. 翻译第n个块body(有块内跳转)，遇到跳转指令，停止翻译，执行当前的TB块；然后再去
   生成一个新TB，新TB的flags-ibpc根据当前实际的ibpc值得到，新TB的用于静态索引的
   变量ibpc_trans在翻译第一条微指令时初始化为ibpc。这样，如果多个块内跳转指令跳
   到一个地址，如果flags-ibpc是一致的，那么则可以用缓存中的TB，这样索引的T寄存器
   是对的；如果flags-ibpc不一致，那么qemu就会重新翻译。

   ..note::

        这里描述一下这个方案要解决的问题：多个块内跳转指令跳到一个地址，第一次跳
        转的时候，我们正确的翻译的跳转目的地址的块，当另外一个块内跳转指令跳到相
        同地址时，如果flags相同，那么qemu会用已经翻译得到的tb。这个时候静态索引
        的T寄存器就有可能不对，可以看下面一个例子理解。tb执行顺序是 tb1_1st ->
        tb2_1st -> tb1_2nd -> tb2_2nd -> .. tb1_1st 和 tb1_2nd 他们的目的t寄存器
        按照执行流(按照预期)分别为T0,T5,不加flags-ibpc时，执行tb1_2nd时，会找到
        tb缓存，不会重复翻译，直接执行tb块，那么写入的T寄存器还是T0，是不符号预期的。

   ..code::

        一个块body:
        tb1:
            const 0x1       T0  T5
            const 0x2       T1  T6
            bc    t#1,t#2   T2  T7

        tb2:
            const -4        T3  T0
            jr    t#1       T4  T1

   ..flags::

      -> cpu_exec()
        -> cpu_get_tb_cpu_state(&flags, ..) //第1步：读取ibpc，得到flags
        -> tb_lookup(flags, ..) //第2步：根据flags查找缓存
        -> if(tb==NULL)
          -> tb_gen_code(flags, ..) //第3步：若在缓存中未找到则生成tb，并将第1步得到的flags作为翻译参数

测例：~/test/lipiao_testOptimize/test/429.mcf
编译：make -f linx.mk clean && make -f linx.mk -j
429-运行：mcf_linx ../test/input/inp.in
464-运行：h264ref_llvm -d foreman_ref_encoder_baseline.cfg
qemu分支：linx-dev-performance-v014-test
性能对比结果如下：

1. 纯静态_1(不用块头load块体，纯静态)(qemu不加flags改动)：所以编译器需要换成
   不支持块内跳转的版本，2,3,4用的编译器都为支持块内跳转的版本。
   -blk_optimize force_tb_chained,disable_mask_check,static_index
2. 半静态_1(块头load块体)(qemu不加flags改动)：
   -blk_optimize force_tb_chained,disable_mask_check
3. 半静态_2(块头load块体)(qemu加flags改动)：
   -blk_optimize force_tb_chained,disable_mask_check
4. 纯静态_2(不用块头load块体，纯静态)(qemu加flags改动)：
   -blk_optimize force_tb_chained,disable_mask_check,static_index

429-gcc::

    编译器版本 0.14 jump：/home/wenjie/block_toolchain/linx64-linux-gnu-20230222/bin/linx64-linux-gnu-gcc

    1. 纯静态_1：无
    2. 半静态_1：0m43.183s (块头load块体)(qemu不加flags改动)
    3. 半静态_2：0m45.165s (块头load块体)(qemu加flags改动)
    4. 纯静态_2：0m42.205s (不用块头load块体，纯静态)(qemu加flags改动)

429-llvm::

    编译器版本 0.14 jump：/home/wenjie/block_toolchain/linx64-llvm-20230222/linx_blockisa_llvm/bin/clang --target=linx64 -march=linx64im -mabi=lp64
    编译器版本 0.14 unjump：/home/wenjie/block_toolchain/linx64-llvm-unjump-20230223/bin/clang --target=linx64 -march=linx64im -mabi=lp64
    1. 纯静态_1：0m14.683s (不用块头load块体，纯静态)(qemu不加flags改动)
    2. 半静态_1：0m16.039s (块头load块体)(qemu不加flags改动)
    3. 半静态_2：0m16.146s (块头load块体)(qemu加flags改动)
    4. 纯静态_2：0m14.711s (不用块头load块体，纯静态)(qemu加flags改动)

仅针对429测例，qemu中添加一个flags-ibpc不会对性能造成什么损失。再跑一个时间长点
的464测例我们会发现，添加flags-ibpc(纯静态_2)虽然比无块内跳转(纯静态_1)的性能
慢2分半，但比块头load块体(半静态_1)的方案要好很多，一个是42m22.929s，一个是
71m12.464s。

464-llvm::

    1.纯静态_1：39m56.380s
    2.半静态_1：71m12.464s
    3.半静态_2：73m15.121s
    4.纯静态_2：42m22.929s

20230309
========

这里记录一下乘法类的指令分析。

1. Multicycle Binary Arithmetic Operation中的
   MUL, MULH, MULHU, MULHSU, DIV, DIVU, DIVW, DIVUW, REM, REMU, REMW, REMUW, MULW
   这几条指令在beta0.13版本中已添加，只有编码有变化。
2. Address Generation & Multiply Divide中的指令有
   AG.TW, AT.TD, AG.UXTW, AG.UXTD, AG.SXTW, AG.SXTD 这6类(因为这些指令的源操作
   数有4种组合，所以这里称为6类，不是6条)。源操作数组合有 t#l,t#r; t#l,RSR;
   RSL,t#r; RSL,RSR 这4种组合。
   指令功能如下::

    AG.TW: ag.tw t#l, t#r 操作：LinkL + LinkR << 2
    AG.TD: ag.td t#l, t#r 操作：LinkL + LinkR << 3
    AG.UXTW: ag.uxtw t#l, t#r 操作：LinkL +(unsigned word(LinkR) <<2)
    AG.UXTD: ag.uxtd t#l, t#r 操作：LinkL +(unsigned word(LinkR) <<3)
    AG.SXTW: ag.sxtw t#l, t#r 操作：LinkL +(signed word(LinkR) <<2)
    AG.SXTD: ag.uxtd t#l, t#r 操作：LinkL +(signed word(LinkR) <<3)

3. REV8, REV16, REV32 这3条指令是为了大小端转换添加的指令。
   指令的执行描述(from 华为内源基金会)：

    ..REV8::

        src0 = id – l;
        T[id][63:56] = T[src0][7:0]
        T[id][55:48] = T[src0][15:8]
        T[id][47:40] = T[src0][23:16]
        T[id][39:32] = T[src0][31:24]
        T[id][31:24] = T[src0][39:32]
        T[id][23:16] = T[src0][47:40]
        T[id][15:8] = T[src0][55:48]
        T[id][7:0] = T[src0][63:56]

    ..REV16::

        src0 = id – l;
        T[id][63:48] = T[src0][15:0]
        T[id][47:32] = T[src0][31:16]
        T[id][31:16] = T[src0][47:32]
        T[id][15:0] = T[src0][63:48]

    ..REV32::

        src0 = id – l;
        T[id][63:32] = T[src0][31:0]
        T[id][31:0] = T[src0][63:32]

   所以   0x11_22_33_44_55_66_77_88 执行REV8, REV16, REV32的结果应该如下
   REV8： 0x88_77_66_55_44_33_22_11 (实现：tcg_gen_bswap_tl)
   REV16：0x77_88_55_66_33_44_11_22 (实现：循环左移16bit & 0x0000ffff0000ffff;
   循环右移16bit & 0xffff0000ffff0000)
   REV32：0x55_66_77_88_11_22_33_44 (实现：循环左/右移32bit)

4. BXU, BXS, BMG, BAM 位提取指令。
   指令定义可以参考 https://openx.huawei.com/mkdocs/project/1410/blockisa-doc/docs/site/docs/isa/inst/BXU/

   1. BXU(Bit eXtract Unsigned): 从左操作数的第M位开始截取N位，并无符号扩展写到
      T寄存器，M、N存储在右操作数中。M 为LinkR的bit[11:6], N 为LinkR的bit[5:0]
      ::

        src0 = id – l;
        src1 = id – r;
        M = T[src1][11:6];
        N = T[src1][5:0] + 1;
        if (N + M) <= 64:
            T[id] = (uint64)T[src0][(N+M-1):M];
        else:
            T[id] = (uint64)T[src0][63:M];

        实现思路：link0 >>> M(逻辑右移M bit), & 1<<(LinkR[5:0]+1) - 1

   2. BXS(Bit eXtract Signed): 从左操作数的第M位开始截取N位，并有符号扩展写到
      T寄存器，M、N存储在右操作数中。

      实现思路：link0 >>> M(逻辑右移M bit) <<< (63 - LinkR[5:0]) >> (63 - LinkR[5:0])

    3. BMG(Bit Mask Generation): 从左操作数截取低N位作为Data，并将[M:N:Data]写到
       T寄存器。BMG和BAM共同完成bit位修改操作。
       M = link1[11:6] => val: 0~63
       N = link1[5:0] => val: 0~63
       T[63:58] = link1[11:6]
       T[57:52] = link1[5:0]
       T[N:0] = link0[N:0] // N<52
    4. BAM(Bit Apply Mask): 基于BMG的结果，从右操作数解析出Mask[M:N:Data]，并修
       改左操作数，写到T寄存器。BMG和BAM共同完成bit位修改操作。
       ::

           M = link1[63:58]
           N = link1[57:52] // N<52
           if M == 0:
              T[63:N+1] = link0[63:N+1]
              T[N:0] = link1[N:0]
           else:
              if (M+N) < 63:
                  T[63:M+N+1]=link0[63:M+N+1]
                  T[M+N:M]=link1[N:0]
                  T[M-1:0]=link0[M-1:0]
              else:
                  T[63:M]=link1[63-M:0]
                  T[M-1:0]=link0[M-1:0]

       上面这种思路用tcg实现比较复杂，考虑换个方式实现：
       temp1 = link1 << 63-N >>> 63-N << M
       mask = -1 << 63-N >>> 63-N << M
       temp2 = link0 & ~mask
       dest = temp1 | temp2

20230309
========

v0.14-v0.20修改点简要汇总
-------------------------

1. (v0.14)开发时叫v0.13beta，新添加了几条指令，修改了几条指令，也删了两条指令。
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

2. (v0.15)改动点::

    1. 块内私有寄存器SR0-SR7替换成LR0-LR31(Local GPR)，块头解码时更新LGPR，块
       提交时更新GPR。
    2. 原有的get, set, sget, sset指令调整为get, set, set.g(global)

3. (v0.16)改动点::

    1. 块头长度减半。块头由原来的128bit/256bit(concat)缩减到64bit/128bit(concat)。
    2. v0.15中的LR0-LR31缩减到LR0-LR16。寄存器名和寄存器别名对应如下：
       a7,  a6,  a5,  a4,  a3,  s5,  s4, s3, s2, s1, s0, a2, a1, a0, sp, ra
       R15, R14, R13, R12, R11, R10, R9, R8, R7, R6, R5, R4, R3, R2, R1, R0
    3. 低3bit表示跳转类型，当前解码块头时，首先判断跳转类型。
    4. attribute: 仅在concat块中有，且当前仅有原子atomic这一个属性。
    5. trap: 块头字段。仅存在return块中。

       1. trap==1 && bsize!=0 为系统块指令，内部接受系统调用，用于给硬件的一个hint。
          表示一个块准备报异常，在块头上给硬件提示，要产生异常了，这样的话硬件就
          不会预取后面的块了。qemu不模拟投机行为的，所以qemu实现不用做特殊的处理。
       2. trap==1 && bsize=0时，表示一个没有块body只有块head的ecall指令。(v0.16
          还有ecall的微指令，v0.20开始ecall微指令被删除)

    6. TP：块头字段。如果置0，那么计算方法为TPC = BPC + BTextOffset * 2；如果置1，
       那么计算方法为 TPC = TP(寄存器) + BTextOffset * 2。当前无TP寄存器，所以
       仅存在第一种情况。
    7. hyp：块头字段。表示为超级块，如果置1，代表块内有跳转。如果为0，那么块内无跳转。

4. (v0.17)改动点::

    1. memcpy, memset块头指令的实现。
    2. mempush, mempop 后续可能添加，待定。

5. (v0.20)改动点::

    1. trap imm，微指令，默认只有trap 0的情况，imm为非0值报非法指令异常。
    2. trap，块头字段（注意和trap微指令区分），仅存在incall, ret 这2种跳转类型中。

       1. 不考虑concat的情况下，trap仅存在indcall，ret这2种跳转类型的块中。

          1.trap==1 && branch type == indcall，不管size是否为0，均表示ecall指令。
          qemu实现可以这么考虑：bsize==0，产生ecall异常，完成系统调用后，按照fall
          though 继续往下取下一个块执行（不按照indcall的逻辑跳转，可以理解成ecall
          已经完成了"call"的功能，块body不再有"call"的功能，所以不用按照indcall
          的方式跳转）；bsize!=0，块body在commit之后执行ecall功能，并按照fall though
          继续往下取下一个块执行(fixme: 当前qemu未实现该功能)。
          2.trap==1 && branch type == ret，不管bsize是否为0，均表示eret指令。eret
          仅存在system模式中，user模式不会执行到该指令。执行逻辑是按照eret指令完
          成跳转，跳转的地方由内核提供的sepc地址决定。(附：执行supertest的时候，
          若bsize==0，表示supertest的结束标志)。

       2. 如果为concat块，也会有trap字段，这里需要分情况讨论。

          1.branch type 为 indccall或ret，和上述描述一致。
          2.branch type != (indcall || ret) && bsize==0，作为trap指令执行，可以
          认为是最后一条块指令。(fixme:不确定)
          3.branch type != (indcall || ret) && bsize!=0，无定义。

          ..note::

            concat中不可能存在memcpy/memset的块指令，因为memcpy/memset作为块头指
            令其编码是固定的，一定为标准块的fall through跳转类型块，不可能为concat块

    3. 添加local gpr之后，源操作数组合变多，如T+T, T+R, R+T, R+R。所以v0.20会
       添加很多仅源操作数组合不同、功能类似的指令。添加的指令较多，这里就不罗列
       在这了，具体需要看v0.20 excel 或 内源基金会网站定义。
    4. setc_trap 微指令。延迟指令，设置下一个块的陷出(trap)，在块提交后产生异常，
       异常模式存在左源寄存器中。指令定义不清晰。(fixme:当前实现有问题，待修复，
       不影响specint测试)
    5. Fixup属性：(fixme:qemu待实现)
       如果Fixup=0，且块内产生异常和被中断，则跳转到特权级中断异常入口中执行;
       如果Fixup=1，且块内产生异常和中断，则跳转至BPC + BNextOffset * 8的块头执行；
       如果Fixup=1，且本块执行不产生异常，则顺延下个块头执行。
    6. ...

20230320
========

今天编译器那提供了一版最新的Supertest，先跑一下，发现所有的测例都有 assert(tb->size!=0)
不通过的情况，之前继钦大概定位到是ret块的问题。ret块这里涉及到块头中的trap字段。
梳理对齐trap字段的定义之后，该问题解决。

20230322
========

v0.20 qemu+Supertest联调已经全部跑通了，这里记录一下定位解决的问题：

1. 确认 eret块指令+bsize==0作为supertest结束标志；
2. 确认 call/indirect call的情况下，块内对ra寄存器不能set.g或者set.gl，只用set
   不会影响到函数返回；
3. 修复 lw.ip, lwu.ip 一个是有符号32bit，一个是无符号32bit；
4. 修复 sw, sd 指令实现问题；
5. 修复 ld_f, lbu_f, lhu_f, lwu_f 指令中imm编码为有符号数；
6. 修复 qemu中concat块高64bit为0时触发的bug。supertest中会存在concat块的
   head[127:64] 全为0的情况。(反汇编发现在多个concat块中间插入一个fallthrough的
   concat块，可能是为了其他concat块的地址对齐吧)

编译器路径：

0.16 gcc: /home/wenjie/block_toolchain/linx64-linux-gnu-v0.16/linx64-linux-gnu/bin
0.16 llvm: /home/wenjie/block_toolchain/linx64-llvm-v0.16/linx_blockisa_llvm/bin
0.20 gcc: /home/wenjie/block_toolchain/linx64-linux-gnu-v0.20/linx64-linux-gnu/bin
0.20 llvm: /home/wenjie/block_toolchain/linx64-llvm-v0.20/linx_blockisa_llvm/bin

单独编译libc-tls.c::

    -ftls-model=local-exec -O2 -Wall -fstack-protector-strong -D_FORTIFY_SOURCE=2
    -Wl,-z,relro,-z,now,-z,noexecstack -Wtrampolines -march=linx64  -Wno-error=array-bounds
    -Wno-error=implicit-function-declaration -Wno-error=missing-attributes
    -Wno-error=stringop-overflow= ../sysdeps/linx/libc-tls.c -E -std=gnu11
    -fgnu89-inline  -g -O2 -falign-functions=16 -Wall -Wwrite-strings -Wundef -Werror
    -fmerge-all-constants -frounding-math -fno-stack-protector -fno-common
    -Wstrict-prototypes -Wold-style-definition -fmath-errno
    -fno-stack-protector -DSTACK_PROTECTOR_LEVEL=0   -ftls-model=local-exec   -U_FORTIFY_SOURCE

20230403
========

1. specint子项v0.20编译器选择:
   Fortran(648子项)和带C++异常(620，641子项)需要GCC编译器，其余LLVM
2. segment fault问题定位思路：
   gdb qemu，加上 -d in_asm,out_asm 跑，然后一般是在 code_gen_buff 这里出现
   segfault，在这里直接打印下 p/x $pc，看看 host 的pc，在去 qemu 的日志里去找
   这个 pc，然后上面的 guest addr 就是 blockisa 的 bpc/tpc
3. sllw单元测试用例
   ..code::

        int __attribute__((noinline)) f(int a, int b) {
          return a << (b&0x1f);
        }
        int main(){
          int a = f(1, 33);
          printf("%d\n", a); // expect 2
          return 0;
        }

4. qemu选择性的打印地址区间的log: -dfilter 0x10+0x100,0x2000..0x3000
   ./build/qemu-linx -dfilter 0x10+0x100, 这个是用来配合-d 参数用的，dfilter
   可以指定你需要打印日志的地址范围，那个范围是 base_addr+addr_offset这样的形式，
   如果有多个范围可以用逗号隔开。
5. 0.16/0.20 464子项验证不正确问题原因
   x86架构默认char是signed类型，arm64/riscv/blockisa默认是unsigned. 464子项的
   验证需要开启编译选项 -fsigned-char(将char load到寄存器做有符号扩展)，才可验证通过。
6. 0.20 502, 523子项跑飞原因
   (from compiler)tail indirect call作为函数退出语句时，需要一个寄存器作为输入
   指定其目标地址。Epilogue在函数退出语句前会还原Callee Save寄存器(CSR)。如果
   tail indirect call被RA分配了CSR，插入Epilogue时就会污染这个寄存器的值。
   在RISCV中，解决方法是声明一个不包含CSR的RegClass提供给tail indirect call使用，
   在BlockISA中，可以直接使用T寄存器作为其输入。
7. v0.20 user mode linx_debug指令使用

   ..code::

        #include <stdio.h>
        #define memory_dump(id, addr, size) do {             \
            register unsigned long a0 asm("a0") = (id);      \
            register unsigned long a1 asm("a1") = (addr);    \
            register unsigned long a2 asm("a2") = (size);    \
            __asm__ __volatile__ (                           \
                ".4byte (0x00000007) \n"                     \
                ".4byte (0x00000000) \n"                     \
                ".4byte (0x00000000) \n"                     \
                ".4byte (0x00000084) \n"                     \
                ".popsection \n"                             \
                : : : "memory");                             \
        } while(0);

        int main()
        {
            int num[3] = {1,2,3};
            int* addr = num;
            //printf("addr is 0x%lx\r\n", addr);
            memory_dump(1, num, 3 * sizeof(int));
            //printf("addr is 0x%lx\r\n", addr);
            return 0;
        }

8. supertest 完整版qemu测试
   路径: /home/wenjie/test/supertest/llvm_020
9. 想要查看某一段C/C++语句汇编出的指令，可通过插入一段汇编查看反汇编即可。
   __asm__ __volatile__("nop");
   编译器似乎对于内嵌汇编会在汇编文件里，加上 '#APP' 和 '#NO_APP' 表示内嵌开始
   和结束，因为内嵌汇编是会被 #APP 和 #NO_APP 包裹着，所以可以直接在汇编文件中
   搜索"APP"快速找到位置。

20230404
========

1. 问题描述模板:

    1. 问题描述：
       1.1 问题的背景以及现象是什么（你看到的是怎样的）？
       1.2 问题的根因以及处理方式是什么？
       1.3 当前问题处理结果是怎样的？

    2. 接受任务前至少弄清以下几个问题：
       2.1 为什么做这个？弄清楚当前做的事情如何与大目标关联
       2.2 方案是什么？为什么选这个方案？
       2.3 期望结果是什么？

2. qemu debug思路(类似-dfilter这种参数的使用):
   https://wiki.huawei.com/domains/4821/wiki/12455/WIKI20230221777948
3. ACR融合，larm+实现对齐，改动点如下(owner: 石晓强)

   1. etrap 改成 scall, 新增imm参数, eret 改成 acri, 语义相关参考 larm;
      redoecall 待适配。
   2. ECAUSE寄存器的异常号trapnum/syndrome变更, qemu需要修改对应代码;
   3. FUTO重新设计, qemu保留了fixup代码, 待larm更新后需要相应修改.
   4. ipending 寄存器物理上只有一个, 目前qemu当作了sysreg寄存器, 需要相应修改.

20230407
========

fixup字段更新
-------------

v0.20 concat的一些域段发生改变。这里更新一下，并分析qemu需要做的改动。
假设head1为concat的高64bit，那么部分域段的含义如下:

1. head1[35:32]: 4bit atomic
2. head1[39:36]: 4bit attributes
3. head1[40:40]: 1bit fixup
4. head1[41:41]: 1bit trap

1. atomic字段。无影响。
2. attr字段。影响有两点:
   1.在 TraceManager 采用自动切片之前，是会通过将attr置为 0xE 的值作为手动切片的
   开始的地方，当解码过程中发现concat块中的attr值为0xE，那么就可以认为这是切片的
   起始点。现在是采用simpoint方案自动切片，qemu可将该处删掉。另外，手动切片的结
   束位置也可以删掉(v0.20 supertest的结束位置现在变更为eret块指令+bsize==0)。

   ..code::

        // 可删除下面的代码
        /* using by TraceManager start inst  */
        if (block_attr == 0xE) {
            ctx->pc_succ_insn += 16;
            ctx->base.is_jmp = DISAS_NEXT;
            return true;
        }

   ..code::

        static bool trans_blk_trap_f(DisasContext *ctx, arg_blk_trap_f *a)
        {
        #ifndef CONFIG_USER_ONLY
            generate_exception(ctx, RISCV_EXCP_U_ECALL);
            return true;
        #else
            // 下面有关supertest的代码逻辑可删除
            CPURISCVState *env = ctx->cs->env_ptr;
            /*
             * This insn is used as the end insn of the Trace Manager
             * and supertest. When the supertest runs the trap command,
             * the QEMU needs to be ended, but the tracemanager does not.
             */
            if (run_supertest) {
                if (env->gpr[xA0] == 0) {
                    printf("test pass!\n");
                    qemu_log("test pass!\n");
                } else {
                    printf("test fail!\n");
                    qemu_log("test fail!\n");
                }
                exit(0);
            }
            return true;
        #endif
        }

    2.影响到linx_debug的功能。linx_debug原来的设计思路简单来说是当attr某一个bit
    置为1，表示这是一条debug指令，想要完成的操作用attr中的剩余bit作为标志，每一
    个bit代表一个功能。由于attr仅占4bit，当前我们可以这样设计：head1[39:39]作为
    是否为debug指令的判断条件，head1[38:32]这7bit表示具体某一个功能，这样当
    head[39:32]值为"0b10000100"表示的是"LD_ATTR_BIT_DUMP_MEM"debug指令；不过需要
    注意的是，仅7bit无法实现所有的debug指令(当前有8个小功能需要支持)，后续需重新
    设计。

    ..code::

        #define LD_ATTR_BIT_STOP_VM        0x001
        #define LD_ATTR_BIT_DUMP_STATE     0x002
        #define LD_ATTR_BIT_DUMP_MEM       0x004
        #define LD_ATTR_BIT_SHOW_ID        0x008
        #define LD_ATTR_BIT_DUMP_STRING    0x010
        #define LD_ATTR_BIT_PREEMPT_REPORT 0x020
        #define LD_ATTR_BIT_LOG_ENABLE     0x040
        #define LD_ATTR_BIT_LOG_DISABLE    0x080

3. fixup字段。无影响。
4. trap字段。无影响。

20230408
========


