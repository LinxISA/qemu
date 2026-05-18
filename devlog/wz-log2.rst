
.. wang Zhou 版权所有 2022

:Authors: Wang Zhou
:Version: 1.0

Wang Zhou的开发日志
**********************

这里记录linx-block方案的qemu开发日志。开始做ACR的使能了，用一个新的文件记录。

20221205
========

 LinxInit的具体实施可以分步走。第一步可以先搞定单核启动，然后第二步再搞定多核启动，
 启动多核启动需要用到LinxInit提供的服务。这样，第一步完成，就可以启动qemu和内核
 的联调，相关任务就可以并行起来。

 第一步，我们需要做的内容有：主核需要做的基本配置，中断异常代理，然后就是启动内核。
 我们在做第一步的时候就需要统一设计下整个代码逻辑，避免添加第二步的代码时再大范围
 的改动代码。

 第二步，我们再把多核启动需要的，从核启动，LinxInit服务接口，相关的基础组件加上去。

20221213
========

 LinxInit的仓库我们先放到这里: ssh://git@codehub-dg-y.huawei.com:2222/w00606512/LinxInit.git

 我们目前的计划是，先按照rv的系统定义，用block的指令实现基本流程。可以预料到的结果
 是内核可以被拉起来，会执行一小段，但是在执行sbi调用的时候，就会挂掉，我们在LinxInit
 里实现ecall的调用后，sbi的调用又会往前走一小段，会运行到timer启动，又会挂掉。

 目前，晓强按照这个思路，简单hack下后，已经可以启动内核，现在是挂到sbi的调用这一步。

 我们考虑下一步怎么继续往下走。LinxInit是可以和qemu配合一起都用ACR的寄存器和特权级
 定义的，但是，qemu现在还在修改，在qemu没有修改好之前，LinxInit可以继续用rv向前
 推进，可以想到的是:
  
  1. 把ecall trap的流程用block实现，这个后续也要打通。
  2. 把dts的代码加到LinxInit里，这个直接把libfdt的库代码copy进来，参与编译就好。
  3. 在LinxInit实现驱动的框架。
  4. 基于2和3，在LinxInit里实现串口的简易驱动，目的可以校验2和3。
  5. 继续把1的框架做好。

 上次开会曾博需要明确下qemu这边这一波要实现的东西，这个我们已有共识，但是还是写下
 吧：我们这一波，qemu实现biso/kernel/user对应的三个ACR以及相关的控制逻辑，我们不
 计划做虚拟化相关实现，中软的同事会把中断控制器包括timer、IPI实现，然后一起合入
 qemu，其中，timer、IPI会在kernel对应的ACR level实现，这次我们我要支持多核。基于
 这样的实现，我们认为boot一个linux系统已经足够了。

20221220
========

 由于larm没有搞定，我们目前只能根据技术分析文档改代码，目前qemu的代码都改到了
 linx-dev-acr这个分支上了，现在的技术分析文档很多细节都没有定义，所以，qemu改代码
 的时候，一定要先写devlog再改代码。这一轮做替换，其实很多和RV的是有映射关系的，
 在改代码的时候，我们首先要写devlog独立设计，但是写的时候，我们还是要尽量的利用
 这些映射关系，这样可以减少我们的工作量。

 李飘在exception-modified-br上的这个改动，就动的范围太大了，而且形成不了一个逻辑
 闭环，虽然看起来一把把rv之前的异常都删了，但是这个patch，其实改了: 1. user mode
 的逻辑，2. mmu的逻辑，3. csr读写的逻辑(我们叫SSR了，所以这个名字要改下)，4. 中断
 异常ebstate的逻辑。这样改下来，每个都不知道改的对不对，如果改错，后面都会把我们
 搞的很被动。

 我们依然可以复用qemu里exception_index的逻辑，把异常号(trapnum+syndrome)映射到
 exception_index的低16bit，trapnum到0-5，syndrome到6-15，这样做，syndrome只能编码
 10bit，不够手册里定义的24bit，但是，我感觉是可以这样hack的，rv上scause都留了63bit，
 它照样编码不到16bit里，这里，我们认为一个trapnum里最多也就定义1024gesyndrome了。

 对于异常这块的改动，我感觉用到哪里改到哪里，就直接用我们新定义的异常数值就好，
 到了最后都改完了，rv的自然删掉就好。这样改比我们现在这样，不知道具体异常的定义，
 简单做替换要靠谱。

 我们可以根据具体代码去排qemu特性的优先级，这样每走一步都可以得到验证。

 1. sysget/sysset ACR寄存器，eret支持。做了这个和LiniInit就可以开始联调，预期结果
    是可以进入内核。

 2. 内核里一开始也是sysget/sysset，然后马上会用到satp，所以相关的缺页异常，sfence.vma,
    fence实现后，和内核联调，就可以走到虚拟地址上，这个预期可以走一段内核。
    (看这个的时候，我发现内核的改动并不是很大，这样瓶颈完全可能会在我们这边)

 3. 内核下一个关键的特性是起timer和串口。廖畅会拼上来一个s timer和外部中断控制器，
    我们qemu的改动主要是把核内的中断处理搞好。

 4. 再后面就是要保准系统调用ok，qemu的改动就是etrap的支持。

 5. 起多核。qemu的改动是支持0x2000位置上的MMIO命令可以用。

20220103
========

 在《系统指令实现对齐》中，我们分析了ACR中需要实现的系统指令，除了ECALL/ERET，
 我们认为其他指令是不要需要修改的，但是我们忽略了，其中的有些指令直接借用了RV的
 实现，这些指令包括wfi和sfence.vma。我们需要重新定义这些指令的实现，基本上是决定
 要先删掉什么特性，基本上是，只保留基础功能，删去虚拟化相关的功能，删去需要格外加
 寄存器bit控制的功能，删去的功能可以在有明确需求的时候再加回来，根据这样的逻辑，
 整理wfi和sfence.vma的功能如下。

 wfi: 在ACR2执行wfi，报非法指令(否则构成一种攻击手段)，在ACR0和ACR1指令wfi，挂起
      CPU。被挂起的CPU，在检测到有中断pending，并且中断被enable是，继续取指令执行
      (同RV逻辑)

 sfence.vma: 只能在ACR0/ACR1执行，在ACR2执行报非法指令异常。正常执行flush本CPU
             的tlb。

20230128
========

 春节前看了文杰那个head-body没有直接跳转导致qemu执行慢的问题，qemu相关代码的限制
 都看了下，相关的技术总结写到blog里了。

 我们不看qemu的限制，看看我们基本的head-body翻译执行的逻辑是不是可以优化下解决这
 个问题。
 
 我们现在的模型是head和body都要翻译成独立的tb块，因为head和body的距离跨度比较大，
 所以head和body对应的tb块之间没法直接chained过去，导致head到body tb在执行的时候
 有很多查找进来，降低了qemu的速度。我们考虑的是，能不能去掉这个tb跳转(另一个方向
 是国柱提到的，扩展一个PageDesc管理的范围，这样一部分或者一大部分head到body的跨度
 就可以在一个PageDesc内，这样这部分head-body就可以chained起来，最后的分析的结果
 也是用这样的方式，这里我们看看还有没有其他的方式解决这个问题)。

 我们考虑head和body能不能翻译到一个tb块里，其实之前head和body连续排列的时候，
 head和body就是放在一个tb里的，但是后来head和body改成分开排列，对应的就做成两个
 tb了。把head和body放到一个tb里，破坏了qemu翻译执行的基本约束，qemu认为一个tb里
 对应的guest指令应该是连续的，我们可以看到的基于这个约束的qemu实现至少有：1. 翻译
 时的tb size，2. 中断时要精确找见guest PC时的算法，感觉这样做还是破坏的东西太多，
 后面不知道哪里会出问题，所以我们不走这条路。

 还有一个种思路是，我们能不能不生成head对应的tb，把head-body的所有信息翻译到一个
 tb里，和上面直接翻译到一个tb里不同，这里head里的信息完全是作为外加的body信息理解
 的，比如，上面的方法里，head-body对应的base pc是head的pc，但是这里，head-body对应
 的base pc是body的pc，即head-body的信息综合翻译得到一个tb。这个tb的跳转地址是下一个
 head的地址，所以，下个head翻译的时候，我们还要修改下上一个tb里跳转地址，使其指向
 下一个head-body翻译的地址。想想都太复杂了:(，我们还是先试试把PageDesc管理page扩大
 的方式。

 我们看把PageDesc管理page扩大的方式。这里的最大问题是qemu里用一个TARGET_PAGE_SIZE
 的宏管理了一堆东西，我们要确定被管理的部件在逻辑上是分离的，这样才能放心的把PageDesc
 里的TARGET_PAGE_SIZE改大。可以看到TARGET_PAGE_SIZE可以分离的逻辑有：1. guest系统
 的page大小; 2. PageDesc管理的page大小; 3. 热迁移脏页管理里的page大小(应该和guest
 系统page大小是一个概念)。看起来我们可以把PageDesc和goto_tb里的TARGET_PAGE_SIZE
 改大而不影响qemu的其它部分，在主线rv上改了一个版本试了下，系统可以起来：
 https://github.com/qemu/qemu/commit/d9024eabb0515627f41c8542ffb029943228bb18
 我们后续需要在block qemu上试试优化的效果，还需要看下这样做带来的开销，理论上只有
 自修改代码才会触发整页对应的tb被刷掉，我们目前测试用的benchmark都没有自修改代码，
 应该不会有开销才对，上面的hack代码把PageDesc的一个页扩大到64KB，这个完全是一个
 hack，具体的效果会和head-body的实际排布有关系，这个值也可以继续调整。

20230202
========

 具体看下要怎么修改，首先如果都没有自修改代码，我们都不用先考虑PageDesc扩大的改动，
 我们可以直接强制chained tb下，在这个基础上，我们的代码里在head-body、body-head
 的时候会exit_tb，为了使用chained tb，我们还要把这里修改成chained tb。

 body-head的是在linx_gen_blk_commit是helper后直接exit_tb，head-body是在
 gen_helper_blk_do_recovery调用的地方，也是helper + exit_tb。qemu里在使用goto_tb
 时需要知道跳转的目标pc，这样在翻译下一个块的时候才能把下一个tb的指针传给上一个tb。
 helper函数里可能动态改变PC，下一个tb是不定的，所以，要使用goto_tb，我们需要把
 helper函数都用中间码重写。

 提出这个方法的时候，文杰提到一个问题，我们有不同head复用一个body的情况，跳转
 在head里定义，但是翻译body的key中没有跳转这个参数，这样不同跳转类型的head复用
 一个body，翻译出的body就是一个，这样显然是错的，为此我们要在翻译body的时候把
 跳转类型作为一个参数也传给翻译过程。用helper时，没有这样的问题，是因为helper里
 可以根据head的不同动态的决定怎么跳转。

 基于以上的认识，如果我们改成都用中间码实现，所有head里的变量(可以改变body行为的)，
 都应该做为body的翻译参数。

 翻译参数的更新是在平台自定义的init_disas_context里。

20230207
========

 如上优化的时候，发现helper函数的开销比较大，做了helper函数和中间码性能的对比。
 大概的思路是，用helper实现一个hack的add指令，在测试程序里大量的使用add，保证add
 被使用的密度足够高，这样在其他指令执行时间可以忽略的情况下，比较helper add和普通
 add测试程序执行的时间，得到helper相对于中间码的开销。

 测试代码和qemu的hack代码传到test-helper-add这个分支上了，测试中使用了两个block
 ISA块，第一个块里放了100个add，第二个块里控制循环次数，除了add之外的开销有：
 1. 第二个块的跳转逻辑，2. 两个块的块头逻辑，3. 生成的四个tb的之间跳转的开销。
 经过跟踪log，可以发现四个tb都是chained的，chained tb直接跳到下一个tb的业务代码
 上，可以认为3是没有开销的，块头是几条mov指令，可以认为开销比较小，第二个块的跳转
 逻辑相对于第一个块的add个数，可以认为比较小。用这样的模型得到helper的开销大概是
 中间码的13倍左右。

 调试的时候有个问题，一开始查看生成的host指令和优化后的中间码，发现中间码的情况，
 100条add被优化的只剩下最后8个add中间码，因为100个add是逐个索引的，后面依赖前面，
 所以不应该出现这样的优化才对，这个问题还没有搞清楚? 为了规避这个问题，手动注释
 掉了tcg优化的宏，这样测试得到了如上的结果。

 国柱提醒要注意测试程序自己的开销，继续把第一个块的中的add指令全部去掉，空跑一下
 得到测试程序自己的开销，这样计算出helper的开销是中间码的46倍左右。

20230210
========

 性能测试需要在当前v0.14的基础上(对应的指令改动已经在Linx15-blockisa-v0.14 LARM
 分支, qemu的改动在linx-dev-v0_13-beta分支上，qemu的改动还差sysset/sysget/lconst),
 逐步实现v0.15/v0.16/v0.20的改动(v0.16之后直接是v0.20)。

 v0.15/v0.16/v0.20这几个版本的改动均没有在LARM上体现，v0.15的改动在之前SEG上讲过
 的一个PDF，名字是(BlockISA微指令改进方案)。改动点包括：
 
 - 私有寄存器扩展到16个，统一用get/set访问，块内对gpr的读写改成块创建和提交
   的时候根据bitmask隐式进行。
 - 增加块内直接set gpr的指令：SET.G。(有GET.GLOBAL么?)

 v0.16/v0.20的改动集中在文博的一个名为BlockISA Encoding V0.20的excel文档中描述。
 其中v0.16主要集中在块头和gpr的变化(excel的Block Header页)，具体包括：

 - 块头根据跳转类型不同，编码也不一样。
 - gpr改成16个。

 v0.20对除系统指令外的全部指令刷新了编码，新的指令编码在excelStandard Block、
 Standard Hyper Block以及Auxiliary Block页，主要的改动点是微指令可以直接索引gpr。

 如下是针对新增指令的qemu实现分析，这个分析不是涉及具体实现上的考虑，需要注意的是
 v0.15/v0.16/v0.20还没有LARM的详细定义，qemu的同学在实现的时候务必要先写设计文档
 和编译器同事对齐。

 sysset/sysget改成16bit微指令，操作数放在T寄存器中了。lconst改成load.ip，16bit微指令，
 从body结尾的地方load一个常数到T寄存器。需要注意的是，整个v0.14除了新加指令，原来
 基础指令的编码也有改变，这样如果我们把v0.14合入主线，会导致system mode + 内核跑
 不起来，所以，我们还不能把v0.14的改动合入linx-dev分支。

 把块头改成多种类型，并不改变我们现在的块头解码整理逻辑，无非是具体的解码逻辑要
 重写下，gpr减少，我们只要做对应的改动就好。这里要注意的是，块头里区分block和rv
 的编码没有了，加上这个之后，我们彻底不支持rv了，不过本来就在独立分支上搞，并且
 只用user mode，对我们也没有影响。

 v0.20对我们的唯一影响就是工作量了，看起来指令的改动也还好，就是测试的工作量大。
 指令编码改变了，之前hello test里面硬编码的测试代码都要改变，如果编译器对应支持，
 我们可以写block的汇编代码，编译后测试，同样的方法可以测试如上版本的指令，不过这
 要和编译器对齐下节奏。

 如上的分析，整个性能相关新增指令，我们还是在一个单独的临时分支上实现。

20230301
========

 v0.16的spec根据head跳转类型，把head拆成了几种不同的编码格，其中六种跳转类型对应
 六种64bit head，concat块对应一个128bit的块，变化的域段有，引入了TP(bit)表示body
 的基地址是用BPC还是TP，加了HYP bit，表示是否超级块(可以块内跳转)，块头和body的
 编码空间重叠，block attribute只在concat块，也就是原子块还有带调试功能的块只能是
 concat块，block type只在fall和concat里，ret块里加了trap bit，这个是一个给硬件的
 hint，表示要不要投机执行后面的块(qemu不能模拟投机，不用考虑这个)。

 这样的改动促使我们考虑head的解码方式是否要做改变。

 我们首先看整个block解码的状态机。现在head已经和rv的解码空间重复，我们也马上要把
 ACR的改动合进来，继续支持rv和block两套解码已经没有必要，所以，v0.16这个版本只有
 block解码了。block解码的状态机还是和以前的一样，我们用bpc是否有效决定是否进入body
 的解码，启动启动bpc是无效的，翻译执行head，bpc变成有效，跳出body或跳出无body head
 时，bpc要无效。

 我们考虑head解码的方式，之前是手工解码的，现在继续这样也是可以的，但是代码会越来
 越乱，我们考虑用decode file方式定义，自动生成head的解码逻辑，进一步查看scripts/decodetree.py
 可以看到64bit指令是支持的，128bit就不支持了，我们可以都按照64bit解码，然后遇到
 concat head的时候在手动解高64bit。

 基于以上的解码逻辑，decode函数会根据branch type的不同调用各自的解码函数，在各自
 解码函数实现的时候，我们可以考虑把公共的代码抽出来形成函数，不过这些都是具体编码
 要考虑的问题了。

20230303
========

 之前搞的各种提升qemu性能的优化里有一个都改成静态T寄存器索引的优化，这个优化落地
 的时候一直有问题，我也进来深入看下。

 首先我们搞清楚现在有块内跳转后到底是怎么索引T寄存器的，说到底在各种情况下，都是
 按照程序的执行循序索引T寄存器的，硬件做一个深度是8的T寄存器环形队列，程序按照
 执行顺序做索引，当这样做索引的时候，指令和产生被索引T寄存器的指令之间有块内跳转
 指令时，我们说这种情况是存在跨块内跳转指令的T寄存器索引，LARM不禁止这样的情况，
 但是，程序要写这样的代码时，需要人脑去运行代码，知道当前到底是访问的那个T寄存器，
 这样很不友好。

 作为qemu，我们要支持LARM里定义的块内跳转行为，深入看了下，其实我们用ibpc和ibpc_trans
 的翻译执行方案，已经可以hold住上面模拟，具体方案之前的devlog里已经写过，我们这里
 再表述一次。

 T寄存器的索引全部在翻译的时候搞定，我们用ibpc_trans表示翻译时T寄存器的下标，那么
 当我们开始翻译一个块的body时，ibpc_trans应该是0，在body内，如果程序顺序执行，ibpc_trans
 就每个微指令都加1，并做循环，当有块内跳转的时候，qemu会创建新tb，新tb开始翻译时，
 ibpc_trans要顺序取值，这样根据ibpc_trans的值，我们做负向的索引也是没有问题的，
 这种情况就是跨块内跳转指令的T寄存器索引。

 李飘提出一种可能出错的场景，就是多个块内跳转指令跳到一个地址，第一次跳转的时候，
 我们正确的翻译的跳转目的地址的块，当另外一个块内跳转指令跳到相同地址时，qemu会用
 已经翻译得到的tb，但是两次跳转对应的ibpc_trans可能是不同的，这样就会出错。

 这个问题提示我们引入T寄存器这种设计，把独立的指令之间建立了联系，T寄存器索引的
 当前位置形成单条指令执行时的一个默认参数，当我们选择翻译到一个tb里时，就要在tb
 里通过helper函数或者中间码动态的得到这个索引参数，这就是我们当前动态的到T寄存器
 索引的方案。我们也可以根据这个参数的不同，产生不同的tb块，qemu中tb的key用
 flags/cflag/pc等组成，其中flags就是具体处理器相关的翻译参数，我们可以在这个翻译
 参数里占用3个bit，表示T寄存器索引。qemu在翻译、查找tb(包括翻译时查找tb和
 lookup_tb_ptr时查找tb)的时候会调用cpu相关的函数cpu_get_tb_cpu_state获得flags，
 我们只对这个函数做修改就可以。

20230315..0316
===============

 分析了下qemu后端翻译的逻辑，分析的笔记在这里：(还需要持续补细节) https://github.com/wangzhou/notes/blob/master/qemu_tcg%E4%B8%AD%E9%97%B4%E7%A0%81%E4%BC%98%E5%8C%96%E5%92%8C%E5%90%8E%E7%AB%AF%E7%BF%BB%E8%AF%91

 这个分析的出发点是看看如何做blockISA的qemu后端翻译。qemu后端翻译可以作为一个ISA
 JIT支持的一个代表(比如JVM以及eBPF)，我们可以尝试支持下blockISA的qemu后端翻译，
 看看其中有没有什么坑或者什么创新点。不过，qemu中IR翻译到BLock ISA和编译器同事
 搞编译器后端是一样的，编译器不会出问题，qemu的后端也不会出问题，可能就是qemu的
 后端要比LLVM简单很多，还有就是针对qemu的中间码具体需要这么实施的工程问题。

 看下我们要搞定哪些翻译，基本的中间码的翻译(向量可以先不管？)，tb头尾的构造，
 tb业务指令可以构造一个block ISA的块(主要是确定块的跳转类型、bset/bset、是否支持
 块内跳转、是否需要原子块)，寄存器分配(主要包括gpr/local gpr/T寄存器的分配)。

 下面一个一个拆开看下：(当前设计基于v0.20指令定义)

 1. 中间码翻译

    qemu的中间码可以分为具体业务相关和qemu控制相关，qemu控制相关的有：
    insn_start/discard/exit_tb/goto_tb等。剩下是具体业务相关，这其中也大概可以
    分为：计算，控制流以及io(load/store)。

 2. tb头尾构造

    之前国柱提到tb的头尾可以分别用一个block封装，感觉可以这样做。

 3. 寄存器分配
    
    tb头和尾对应的block块是固定死的，不在这里考虑。我们这里的问题是一连串IR的TCG
    虚拟寄存器怎么在gpr、local gpr、T寄存器上分配。

    TCG虚拟寄存器有global、temp、local temp、fixed、ebb以及const，其中temp和
    local temp用来存tb中计算的临时变量，他们总是生成于一个左值，在host寄存器层面
    计算，local temp可能会刷会内存，而temp生命周期只在bb，永远不会刷会内存。
    global一般映射到guest的gpr上，使用前要先load到host寄存器，使用完后刷回内存。
    const可以直接编码到host指令，也可以像global那样load到host寄存器参与计算，但是
    它是常量，只参加计算，一定不会再刷回内存。fixed是直接和host特定寄存器绑定的
    TCG寄存器，一般有cpu_env(需要考虑这个和谁绑定)和sp。ebb主要是支持indirect global
    的访问，所谓indirect global是指不通过fixed寄存器访问的global寄存器。

    blockISA中都要在block里进行计算，对于global是一定要先load到T寄存器上的，load
    使用的cpu_env和偏移可以编码到ld [RSL, imm]或者ld [t#1, imm](注意，这里就有可能
    出来先imm不够用的情况)。T寄存器上保存的global参与计算的逻辑和其他ISA都不一样，
    T寄存器只有8个，需要循环使用，当索引距离不足时，需要把T寄存器copy到新位置，
    这个需要想下怎么实现？temp、local temp和ebb都可以使用T寄存器，但是这会不会导致T
    寄存器不够用，如果不够用又要怎么办？const也需要load进T寄存器使用。

    如果按照上面的分析，那么，我们完全pass了gpr和local gpr。如果，在block内部我们
    把local gpr也拉进来做临时变量寄存器(注意因为计算的结果一定会在block内刷回内存，
    块的bset是为0的)，那么外部的gpr我们还是利用不起来，利用不来这些gpr总归要把资源
    闲置，损失性能，需要再想下!?

 我们考虑具体的实施办法。需要持续分析完上面的各种情况怎么实现，具体测试时，我们
 可以编译一个BlockISA的user mode qemu，然后把这个qemu跑在一个当前模拟Block的qemu上，
 就是用两层qemu来做测试，编译第二层的user mode qemu需要依赖几个库，那么就需要block
 ISA版本的依赖库。

 另外一个严重的问题是，现在qemu的版本严重分裂，不同版本的ISA又差别很大，我们最好
 基于最新的v0.20 ISA进行测试，好在我们可以这样做测试：
 ./qemu-linx(guest blockISA, host X86) ./qemu-riscv(guest riscv, host blockISA) app(riscv程序)

 上面是对问题的一个大概认识，现在我们开始考虑解决办法。

 首先是一个tb翻译到后端BlockISA时block如何划分，一个tb一般由prologue、业务代码和
 epilogue三部分组成，整个tb是一个host上的函数调用，prologue一般的工作是准备堆栈、
 保存callee save寄存器、配置fix寄存器以及最后跳入业务代码执行，业务代码为实际翻译
 的host指令，可以有跳转，epilogue为函数退出恢复环境，然后做函数返回。这样，我们可以
 把prologue/epilogue分别组织到一个块里，为了简单，我们把业务代码组织在一个块里，
 当业务代码里跳转时，我们用块内跳转支持。
 
 示意如下::

   +---------------------------------------------------------------+  <-- one TB
   |  +------------------+  +-----------------+  +--------------+  |
   |  | prologue         |  |                 |  | epilogue     |  |
   |  |                  |  |                 |  |              |  |
   |  |  head:           |  | head:           |  |  head:       |  |
   |  |                  |  |                 |  |              |  |
   |  |                  |  |  function input |  |              |  |
   |  |                  |  |                 |  |              |  |
   |  |  body:           |  | body:           |  |  body:       |  |
   |  |                  |  |                 |  |              |  |
   |  +------------------+  +-----------------+  +--------------+  |
   +---------------------------------------------------------------+

 如上所示，TB函数的入参传入业务代码对应的block块，整个业务代码被封装到一个block块
 里，封装到一个block块里的代码只能只用T寄存器或者local gpr完成计算，第一层gpr中的
 callee save gpr并没有使用到，没有使用的寄存器有S0-S5。

 如上，global要先load到T或local_gpr寄存器上使用，计算产生的临时变量(temp)也直接放
 到T寄存器上，当索引距离不够时，我们把T寄存器的值copy下，继续使用。
 
 T寄存器有索引距离的限制，但是local_gpr是按名字索引的，不存在这个问题，因为T寄存器
 是按照程序执行顺序动态索引的，这使得跨块内跳转的T寄存器索引变得很困难，为此，
 在业务代码里出现块内跳转指令时(我们还是把br/brcond映射成一个块内跳转)，我们要把
 跨块内跳转的寄存器放到local_gpr上，块内跳转后使用名字做索引。这样，最终的情形是，
 只是在BB内使用T寄存器(对应normal temp寄存器)，当br/brcond导致跨BB，我们使用
 local_gpr保存local temp。这样的设计可能会导致local_gpr被闲置，因为local temp使用
 的并不多，还可能导致频繁的出现T寄存器的copy，为此，我们可以分一些local_gpr给
 normal temp使用。

 call IR的实现会比较复杂，因为blockISA下，call在head里，所以如果有call IR，那就
 必须对业务代码分block块了，call IR作为一个block的结尾，这样切分直接破坏了所有业务
 代码放到一个block块里的基本设计，显然，我们改变这个基础设计，现在我们的基础改变
 成遇到call IR要分block块，当然这并不妨碍没有call的时候，全部用一个block。
 
 示意如下::

    +-----------------+           +------------------+
    |  IR0            |           |  IR0             |
    |  IR1            |           |  IR1             |
    |                 |           +------------------+
    |  lable: IR2     |           |  lable: IR2      |
    |  IR3            |           |  IR3             |
    |  IR4(call)      |           |  IR4(call)       |
    +-----------------+           +------------------+
    |  IR5            |           |  IR5             |
    |  IR6            |           |  IR6             |
    |  IR7(jump lable)|           |  IR7(jump lable) |
    +-----------------+           +------------------+

 也就是IR4是一定要做block断开的，但是这样做还是不够的(左图)，因为如果IR向上跳到
 一个block块的中间，block是没法支持的，所以call+branch的IR最终导致在lable处还需要
 做block分块(右图)，其中以第一个块是fall，第二个是call，第三个是indcall。

 因为，只有出现跨call的branch时，我们才不得不在lable处分block，所以，实现上可以
 在这样的条件出现时，我们才在lable处分block(方案1)，也可以直接就在lable处分block，
 直接在lable处分block，导致我们必须直接在branch处分block(方案2)。

 示意如下::

    +------------------+           +------------------+
    |  IR0             |           |  IR0             |
    |  IR1             |           |  IR1             |
    |  IR2             |           |  IR2             |
    |  IR3             |           |  IR3             |
    |  IR4(call)       |           |  IR4(call)       |
    +------------------+           +------------------+
    |  IR5             |           |  IR5             |
    |  IR6(jump lable) |           |  IR6(jump lable) |
    |                  |           +------------------+
    |  IR7             |           |  IR7             |
    |  IR8             |           |  IR8             |
    |                  |           +------------------+
    |  lable: IR9      |           |  lable: IR9      |
    |  IR10            |           |  IR10            |
    +------------------+           +------------------+
 
 如上所示，本来可以按照左边分块，但是如果使用统一的逻辑(方案2)，就需要按照右边分块。
 右边的分块基本上是qemu的BB的定义了(除了call，qemu里call并不划分BB)。

 方案1减少了分块，只在少数情况下才进一步拆分block，似乎性能会好，方案2基本依托qemu
 的BB，似乎后面寄存器分配要好做一点。我们先选方案2。

 沿着方案2，我们再看下寄存器分配的大逻辑，T寄存器的逻辑是一样的，因为没有了块内
 跳转，local gpr可以放出来给临时变量用。

 考虑goto_tb和exit_tb的实现，这两个IR会创建BB出来，也就是有独立的block与之对应，
 对于exit_tb，它对应的block固定的跳转到epilogue块，根据exit_tb的参数刷新gpr就好，
 对于goto_tb，它对应的block首先应该是fall，然后在翻译上下文里改变block的跳转类型
 和跳转地址。

 load global寄存器时，blockISA的load的立即数位数可能不够，这里可以参考riscv中的
 做法，在翻译指令的时候调整下base和offset的数值就可以。

 todo: T寄存器分配以及插入copy的设计。
 todo: T寄存器和local_gpr分配的设计。

 为了测试，我们要交叉编译出blockISA的qemu user mode，依赖的库需要有blockISA的版本，
 或者我们看看能不能尽量不依赖相关的库。

20230321
=========

 review了下现在v0.20版本对应的qemu代码，意见如下；

 1. 类似MXL_RV32的用法不对(而且不应该还起rv这个名字)，这个是说支持的寄存器长度，
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

20230323-324-325-327-328
=========================
 
 具体看qemu user mode的编译问题，ldd看下qemu-linx依赖的库(configure --target-list=linx-linux-user)::

	linux-vdso.so.1 (0x00007ffe995cb000)                                                   少量使用，需要hack
	libgmodule-2.0.so.0 => /lib/x86_64-linux-gnu/libgmodule-2.0.so.0 (0x00007f3b9fae2000)  --disable-plugins就没有了
	libglib-2.0.so.0 => /lib/x86_64-linux-gnu/libglib-2.0.so.0 (0x00007f3b9f9a8000)        <必要>
	libm.so.6 => /lib/x86_64-linux-gnu/libm.so.6 (0x00007f3b9f8c1000)                      一些文件用，感觉可以去掉
	libgcc_s.so.1 => /lib/x86_64-linux-gnu/libgcc_s.so.1 (0x00007f3b9f8a1000)              资料较少
	libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f3b9f679000)                      <必要>
	libpcre.so.3 => /lib/x86_64-linux-gnu/libpcre.so.3 (0x00007f3b9f601000)                搜pcre*.h没有发现有人用
	/lib64/ld-linux-x86-64.so.2 (0x00007f3ba0029000)                                       静态编译不需要这个了

 因为要编译出blockISA的qemu-linx，我们看看能不能关编译选项，从而去掉一些依赖库,
 从上面看，我们必须有glib和libc，其他估计是可以hack过去的。

 进一步用libpcre和libgcc_s搜索，发现这个都和qga(qemu guest agent)有关系，qga/installer/下
 的文件会有相关名字，但是，configure 加上--disable-guest-agent，编译出的qemu-linx
 还是有这两个库，现在基本是在瞎试了，原理还没有搞明白。

 libc库，据说春华那边有基于v0.20的版本，glib还依赖libpcre/libm/libc, libpcre依赖
 libc，libm也依赖libc。

 即使全部库编译出来，还要hack代码，以及改动qemu的编译系统，看来成功编译出blockISA
 版本的qemu-linx已经比较困难了，不过也没有别的办法，只能一个一个来了。

 调查了下v0.20的linux-gnu编译器里有libc.a、libm.a、libgcc.a(libgcc.a/libgcc_s.so
 一个是静态库一个是动态库，认为libgcc也有了，我们随后使用静态链接)。

 现在就是要编译出libpcre和glib。google显示，虽然ubuntu上叫libpcre3，但是对应的
 项目的名字应该叫pcre2，我们就下载pcre2的代码编译。有一个问题，ldd只是显示第一层
 动态链接的库，那么qemu里必然直接链接了libpcre3和libgcc_s，相关的地方还没有找见?

 结论是ldd app显示的是app所有依赖的动态库，包括直接依赖和间接依赖，用
 readelf -d ./qemu-riscv看到有[NEEDED]字段标记的库是qemu-riscv直接依赖的库::

   0x0000000000000001 (NEEDED)             Shared library: [libglib-2.0.so.0]
   0x0000000000000001 (NEEDED)             Shared library: [libm.so.6]
   0x0000000000000001 (NEEDED)             Shared library: [libgcc_s.so.1]
   0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 
 那就明白了，libpcre是glib间接依赖的库，不过直接依赖libgcc的地方还没有找见?
 下一步就是只要编译出glib(依赖libpcre)，然后用glib和已经有的libm/libc/libgcc编译
 出qemu。

 我们先试试交叉编译(arm-rv)(静态)下rv的glib，这个目前已经可以搞定，相关的细节如下：todo
 那可以基于这个静态编译一个rv的qemu user出来。返现qemu的user mode也可以跑动态编译
 的程序，似乎我们也可以用交叉编译出动态库的方式做，动态库应该更简单？
 
 我们的测试环境是blockISA host的qemu-linx(test)要跑在目前的qemu-linx(machine)上，
 现在认识到machine是可有跑动态链接的程序的，也就是test可以是动态链接的，也就是说
 我们要有blockISA版本的linux-vdso，glib，libm， gcc_s，libc，ld就好(libpcre可以静态
 链接到glib的动态库？)，现在v0.20动态库支持了，貌似这条路也可以走通，依赖太多了，
 我们还是先专心搞定静态编译吧。

 我们先尝试arm上静态编译rv qemu，arm上qemu的依赖库又多了librt、libpthread和
 libstdc++，真是头大... 转到家里x86服务器上继续搞吧。按照上面已有的经验是比较容易
 搞定rv版本的glib编译的，但是glib也是用meson构建，在编译的时候会自动下载依赖的pcre
 库代码，家里的网络的proxy会有点问题，导致pcre下载超时，需要先解决这个问题，这个
 问题在自己的电脑上是没有的，估计是公司网络的问题，我们迂回一下，meson把依赖的库
 定义到了subprojects下的.wrap文件里，这文件里会定义具体下载的url，还会定义patch_url，
 这个不是真正的patch，而是对于不用meson构建的库对应的meson.build文件，如果有源码
 的patch要打会定义diff_patch的域段，手动wget --no-check-certificate下载pcre和对应
 的meson构建描述文件，把meson构建文件放入解压后的pcre更目录，直接构建就可以编译
 出libglib(glib其实是包含三个so的，为了减少依赖，修改下glib的meson.build文件，
 去掉了不必要的gio，gmodule以及一些依赖库，我们要构建静态链接的版本，在build目录下，
 meson configure -Ddefault_library=static重新配置成构建静态库)

 开始构建静态的rv qemu，还是对qemu的构建不很熟悉，重新看下qemu构建的逻辑(todo: qemu构建基本逻辑)。
 基于qemu构建的基本逻辑：1. disable掉不必要的所有feature，2. 配置编译和链接的flag。
 做了一堆hack后终于可以编译过了，然后使用qemu-riscv(host: x86, guest: rv) + qemu-riscv(
 host: rv, guest: rv)测试了下，是可以跑rv app的。

 configure现在是这个样子::

   ./configure --target-list=riscv64-linux-user --cross-prefix=riscv64-linux-gnu- --static --disable-plugins --disable-guest-agent --disable-system --disable-guest-agent-msi --disable-capstone --extra-ldflags=-L/home/sherlock/repos/linx_qemu/lib_to_build_qemu --extra-cflags='-I/home/sherlock/repos/glib/glib -I/home/sherlock/repos/glib -lglib-2.0' --disable-vhost-kernel --disable-vhost-net --disable-vhost-crypto --disable-vhost-scsi --disable-vhost-user --disable-vhost-user-fs --disable-vhost-vdpa --disable-debug-info

 其中的关键是：1. 指定cross-file，2. 配置glib库和头文件的搜索路径，3. 关掉不必要
 的配置(还包括改meson_option.txt, 改meson.build, 改configure等)，4. 编译glib的时候
 关掉了gio和gmodule，但是qemu编译的时候又会依赖gmodule的头文件，其中有一个头文件
 gmodule-visibility.h比较特殊，它是编译gmodule自动生成的，所以，又去全量编译了下
 x86下的glib，取出其中的gmodule-visibility.h，放到对应rv glib构建目录下。
 
 这样下一步，我们可以用同样的方法构建BlockISA的qemu。

 想法简单了，把交叉编译器换成最新的v0.20 blockISA gcc的版本，发现各种问题。首先
 编译blockISA版本的glib，用不支持动态链接的gcc，发现glib编译时编译依赖库(pcre)里的文件，
 编译选项里还是有-fPIC，因为编译器不支持-fPIC，这里就会报错，但是已经配置glib是
 static编译，返回去查rv版本的glib，居然也是有-fPIC这个配置，这里有点想不明白？
 (meson在build目录下会放一个叫compile-commands.json文件，会记录每个编译.o时的编译
 选项)。熠琛提示基于v0.13的gcc支持动态链接，我们先试试这个工具链，编译的过程中会出
 错，编译glib/gmem.c，出错的原因是没有检测到相关的对齐内存申请函数，相关的函数都是
 c库里的，readelf看了下blockISA的C库，相关的函数的符号都是有的，但是都显示是弱符号，
 不知道是不是这个原因？手动hack下，叫gmem.c里走memalign这个分支(rv的版本就是用的这个
 函数，但是这个hack不一定对)，同时，去看了下rv版本编译的时候，c库里对应的符号除了
 有weak版本的还有两个GLOBAL的::

   sherlock@kl-dev:/usr/riscv64-linux-gnu/lib$ riscv64-linux-gnu-readelf -s libc.a | grep memalign
     3755: 0000000000003320   400 FUNC    LOCAL  DEFAULT    1 _int_memalign
     4213: 0000000000003904   566 FUNC    LOCAL  DEFAULT    1 _mid_memalign.co[...]
    10197: 0000000000004130    50 FUNC    GLOBAL HIDDEN     1 __libc_memalign
    10198: 0000000000004130    50 FUNC    WEAK   DEFAULT    1 memalign
    10199: 0000000000004130    50 FUNC    GLOBAL DEFAULT    1 __memalign
    10226: 0000000000004c1a    94 FUNC    GLOBAL DEFAULT    1 __posix_memalign
    10227: 0000000000004c1a    94 FUNC    WEAK   DEFAULT    1 posix_memalign

 所以，感觉这块block的版本还是有点问题，这个可能要编译器的人去看看。
 如上强行hack后，编译过了，但是链接的时候直接挂了::

   [1/7] Linking target subprojects/pcre2-10.42/libpcre2-8.so.0.11.0
   FAILED: subprojects/pcre2-10.42/libpcre2-8.so.0.11.0 
   linx64-linux-gnu-gcc  -o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0 subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_auto_possess.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p
   /src_pcre2_compile.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_config.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_context.c.o subprojects/pcre2-10.42/libpcr
   e2-8.so.0.11.0.p/src_pcre2_convert.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_dfa_match.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_error.c.o subprojects/p
   cre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_extuni.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_find_bracket.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_jit_
   compile.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_maketables.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match.c.o subprojects/pcre2-10.42/libpcre2-8.so.0
   .11.0.p/src_pcre2_match_data.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_newline.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ord2utf.c.o subprojects/pcre2-1
   0.42/libpcre2-8.so.0.11.0.p/src_pcre2_pattern_info.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_script_run.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_serial
   ize.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_string_utils.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_study.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.1
   1.0.p/src_pcre2_substitute.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substring.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_tables.c.o subprojects/pcre2-10
   .42/libpcre2-8.so.0.11.0.p/src_pcre2_ucd.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_valid_utf.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_xclass.c.o subpro
   jects/pcre2-10.42/libpcre2-8.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o -Wl,--as-needed -Wl,--no-undefined -shared -fPIC -Wl,--start-group -Wl,-soname,libpcre2-8.so.0 -Wl,--end-grou
   p
   collect2: fatal error: ld terminated with signal 11 [Segmentation fault]
   compilation terminated.

 进一步下glib的编译，虽然配置的是静态链接，但是pcre库还是动态链接出来的，pcre库和
 glib库里各个文件的编译还是加了-fPIC，libglib最后静态链接使用的都是加了-fPIC的.o
 (貌似只用了pcre的头文件)。知道了这些，我们的现状是，如果用v0.20 gcc它只支持了静态
 链接，不支持-fPIC，如果继续用这样的办法，我们要想办法hack glib的配置，看看去了-fPIC
 可以不可以，如果用v0.13 gcc，它支持了-fPCI，但是ld的时候segmentation fault了，貌似
 编译器有bug。

 和刘盈盈交流了下，现在这条支持动态链接的工具链只做过简单测试，盈盈提示可以只link
 需要的，看起来libglib最后静态link的时候也没有用pcre，所以，hack了下ninja.build,
 把all target的其它都去掉，只保留libglib-2.0.a，这样的到了需要的libglib。我们先
 用支持动态库的这条工具链编译吧，因为这条才支持-fPIC的选项，glib里编译各个.o的时候
 都带了这个选项。现在libglib是编译出来了，但是不知道对不对？

 现在已经有了glib，我们来试试block qemu的编译。hack了一些内容后(hack的patch见后面
 附录：编译block qemu hack patch)，编译的时候报这个错::

   [786/946] Compiling C object libqemu-riscv64-linux-user.fa.p/linux-user_safe-syscall.S.o
   FAILED: libqemu-riscv64-linux-user.fa.p/linux-user_safe-syscall.S.o 
   linx64-linux-gnu-gcc -Ilibqemu-riscv64-linux-user.fa.p -I. -I.. -Itarget/riscv -I../target/riscv -I../linux-user/host/riscv -Ilinux-user -I../linux-user -I../linux-user/riscv -Itrace -Iqapi 
   -I/home/sherlock/repos/glib/glib -I/home/sherlock/repos/glib -fdiagnostics-color=auto -Wall -Winvalid-pch -std=gnu11 -O2 -isystem /home/sherlock/repos/linx_qemu/linux-headers -isystem linux-
   headers -iquote . -iquote /home/sherlock/repos/linx_qemu -iquote /home/sherlock/repos/linx_qemu/include -iquote /home/sherlock/repos/linx_qemu/disas/libvixl -iquote /home/sherlock/repos/linx
   _qemu/tcg/riscv -Wno-unused-function -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -Wstrict-prototypes -Wredundant-decls -Wundef -Wwrite-stri
   ngs -Wmissing-prototypes -fno-strict-aliasing -fno-common -fwrapv -Wold-style-declaration -Wold-style-definition -Wtype-limits -Wformat-security -Wformat-y2k -Winit-self -Wignored-qualifiers
    -Wempty-body -Wnested-externs -Wendif-labels -Wexpansion-to-defined -Wimplicit-fallthrough=2 -Wno-missing-include-dirs -Wno-shift-negative-value -Wno-psabi -fstack-protector-strong -Wl,--st
   art-group -lglib-2.0 -Wl,--end-group -isystem../linux-headers -isystemlinux-headers -DNEED_CPU_H '-DCONFIG_TARGET="riscv64-linux-user-config-target.h"' '-DCONFIG_DEVICES="riscv64-linux-user-
   config-devices.h"' -MD -MQ libqemu-riscv64-linux-user.fa.p/linux-user_safe-syscall.S.o -MF libqemu-riscv64-linux-user.fa.p/linux-user_safe-syscall.S.o.d -o libqemu-riscv64-linux-user.fa.p/li
   nux-user_safe-syscall.S.o -c ../linux-user/safe-syscall.S
   ../linux-user/host/riscv/safe-syscall.inc.S: Assembler messages:
   ../linux-user/host/riscv/safe-syscall.inc.S:44: Error: unrecognized opcode `mv t0,a0'
   ../linux-user/host/riscv/safe-syscall.inc.S:45: Error: unrecognized opcode `mv t1,a1'
   ../linux-user/host/riscv/safe-syscall.inc.S:46: Error: unrecognized opcode `mv a0,a2'
   ../linux-user/host/riscv/safe-syscall.inc.S:47: Error: unrecognized opcode `mv a1,a3'
   ../linux-user/host/riscv/safe-syscall.inc.S:48: Error: unrecognized opcode `mv a2,a4'
   ../linux-user/host/riscv/safe-syscall.inc.S:49: Error: unrecognized opcode `mv a3,a5'
   ../linux-user/host/riscv/safe-syscall.inc.S:50: Error: unrecognized opcode `mv a4,a6'
   ../linux-user/host/riscv/safe-syscall.inc.S:51: Error: unrecognized opcode `mv a5,a7'
   ../linux-user/host/riscv/safe-syscall.inc.S:52: Error: unrecognized opcode `mv a7,t1'
   ../linux-user/host/riscv/safe-syscall.inc.S:64: Error: illegal operands `lw t1,0(t0)'
   ../linux-user/host/riscv/safe-syscall.inc.S:65: Error: unrecognized opcode `bnez t1,0f'
   ../linux-user/host/riscv/safe-syscall.inc.S:66: Error: unrecognized opcode `scall'
   ../linux-user/host/riscv/safe-syscall.inc.S:69: Error: unrecognized opcode `ret'
   ../linux-user/host/riscv/safe-syscall.inc.S:73: Error: unrecognized opcode `li a0,-512'
   ../linux-user/host/riscv/safe-syscall.inc.S:74: Error: unrecognized opcode `ret'
   [787/946] Compiling C object libqemu-riscv64-linux-user.fa.p/target_riscv_cpu_helper.c.o

 可以看到qemu认为他的后端还是riscv，这样编译器就不认识手工写的riscv的汇编了，这
 是可以遇见到了，后端的设计这个时候需要介入了。(todo)

 我们先把编译彻底搞通，先注释掉linux-user/host/riscv/safe-syscall.inc.S里的汇编，
 如上的错误没有了。之后又显示qemu最后衔接的时候glib找不见libintl库里的符号，这个
 是因为glib里要编译libintl的动态库，glib的target只有libglib的话就没有编译libintl
 的内容了，改动build.ninja把libintl的.o也加到target里，然后把libintl.c.o加到libglib
 的链接.o里，这样再编译qemu，就找见相关的符号了。继续编译，在最后链接qemu的时候，
 出现c库和glib里符号冲突的问题，发现是libintl里的_nl_msg_cat_cntr这个全局变量和
 c库里的冲突了，删掉libintl里的全局变量，就可以编译出qemu了。不过，现在编译的时候
 hack太多，心里还是比较虚。

 qemu configure现在是这个样了::

   ./configure --target-list=riscv64-linux-user --cross-prefix=linx64-linux-gnu- --static --disable-plugins --disable-guest-agent --disable-system --disable-guest-agent-msi --disable-capstone --extra-ldflags=-L/home/sherlock/repos/linx_qemu/lib_to_build_block_qemu --extra-cflags='-I/home/sherlock/repos/glib/glib -I/home/sherlock/repos/glib -lglib-2.0 -lc -lgcc' --disable-vhost-kernel --disable-vhost-net --disable-vhost-crypto --disable-vhost-scsi --disable-vhost-user --disable-vhost-user-fs --disable-vhost-vdpa --disable-debug-info --disable-werror

 依赖的静态库被统一copy到了qemu下的lib_to_build_block_qemu目录里。

 编译glib时，对glib的hack patch见下面附录。

20230403..0406
===============

 开始按照如上的思路写代码验证，我们的基本思路是，参考riscv backend使能的patch,
 先搭出后端的框架，叫后端可以跑起来，再逐步把各种IR的支持加全。之前考虑是在v0.20
 的spec上做，现在看来由于编译器的原因(编译glib需要带-fPIC, 现在只有v0.13的编译器
 做了-fPIC的支持)，我们不去折腾去掉fPIC的编译了，就用v0.13的指令定义先做。

 具体上，第一步搭起框架，我们要搞定：1. 带linx后端的编译，2. 基本的(要用到的)指令
 和寄存器定义，3. tb头和尾的支持，4. 基本的IR的支持(exit_tb，start_tb)，5. 基本
 寄存器分配的支持。第二部，我们可以逐步把call、goto_tb以及更多的IR支持上。

 v0.13是使用的rv的ABI，所以，我们继续使用rv一样的ABI做实现。

 我们考虑业务代码需要加block head的地方::

   +------------------+
   |  IR0             |
   |  IR1             |
   |  IR2             | block 1
   |  IR3             |
   |  IR4(call)       |
   +------------------+
   |  IR5             | block 2
   |  IR6(jump lable) |
   +------------------+
   |  IR7             | block 3
   |  IR8             |
   +------------------+
   |  lable: IR9      | block 4
   |  IR10            |
   +------------------+

 也就是上面这个图标识的地方。我们需要做的是在寄存器活性分析的时候，切分block，
 并且确定block的跳转类型。上面的划分，除了call切分block，其他地方的划分和BB的划分
 是一样的，那么在blockISA上，我们可以把call IR也纳入BB划分，然后用BB划分直接对应
 block的划分。顺序遍历IR，我们可以识别block的开始，但是在一开始无法知道block head
 的跳转类型，比如block1和block2，我们只有遍历到最后才能确定跳转类型，但是，逆序
 遍历IR，我们可以在block的最后一个IR就判断出这个block的head的类型：以call和分支
 结尾的block是call或者direct link(block1和block2)，其他的都是fall类型的head，所以
 我们在逆序做寄存器活性分析的时候确定block head以及head的类型，另外，后面正序遍历
 IR已经开始做物理寄存器的分配，这个时候再确定block的划分也是不合适的。具体的，当
 我们遍历到一个block的最后一个IR是就生成一个head的标记，然后遍历到下一个block时
 把当前的head标记插入IR列表，为此需要新加IR表示这是一个blockISA的head，从语义上
 看新加这个ISA相关的IR是说不通的，目前看qemu和具体host相关的后端太薄，基本上做
 IR到host汇编的一对一翻译，但是blockISA和传统ISA又是很不一样，导致很难像传统ISA
 那样适配，对于qemu适配，这是一个可以预见的问题，我们先这样实现，过程中看看有什么
 好办法。
 
 我们再看下如上jump和call的head要具体怎么生成。qemu IR的jump会是直接跳转或者
 是条件跳转，生成对应的head就好，v0.13支持的跳转最大范围是12+4，也就是64KB，我们
 认为支持顺序排放的一个tb内的多个block足够了。
 
 call IR如果用call normal head去实现，可能会超出跳转范围，为了简单起见，我们用
 indirect call实现call。这里也和传统的ISA有很大不同，不能走qemu默认的处理。
 todo: 分析下是否可以复用?

 考虑寄存器分配的具体做法。一个IR的输入寄存器要先放到host寄存器上才能参与计算，
 具体到blockISA上，要放到块内寄存器上(T寄存器和scratch寄存器)才能参与计算，而且
 这个“放”的动作也需要微指令完成(v0.20可能在块头初始化时完成)，所以，对于IR的输入
 寄存器可以依然插入load指令来做，只不过传统ISA是load到gpr上，blockISA是load到T寄
 存器上。

 一个IR的输出寄存器只能是T寄存器，但是T寄存器只有8个，本质上看输出寄存器分配完
 的情况blockISA和传统ISA是一样的，传统ISA怎么解决，blockISA也可以用相应的办法解
 决，就是把之前使用的寄存器换到内存上，这样就有空余的寄存器出来了。

 但是，T寄存器还有起独特的地方: 1. 每个指令不管有没有输出都要分一个T寄存器，2.
 每过8个指令就可以释放之前的T寄存器(不是一般意义的寄存器dead，是超出索引范围了)，
 3. T寄存器按偏移索引(下面分析会分析到)，4. T寄存器的有效最大索引距离是8，5. T
 寄存器只能顺序的申请和释放，即使dead了，位置也不能释放出来，这个特性提醒我们T
 寄存器可能有浪费，比如一个T寄存器可能就用了一次，那么要过8个微指令才能再次用它。


 我们就先按这个思路搞。还可以把scratch寄存器拉进来分配，但是，blockISA的输出寄存
 器不能直接是scratch寄存器，具体可以这样操作，分scratch寄存器后，需要用两条微指令
 配合实现：首先load到T寄存器，然后在移动到s寄存器上。这里可以看下加直接load到local
 scratch寄存器的指令。

 scratch寄存器分配算法上也会有问题，v0.20上，scratch寄存器被整合到local gpr上，
 这样会有两个分配者，不过除了call，估计也不会用到gpr和local gpr，在call的实现上
 具体看吧。

 如上分析了T寄存器的特点，我们看下具体怎么应对。

 每个host微指令都要对应一个T寄存器。因为具体创建host微指令都使用host架构自定义的
 函数实现，我们只要针对没有T寄存器输出的微指令也强制占位下翻译上下文里的T寄存器
 就好。

 中间码中的虚拟寄存器的索引距离可能超过8，按名字索引的，距离是多少都可能。比如得
 到的中间码可能是::

   T0: const #5   
   T1: addi T0, #1
   T2: addi T0, #2
   T3: sub  T0, #3
   T4: addi T0, #1
   T5: addi T0, #2
   T6: sub  T0, #3
   T7: sub  T0, #3
   T0: sub  T0, #3  
   T1: sub  T0, #3   <---- 这条微指令已经看不到第一条微指令产生的T0。

   T0: const #5   
   T1: addi T0, #1
   T2: addi T0, #2
   T3: sub  T0, #3
   T4: addi T0, #1
   T5: addi T0, #2
   T6: sub  T0, #3
   T7: sub  T0, #3
   T0: copy from T0  <---- 为此要在T0过期前，如果T0依然没有dead，要插入一条COPY
                           指令拷贝下T0的值。
   T1: sub  T0, #3   <---- 在覆盖T寄存器时，都要检查T寄存器是否dead，如果dead，
                           直接生成新指令就好，如果T寄存器还没有dead，就要copy
                           下T寄存器的值，在生成新指令。一个微指令之前的八个T寄
                           存器总有dead的情况。
   T2: sub  T0, #3   <---- 这里就可以索引到T0。

 我们在表述下T寄存器特点2和4，对于T寄存器分配，每过8个就需要覆盖对应的T寄存器，
 如果旧T寄存器已经dead，我们可以直接用新指令的输出覆盖，但是如果对应的T寄存器还
 没有dead，就是后面还有指令要使用，就要在插入一条微指令把T寄存器值copy下。每条有
 T寄存器需要覆盖的指令都要这样处理下。我们具体考虑qemu的实现: 寄存器的分配次序应
 该定义为T0-T7，寄存器dead时不能直接释放，比如已经使用了T0-T4，到第五条微指令，也就
 产生T4的微指令发现T3 dead了，这个时候qemu不能释放T3，因为一但释放，下次分配T寄存
 器就会分配T3，和我们的逻辑不符。qemu的实现上，在要分T8时，应该刷新已经分配的T0-T7
 的标记，把他们标记为没有分配，然后开始顺序分配T0-T7，分配的时候要检查对应的寄存器
 是否dead，dead才可以分配，如果还没有dead，就插入一条copy指令传递下T寄存器。如上
 的实现都发生在qemu后端host寄存器分配时。

 qemu在做具体寄存器分配的时候，针对IR，是先分配IR的输入寄存器，在分IR的输出寄存器，
 然后在创建host指令，这里存在一个问题，如果一个IR要对应多条host指令，在这个host指令
 内部还会需要临时寄存器，qemu的实现是reserve出几个temp寄存器作为这里的使用，但是
 在blockISA上，输出都在T寄存器上，T寄存器要顺序分配，这里就会有问题，一个直白的
 解决办法是，先创建host指令，再分配host的输出寄存器，最后填回host的输出寄存器，但是
 这里的改动太大，需要深入分析下才行。不过这里只是在一个IR内部的问题，就是顺序处理
 当前IR时发生的问题，大不了host指令内部先顺序分T寄存器，随后再重新分下整个IR的输出
 寄存器，这个问题对应的指令是qemu_ld_i64，实现的时候，需要根据guest_base计算下
 host va(user mode下)，这样也比较hack，会导致IR和host有了关联。

 T寄存器是按偏移索引的，这个也是传统的按名字索引的方式不一样。我们可以按名字分配
 寄存器，生成微指令的时候再改成索引::

   T0: const #5                     T0: const #5
   T1: addi T0, #1     ---->        T1: addi offset_1, #1
   T2: addi T0, #2                  T2: addi offset_2, #2
   T3: sub  T0, #3                  T3: sub  offset_3, #3

 这就需要生成每个block的微指令时，维护一个当前微指令是block中第几条指令的标记。
 注意，按照目前的做法，block内部没有块内跳转，所以我们可以这样做。如果后续支持
 生成block内的块内跳转，想不出这里要怎么搞??

20230410
=========
 
 先不考虑复杂情况，我们先实现一个有tb头有tb尾，可以分配寄存器可以跑起来的qemu，
 先不管call，goto_tb，br/brcong和set_label等等。这样tb的业务代码我们可以先不考虑
 block划分的实现，我们先专心搞寄存器分配。

 fixed实现的cpu_env的指针，在blockISA上是不能继续放到gpr上的，因为blockISA要在
 block里完成计算，所以我们可以直接把cpu_env绑定到block内的一个scratch寄存器上，
 就scratch0上吧，因为块内寄存器在开启每个块时要初始化，scratch0每次都要初始化下，
 如果把cpu_env绑定到gpr上，因为gpr直接参与不了运算，还是要转存下，而且qemu是加载
 寄存器的时候是直接用fixed寄存器作为base找被加载寄存器对应的内存地址的，转一下
 qemu的实现也会有问题。

 如果fixed按如上去实现，每次生成一个block时，就要把fixed寄存器的值传入，这样又要
 占用一个gpr，而且要在一开始插入一条get gpr的指令，原来的fixed实现语义硬生生的被
 搞成了非fixed，真是悲催。如果存在bloc ,k内可以直接索引的全局寄存器，这个问题就可以
 解决，但是这又和使用block传递信息的设计相违背。(这个问题展开讲下)

 翻译最开始有一个gen_tb_start这个还没有搞，qemu在开始翻译业务代码的时候，在gen_tb_start
 里插入了几条中间码，这个并不破坏我们之前的分析，无非是之前认为没有call，goto_tb，
 br/brcong和set_label，所有业务代码可以放在一个tb里，现在加了gen_tb_start里的中间
 码，因为里面会有brcond，我们最简的实现也要至少分两个block了。不过我们最简的实现
 也可以先把gen_tb_start这个函数以及对应set_label的代码删去，先聚焦寄存器分配吧。

20230512
=========
 
最近v0.20上bget/bset mask的处理问题比较多，这里重新梳理下这种概念，基本概念来自
文杰的devlog。

文杰的blog中定义了需要做mask检查的情况，我们先copy如下::

 1. 获取未初始化的 lgpr
 2. 重复 set ggpr
 3. set 的 ggpr 不在 smask 中
 4. gmask 中标注的 lgpr 在块体中未使用

我们把如上的硬件或者编译器给我们输入做下"需求分析"，需要注意的点如下::
 
 1. 第一条初看起来，就是从非bget的lgpr上读信息，但是，我们把set local后的lgpr也
    认为是初始化过的，其实这个是scrath寄存器的典型使用场景。其他的都是明确的。
 2. 和若愚对其的情况是：23是硬件定义特性，14是调试阶段帮助debug编译器错误的调试
    特性。所以，23必须要实现的，理论上14无须实现，但是为了前期debug qemu和编译器，
    我们保留14，但是实现上可以用参数将14隔离开。

基于如上的认识，我们引入新的寄存器或者是变量做实现:

为了实现2.3, 我们引入一个cpu架构状态，用current_set_mask寄存器表示，这个寄存器的
语义是block在当前时刻需要做的set到ggpr的寄存器的集合。根据current_set_mask寄存器
的语义，current_set_mask应该被初始化为smask, 然后在每次执行setg时，去掉current_set_mask
的对应bit，这表示已经输出对应的寄存器到ggpr。这个过程中，在"去掉current_set_mask
的对应bit"时，有可能发现将要去掉的bit已经是0，这显然是情况2和3的一种，硬件需要报
异常。block提交阶段把current_set_mask剩余的bit对应的lgpr提交到gpr。

为了实现4，我们需要增加一个辅助的状体current_get_mask, 这个变量表示block当前时
刻需要get的外部寄存器的集合。根据current_get_mask的语义，current_get_mask被初始化
为gmask。每个get微指令执行时去掉current_get_mask上的对应bit，表示用掉了对应的输入
寄存器，这样block提交的时候，如果current_get_mask上还有剩余bit，说明4描述的情况
存在，qemu可以输出debug提示信息。

为了实现1，需要有一个辅助变量记录已经初始化的lgpr的集合，这样每次使用lgpr的时候，
比对下这个集合，就知道有没有出现1描述的场景。已经初始化的lgpr集合包括gmask对应的
lgpr，head执行时自动初始化，还有对lgpr有更新的微指令初始化的lgpr，所以我们可以扩
展下gmask的语义，用gmask表示block执行过程中已经初始化的lgpr的集合，对lgpr有更新
的微指令(目前看只有set local)执行时同步更新下gmask就可以维持gmask的语义(当然也可
以用一个新的变量表示)。

实际实现上，我们之前已经识别到，检查mask是一个比较耗时的操作，由于切片生成需要考虑
效率，所以，我们可以还是沿用之前的解决办法，使用变量控制是否做全量的mask检查。在
没有功能问题的前提下，为了效率我们可以全部不做mask检查。

todo: 我们需要把块内跳转和mask的逻辑整合到一起看下。

分析下ACR和v0.20 qemu分支合并的基本逻辑：

ACR主要在改特权级相关的东西，v0.20在改head编码和微指令的编码，其实相对来说是比较
正交的。v0.20 head编码里引入了trap域段，这个和ACR里的scall/acri指令的逻辑需要整合
下，qemu才能根据最终的逻辑做实现。所以，总体看qemu持续推进就好。


qemu发现的构架问题
===================

 1. v0.20的改动里加了直接set global寄存器的微指令，原子块因为整体不成功时需要回滚
    整个块，所以原子块里不能有对全局状态立即生效的指令，spec里应该加上这个限制，
    实现上，编译应该报错，硬件检测到这样的情况需要报异常。

附录
======

编译block qemu hack patch::

  diff --git a/configure b/configure
  index c80c1dab6d..0a1f7f104a 100755
  --- a/configure
  +++ b/configure
  @@ -1944,9 +1944,9 @@ fi
   ##########################################
   # pkg-config probe
   
  -if ! has "$pkg_config_exe"; then
  -  error_exit "pkg-config binary '$pkg_config_exe' not found"
  -fi
  +# if ! has "$pkg_config_exe"; then
  +#   error_exit "pkg-config binary '$pkg_config_exe' not found"
  +# fi
   
   ##########################################
   # xen probe
  @@ -2448,8 +2448,8 @@ for i in $glib_modules; do
       if $pkg_config --atleast-version=$glib_req_ver $i; then
           glib_cflags=$($pkg_config --cflags $i)
           glib_libs=$($pkg_config --libs $i)
  -    else
  -        error_exit "glib-$glib_req_ver $i is required to compile QEMU"
  +#    else
  +#        error_exit "glib-$glib_req_ver $i is required to compile QEMU"
       fi
   done
   
  diff --git a/linux-user/host/riscv/safe-syscall.inc.S b/linux-user/host/riscv/safe-syscall.inc.S
  index 9ca3fbfd1e..df7bc28c3f 100644
  --- a/linux-user/host/riscv/safe-syscall.inc.S
  +++ b/linux-user/host/riscv/safe-syscall.inc.S
  @@ -41,15 +41,15 @@ safe_syscall_base:
   	 *               and returns the result in a0
   	 * Shuffle everything around appropriately.
   	 */
  -	mv	t0, a0		/* signal_pending pointer */
  -	mv	t1, a1		/* syscall number */
  -	mv	a0, a2		/* syscall arguments */
  -	mv	a1, a3
  -	mv	a2, a4
  -	mv	a3, a5
  -	mv	a4, a6
  -	mv	a5, a7
  -	mv	a7, t1
  +//	mv	t0, a0		/* signal_pending pointer */
  +//	mv	t1, a1		/* syscall number */
  +//	mv	a0, a2		/* syscall arguments */
  +//	mv	a1, a3
  +//	mv	a2, a4
  +//	mv	a3, a5
  +//	mv	a4, a6
  +//	mv	a5, a7
  +//	mv	a7, t1
   
   	/*
   	 * This next sequence of code works in conjunction with the
  @@ -61,17 +61,17 @@ safe_syscall_base:
   	 */
   safe_syscall_start:
   	/* If signal_pending is non-zero, don't do the call */
  -	lw	t1, 0(t0)
  -	bnez	t1, 0f
  -	scall
  +//	lw	t1, 0(t0)
  +//	bnez	t1, 0f
  +//	scall
   safe_syscall_end:
   	/* code path for having successfully executed the syscall */
  -	ret
  +//	ret
   
   0:
   	/* code path when we didn't execute the syscall */
  -	li	a0, -TARGET_ERESTARTSYS
  -	ret
  +//	li	a0, -TARGET_ERESTARTSYS
  +//	ret
   	.cfi_endproc
   
   	.size	safe_syscall_base, .-safe_syscall_base
  diff --git a/meson.build b/meson.build
  index fe603cbe18..ae5578b185 100644
  --- a/meson.build
  +++ b/meson.build
  @@ -415,7 +415,7 @@ if have_system or have_tools
     pixman = dependency('pixman-1', required: have_system, version:'>=0.21.8',
                         method: 'pkg-config', kwargs: static_kwargs)
   endif
  -zlib = dependency('zlib', required: true, kwargs: static_kwargs)
  +# zlib = dependency('zlib', required: true, kwargs: static_kwargs)
   
   libaio = not_found
   if not get_option('linux_aio').auto() or have_block
  @@ -2515,7 +2515,7 @@ subdir('util')
   subdir('qom')
   subdir('authz')
   subdir('crypto')
  -subdir('ui')
  +# subdir('ui')
   
   
   if enable_modules
  @@ -2767,12 +2767,12 @@ libio = static_library('io', io_ss.sources() + genh,
   
   io = declare_dependency(link_whole: libio, dependencies: [crypto, qom])
   
  -libmigration = static_library('migration', sources: migration_files + genh,
  -                              name_suffix: 'fa',
  -                              build_by_default: false)
  -migration = declare_dependency(link_with: libmigration,
  -                               dependencies: [zlib, qom, io])
  -softmmu_ss.add(migration)
  +#libmigration = static_library('migration', sources: migration_files + genh,
  +#                              name_suffix: 'fa',
  +#                              build_by_default: false)
  +#migration = declare_dependency(link_with: libmigration,
  +#                               dependencies: [zlib, qom, io])
  +#softmmu_ss.add(migration)
   
   block_ss = block_ss.apply(config_host, strict: false)
   libblock = static_library('block', block_ss.sources() + genh,
  @@ -3107,9 +3107,9 @@ endif
   
   subdir('scripts')
   subdir('tools')
  -subdir('pc-bios')
  +#subdir('pc-bios')
   subdir('docs')
  -subdir('tests')
  +#subdir('tests')
   if gtk.found()
     subdir('po')
   endif
  diff --git a/target/arm/meson.build b/target/arm/meson.build
  index 50f152214a..4707f2fe10 100644
  --- a/target/arm/meson.build
  +++ b/target/arm/meson.build
  @@ -36,7 +36,7 @@ arm_ss.add(files(
     'vfp_helper.c',
     'cpu_tcg.c',
   ))
  -arm_ss.add(zlib)
  +# arm_ss.add(zlib)
   
   arm_ss.add(when: 'CONFIG_KVM', if_true: files('kvm.c', 'kvm64.c'), if_false: files('kvm-stub.c'))
   
  diff --git a/target/tricore/meson.build b/target/tricore/meson.build
  index 0ccc829517..07df3b6b7c 100644
  --- a/target/tricore/meson.build
  +++ b/target/tricore/meson.build
  @@ -7,7 +7,7 @@ tricore_ss.add(files(
     'translate.c',
     'gdbstub.c',
   ))
  -tricore_ss.add(zlib)
  +#tricore_ss.add(zlib)
   
   tricore_softmmu_ss = ss.source_set()
   
  -- 
  2.34.1

glib hack patch::

  Subject: [PATCH] current we can build qemu by this glib
  MIME-Version: 1.0
  Content-Type: text/plain; charset=UTF-8
  Content-Transfer-Encoding: 8bit
  
  as we modify build.ninya, we save a copy for it. And pay attention: we
  also remove _nl_msg_cat_cntr in libintl.c to pass qemu compile as it
  has conflict with glibc, which version is v0.13 blockISA gcc(support
  dynamical link)。
  
  Signed-off-by: Zhou Wang <wangzhou1@hisilicon.com>
  ---
   build.ninja_backup      | 1556 +++++++++++++++++++++++++++++++++++++++
   glib/gmem.c             |   11 +-
   glib/gnulib/meson.build |    8 +-
   glib/meson.build        |    2 +-
   linx0.20_cross_file     |   17 +
   meson.build             |   82 +--
   6 files changed, 1625 insertions(+), 51 deletions(-)
   create mode 100644 build.ninja_backup
   create mode 100644 linx0.20_cross_file
  
  diff --git a/build.ninja_backup b/build.ninja_backup
  new file mode 100644
  index 000000000..2cba2235b
  --- /dev/null
  +++ b/build.ninja_backup
  @@ -0,0 +1,1556 @@
  +# This is the build file for project "glib"
  +# It is autogenerated by the Meson build system.
  +# Do not edit by hand.
  +
  +ninja_required_version = 1.8.2
  +
  +# Rules for module scanning.
  +
  +# Rules for compiling.
  +
  +rule c_COMPILER
  + command = linx64-linux-gnu-gcc $ARGS -MD -MQ $out -MF $DEPFILE -o $out -c $in
  + deps = gcc
  + depfile = $DEPFILE_UNQUOTED
  + description = Compiling C object $out
  +
  +# Rules for linking.
  +
  +rule STATIC_LINKER
  + command = rm -f $out && linx64-linux-gnu-ar $LINK_ARGS $out $in
  + description = Linking static target $out
  +
  +rule c_LINKER
  + command = linx64-linux-gnu-gcc $ARGS -o $out $in $LINK_ARGS
  + description = Linking target $out
  +
  +rule SHSYM
  + command = /usr/bin/meson --internal symbolextractor /home/sherlock/repos/glib/linx0.20_build $in $IMPLIB $out $CROSS
  + description = Generating symbol file $out
  + restat = 1
  +
  +# Other rules
  +
  +rule CUSTOM_COMMAND
  + command = $COMMAND
  + description = $DESC
  + restat = 1
  +
  +rule REGENERATE_BUILD
  + command = /usr/bin/meson --internal regenerate /home/sherlock/repos/glib /home/sherlock/repos/glib/linx0.20_build --backend ninja
  + description = Regenerating build files.
  + generator = 1
  +
  +# Phony build target, always out of date
  +
  +build PHONY: phony 
  +
  +# Build rules for targets
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_auto_possess.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_auto_possess.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_auto_possess.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_auto_possess.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_compile.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_compile.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_compile.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_compile.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_config.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_config.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_config.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_config.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_context.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_context.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_context.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_context.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_convert.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_convert.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_convert.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_convert.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_dfa_match.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_dfa_match.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_dfa_match.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_dfa_match.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_error.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_error.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_error.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_error.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_extuni.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_extuni.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_extuni.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_extuni.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_find_bracket.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_find_bracket.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_find_bracket.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_find_bracket.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_jit_compile.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_jit_compile.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_jit_compile.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_jit_compile.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_maketables.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_maketables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_maketables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_maketables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_match.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match_data.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_match_data.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match_data.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match_data.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_newline.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_newline.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_newline.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_newline.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ord2utf.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_ord2utf.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ord2utf.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ord2utf.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_pattern_info.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_pattern_info.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_pattern_info.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_pattern_info.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_script_run.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_script_run.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_script_run.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_script_run.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_serialize.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_serialize.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_serialize.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_serialize.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_string_utils.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_string_utils.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_string_utils.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_string_utils.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_study.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_study.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_study.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_study.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substitute.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_substitute.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substitute.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substitute.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substring.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_substring.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substring.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substring.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_tables.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_tables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_tables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_tables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ucd.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_ucd.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ucd.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ucd.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_valid_utf.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_valid_utf.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_valid_utf.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_valid_utf.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_xclass.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_xclass.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_xclass.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_xclass.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o: c_COMPILER subprojects/pcre2-10.42/pcre2_chartables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/libpcre2-8.so.0.11.0.symbols: SHSYM subprojects/pcre2-10.42/libpcre2-8.so.0.11.0
  + IMPLIB = subprojects/pcre2-10.42/libpcre2-8.so.0.11.0
  + CROSS = --cross-host=linux
  +
  +# build subprojects/pcre2-10.42/libpcre2-8.so.0.11.0: c_LINKER subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_auto_possess.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_compile.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_config.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_context.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_convert.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_dfa_match.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_error.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_extuni.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_find_bracket.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_jit_compile.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_maketables.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_match_data.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_newline.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ord2utf.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_pattern_info.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_script_run.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_serialize.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_string_utils.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_study.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substitute.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_substring.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_tables.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_ucd.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_valid_utf.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/src_pcre2_xclass.c.o subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o
  +# LINK_ARGS = -Wl,--as-needed -Wl,--no-undefined -shared -fPIC -Wl,--start-group -Wl,-soname,libpcre2-8.so.0 -Wl,--end-group
  +
  +build subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2.p/src_pcre2posix.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2posix.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2.p/src_pcre2posix.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2.p/src_pcre2posix.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-posix.so.3.0.2.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8
  +
  +build subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2.p/libpcre2-posix.so.3.0.2.symbols: SHSYM subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2
  + IMPLIB = subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2
  + CROSS = --cross-host=linux
  +
  +# build subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2: c_LINKER subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2.p/src_pcre2posix.c.o | subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/libpcre2-8.so.0.11.0.symbols
  +# LINK_ARGS = -Wl,--as-needed -Wl,--no-undefined -shared -fPIC -Wl,--start-group -Wl,-soname,libpcre2-posix.so.3 '-Wl,-rpath,$$ORIGIN/' -Wl,-rpath-link,/home/sherlock/repos/glib/linx0.20_build/subprojects/pcre2-10.42 subprojects/pcre2-10.42/libpcre2-8.so.0.11.0 -Wl,--end-group
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_auto_possess.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_auto_possess.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_auto_possess.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_auto_possess.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_compile.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_compile.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_compile.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_compile.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_config.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_config.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_config.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_config.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_context.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_context.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_context.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_context.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_convert.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_convert.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_convert.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_convert.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_dfa_match.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_dfa_match.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_dfa_match.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_dfa_match.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_error.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_error.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_error.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_error.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_extuni.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_extuni.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_extuni.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_extuni.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_find_bracket.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_find_bracket.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_find_bracket.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_find_bracket.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_jit_compile.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_jit_compile.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_jit_compile.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_jit_compile.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_maketables.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_maketables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_maketables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_maketables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_match.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_match.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_match.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_match.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_match_data.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_match_data.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_match_data.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_match_data.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_newline.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_newline.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_newline.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_newline.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_ord2utf.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_ord2utf.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_ord2utf.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_ord2utf.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_pattern_info.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_pattern_info.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_pattern_info.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_pattern_info.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_script_run.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_script_run.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_script_run.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_script_run.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_serialize.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_serialize.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_serialize.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_serialize.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_string_utils.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_string_utils.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_string_utils.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_string_utils.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_study.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_study.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_study.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_study.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_substitute.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_substitute.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_substitute.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_substitute.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_substring.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_substring.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_substring.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_substring.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_tables.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_tables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_tables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_tables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_ucd.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_ucd.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_ucd.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_ucd.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_valid_utf.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_valid_utf.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_valid_utf.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_valid_utf.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_xclass.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_xclass.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_xclass.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_xclass.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o: c_COMPILER subprojects/pcre2-10.42/pcre2_chartables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=16
  +
  +build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/libpcre2-16.so.0.11.0.symbols: SHSYM subprojects/pcre2-10.42/libpcre2-16.so.0.11.0
  + IMPLIB = subprojects/pcre2-10.42/libpcre2-16.so.0.11.0
  + CROSS = --cross-host=linux
  +
  +# build subprojects/pcre2-10.42/libpcre2-16.so.0.11.0: c_LINKER subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_auto_possess.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_compile.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_config.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_context.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_convert.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_dfa_match.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_error.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_extuni.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_find_bracket.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_jit_compile.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_maketables.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_match.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_match_data.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_newline.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_ord2utf.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_pattern_info.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_script_run.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_serialize.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_string_utils.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_study.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_substitute.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_substring.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_tables.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_ucd.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_valid_utf.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/src_pcre2_xclass.c.o subprojects/pcre2-10.42/libpcre2-16.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o
  + # LINK_ARGS = -Wl,--as-needed -Wl,--no-undefined -shared -fPIC -Wl,--start-group -Wl,-soname,libpcre2-16.so.0 -Wl,--end-group
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_auto_possess.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_auto_possess.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_auto_possess.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_auto_possess.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_compile.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_compile.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_compile.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_compile.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_config.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_config.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_config.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_config.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_context.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_context.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_context.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_context.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_convert.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_convert.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_convert.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_convert.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_dfa_match.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_dfa_match.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_dfa_match.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_dfa_match.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_error.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_error.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_error.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_error.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_extuni.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_extuni.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_extuni.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_extuni.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_find_bracket.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_find_bracket.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_find_bracket.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_find_bracket.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_jit_compile.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_jit_compile.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_jit_compile.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_jit_compile.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_maketables.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_maketables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_maketables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_maketables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_match.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_match.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_match.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_match.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_match_data.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_match_data.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_match_data.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_match_data.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_newline.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_newline.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_newline.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_newline.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_ord2utf.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_ord2utf.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_ord2utf.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_ord2utf.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_pattern_info.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_pattern_info.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_pattern_info.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_pattern_info.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_script_run.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_script_run.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_script_run.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_script_run.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_serialize.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_serialize.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_serialize.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_serialize.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_string_utils.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_string_utils.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_string_utils.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_string_utils.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_study.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_study.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_study.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_study.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_substitute.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_substitute.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_substitute.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_substitute.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_substring.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_substring.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_substring.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_substring.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_tables.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_tables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_tables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_tables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_ucd.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_ucd.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_ucd.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_ucd.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_valid_utf.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_valid_utf.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_valid_utf.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_valid_utf.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_xclass.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2_xclass.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_xclass.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_xclass.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o: c_COMPILER subprojects/pcre2-10.42/pcre2_chartables.c
  + DEPFILE = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -fPIC -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=32
  +
  +build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/libpcre2-32.so.0.11.0.symbols: SHSYM subprojects/pcre2-10.42/libpcre2-32.so.0.11.0
  + IMPLIB = subprojects/pcre2-10.42/libpcre2-32.so.0.11.0
  + CROSS = --cross-host=linux
  +
  +# build subprojects/pcre2-10.42/libpcre2-32.so.0.11.0: c_LINKER subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_auto_possess.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_compile.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_config.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_context.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_convert.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_dfa_match.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_error.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_extuni.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_find_bracket.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_jit_compile.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_maketables.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_match.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_match_data.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_newline.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_ord2utf.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_pattern_info.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_script_run.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_serialize.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_string_utils.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_study.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_substitute.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_substring.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_tables.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_ucd.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_valid_utf.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/src_pcre2_xclass.c.o subprojects/pcre2-10.42/libpcre2-32.so.0.11.0.p/meson-generated_.._pcre2_chartables.c.o
  +# LINK_ARGS = -Wl,--as-needed -Wl,--no-undefined -shared -fPIC -Wl,--start-group -Wl,-soname,libpcre2-32.so.0 -Wl,--end-group
  +
  +build subprojects/pcre2-10.42/pcre2grep.p/src_pcre2grep.c.o: c_COMPILER ../subprojects/pcre2-10.42/src/pcre2grep.c
  + DEPFILE = subprojects/pcre2-10.42/pcre2grep.p/src_pcre2grep.c.o.d
  + DEPFILE_UNQUOTED = subprojects/pcre2-10.42/pcre2grep.p/src_pcre2grep.c.o.d
  + ARGS = -Isubprojects/pcre2-10.42/pcre2grep.p -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -DHAVE_SYS_STAT_H -DHAVE_SYS_TYPES_H -DHAVE_SYS_WAIT_H -DHAVE_INTTYPES_H -DHAVE_DIRENT_H -DHAVE_DLFCN_H -DHAVE_LIMITS_H -DHAVE_STDINT_H -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_UNISTD_H -DSTDC_HEADERS -DSUPPORT_PCRE2_8 -DSUPPORT_UNICODE -DHAVE_CONFIG_H
  +
  +build subprojects/pcre2-10.42/pcre2grep: c_LINKER subprojects/pcre2-10.42/pcre2grep.p/src_pcre2grep.c.o | subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/libpcre2-8.so.0.11.0.symbols
  + LINK_ARGS = -Wl,--as-needed -Wl,--no-undefined '-Wl,-rpath,$$ORIGIN/' -Wl,-rpath-link,/home/sherlock/repos/glib/linx0.20_build/subprojects/pcre2-10.42 -Wl,--start-group subprojects/pcre2-10.42/libpcre2-8.so.0.11.0 -Wl,--end-group
  +
  +build subprojects/proxy-libintl/libintl.so.8.p/libintl.c.o: c_COMPILER ../subprojects/proxy-libintl/libintl.c
  + DEPFILE = subprojects/proxy-libintl/libintl.so.8.p/libintl.c.o.d
  + DEPFILE_UNQUOTED = subprojects/proxy-libintl/libintl.so.8.p/libintl.c.o.d
  + ARGS = -Isubprojects/proxy-libintl/libintl.so.8.p -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -std=gnu99 -O2 -g -fPIC -DSTUB_ONLY
  +
  +build subprojects/proxy-libintl/libintl.so.8.p/libintl.so.8.symbols: SHSYM subprojects/proxy-libintl/libintl.so.8
  + IMPLIB = subprojects/proxy-libintl/libintl.so.8
  + CROSS = --cross-host=linux
  +
  +# build subprojects/proxy-libintl/libintl.so.8: c_LINKER subprojects/proxy-libintl/libintl.so.8.p/libintl.c.o
  +# LINK_ARGS = -Wl,--as-needed -Wl,--no-undefined -shared -fPIC -Wl,--start-group -Wl,-soname,libintl.so.8 -Wl,--end-group
  +
  +build glib/gversionmacros.h: CUSTOM_COMMAND ../glib/gversionmacros.h.in | /home/sherlock/repos/glib/tools/gen-visibility-macros.py
  + COMMAND = /home/sherlock/repos/glib/tools/gen-visibility-macros.py 2.76.1 versions-macros ../glib/gversionmacros.h.in glib/gversionmacros.h
  + description = Generating$ glib/gversionmacros.h$ with$ a$ custom$ command
  +
  +build glib/glib-visibility.h: CUSTOM_COMMAND  | /home/sherlock/repos/glib/tools/gen-visibility-macros.py
  + COMMAND = /home/sherlock/repos/glib/tools/gen-visibility-macros.py 2.76.1 visibility-macros GLIB glib/glib-visibility.h
  + description = Generating$ glib/glib-visibility.h$ with$ a$ custom$ command
  +
  +build glib/libcharset/libcharset.a.p/localcharset.c.o: c_COMPILER ../glib/libcharset/localcharset.c
  + DEPFILE = glib/libcharset/libcharset.a.p/localcharset.c.o.d
  + DEPFILE_UNQUOTED = glib/libcharset/libcharset.a.p/localcharset.c.o.d
  + ARGS = -Iglib/libcharset/libcharset.a.p -Iglib/libcharset -I../glib/libcharset -I. -I.. -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC '-DGLIB_CHARSETALIAS_DIR="/usr/local/lib"'
  +
  +build glib/libcharset/libcharset.a: STATIC_LINKER glib/libcharset/libcharset.a.p/localcharset.c.o
  + LINK_ARGS = csrDT
  +
  +build glib/gnulib/libgnulib.a.p/asnprintf.c.o: c_COMPILER ../glib/gnulib/asnprintf.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gnulib/libgnulib.a.p/asnprintf.c.o.d
  + DEPFILE_UNQUOTED = glib/gnulib/libgnulib.a.p/asnprintf.c.o.d
  + ARGS = -Iglib/gnulib/libgnulib.a.p -Iglib/gnulib -I../glib/gnulib -I. -I.. -Iglib -I../glib -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -DGCC_LINT=1 '-DLIBDIR="/usr/local/lib"' '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION -Wno-format-nonliteral -Wno-duplicated-branches -DHAVE_ISNAN_IN_LIBC -DHAVE_ISNAND_IN_LIBC -DHAVE_ISNANF_IN_LIBC -DHAVE_ISNANL_IN_LIBC
  +
  +build glib/gnulib/libgnulib.a.p/printf.c.o: c_COMPILER ../glib/gnulib/printf.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gnulib/libgnulib.a.p/printf.c.o.d
  + DEPFILE_UNQUOTED = glib/gnulib/libgnulib.a.p/printf.c.o.d
  + ARGS = -Iglib/gnulib/libgnulib.a.p -Iglib/gnulib -I../glib/gnulib -I. -I.. -Iglib -I../glib -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -DGCC_LINT=1 '-DLIBDIR="/usr/local/lib"' '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION -Wno-format-nonliteral -Wno-duplicated-branches -DHAVE_ISNAN_IN_LIBC -DHAVE_ISNAND_IN_LIBC -DHAVE_ISNANF_IN_LIBC -DHAVE_ISNANL_IN_LIBC
  +
  +build glib/gnulib/libgnulib.a.p/printf-args.c.o: c_COMPILER ../glib/gnulib/printf-args.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gnulib/libgnulib.a.p/printf-args.c.o.d
  + DEPFILE_UNQUOTED = glib/gnulib/libgnulib.a.p/printf-args.c.o.d
  + ARGS = -Iglib/gnulib/libgnulib.a.p -Iglib/gnulib -I../glib/gnulib -I. -I.. -Iglib -I../glib -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -DGCC_LINT=1 '-DLIBDIR="/usr/local/lib"' '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION -Wno-format-nonliteral -Wno-duplicated-branches -DHAVE_ISNAN_IN_LIBC -DHAVE_ISNAND_IN_LIBC -DHAVE_ISNANF_IN_LIBC -DHAVE_ISNANL_IN_LIBC
  +
  +build glib/gnulib/libgnulib.a.p/printf-parse.c.o: c_COMPILER ../glib/gnulib/printf-parse.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gnulib/libgnulib.a.p/printf-parse.c.o.d
  + DEPFILE_UNQUOTED = glib/gnulib/libgnulib.a.p/printf-parse.c.o.d
  + ARGS = -Iglib/gnulib/libgnulib.a.p -Iglib/gnulib -I../glib/gnulib -I. -I.. -Iglib -I../glib -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -DGCC_LINT=1 '-DLIBDIR="/usr/local/lib"' '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION -Wno-format-nonliteral -Wno-duplicated-branches -DHAVE_ISNAN_IN_LIBC -DHAVE_ISNAND_IN_LIBC -DHAVE_ISNANF_IN_LIBC -DHAVE_ISNANL_IN_LIBC
  +
  +build glib/gnulib/libgnulib.a.p/printf-frexp.c.o: c_COMPILER ../glib/gnulib/printf-frexp.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gnulib/libgnulib.a.p/printf-frexp.c.o.d
  + DEPFILE_UNQUOTED = glib/gnulib/libgnulib.a.p/printf-frexp.c.o.d
  + ARGS = -Iglib/gnulib/libgnulib.a.p -Iglib/gnulib -I../glib/gnulib -I. -I.. -Iglib -I../glib -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -DGCC_LINT=1 '-DLIBDIR="/usr/local/lib"' '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION -Wno-format-nonliteral -Wno-duplicated-branches -DHAVE_ISNAN_IN_LIBC -DHAVE_ISNAND_IN_LIBC -DHAVE_ISNANF_IN_LIBC -DHAVE_ISNANL_IN_LIBC
  +
  +build glib/gnulib/libgnulib.a.p/printf-frexpl.c.o: c_COMPILER ../glib/gnulib/printf-frexpl.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gnulib/libgnulib.a.p/printf-frexpl.c.o.d
  + DEPFILE_UNQUOTED = glib/gnulib/libgnulib.a.p/printf-frexpl.c.o.d
  + ARGS = -Iglib/gnulib/libgnulib.a.p -Iglib/gnulib -I../glib/gnulib -I. -I.. -Iglib -I../glib -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -DGCC_LINT=1 '-DLIBDIR="/usr/local/lib"' '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION -Wno-format-nonliteral -Wno-duplicated-branches -DHAVE_ISNAN_IN_LIBC -DHAVE_ISNAND_IN_LIBC -DHAVE_ISNANF_IN_LIBC -DHAVE_ISNANL_IN_LIBC
  +
  +build glib/gnulib/libgnulib.a.p/vasnprintf.c.o: c_COMPILER ../glib/gnulib/vasnprintf.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gnulib/libgnulib.a.p/vasnprintf.c.o.d
  + DEPFILE_UNQUOTED = glib/gnulib/libgnulib.a.p/vasnprintf.c.o.d
  + ARGS = -Iglib/gnulib/libgnulib.a.p -Iglib/gnulib -I../glib/gnulib -I. -I.. -Iglib -I../glib -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -DGCC_LINT=1 '-DLIBDIR="/usr/local/lib"' '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION -Wno-format-nonliteral -Wno-duplicated-branches -DHAVE_ISNAN_IN_LIBC -DHAVE_ISNAND_IN_LIBC -DHAVE_ISNANF_IN_LIBC -DHAVE_ISNANL_IN_LIBC
  +
  +build glib/gnulib/libgnulib.a.p/xsize.c.o: c_COMPILER ../glib/gnulib/xsize.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gnulib/libgnulib.a.p/xsize.c.o.d
  + DEPFILE_UNQUOTED = glib/gnulib/libgnulib.a.p/xsize.c.o.d
  + ARGS = -Iglib/gnulib/libgnulib.a.p -Iglib/gnulib -I../glib/gnulib -I. -I.. -Iglib -I../glib -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -DGCC_LINT=1 '-DLIBDIR="/usr/local/lib"' '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION -Wno-format-nonliteral -Wno-duplicated-branches -DHAVE_ISNAN_IN_LIBC -DHAVE_ISNAND_IN_LIBC -DHAVE_ISNANF_IN_LIBC -DHAVE_ISNANL_IN_LIBC
  +
  +build glib/gnulib/libgnulib.a: STATIC_LINKER glib/gnulib/libgnulib.a.p/asnprintf.c.o glib/gnulib/libgnulib.a.p/printf.c.o glib/gnulib/libgnulib.a.p/printf-args.c.o glib/gnulib/libgnulib.a.p/printf-parse.c.o glib/gnulib/libgnulib.a.p/printf-frexp.c.o glib/gnulib/libgnulib.a.p/printf-frexpl.c.o glib/gnulib/libgnulib.a.p/vasnprintf.c.o glib/gnulib/libgnulib.a.p/xsize.c.o
  + LINK_ARGS = csrDT
  +
  +build glib/libglib-2.0.a.p/deprecated_gallocator.c.o: c_COMPILER ../glib/deprecated/gallocator.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/deprecated_gallocator.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/deprecated_gallocator.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/deprecated_gcache.c.o: c_COMPILER ../glib/deprecated/gcache.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/deprecated_gcache.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/deprecated_gcache.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/deprecated_gcompletion.c.o: c_COMPILER ../glib/deprecated/gcompletion.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/deprecated_gcompletion.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/deprecated_gcompletion.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/deprecated_grel.c.o: c_COMPILER ../glib/deprecated/grel.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/deprecated_grel.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/deprecated_grel.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/deprecated_gthread-deprecated.c.o: c_COMPILER ../glib/deprecated/gthread-deprecated.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/deprecated_gthread-deprecated.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/deprecated_gthread-deprecated.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/garcbox.c.o: c_COMPILER ../glib/garcbox.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/garcbox.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/garcbox.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/garray.c.o: c_COMPILER ../glib/garray.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/garray.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/garray.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gasyncqueue.c.o: c_COMPILER ../glib/gasyncqueue.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gasyncqueue.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gasyncqueue.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gatomic.c.o: c_COMPILER ../glib/gatomic.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gatomic.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gatomic.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gbacktrace.c.o: c_COMPILER ../glib/gbacktrace.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gbacktrace.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gbacktrace.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gbase64.c.o: c_COMPILER ../glib/gbase64.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gbase64.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gbase64.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gbitlock.c.o: c_COMPILER ../glib/gbitlock.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gbitlock.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gbitlock.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gbookmarkfile.c.o: c_COMPILER ../glib/gbookmarkfile.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gbookmarkfile.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gbookmarkfile.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gbytes.c.o: c_COMPILER ../glib/gbytes.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gbytes.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gbytes.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gcharset.c.o: c_COMPILER ../glib/gcharset.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gcharset.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gcharset.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gchecksum.c.o: c_COMPILER ../glib/gchecksum.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gchecksum.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gchecksum.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gconvert.c.o: c_COMPILER ../glib/gconvert.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gconvert.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gconvert.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gdataset.c.o: c_COMPILER ../glib/gdataset.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gdataset.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gdataset.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gdate.c.o: c_COMPILER ../glib/gdate.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gdate.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gdate.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gdatetime.c.o: c_COMPILER ../glib/gdatetime.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gdatetime.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gdatetime.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gdir.c.o: c_COMPILER ../glib/gdir.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gdir.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gdir.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/genviron.c.o: c_COMPILER ../glib/genviron.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/genviron.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/genviron.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gerror.c.o: c_COMPILER ../glib/gerror.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gerror.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gerror.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gfileutils.c.o: c_COMPILER ../glib/gfileutils.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gfileutils.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gfileutils.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/ggettext.c.o: c_COMPILER ../glib/ggettext.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/ggettext.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/ggettext.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/ghash.c.o: c_COMPILER ../glib/ghash.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/ghash.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/ghash.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/ghmac.c.o: c_COMPILER ../glib/ghmac.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/ghmac.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/ghmac.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/ghook.c.o: c_COMPILER ../glib/ghook.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/ghook.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/ghook.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/ghostutils.c.o: c_COMPILER ../glib/ghostutils.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/ghostutils.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/ghostutils.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/giochannel.c.o: c_COMPILER ../glib/giochannel.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/giochannel.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/giochannel.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gkeyfile.c.o: c_COMPILER ../glib/gkeyfile.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gkeyfile.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gkeyfile.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/glib-init.c.o: c_COMPILER ../glib/glib-init.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/glib-init.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/glib-init.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/glib-private.c.o: c_COMPILER ../glib/glib-private.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/glib-private.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/glib-private.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/glist.c.o: c_COMPILER ../glib/glist.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/glist.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/glist.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gmain.c.o: c_COMPILER ../glib/gmain.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gmain.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gmain.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gmappedfile.c.o: c_COMPILER ../glib/gmappedfile.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gmappedfile.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gmappedfile.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gmarkup.c.o: c_COMPILER ../glib/gmarkup.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gmarkup.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gmarkup.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gmem.c.o: c_COMPILER ../glib/gmem.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gmem.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gmem.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gmessages.c.o: c_COMPILER ../glib/gmessages.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gmessages.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gmessages.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gnode.c.o: c_COMPILER ../glib/gnode.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gnode.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gnode.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/goption.c.o: c_COMPILER ../glib/goption.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/goption.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/goption.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gpathbuf.c.o: c_COMPILER ../glib/gpathbuf.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gpathbuf.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gpathbuf.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gpattern.c.o: c_COMPILER ../glib/gpattern.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gpattern.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gpattern.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gpoll.c.o: c_COMPILER ../glib/gpoll.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gpoll.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gpoll.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gprimes.c.o: c_COMPILER ../glib/gprimes.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gprimes.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gprimes.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gqsort.c.o: c_COMPILER ../glib/gqsort.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gqsort.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gqsort.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gquark.c.o: c_COMPILER ../glib/gquark.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gquark.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gquark.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gqueue.c.o: c_COMPILER ../glib/gqueue.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gqueue.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gqueue.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/grand.c.o: c_COMPILER ../glib/grand.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/grand.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/grand.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/grcbox.c.o: c_COMPILER ../glib/grcbox.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/grcbox.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/grcbox.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/grefcount.c.o: c_COMPILER ../glib/grefcount.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/grefcount.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/grefcount.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/grefstring.c.o: c_COMPILER ../glib/grefstring.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/grefstring.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/grefstring.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gregex.c.o: c_COMPILER ../glib/gregex.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gregex.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gregex.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gscanner.c.o: c_COMPILER ../glib/gscanner.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gscanner.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gscanner.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gsequence.c.o: c_COMPILER ../glib/gsequence.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gsequence.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gsequence.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gshell.c.o: c_COMPILER ../glib/gshell.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gshell.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gshell.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gslice.c.o: c_COMPILER ../glib/gslice.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gslice.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gslice.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gslist.c.o: c_COMPILER ../glib/gslist.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gslist.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gslist.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gstdio.c.o: c_COMPILER ../glib/gstdio.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gstdio.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gstdio.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gstrfuncs.c.o: c_COMPILER ../glib/gstrfuncs.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gstrfuncs.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gstrfuncs.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gstring.c.o: c_COMPILER ../glib/gstring.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gstring.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gstring.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gstringchunk.c.o: c_COMPILER ../glib/gstringchunk.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gstringchunk.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gstringchunk.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gstrvbuilder.c.o: c_COMPILER ../glib/gstrvbuilder.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gstrvbuilder.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gstrvbuilder.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gtestutils.c.o: c_COMPILER ../glib/gtestutils.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gtestutils.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gtestutils.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gthread.c.o: c_COMPILER ../glib/gthread.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gthread.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gthread.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gthreadpool.c.o: c_COMPILER ../glib/gthreadpool.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gthreadpool.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gthreadpool.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gtimer.c.o: c_COMPILER ../glib/gtimer.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gtimer.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gtimer.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gtimezone.c.o: c_COMPILER ../glib/gtimezone.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gtimezone.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gtimezone.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gtrace.c.o: c_COMPILER ../glib/gtrace.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gtrace.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gtrace.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gtranslit.c.o: c_COMPILER ../glib/gtranslit.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gtranslit.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gtranslit.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gtrashstack.c.o: c_COMPILER ../glib/gtrashstack.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gtrashstack.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gtrashstack.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gtree.c.o: c_COMPILER ../glib/gtree.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gtree.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gtree.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/guniprop.c.o: c_COMPILER ../glib/guniprop.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/guniprop.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/guniprop.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gutf8.c.o: c_COMPILER ../glib/gutf8.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gutf8.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gutf8.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gunibreak.c.o: c_COMPILER ../glib/gunibreak.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gunibreak.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gunibreak.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gunicollate.c.o: c_COMPILER ../glib/gunicollate.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gunicollate.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gunicollate.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gunidecomp.c.o: c_COMPILER ../glib/gunidecomp.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gunidecomp.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gunidecomp.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/guri.c.o: c_COMPILER ../glib/guri.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/guri.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/guri.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gutils.c.o: c_COMPILER ../glib/gutils.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gutils.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gutils.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/guuid.c.o: c_COMPILER ../glib/guuid.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/guuid.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/guuid.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gvariant.c.o: c_COMPILER ../glib/gvariant.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gvariant.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gvariant.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gvariant-core.c.o: c_COMPILER ../glib/gvariant-core.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gvariant-core.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gvariant-core.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gvariant-parser.c.o: c_COMPILER ../glib/gvariant-parser.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gvariant-parser.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gvariant-parser.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gvariant-serialiser.c.o: c_COMPILER ../glib/gvariant-serialiser.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gvariant-serialiser.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gvariant-serialiser.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gvarianttypeinfo.c.o: c_COMPILER ../glib/gvarianttypeinfo.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gvarianttypeinfo.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gvarianttypeinfo.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gvarianttype.c.o: c_COMPILER ../glib/gvarianttype.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gvarianttype.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gvarianttype.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gversion.c.o: c_COMPILER ../glib/gversion.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gversion.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gversion.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gwakeup.c.o: c_COMPILER ../glib/gwakeup.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gwakeup.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gwakeup.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gprintf.c.o: c_COMPILER ../glib/gprintf.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gprintf.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gprintf.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/glib-unix.c.o: c_COMPILER ../glib/glib-unix.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/glib-unix.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/glib-unix.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gspawn.c.o: c_COMPILER ../glib/gspawn.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gspawn.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gspawn.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/giounix.c.o: c_COMPILER ../glib/giounix.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/giounix.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/giounix.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gjournal-private.c.o: c_COMPILER ../glib/gjournal-private.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gjournal-private.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gjournal-private.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a.p/gthread-posix.c.o: c_COMPILER ../glib/gthread-posix.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/libglib-2.0.a.p/gthread-posix.c.o.d
  + DEPFILE_UNQUOTED = glib/libglib-2.0.a.p/gthread-posix.c.o.d
  + ARGS = -Iglib/libglib-2.0.a.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -Isubprojects/pcre2-10.42 -I../subprojects/pcre2-10.42 -I../subprojects/pcre2-10.42/src -fvisibility=hidden -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -fPIC -pthread '-DG_LOG_DOMAIN="GLib"' -DGLIB_COMPILATION
  +
  +build glib/libglib-2.0.a: STATIC_LINKER glib/libglib-2.0.a.p/deprecated_gallocator.c.o glib/libglib-2.0.a.p/deprecated_gcache.c.o glib/libglib-2.0.a.p/deprecated_gcompletion.c.o glib/libglib-2.0.a.p/deprecated_grel.c.o glib/libglib-2.0.a.p/deprecated_gthread-deprecated.c.o glib/libglib-2.0.a.p/garcbox.c.o glib/libglib-2.0.a.p/garray.c.o glib/libglib-2.0.a.p/gasyncqueue.c.o glib/libglib-2.0.a.p/gatomic.c.o glib/libglib-2.0.a.p/gbacktrace.c.o glib/libglib-2.0.a.p/gbase64.c.o glib/libglib-2.0.a.p/gbitlock.c.o glib/libglib-2.0.a.p/gbookmarkfile.c.o glib/libglib-2.0.a.p/gbytes.c.o glib/libglib-2.0.a.p/gcharset.c.o glib/libglib-2.0.a.p/gchecksum.c.o glib/libglib-2.0.a.p/gconvert.c.o glib/libglib-2.0.a.p/gdataset.c.o glib/libglib-2.0.a.p/gdate.c.o glib/libglib-2.0.a.p/gdatetime.c.o glib/libglib-2.0.a.p/gdir.c.o glib/libglib-2.0.a.p/genviron.c.o glib/libglib-2.0.a.p/gerror.c.o glib/libglib-2.0.a.p/gfileutils.c.o glib/libglib-2.0.a.p/ggettext.c.o glib/libglib-2.0.a.p/ghash.c.o glib/libglib-2.0.a.p/ghmac.c.o glib/libglib-2.0.a.p/ghook.c.o glib/libglib-2.0.a.p/ghostutils.c.o glib/libglib-2.0.a.p/giochannel.c.o glib/libglib-2.0.a.p/gkeyfile.c.o glib/libglib-2.0.a.p/glib-init.c.o glib/libglib-2.0.a.p/glib-private.c.o glib/libglib-2.0.a.p/glist.c.o glib/libglib-2.0.a.p/gmain.c.o glib/libglib-2.0.a.p/gmappedfile.c.o glib/libglib-2.0.a.p/gmarkup.c.o glib/libglib-2.0.a.p/gmem.c.o glib/libglib-2.0.a.p/gmessages.c.o glib/libglib-2.0.a.p/gnode.c.o glib/libglib-2.0.a.p/goption.c.o glib/libglib-2.0.a.p/gpathbuf.c.o glib/libglib-2.0.a.p/gpattern.c.o glib/libglib-2.0.a.p/gpoll.c.o glib/libglib-2.0.a.p/gprimes.c.o glib/libglib-2.0.a.p/gqsort.c.o glib/libglib-2.0.a.p/gquark.c.o glib/libglib-2.0.a.p/gqueue.c.o glib/libglib-2.0.a.p/grand.c.o glib/libglib-2.0.a.p/grcbox.c.o glib/libglib-2.0.a.p/grefcount.c.o glib/libglib-2.0.a.p/grefstring.c.o glib/libglib-2.0.a.p/gregex.c.o glib/libglib-2.0.a.p/gscanner.c.o glib/libglib-2.0.a.p/gsequence.c.o glib/libglib-2.0.a.p/gshell.c.o glib/libglib-2.0.a.p/gslice.c.o glib/libglib-2.0.a.p/gslist.c.o glib/libglib-2.0.a.p/gstdio.c.o glib/libglib-2.0.a.p/gstrfuncs.c.o glib/libglib-2.0.a.p/gstring.c.o glib/libglib-2.0.a.p/gstringchunk.c.o glib/libglib-2.0.a.p/gstrvbuilder.c.o glib/libglib-2.0.a.p/gtestutils.c.o glib/libglib-2.0.a.p/gthread.c.o glib/libglib-2.0.a.p/gthreadpool.c.o glib/libglib-2.0.a.p/gtimer.c.o glib/libglib-2.0.a.p/gtimezone.c.o glib/libglib-2.0.a.p/gtrace.c.o glib/libglib-2.0.a.p/gtranslit.c.o glib/libglib-2.0.a.p/gtrashstack.c.o glib/libglib-2.0.a.p/gtree.c.o glib/libglib-2.0.a.p/guniprop.c.o glib/libglib-2.0.a.p/gutf8.c.o glib/libglib-2.0.a.p/gunibreak.c.o glib/libglib-2.0.a.p/gunicollate.c.o glib/libglib-2.0.a.p/gunidecomp.c.o glib/libglib-2.0.a.p/guri.c.o glib/libglib-2.0.a.p/gutils.c.o glib/libglib-2.0.a.p/guuid.c.o glib/libglib-2.0.a.p/gvariant.c.o glib/libglib-2.0.a.p/gvariant-core.c.o glib/libglib-2.0.a.p/gvariant-parser.c.o glib/libglib-2.0.a.p/gvariant-serialiser.c.o glib/libglib-2.0.a.p/gvarianttypeinfo.c.o glib/libglib-2.0.a.p/gvarianttype.c.o glib/libglib-2.0.a.p/gversion.c.o glib/libglib-2.0.a.p/gwakeup.c.o glib/libglib-2.0.a.p/gprintf.c.o glib/libglib-2.0.a.p/glib-unix.c.o glib/libglib-2.0.a.p/gspawn.c.o glib/libglib-2.0.a.p/giounix.c.o glib/libglib-2.0.a.p/gjournal-private.c.o glib/libglib-2.0.a.p/gthread-posix.c.o glib/libcharset/libcharset.a.p/localcharset.c.o glib/gnulib/libgnulib.a.p/asnprintf.c.o glib/gnulib/libgnulib.a.p/printf.c.o glib/gnulib/libgnulib.a.p/printf-args.c.o glib/gnulib/libgnulib.a.p/printf-parse.c.o glib/gnulib/libgnulib.a.p/printf-frexp.c.o glib/gnulib/libgnulib.a.p/printf-frexpl.c.o glib/gnulib/libgnulib.a.p/vasnprintf.c.o glib/gnulib/libgnulib.a.p/xsize.c.o subprojects/proxy-libintl/libintl.so.8.p/libintl.c.o
  + LINK_ARGS = csrD
  +
  +build glib/gtester.p/gtester.c.o: c_COMPILER ../glib/gtester.c || glib/glib-visibility.h glib/gversionmacros.h
  + DEPFILE = glib/gtester.p/gtester.c.o.d
  + DEPFILE_UNQUOTED = glib/gtester.p/gtester.c.o.d
  + ARGS = -Iglib/gtester.p -Iglib -I../glib -I. -I.. -Isubprojects/proxy-libintl -I../subprojects/proxy-libintl -fdiagnostics-color=always -D_FILE_OFFSET_BITS=64 -Wall -Winvalid-pch -Wextra -Wpedantic -std=gnu99 -O2 -g -D_GNU_SOURCE -fno-strict-aliasing -DG_DISABLE_CAST_CHECKS -Wduplicated-branches -Wimplicit-fallthrough -Wmisleading-indentation -Wmissing-field-initializers -Wnonnull -Wunused -Wno-unused-parameter -Wno-cast-function-type -Wno-pedantic -Wno-format-zero-length -Wno-variadic-macros -Werror=format=2 -Werror=init-self -Werror=missing-include-dirs -Werror=pointer-arith -Werror=unused-result -Wstrict-prototypes -Wno-bad-function-cast -Werror=implicit-function-declaration -Werror=missing-prototypes -Werror=pointer-sign -UG_DISABLE_ASSERT
  +
  +build glib/gtester: c_LINKER glib/gtester.p/gtester.c.o | /home/sherlock/blockISA_toolchain/linx64-linux-gnu/sysroot/lib/libm.so.6 /home/sherlock/blockISA_toolchain/linx64-linux-gnu/sysroot/usr/lib/libm.a glib/gnulib/libgnulib.a glib/libcharset/libcharset.a glib/libglib-2.0.a subprojects/pcre2-10.42/libpcre2-8.so.0.11.0.p/libpcre2-8.so.0.11.0.symbols subprojects/proxy-libintl/libintl.so.8.p/libintl.so.8.symbols
  + LINK_ARGS = -Wl,--as-needed -Wl,--no-undefined '-Wl,-rpath,$$ORIGIN/../subprojects/proxy-libintl:$$ORIGIN/../subprojects/pcre2-10.42' -Wl,-rpath-link,/home/sherlock/repos/glib/linx0.20_build/subprojects/proxy-libintl -Wl,-rpath-link,/home/sherlock/repos/glib/linx0.20_build/subprojects/pcre2-10.42 -Wl,--start-group glib/libglib-2.0.a subprojects/proxy-libintl/libintl.so.8 subprojects/pcre2-10.42/libpcre2-8.so.0.11.0 glib/libcharset/libcharset.a glib/gnulib/libgnulib.a -lm -Wl,--end-group -pthread
  +
  +build glib20-pot: phony meson-glib20-pot
  +
  +build meson-glib20-pot: CUSTOM_COMMAND 
  + COMMAND = /usr/bin/meson --internal exe --unpickle /home/sherlock/repos/glib/linx0.20_build/meson-private/meson_exe_meson_62e9e6c5176409bce27f3218b0a32e06c6e04ead.dat
  + description = Running$ external$ command$ glib20-pot$ (wrapped$ by$ meson$ to$ set$ env)
  + pool = console
  +
  +build po/ab/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ab.po
  + COMMAND = msgfmt ../po/ab.po -o po/ab/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ab/LC_MESSAGES/glib20-ab.mo$ with$ a$ custom$ command
  +
  +build po/af/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/af.po
  + COMMAND = msgfmt ../po/af.po -o po/af/LC_MESSAGES/glib20.mo
  + description = Generating$ po/af/LC_MESSAGES/glib20-af.mo$ with$ a$ custom$ command
  +
  +build po/am/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/am.po
  + COMMAND = msgfmt ../po/am.po -o po/am/LC_MESSAGES/glib20.mo
  + description = Generating$ po/am/LC_MESSAGES/glib20-am.mo$ with$ a$ custom$ command
  +
  +build po/an/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/an.po
  + COMMAND = msgfmt ../po/an.po -o po/an/LC_MESSAGES/glib20.mo
  + description = Generating$ po/an/LC_MESSAGES/glib20-an.mo$ with$ a$ custom$ command
  +
  +build po/ar/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ar.po
  + COMMAND = msgfmt ../po/ar.po -o po/ar/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ar/LC_MESSAGES/glib20-ar.mo$ with$ a$ custom$ command
  +
  +build po/as/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/as.po
  + COMMAND = msgfmt ../po/as.po -o po/as/LC_MESSAGES/glib20.mo
  + description = Generating$ po/as/LC_MESSAGES/glib20-as.mo$ with$ a$ custom$ command
  +
  +build po/ast/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ast.po
  + COMMAND = msgfmt ../po/ast.po -o po/ast/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ast/LC_MESSAGES/glib20-ast.mo$ with$ a$ custom$ command
  +
  +build po/az/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/az.po
  + COMMAND = msgfmt ../po/az.po -o po/az/LC_MESSAGES/glib20.mo
  + description = Generating$ po/az/LC_MESSAGES/glib20-az.mo$ with$ a$ custom$ command
  +
  +build po/be/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/be.po
  + COMMAND = msgfmt ../po/be.po -o po/be/LC_MESSAGES/glib20.mo
  + description = Generating$ po/be/LC_MESSAGES/glib20-be.mo$ with$ a$ custom$ command
  +
  +build po/be@latin/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/be@latin.po
  + COMMAND = msgfmt ../po/be@latin.po -o po/be@latin/LC_MESSAGES/glib20.mo
  + description = Generating$ po/be@latin/LC_MESSAGES/glib20-be@latin.mo$ with$ a$ custom$ command
  +
  +build po/bg/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/bg.po
  + COMMAND = msgfmt ../po/bg.po -o po/bg/LC_MESSAGES/glib20.mo
  + description = Generating$ po/bg/LC_MESSAGES/glib20-bg.mo$ with$ a$ custom$ command
  +
  +build po/bn/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/bn.po
  + COMMAND = msgfmt ../po/bn.po -o po/bn/LC_MESSAGES/glib20.mo
  + description = Generating$ po/bn/LC_MESSAGES/glib20-bn.mo$ with$ a$ custom$ command
  +
  +build po/bn_IN/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/bn_IN.po
  + COMMAND = msgfmt ../po/bn_IN.po -o po/bn_IN/LC_MESSAGES/glib20.mo
  + description = Generating$ po/bn_IN/LC_MESSAGES/glib20-bn_IN.mo$ with$ a$ custom$ command
  +
  +build po/bs/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/bs.po
  + COMMAND = msgfmt ../po/bs.po -o po/bs/LC_MESSAGES/glib20.mo
  + description = Generating$ po/bs/LC_MESSAGES/glib20-bs.mo$ with$ a$ custom$ command
  +
  +build po/ca/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ca.po
  + COMMAND = msgfmt ../po/ca.po -o po/ca/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ca/LC_MESSAGES/glib20-ca.mo$ with$ a$ custom$ command
  +
  +build po/ca@valencia/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ca@valencia.po
  + COMMAND = msgfmt ../po/ca@valencia.po -o po/ca@valencia/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ca@valencia/LC_MESSAGES/glib20-ca@valencia.mo$ with$ a$ custom$ command
  +
  +build po/cs/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/cs.po
  + COMMAND = msgfmt ../po/cs.po -o po/cs/LC_MESSAGES/glib20.mo
  + description = Generating$ po/cs/LC_MESSAGES/glib20-cs.mo$ with$ a$ custom$ command
  +
  +build po/cy/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/cy.po
  + COMMAND = msgfmt ../po/cy.po -o po/cy/LC_MESSAGES/glib20.mo
  + description = Generating$ po/cy/LC_MESSAGES/glib20-cy.mo$ with$ a$ custom$ command
  +
  +build po/da/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/da.po
  + COMMAND = msgfmt ../po/da.po -o po/da/LC_MESSAGES/glib20.mo
  + description = Generating$ po/da/LC_MESSAGES/glib20-da.mo$ with$ a$ custom$ command
  +
  +build po/de/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/de.po
  + COMMAND = msgfmt ../po/de.po -o po/de/LC_MESSAGES/glib20.mo
  + description = Generating$ po/de/LC_MESSAGES/glib20-de.mo$ with$ a$ custom$ command
  +
  +build po/dz/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/dz.po
  + COMMAND = msgfmt ../po/dz.po -o po/dz/LC_MESSAGES/glib20.mo
  + description = Generating$ po/dz/LC_MESSAGES/glib20-dz.mo$ with$ a$ custom$ command
  +
  +build po/el/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/el.po
  + COMMAND = msgfmt ../po/el.po -o po/el/LC_MESSAGES/glib20.mo
  + description = Generating$ po/el/LC_MESSAGES/glib20-el.mo$ with$ a$ custom$ command
  +
  +build po/en_CA/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/en_CA.po
  + COMMAND = msgfmt ../po/en_CA.po -o po/en_CA/LC_MESSAGES/glib20.mo
  + description = Generating$ po/en_CA/LC_MESSAGES/glib20-en_CA.mo$ with$ a$ custom$ command
  +
  +build po/en_GB/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/en_GB.po
  + COMMAND = msgfmt ../po/en_GB.po -o po/en_GB/LC_MESSAGES/glib20.mo
  + description = Generating$ po/en_GB/LC_MESSAGES/glib20-en_GB.mo$ with$ a$ custom$ command
  +
  +build po/en@shaw/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/en@shaw.po
  + COMMAND = msgfmt ../po/en@shaw.po -o po/en@shaw/LC_MESSAGES/glib20.mo
  + description = Generating$ po/en@shaw/LC_MESSAGES/glib20-en@shaw.mo$ with$ a$ custom$ command
  +
  +build po/eo/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/eo.po
  + COMMAND = msgfmt ../po/eo.po -o po/eo/LC_MESSAGES/glib20.mo
  + description = Generating$ po/eo/LC_MESSAGES/glib20-eo.mo$ with$ a$ custom$ command
  +
  +build po/es/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/es.po
  + COMMAND = msgfmt ../po/es.po -o po/es/LC_MESSAGES/glib20.mo
  + description = Generating$ po/es/LC_MESSAGES/glib20-es.mo$ with$ a$ custom$ command
  +
  +build po/et/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/et.po
  + COMMAND = msgfmt ../po/et.po -o po/et/LC_MESSAGES/glib20.mo
  + description = Generating$ po/et/LC_MESSAGES/glib20-et.mo$ with$ a$ custom$ command
  +
  +build po/eu/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/eu.po
  + COMMAND = msgfmt ../po/eu.po -o po/eu/LC_MESSAGES/glib20.mo
  + description = Generating$ po/eu/LC_MESSAGES/glib20-eu.mo$ with$ a$ custom$ command
  +
  +build po/fa/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/fa.po
  + COMMAND = msgfmt ../po/fa.po -o po/fa/LC_MESSAGES/glib20.mo
  + description = Generating$ po/fa/LC_MESSAGES/glib20-fa.mo$ with$ a$ custom$ command
  +
  +build po/fi/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/fi.po
  + COMMAND = msgfmt ../po/fi.po -o po/fi/LC_MESSAGES/glib20.mo
  + description = Generating$ po/fi/LC_MESSAGES/glib20-fi.mo$ with$ a$ custom$ command
  +
  +build po/fr/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/fr.po
  + COMMAND = msgfmt ../po/fr.po -o po/fr/LC_MESSAGES/glib20.mo
  + description = Generating$ po/fr/LC_MESSAGES/glib20-fr.mo$ with$ a$ custom$ command
  +
  +build po/fur/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/fur.po
  + COMMAND = msgfmt ../po/fur.po -o po/fur/LC_MESSAGES/glib20.mo
  + description = Generating$ po/fur/LC_MESSAGES/glib20-fur.mo$ with$ a$ custom$ command
  +
  +build po/ga/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ga.po
  + COMMAND = msgfmt ../po/ga.po -o po/ga/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ga/LC_MESSAGES/glib20-ga.mo$ with$ a$ custom$ command
  +
  +build po/gd/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/gd.po
  + COMMAND = msgfmt ../po/gd.po -o po/gd/LC_MESSAGES/glib20.mo
  + description = Generating$ po/gd/LC_MESSAGES/glib20-gd.mo$ with$ a$ custom$ command
  +
  +build po/gl/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/gl.po
  + COMMAND = msgfmt ../po/gl.po -o po/gl/LC_MESSAGES/glib20.mo
  + description = Generating$ po/gl/LC_MESSAGES/glib20-gl.mo$ with$ a$ custom$ command
  +
  +build po/gu/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/gu.po
  + COMMAND = msgfmt ../po/gu.po -o po/gu/LC_MESSAGES/glib20.mo
  + description = Generating$ po/gu/LC_MESSAGES/glib20-gu.mo$ with$ a$ custom$ command
  +
  +build po/he/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/he.po
  + COMMAND = msgfmt ../po/he.po -o po/he/LC_MESSAGES/glib20.mo
  + description = Generating$ po/he/LC_MESSAGES/glib20-he.mo$ with$ a$ custom$ command
  +
  +build po/hi/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/hi.po
  + COMMAND = msgfmt ../po/hi.po -o po/hi/LC_MESSAGES/glib20.mo
  + description = Generating$ po/hi/LC_MESSAGES/glib20-hi.mo$ with$ a$ custom$ command
  +
  +build po/hr/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/hr.po
  + COMMAND = msgfmt ../po/hr.po -o po/hr/LC_MESSAGES/glib20.mo
  + description = Generating$ po/hr/LC_MESSAGES/glib20-hr.mo$ with$ a$ custom$ command
  +
  +build po/hu/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/hu.po
  + COMMAND = msgfmt ../po/hu.po -o po/hu/LC_MESSAGES/glib20.mo
  + description = Generating$ po/hu/LC_MESSAGES/glib20-hu.mo$ with$ a$ custom$ command
  +
  +build po/hy/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/hy.po
  + COMMAND = msgfmt ../po/hy.po -o po/hy/LC_MESSAGES/glib20.mo
  + description = Generating$ po/hy/LC_MESSAGES/glib20-hy.mo$ with$ a$ custom$ command
  +
  +build po/id/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/id.po
  + COMMAND = msgfmt ../po/id.po -o po/id/LC_MESSAGES/glib20.mo
  + description = Generating$ po/id/LC_MESSAGES/glib20-id.mo$ with$ a$ custom$ command
  +
  +build po/ie/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ie.po
  + COMMAND = msgfmt ../po/ie.po -o po/ie/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ie/LC_MESSAGES/glib20-ie.mo$ with$ a$ custom$ command
  +
  +build po/is/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/is.po
  + COMMAND = msgfmt ../po/is.po -o po/is/LC_MESSAGES/glib20.mo
  + description = Generating$ po/is/LC_MESSAGES/glib20-is.mo$ with$ a$ custom$ command
  +
  +build po/it/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/it.po
  + COMMAND = msgfmt ../po/it.po -o po/it/LC_MESSAGES/glib20.mo
  + description = Generating$ po/it/LC_MESSAGES/glib20-it.mo$ with$ a$ custom$ command
  +
  +build po/ja/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ja.po
  + COMMAND = msgfmt ../po/ja.po -o po/ja/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ja/LC_MESSAGES/glib20-ja.mo$ with$ a$ custom$ command
  +
  +build po/ka/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ka.po
  + COMMAND = msgfmt ../po/ka.po -o po/ka/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ka/LC_MESSAGES/glib20-ka.mo$ with$ a$ custom$ command
  +
  +build po/kk/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/kk.po
  + COMMAND = msgfmt ../po/kk.po -o po/kk/LC_MESSAGES/glib20.mo
  + description = Generating$ po/kk/LC_MESSAGES/glib20-kk.mo$ with$ a$ custom$ command
  +
  +build po/kn/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/kn.po
  + COMMAND = msgfmt ../po/kn.po -o po/kn/LC_MESSAGES/glib20.mo
  + description = Generating$ po/kn/LC_MESSAGES/glib20-kn.mo$ with$ a$ custom$ command
  +
  +build po/ko/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ko.po
  + COMMAND = msgfmt ../po/ko.po -o po/ko/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ko/LC_MESSAGES/glib20-ko.mo$ with$ a$ custom$ command
  +
  +build po/ku/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ku.po
  + COMMAND = msgfmt ../po/ku.po -o po/ku/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ku/LC_MESSAGES/glib20-ku.mo$ with$ a$ custom$ command
  +
  +build po/lt/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/lt.po
  + COMMAND = msgfmt ../po/lt.po -o po/lt/LC_MESSAGES/glib20.mo
  + description = Generating$ po/lt/LC_MESSAGES/glib20-lt.mo$ with$ a$ custom$ command
  +
  +build po/lv/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/lv.po
  + COMMAND = msgfmt ../po/lv.po -o po/lv/LC_MESSAGES/glib20.mo
  + description = Generating$ po/lv/LC_MESSAGES/glib20-lv.mo$ with$ a$ custom$ command
  +
  +build po/mai/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/mai.po
  + COMMAND = msgfmt ../po/mai.po -o po/mai/LC_MESSAGES/glib20.mo
  + description = Generating$ po/mai/LC_MESSAGES/glib20-mai.mo$ with$ a$ custom$ command
  +
  +build po/mg/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/mg.po
  + COMMAND = msgfmt ../po/mg.po -o po/mg/LC_MESSAGES/glib20.mo
  + description = Generating$ po/mg/LC_MESSAGES/glib20-mg.mo$ with$ a$ custom$ command
  +
  +build po/mk/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/mk.po
  + COMMAND = msgfmt ../po/mk.po -o po/mk/LC_MESSAGES/glib20.mo
  + description = Generating$ po/mk/LC_MESSAGES/glib20-mk.mo$ with$ a$ custom$ command
  +
  +build po/ml/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ml.po
  + COMMAND = msgfmt ../po/ml.po -o po/ml/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ml/LC_MESSAGES/glib20-ml.mo$ with$ a$ custom$ command
  +
  +build po/mn/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/mn.po
  + COMMAND = msgfmt ../po/mn.po -o po/mn/LC_MESSAGES/glib20.mo
  + description = Generating$ po/mn/LC_MESSAGES/glib20-mn.mo$ with$ a$ custom$ command
  +
  +build po/mr/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/mr.po
  + COMMAND = msgfmt ../po/mr.po -o po/mr/LC_MESSAGES/glib20.mo
  + description = Generating$ po/mr/LC_MESSAGES/glib20-mr.mo$ with$ a$ custom$ command
  +
  +build po/ms/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ms.po
  + COMMAND = msgfmt ../po/ms.po -o po/ms/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ms/LC_MESSAGES/glib20-ms.mo$ with$ a$ custom$ command
  +
  +build po/nb/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/nb.po
  + COMMAND = msgfmt ../po/nb.po -o po/nb/LC_MESSAGES/glib20.mo
  + description = Generating$ po/nb/LC_MESSAGES/glib20-nb.mo$ with$ a$ custom$ command
  +
  +build po/nds/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/nds.po
  + COMMAND = msgfmt ../po/nds.po -o po/nds/LC_MESSAGES/glib20.mo
  + description = Generating$ po/nds/LC_MESSAGES/glib20-nds.mo$ with$ a$ custom$ command
  +
  +build po/ne/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ne.po
  + COMMAND = msgfmt ../po/ne.po -o po/ne/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ne/LC_MESSAGES/glib20-ne.mo$ with$ a$ custom$ command
  +
  +build po/nl/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/nl.po
  + COMMAND = msgfmt ../po/nl.po -o po/nl/LC_MESSAGES/glib20.mo
  + description = Generating$ po/nl/LC_MESSAGES/glib20-nl.mo$ with$ a$ custom$ command
  +
  +build po/nn/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/nn.po
  + COMMAND = msgfmt ../po/nn.po -o po/nn/LC_MESSAGES/glib20.mo
  + description = Generating$ po/nn/LC_MESSAGES/glib20-nn.mo$ with$ a$ custom$ command
  +
  +build po/oc/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/oc.po
  + COMMAND = msgfmt ../po/oc.po -o po/oc/LC_MESSAGES/glib20.mo
  + description = Generating$ po/oc/LC_MESSAGES/glib20-oc.mo$ with$ a$ custom$ command
  +
  +build po/or/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/or.po
  + COMMAND = msgfmt ../po/or.po -o po/or/LC_MESSAGES/glib20.mo
  + description = Generating$ po/or/LC_MESSAGES/glib20-or.mo$ with$ a$ custom$ command
  +
  +build po/pa/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/pa.po
  + COMMAND = msgfmt ../po/pa.po -o po/pa/LC_MESSAGES/glib20.mo
  + description = Generating$ po/pa/LC_MESSAGES/glib20-pa.mo$ with$ a$ custom$ command
  +
  +build po/pl/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/pl.po
  + COMMAND = msgfmt ../po/pl.po -o po/pl/LC_MESSAGES/glib20.mo
  + description = Generating$ po/pl/LC_MESSAGES/glib20-pl.mo$ with$ a$ custom$ command
  +
  +build po/ps/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ps.po
  + COMMAND = msgfmt ../po/ps.po -o po/ps/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ps/LC_MESSAGES/glib20-ps.mo$ with$ a$ custom$ command
  +
  +build po/pt/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/pt.po
  + COMMAND = msgfmt ../po/pt.po -o po/pt/LC_MESSAGES/glib20.mo
  + description = Generating$ po/pt/LC_MESSAGES/glib20-pt.mo$ with$ a$ custom$ command
  +
  +build po/pt_BR/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/pt_BR.po
  + COMMAND = msgfmt ../po/pt_BR.po -o po/pt_BR/LC_MESSAGES/glib20.mo
  + description = Generating$ po/pt_BR/LC_MESSAGES/glib20-pt_BR.mo$ with$ a$ custom$ command
  +
  +build po/ro/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ro.po
  + COMMAND = msgfmt ../po/ro.po -o po/ro/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ro/LC_MESSAGES/glib20-ro.mo$ with$ a$ custom$ command
  +
  +build po/ru/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ru.po
  + COMMAND = msgfmt ../po/ru.po -o po/ru/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ru/LC_MESSAGES/glib20-ru.mo$ with$ a$ custom$ command
  +
  +build po/rw/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/rw.po
  + COMMAND = msgfmt ../po/rw.po -o po/rw/LC_MESSAGES/glib20.mo
  + description = Generating$ po/rw/LC_MESSAGES/glib20-rw.mo$ with$ a$ custom$ command
  +
  +build po/si/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/si.po
  + COMMAND = msgfmt ../po/si.po -o po/si/LC_MESSAGES/glib20.mo
  + description = Generating$ po/si/LC_MESSAGES/glib20-si.mo$ with$ a$ custom$ command
  +
  +build po/sk/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/sk.po
  + COMMAND = msgfmt ../po/sk.po -o po/sk/LC_MESSAGES/glib20.mo
  + description = Generating$ po/sk/LC_MESSAGES/glib20-sk.mo$ with$ a$ custom$ command
  +
  +build po/sl/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/sl.po
  + COMMAND = msgfmt ../po/sl.po -o po/sl/LC_MESSAGES/glib20.mo
  + description = Generating$ po/sl/LC_MESSAGES/glib20-sl.mo$ with$ a$ custom$ command
  +
  +build po/sq/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/sq.po
  + COMMAND = msgfmt ../po/sq.po -o po/sq/LC_MESSAGES/glib20.mo
  + description = Generating$ po/sq/LC_MESSAGES/glib20-sq.mo$ with$ a$ custom$ command
  +
  +build po/sr/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/sr.po
  + COMMAND = msgfmt ../po/sr.po -o po/sr/LC_MESSAGES/glib20.mo
  + description = Generating$ po/sr/LC_MESSAGES/glib20-sr.mo$ with$ a$ custom$ command
  +
  +build po/sr@latin/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/sr@latin.po
  + COMMAND = msgfmt ../po/sr@latin.po -o po/sr@latin/LC_MESSAGES/glib20.mo
  + description = Generating$ po/sr@latin/LC_MESSAGES/glib20-sr@latin.mo$ with$ a$ custom$ command
  +
  +build po/sr@ije/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/sr@ije.po
  + COMMAND = msgfmt ../po/sr@ije.po -o po/sr@ije/LC_MESSAGES/glib20.mo
  + description = Generating$ po/sr@ije/LC_MESSAGES/glib20-sr@ije.mo$ with$ a$ custom$ command
  +
  +build po/sv/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/sv.po
  + COMMAND = msgfmt ../po/sv.po -o po/sv/LC_MESSAGES/glib20.mo
  + description = Generating$ po/sv/LC_MESSAGES/glib20-sv.mo$ with$ a$ custom$ command
  +
  +build po/ta/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ta.po
  + COMMAND = msgfmt ../po/ta.po -o po/ta/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ta/LC_MESSAGES/glib20-ta.mo$ with$ a$ custom$ command
  +
  +build po/te/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/te.po
  + COMMAND = msgfmt ../po/te.po -o po/te/LC_MESSAGES/glib20.mo
  + description = Generating$ po/te/LC_MESSAGES/glib20-te.mo$ with$ a$ custom$ command
  +
  +build po/tg/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/tg.po
  + COMMAND = msgfmt ../po/tg.po -o po/tg/LC_MESSAGES/glib20.mo
  + description = Generating$ po/tg/LC_MESSAGES/glib20-tg.mo$ with$ a$ custom$ command
  +
  +build po/th/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/th.po
  + COMMAND = msgfmt ../po/th.po -o po/th/LC_MESSAGES/glib20.mo
  + description = Generating$ po/th/LC_MESSAGES/glib20-th.mo$ with$ a$ custom$ command
  +
  +build po/tl/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/tl.po
  + COMMAND = msgfmt ../po/tl.po -o po/tl/LC_MESSAGES/glib20.mo
  + description = Generating$ po/tl/LC_MESSAGES/glib20-tl.mo$ with$ a$ custom$ command
  +
  +build po/tr/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/tr.po
  + COMMAND = msgfmt ../po/tr.po -o po/tr/LC_MESSAGES/glib20.mo
  + description = Generating$ po/tr/LC_MESSAGES/glib20-tr.mo$ with$ a$ custom$ command
  +
  +build po/ug/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/ug.po
  + COMMAND = msgfmt ../po/ug.po -o po/ug/LC_MESSAGES/glib20.mo
  + description = Generating$ po/ug/LC_MESSAGES/glib20-ug.mo$ with$ a$ custom$ command
  +
  +build po/tt/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/tt.po
  + COMMAND = msgfmt ../po/tt.po -o po/tt/LC_MESSAGES/glib20.mo
  + description = Generating$ po/tt/LC_MESSAGES/glib20-tt.mo$ with$ a$ custom$ command
  +
  +build po/uk/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/uk.po
  + COMMAND = msgfmt ../po/uk.po -o po/uk/LC_MESSAGES/glib20.mo
  + description = Generating$ po/uk/LC_MESSAGES/glib20-uk.mo$ with$ a$ custom$ command
  +
  +build po/vi/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/vi.po
  + COMMAND = msgfmt ../po/vi.po -o po/vi/LC_MESSAGES/glib20.mo
  + description = Generating$ po/vi/LC_MESSAGES/glib20-vi.mo$ with$ a$ custom$ command
  +
  +build po/wa/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/wa.po
  + COMMAND = msgfmt ../po/wa.po -o po/wa/LC_MESSAGES/glib20.mo
  + description = Generating$ po/wa/LC_MESSAGES/glib20-wa.mo$ with$ a$ custom$ command
  +
  +build po/xh/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/xh.po
  + COMMAND = msgfmt ../po/xh.po -o po/xh/LC_MESSAGES/glib20.mo
  + description = Generating$ po/xh/LC_MESSAGES/glib20-xh.mo$ with$ a$ custom$ command
  +
  +build po/yi/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/yi.po
  + COMMAND = msgfmt ../po/yi.po -o po/yi/LC_MESSAGES/glib20.mo
  + description = Generating$ po/yi/LC_MESSAGES/glib20-yi.mo$ with$ a$ custom$ command
  +
  +build po/zh_CN/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/zh_CN.po
  + COMMAND = msgfmt ../po/zh_CN.po -o po/zh_CN/LC_MESSAGES/glib20.mo
  + description = Generating$ po/zh_CN/LC_MESSAGES/glib20-zh_CN.mo$ with$ a$ custom$ command
  +
  +build po/zh_HK/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/zh_HK.po
  + COMMAND = msgfmt ../po/zh_HK.po -o po/zh_HK/LC_MESSAGES/glib20.mo
  + description = Generating$ po/zh_HK/LC_MESSAGES/glib20-zh_HK.mo$ with$ a$ custom$ command
  +
  +build po/zh_TW/LC_MESSAGES/glib20.mo: CUSTOM_COMMAND ../po/zh_TW.po
  + COMMAND = msgfmt ../po/zh_TW.po -o po/zh_TW/LC_MESSAGES/glib20.mo
  + description = Generating$ po/zh_TW/LC_MESSAGES/glib20-zh_TW.mo$ with$ a$ custom$ command
  +
  +build glib20-gmo: phony  | po/ab/LC_MESSAGES/glib20.mo po/af/LC_MESSAGES/glib20.mo po/am/LC_MESSAGES/glib20.mo po/an/LC_MESSAGES/glib20.mo po/ar/LC_MESSAGES/glib20.mo po/as/LC_MESSAGES/glib20.mo po/ast/LC_MESSAGES/glib20.mo po/az/LC_MESSAGES/glib20.mo po/be/LC_MESSAGES/glib20.mo po/be@latin/LC_MESSAGES/glib20.mo po/bg/LC_MESSAGES/glib20.mo po/bn/LC_MESSAGES/glib20.mo po/bn_IN/LC_MESSAGES/glib20.mo po/bs/LC_MESSAGES/glib20.mo po/ca/LC_MESSAGES/glib20.mo po/ca@valencia/LC_MESSAGES/glib20.mo po/cs/LC_MESSAGES/glib20.mo po/cy/LC_MESSAGES/glib20.mo po/da/LC_MESSAGES/glib20.mo po/de/LC_MESSAGES/glib20.mo po/dz/LC_MESSAGES/glib20.mo po/el/LC_MESSAGES/glib20.mo po/en@shaw/LC_MESSAGES/glib20.mo po/en_CA/LC_MESSAGES/glib20.mo po/en_GB/LC_MESSAGES/glib20.mo po/eo/LC_MESSAGES/glib20.mo po/es/LC_MESSAGES/glib20.mo po/et/LC_MESSAGES/glib20.mo po/eu/LC_MESSAGES/glib20.mo po/fa/LC_MESSAGES/glib20.mo po/fi/LC_MESSAGES/glib20.mo po/fr/LC_MESSAGES/glib20.mo po/fur/LC_MESSAGES/glib20.mo po/ga/LC_MESSAGES/glib20.mo po/gd/LC_MESSAGES/glib20.mo po/gl/LC_MESSAGES/glib20.mo po/gu/LC_MESSAGES/glib20.mo po/he/LC_MESSAGES/glib20.mo po/hi/LC_MESSAGES/glib20.mo po/hr/LC_MESSAGES/glib20.mo po/hu/LC_MESSAGES/glib20.mo po/hy/LC_MESSAGES/glib20.mo po/id/LC_MESSAGES/glib20.mo po/ie/LC_MESSAGES/glib20.mo po/is/LC_MESSAGES/glib20.mo po/it/LC_MESSAGES/glib20.mo po/ja/LC_MESSAGES/glib20.mo po/ka/LC_MESSAGES/glib20.mo po/kk/LC_MESSAGES/glib20.mo po/kn/LC_MESSAGES/glib20.mo po/ko/LC_MESSAGES/glib20.mo po/ku/LC_MESSAGES/glib20.mo po/lt/LC_MESSAGES/glib20.mo po/lv/LC_MESSAGES/glib20.mo po/mai/LC_MESSAGES/glib20.mo po/mg/LC_MESSAGES/glib20.mo po/mk/LC_MESSAGES/glib20.mo po/ml/LC_MESSAGES/glib20.mo po/mn/LC_MESSAGES/glib20.mo po/mr/LC_MESSAGES/glib20.mo po/ms/LC_MESSAGES/glib20.mo po/nb/LC_MESSAGES/glib20.mo po/nds/LC_MESSAGES/glib20.mo po/ne/LC_MESSAGES/glib20.mo po/nl/LC_MESSAGES/glib20.mo po/nn/LC_MESSAGES/glib20.mo po/oc/LC_MESSAGES/glib20.mo po/or/LC_MESSAGES/glib20.mo po/pa/LC_MESSAGES/glib20.mo po/pl/LC_MESSAGES/glib20.mo po/ps/LC_MESSAGES/glib20.mo po/pt/LC_MESSAGES/glib20.mo po/pt_BR/LC_MESSAGES/glib20.mo po/ro/LC_MESSAGES/glib20.mo po/ru/LC_MESSAGES/glib20.mo po/rw/LC_MESSAGES/glib20.mo po/si/LC_MESSAGES/glib20.mo po/sk/LC_MESSAGES/glib20.mo po/sl/LC_MESSAGES/glib20.mo po/sq/LC_MESSAGES/glib20.mo po/sr/LC_MESSAGES/glib20.mo po/sr@ije/LC_MESSAGES/glib20.mo po/sr@latin/LC_MESSAGES/glib20.mo po/sv/LC_MESSAGES/glib20.mo po/ta/LC_MESSAGES/glib20.mo po/te/LC_MESSAGES/glib20.mo po/tg/LC_MESSAGES/glib20.mo po/th/LC_MESSAGES/glib20.mo po/tl/LC_MESSAGES/glib20.mo po/tr/LC_MESSAGES/glib20.mo po/tt/LC_MESSAGES/glib20.mo po/ug/LC_MESSAGES/glib20.mo po/uk/LC_MESSAGES/glib20.mo po/vi/LC_MESSAGES/glib20.mo po/wa/LC_MESSAGES/glib20.mo po/xh/LC_MESSAGES/glib20.mo po/yi/LC_MESSAGES/glib20.mo po/zh_CN/LC_MESSAGES/glib20.mo po/zh_HK/LC_MESSAGES/glib20.mo po/zh_TW/LC_MESSAGES/glib20.mo
  +
  +build glib20-update-po: phony meson-glib20-update-po
  +
  +build meson-glib20-update-po: CUSTOM_COMMAND 
  + COMMAND = /usr/bin/meson --internal exe --unpickle /home/sherlock/repos/glib/linx0.20_build/meson-private/meson_exe_meson_c0c7345012ec4138f3f5369ae8bb8aac6a4edf00.dat
  + description = Running$ external$ command$ glib20-update-po$ (wrapped$ by$ meson$ to$ set$ env)
  + pool = console
  +
  +build docs/reference/glib/gvariant-specification-1.0.html: CUSTOM_COMMAND ../docs/reference/glib/gvariant-specification-1.0.rst | ../docs/reference/glib/gvariant-byte-boundaries.svg ../docs/reference/glib/gvariant-integer-and-string-structure.svg ../docs/reference/glib/gvariant-integer-array.svg ../docs/reference/glib/gvariant-string-array.svg /usr/bin/rst2html5
  + COMMAND = /usr/bin/meson --internal exe --capture docs/reference/glib/gvariant-specification-1.0.html -- /usr/bin/rst2html5 ../docs/reference/glib/gvariant-specification-1.0.rst
  + description = Generating$ docs/reference/glib/gvariant-specification-1.0$ with$ a$ custom$ command$ (wrapped$ by$ meson$ to$ capture$ output)
  +
  +# Test rules
  +
  +build meson-test: CUSTOM_COMMAND all PHONY
  + COMMAND = /usr/bin/meson test --no-rebuild --print-errorlogs
  + DESC = Running$ all$ tests.
  + pool = console
  +
  +build test: phony meson-test
  +
  +build meson-benchmark: CUSTOM_COMMAND all PHONY
  + COMMAND = /usr/bin/meson test --benchmark --logbase benchmarklog --num-processes=1 --no-rebuild
  + DESC = Running$ benchmark$ suite.
  + pool = console
  +
  +build benchmark: phony meson-benchmark
  +
  +# Install rules
  +
  +build meson-install: CUSTOM_COMMAND PHONY | all
  + DESC = Installing$ files.
  + COMMAND = /usr/bin/meson install --no-rebuild
  + pool = console
  +
  +build install: phony meson-install
  +
  +build meson-dist: CUSTOM_COMMAND PHONY
  + DESC = Creating$ source$ packages
  + COMMAND = /usr/bin/meson dist
  + pool = console
  +
  +build dist: phony meson-dist
  +
  +# Suffix
  +
  +build meson-scan-build: CUSTOM_COMMAND PHONY
  + COMMAND = /usr/bin/meson --internal scanbuild /home/sherlock/repos/glib /home/sherlock/repos/glib/linx0.20_build /usr/bin/meson -Dbsymbolic_functions=true -Dcharsetalias_dir= -Ddtrace=false -Dforce_posix_threads=false -Dgio_module_dir= -Dglib_assert=true -Dglib_checks=true -Dglib_debug=auto -Dgtk_doc=false -Dinstalled_tests=false -Dlibelf=auto -Dlibmount=auto -Dman=false -Dmultiarch=false -Dnls=auto -Doss_fuzz=disabled -Dpcre2:grep=true -Dpcre2:test=true -Druntime_dir= -Druntime_libdir= -Dselinux=auto -Dsysprof=disabled -Dsystemtap=false -Dtapset_install_dir= -Dtests=true -Dxattr=true
  + pool = console
  +
  +build scan-build: phony meson-scan-build
  +
  +build meson-TAGS: CUSTOM_COMMAND PHONY
  + COMMAND = /usr/bin/meson --internal tags etags /home/sherlock/repos/glib
  + pool = console
  +
  +build TAGS: phony meson-TAGS
  +
  +build meson-ctags: CUSTOM_COMMAND PHONY
  + COMMAND = /usr/bin/meson --internal tags ctags /home/sherlock/repos/glib
  + pool = console
  +
  +build ctags: phony meson-ctags
  +
  +build meson-cscope: CUSTOM_COMMAND PHONY
  + COMMAND = /usr/bin/meson --internal tags cscope /home/sherlock/repos/glib
  + pool = console
  +
  +build cscope: phony meson-cscope
  +
  +build meson-uninstall: CUSTOM_COMMAND PHONY
  + COMMAND = /usr/bin/meson --internal uninstall
  + pool = console
  +
  +build uninstall: phony meson-uninstall
  +
  +# build all: phony subprojects/pcre2-10.42/libpcre2-8.so.0.11.0 subprojects/pcre2-10.42/libpcre2-posix.so.3.0.2 subprojects/pcre2-10.42/libpcre2-16.so.0.11.0 subprojects/pcre2-10.42/libpcre2-32.so.0.11.0 subprojects/pcre2-10.42/pcre2grep subprojects/proxy-libintl/libintl.so.8 glib/gversionmacros.h glib/glib-visibility.h glib/libcharset/libcharset.a glib/gnulib/libgnulib.a glib/libglib-2.0.a glib/gtester po/ab/LC_MESSAGES/glib20.mo po/af/LC_MESSAGES/glib20.mo po/am/LC_MESSAGES/glib20.mo po/an/LC_MESSAGES/glib20.mo po/ar/LC_MESSAGES/glib20.mo po/as/LC_MESSAGES/glib20.mo po/ast/LC_MESSAGES/glib20.mo po/az/LC_MESSAGES/glib20.mo po/be/LC_MESSAGES/glib20.mo po/be@latin/LC_MESSAGES/glib20.mo po/bg/LC_MESSAGES/glib20.mo po/bn/LC_MESSAGES/glib20.mo po/bn_IN/LC_MESSAGES/glib20.mo po/bs/LC_MESSAGES/glib20.mo po/ca/LC_MESSAGES/glib20.mo po/ca@valencia/LC_MESSAGES/glib20.mo po/cs/LC_MESSAGES/glib20.mo po/cy/LC_MESSAGES/glib20.mo po/da/LC_MESSAGES/glib20.mo po/de/LC_MESSAGES/glib20.mo po/dz/LC_MESSAGES/glib20.mo po/el/LC_MESSAGES/glib20.mo po/en_CA/LC_MESSAGES/glib20.mo po/en_GB/LC_MESSAGES/glib20.mo po/en@shaw/LC_MESSAGES/glib20.mo po/eo/LC_MESSAGES/glib20.mo po/es/LC_MESSAGES/glib20.mo po/et/LC_MESSAGES/glib20.mo po/eu/LC_MESSAGES/glib20.mo po/fa/LC_MESSAGES/glib20.mo po/fi/LC_MESSAGES/glib20.mo po/fr/LC_MESSAGES/glib20.mo po/fur/LC_MESSAGES/glib20.mo po/ga/LC_MESSAGES/glib20.mo po/gd/LC_MESSAGES/glib20.mo po/gl/LC_MESSAGES/glib20.mo po/gu/LC_MESSAGES/glib20.mo po/he/LC_MESSAGES/glib20.mo po/hi/LC_MESSAGES/glib20.mo po/hr/LC_MESSAGES/glib20.mo po/hu/LC_MESSAGES/glib20.mo po/hy/LC_MESSAGES/glib20.mo po/id/LC_MESSAGES/glib20.mo po/ie/LC_MESSAGES/glib20.mo po/is/LC_MESSAGES/glib20.mo po/it/LC_MESSAGES/glib20.mo po/ja/LC_MESSAGES/glib20.mo po/ka/LC_MESSAGES/glib20.mo po/kk/LC_MESSAGES/glib20.mo po/kn/LC_MESSAGES/glib20.mo po/ko/LC_MESSAGES/glib20.mo po/ku/LC_MESSAGES/glib20.mo po/lt/LC_MESSAGES/glib20.mo po/lv/LC_MESSAGES/glib20.mo po/mai/LC_MESSAGES/glib20.mo po/mg/LC_MESSAGES/glib20.mo po/mk/LC_MESSAGES/glib20.mo po/ml/LC_MESSAGES/glib20.mo po/mn/LC_MESSAGES/glib20.mo po/mr/LC_MESSAGES/glib20.mo po/ms/LC_MESSAGES/glib20.mo po/nb/LC_MESSAGES/glib20.mo po/nds/LC_MESSAGES/glib20.mo po/ne/LC_MESSAGES/glib20.mo po/nl/LC_MESSAGES/glib20.mo po/nn/LC_MESSAGES/glib20.mo po/oc/LC_MESSAGES/glib20.mo po/or/LC_MESSAGES/glib20.mo po/pa/LC_MESSAGES/glib20.mo po/pl/LC_MESSAGES/glib20.mo po/ps/LC_MESSAGES/glib20.mo po/pt/LC_MESSAGES/glib20.mo po/pt_BR/LC_MESSAGES/glib20.mo po/ro/LC_MESSAGES/glib20.mo po/ru/LC_MESSAGES/glib20.mo po/rw/LC_MESSAGES/glib20.mo po/si/LC_MESSAGES/glib20.mo po/sk/LC_MESSAGES/glib20.mo po/sl/LC_MESSAGES/glib20.mo po/sq/LC_MESSAGES/glib20.mo po/sr/LC_MESSAGES/glib20.mo po/sr@latin/LC_MESSAGES/glib20.mo po/sr@ije/LC_MESSAGES/glib20.mo po/sv/LC_MESSAGES/glib20.mo po/ta/LC_MESSAGES/glib20.mo po/te/LC_MESSAGES/glib20.mo po/tg/LC_MESSAGES/glib20.mo po/th/LC_MESSAGES/glib20.mo po/tl/LC_MESSAGES/glib20.mo po/tr/LC_MESSAGES/glib20.mo po/ug/LC_MESSAGES/glib20.mo po/tt/LC_MESSAGES/glib20.mo po/uk/LC_MESSAGES/glib20.mo po/vi/LC_MESSAGES/glib20.mo po/wa/LC_MESSAGES/glib20.mo po/xh/LC_MESSAGES/glib20.mo po/yi/LC_MESSAGES/glib20.mo po/zh_CN/LC_MESSAGES/glib20.mo po/zh_HK/LC_MESSAGES/glib20.mo po/zh_TW/LC_MESSAGES/glib20.mo docs/reference/glib/gvariant-specification-1.0.html
  +
  +build all: phony  glib/libglib-2.0.a subprojects/proxy-libintl/libintl.so.8.p/libintl.c.o
  +build clean: phony meson-clean
  +
  +build meson-clean-ctlist: CUSTOM_COMMAND PHONY
  + COMMAND = /usr/bin/meson --internal cleantrees /home/sherlock/repos/glib/linx0.20_build/meson-private/cleantrees.dat
  + description = Cleaning$ custom$ target$ directories
  +
  +build clean-ctlist: phony meson-clean-ctlist
  +
  +build meson-clean: CUSTOM_COMMAND PHONY | clean-ctlist
  + COMMAND = /usr/bin/ninja -t clean
  + description = Cleaning
  +
  +build build.ninja: REGENERATE_BUILD ../meson_options.txt ../subprojects/pcre2-10.42/src/pcre2.h.generic ../meson.build ../subprojects/pcre2-10.42/src/config.h.generic ../subprojects/pcre2-10.42/meson.build ../subprojects/pcre2-10.42/src/pcre2_chartables.c.dist ../subprojects/gvdb/meson.build ../subprojects/proxy-libintl/meson.build ../subprojects/pcre2-10.42/meson_options.txt ../tools/meson.build ../tools/glib-gettextize.in ../glib/meson.build ../glib/glibconfig.h.in ../glib/libcharset/meson.build ../glib/gnulib/gl_extern_inline/meson.build ../glib/gnulib/gl_cv_long_double_equals_double/meson.build ../glib/gnulib/gl_cv_cc_double_expbit0/meson.build ../glib/gnulib/gl_cv_func_printf_precision/meson.build ../glib/gnulib/gl_cv_func_printf_enomem/meson.build ../glib/gnulib/gl_cv_func_printf_flag_zero/meson.build ../glib/gnulib/gl_cv_func_printf_flag_leftadjust/meson.build ../glib/gnulib/gl_cv_func_printf_flag_grouping/meson.build ../glib/gnulib/gl_cv_func_printf_directive_a/meson.build ../glib/gnulib/gl_cv_func_printf_directive_f/meson.build ../glib/gnulib/gl_cv_func_printf_directive_ls/meson.build ../glib/gnulib/gl_cv_func_printf_long_double/meson.build ../glib/gnulib/gl_cv_func_printf_infinite/meson.build ../glib/gnulib/gl_cv_func_printf_infinite_long_double/meson.build ../glib/gnulib/meson.build ../glib/gnulib/gl_cv_func_frexp_works/meson.build ../glib/gnulib/gl_cv_func_frexpl_works/meson.build ../glib/gnulib/gl_cv_func_ldexpl_works/meson.build ../glib/gnulib/gnulib_math.h.in ../glib/gtester-report.in ../glib/libglib-gdb.py.in ../po/meson.build ../docs/reference/meson.build ../docs/reference/gio/meson.build ../docs/reference/glib/meson.build ../docs/reference/gobject/meson.build /home/sherlock/repos/glib/linx0.20_cross_file meson-private/coredata.dat
  + pool = console
  +
  +build reconfigure: REGENERATE_BUILD PHONY
  + pool = console
  +
  +build ../meson_options.txt ../subprojects/pcre2-10.42/src/pcre2.h.generic ../meson.build ../subprojects/pcre2-10.42/src/config.h.generic ../subprojects/pcre2-10.42/meson.build ../subprojects/pcre2-10.42/src/pcre2_chartables.c.dist ../subprojects/gvdb/meson.build ../subprojects/proxy-libintl/meson.build ../subprojects/pcre2-10.42/meson_options.txt ../tools/meson.build ../tools/glib-gettextize.in ../glib/meson.build ../glib/glibconfig.h.in ../glib/libcharset/meson.build ../glib/gnulib/gl_extern_inline/meson.build ../glib/gnulib/gl_cv_long_double_equals_double/meson.build ../glib/gnulib/gl_cv_cc_double_expbit0/meson.build ../glib/gnulib/gl_cv_func_printf_precision/meson.build ../glib/gnulib/gl_cv_func_printf_enomem/meson.build ../glib/gnulib/gl_cv_func_printf_flag_zero/meson.build ../glib/gnulib/gl_cv_func_printf_flag_leftadjust/meson.build ../glib/gnulib/gl_cv_func_printf_flag_grouping/meson.build ../glib/gnulib/gl_cv_func_printf_directive_a/meson.build ../glib/gnulib/gl_cv_func_printf_directive_f/meson.build ../glib/gnulib/gl_cv_func_printf_directive_ls/meson.build ../glib/gnulib/gl_cv_func_printf_long_double/meson.build ../glib/gnulib/gl_cv_func_printf_infinite/meson.build ../glib/gnulib/gl_cv_func_printf_infinite_long_double/meson.build ../glib/gnulib/meson.build ../glib/gnulib/gl_cv_func_frexp_works/meson.build ../glib/gnulib/gl_cv_func_frexpl_works/meson.build ../glib/gnulib/gl_cv_func_ldexpl_works/meson.build ../glib/gnulib/gnulib_math.h.in ../glib/gtester-report.in ../glib/libglib-gdb.py.in ../po/meson.build ../docs/reference/meson.build ../docs/reference/gio/meson.build ../docs/reference/glib/meson.build ../docs/reference/gobject/meson.build /home/sherlock/repos/glib/linx0.20_cross_file meson-private/coredata.dat: phony 
  +
  +default all
  +
  diff --git a/glib/gmem.c b/glib/gmem.c
  index 7e19aed65..e5cc577fc 100644
  --- a/glib/gmem.c
  +++ b/glib/gmem.c
  @@ -36,10 +36,10 @@
   # define _XOPEN_SOURCE 600
   #endif
   
  -#if defined(HAVE_MEMALIGN) || defined(HAVE__ALIGNED_MALLOC)
  +//#if defined(HAVE_MEMALIGN) || defined(HAVE__ALIGNED_MALLOC)
   /* Required for _aligned_malloc() and _aligned_free() on Windows */
   #include <malloc.h>
  -#endif
  +//#endif
   
   #ifdef HAVE__ALIGNED_MALLOC
   /* _aligned_malloc() takes parameters of aligned_malloc() in reverse order */
  @@ -676,10 +676,11 @@ g_aligned_alloc (gsize n_blocks,
       }
   
     res = aligned_alloc (alignment, real_size);
  -#elif defined(HAVE_MEMALIGN)
  -  res = memalign (alignment, real_size);
  +//#elif defined(HAVE_MEMALIGN)
  +  //res = memalign (alignment, real_size);
   #else
  -# error "This platform does not have an aligned memory allocator."
  +  res = memalign (alignment, real_size);
  +//# error "This platform does not have an aligned memory allocator."
   #endif
   
     TRACE (GLIB_MEM_ALLOC((void*) res, (unsigned int) real_size, 0, 0));
  diff --git a/glib/gnulib/meson.build b/glib/gnulib/meson.build
  index c8040f648..117245775 100644
  --- a/glib/gnulib/meson.build
  +++ b/glib/gnulib/meson.build
  @@ -310,15 +310,15 @@ else
   endif
   
   if not gl_cv_func_frexp_works and gl_cv_func_frexp_broken_beyond_repair
  -  error ('frexp() is missing or broken beyond repair, and we have nothing to replace it with')
  +#  error ('frexp() is missing or broken beyond repair, and we have nothing to replace it with')
   endif
   if not gl_cv_func_frexpl_works and gl_cv_func_frexpl_broken_beyond_repair
  -  error ('frexpl() is missing or broken beyond repair, and we have nothing to replace it with')
  +#  error ('frexpl() is missing or broken beyond repair, and we have nothing to replace it with')
   endif
   
   math_h_config.set ('REPLACE_FREXP', gl_cv_func_frexp_works ? 0 : 1)
   math_h_config.set ('REPLACE_FREXPL', gl_cv_func_frexpl_works ? 0 : 1)
  -math_h_config.set ('HAVE_DECL_FREXPL', gl_cv_func_frexpl_decl ? 0 : 1)
  +# math_h_config.set ('HAVE_DECL_FREXPL', gl_cv_func_frexpl_decl ? 0 : 1)
   
   math_h_config.set ('REPLACE_ITOLD', 0)
   math_h_config.set ('REPLACE_HUGE_VAL', 0)
  @@ -330,7 +330,7 @@ else
     gl_cv_func_ldexpl_works = false
   endif
   math_h_config.set ('REPLACE_LDEXPL', gl_cv_func_ldexpl_works ? 0 : 1)
  -math_h_config.set ('HAVE_DECL_LDEXPL', gl_cv_func_ldexpl_decl ? 0 : 1)
  +# math_h_config.set ('HAVE_DECL_LDEXPL', gl_cv_func_ldexpl_decl ? 0 : 1)
   
   inf_tmpl = '''#include <math.h>
                 double x;
  diff --git a/glib/meson.build b/glib/meson.build
  index ebb4c74f1..23fc9c794 100644
  --- a/glib/meson.build
  +++ b/glib/meson.build
  @@ -414,7 +414,7 @@ libglib = library('glib-2.0',
     link_with: [charset_lib, gnulib_lib],
     dependencies : [
       gnulib_libm_dependency,
  -    libiconv,
  +#    libiconv,
       libintl_deps,
       libm,
       librt,
  diff --git a/linx0.20_cross_file b/linx0.20_cross_file
  new file mode 100644
  index 000000000..f39ca2808
  --- /dev/null
  +++ b/linx0.20_cross_file
  @@ -0,0 +1,17 @@
  +[host_machine]
  +system = 'linux'
  +cpu_family = 'linx'
  +cpu = 'linx'
  +endian = 'little'
  +
  +[properties]
  +c_args = []
  +c_link_args = []
  +
  +[binaries]
  +c = 'linx64-linux-gnu-gcc'
  +cpp = 'linx64-linux-gnu-g++'
  +ar = 'linx64-linux-gnu-ar'
  +ld = 'linx64-linux-gnu-ld'
  +objcopy = 'linx64-linux-gnu-objcopy'
  +strip = 'linx64-linux-gnu-strip'
  diff --git a/meson.build b/meson.build
  index fbade0506..174115a31 100644
  --- a/meson.build
  +++ b/meson.build
  @@ -2034,7 +2034,7 @@ if host_system == 'windows'
     # any external library for it
     libiconv = []
   else
  -  libiconv = dependency('iconv')
  +  # libiconv = dependency('iconv')
   endif
   
   pcre2_req = '>=10.32'
  @@ -2153,45 +2153,45 @@ if host_system == 'linux'
     glib_conf.set('HAVE_SELINUX', selinux_dep.found())
   endif
   
  -xattr_dep = []
  -if host_system != 'windows' and get_option('xattr')
  -  # either glibc or libattr can provide xattr support
  -  # for both of them, we check for getxattr being in
  -  # the library and a valid xattr header.
  -
  -  # try glibc
  -  if cc.has_function('getxattr') and cc.has_header('sys/xattr.h')
  -    glib_conf.set('HAVE_SYS_XATTR_H', 1)
  -    glib_conf_prefix = glib_conf_prefix + '#define @0@ 1\n'.format('HAVE_SYS_XATTR_H')
  -  #failure. try libattr
  -  elif cc.has_header_symbol('attr/xattr.h', 'getxattr')
  -    glib_conf.set('HAVE_ATTR_XATTR_H', 1)
  -    glib_conf_prefix = glib_conf_prefix + '#define @0@ 1\n'.format('HAVE_ATTR_XATTR_H')
  -    xattr_dep = [cc.find_library('xattr')]
  -  else
  -    error('No getxattr implementation found in C library or libxattr')
  -  endif
  -
  -  glib_conf.set('HAVE_XATTR', 1)
  -  if cc.compiles(glib_conf_prefix + '''
  -                 #include <stdio.h>
  -                 #ifdef HAVE_SYS_TYPES_H
  -                 #include <sys/types.h>
  -                 #endif
  -                 #ifdef HAVE_SYS_XATTR_H
  -                 #include <sys/xattr.h>
  -                 #elif HAVE_ATTR_XATTR_H
  -                 #include <attr/xattr.h>
  -                 #endif
  -
  -                 int main (void) {
  -                   ssize_t len = getxattr("", "", NULL, 0, 0, XATTR_NOFOLLOW);
  -                   return len;
  -                 }''',
  -                 name : 'XATTR_NOFOLLOW')
  -    glib_conf.set('HAVE_XATTR_NOFOLLOW', 1)
  -  endif
  -endif
  +# xattr_dep = []
  +# if host_system != 'windows' and get_option('xattr')
  +#   # either glibc or libattr can provide xattr support
  +#   # for both of them, we check for getxattr being in
  +#   # the library and a valid xattr header.
  +# 
  +#   # try glibc
  +#   if cc.has_function('getxattr') and cc.has_header('sys/xattr.h')
  +#     glib_conf.set('HAVE_SYS_XATTR_H', 1)
  +#     glib_conf_prefix = glib_conf_prefix + '#define @0@ 1\n'.format('HAVE_SYS_XATTR_H')
  +#   #failure. try libattr
  +#   elif cc.has_header_symbol('attr/xattr.h', 'getxattr')
  +#     glib_conf.set('HAVE_ATTR_XATTR_H', 1)
  +#     glib_conf_prefix = glib_conf_prefix + '#define @0@ 1\n'.format('HAVE_ATTR_XATTR_H')
  +#     xattr_dep = [cc.find_library('xattr')]
  +#   else
  +#     error('No getxattr implementation found in C library or libxattr')
  +#   endif
  +# 
  +#   glib_conf.set('HAVE_XATTR', 1)
  +#   if cc.compiles(glib_conf_prefix + '''
  +#                  #include <stdio.h>
  +#                  #ifdef HAVE_SYS_TYPES_H
  +#                  #include <sys/types.h>
  +#                  #endif
  +#                  #ifdef HAVE_SYS_XATTR_H
  +#                  #include <sys/xattr.h>
  +#                  #elif HAVE_ATTR_XATTR_H
  +#                  #include <attr/xattr.h>
  +#                  #endif
  +# 
  +#                  int main (void) {
  +#                    ssize_t len = getxattr("", "", NULL, 0, 0, XATTR_NOFOLLOW);
  +#                    return len;
  +#                  }''',
  +#                  name : 'XATTR_NOFOLLOW')
  +#     glib_conf.set('HAVE_XATTR_NOFOLLOW', 1)
  +#   endif
  +# endif
   
   # If strlcpy is present (BSD and similar), check that it conforms to the BSD
   # specification. Specifically Solaris 8's strlcpy() does not, see
  @@ -2493,7 +2493,7 @@ if host_system == 'linux'
   endif
   
   summary({
  -  'xattr' : xattr_dep.length() > 0,
  +#  'xattr' : xattr_dep.length() > 0,
     'man' : get_option('man'),
     'dtrace' : get_option('dtrace'),
     'systemtap' : enable_systemtap,
  -- 
  2.34.1
  
