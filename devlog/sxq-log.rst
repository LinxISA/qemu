.. shixiaoqiang 版权所有 2022

:Authors:  shixiaoqiang
:Version:  1.0


石晓强的开发日志
**********************
20230504
=======================
块内部状态的初始值是未定义的,
所以 此patch(aef1993e3f482f6fa49b674e902f15621d07fe14)
取消了gen_linx_reset_bstate函数中对t寄存器的清除.

这样每个块开始时, t寄存器中有上个块留下来的值. 如果软件指令正确的话, 是不会出现问题的.

然后启动内核, 从核开始执行时qemu某个断言会出错,
经检查发现是由于bios中的acri/mret 指令没有设置rra_type(默认是t#0), 且被编译进了同一个块内,
rra_type被错误设置为了上个块的残留值(1), 结果导致acri/mret 错误地恢复了块内状态, 导致qemu断言出错.

bios修复这个问题后, 从核可以顺利启动, 但是内核走到最后"Run /sbin/init as init process"挂掉了.
用户态的指令可能有问题, 这里待定位.


20230406
=======================
scall 增加了一个imm参数

对于这个imm参数 qemu需要做的处理:

1. 解码时检查imm参数,  如果检查失败 生成 ill异常
2. 异常路由需要用到这个imm参数

于是, 解码时 imm参数会被存到 cpu结构体里, 异常路由时需要用imm 判断handle_acr

考虑下 redo scall 发生的场景:

1第一次scall 异常
2处理异常
3使用 eret_redo 返回
4再次触发redo_scall异常

2可能会再次使用scall指令触发异常.
对于qemu来说只用一个变量存储imm参数有可能出错, 因为4的imm参数要保证和1的一样.

使用栈来保存imm参数.
每次执行scall时将其imm放入栈中.
每次 异常路由时, 从栈顶取出一个元素.
这样可以解决scall 和 redo scall之间嵌套scall的问题.


20230301
=======================
仿照opensbi的驱动框架为linxinit添加了驱动框架, 将串口和reset_mmio的初始化添加到了驱动框架流程中.

此驱动框架涉及到的文件大概分为5类.

第1类文件实现了bios具体的功能, 如printf.c/reset.c, 这些功能被存放在静态device结构体里, bios将直接调用这些功能,
(打印, 唤醒从核等), 但是device结构体的初始状态是null, 需要被初始化.
这个文件也提供了初始化device的方法xxx_set_device.

第2类文件描述了具体的设备, 如8250.c/virt_reset_mmio.c, 提供了设备相关的功能实现, 提供了xxx_init方法,
xxx_init方法将调用xxx_set_device去填充device结构体. xxx_init方法可直接用于设备的初始化.

第3类文件使用fdt去定位到具体的设备, 然后初始化它, 如fdt_serial.c, 提供了fdt_xxx_init方法, 可通过设备树
定位到具体的设备名, 如8250/sifive/litex/htif等等.然后根据设备名去调用第二级的fdt函数.

第4类文件提供第二级的fdt函数, 如fdt_serial_uart8250.c, 第二级fdt函数会继续从设备树获取到设备的详细参数,
如mmio_base, 波特率, 频率等, 然后使用这些参数去调用xxx_init方法进行设备的初始化.

第5类文件是平台相关的platform文件, 如linx_virt.c, 提供了platform结构体, 此结构体中指定了设备的初始化方法,
如串口使用fdt的方式初始化, reset设备直接制指定了具体的驱动, 如下:

::

    static int virt_reset_mmio_init(void)
    {
	    return virt_reset_init(0x2000);
    }

    struct platform_info platform = {
        .name               = "linx_virt",
        .console_init       = fdt_serial_init,
        .reset_mmio_init    = virt_reset_mmio_init
    };

主核cool_boot过程中时, 可直接调用platform.console_init()和platform.reset_mmio_init()来完成初始化.

如果后续要添加新的驱动, 只需要在platform中添加新的设备, 然后新增前4类文件, 最后在冷启动阶段初始化即可.
(如果指定了具体的设备, 仅需要新增前2类文件)



20230227
=======================
opensbi的驱动框架

首先 存在一些描述设备的结构体, 包含了一些函数指针, 他们初始状态是null, 需要被初始化.

有一些opensbi的功能函数, 如sbi_timer_event_start/hsm_device_hart_start等,
这些函数最终会调用到设备结构体里函数指针指向的方法.

驱动的初始化, 主要就是给这些函数指针赋值的过程.

其中, platform结构体里也包含了platform_ops_addr 包含了一系列的函数指针, 其中一些用于指定初始化驱动的方法.
这些方法大致分为两类, 一类是指定固定的设备, 另一类是使用fdt来确定设备, 他们的目的是一样的: 填充设备结构体的
函数指针. (填充结构体由主核完成)

目前串口, reset_mmio可以添加驱动(reset_mmio需要先添加到设备树, 可以类比sifive_test).

20230220
=======================
目前linxinit的多核启动功能已经调试完成, 现需要将linxinit移植到
东匡提供的Ubi框架里.仓库地址: https://codehub-dg-y.huawei.com/UBIOS/Ubi.git

Ubi框架用cmake构筑, 使用python+json来配置board的各种信息, 包括arch/lib/module/源文件/头文件等.

cmake 会把 core+lib编译成固件, module会被编译成多个so文件(elf格式)

然后所有的so文件会被写入一个img镜像中, 作为flash添加到qemu的启动参数中.

固件运行起来之后会将falsh里的module加载到内存中依次运行, 这样只需要加入不同的module, 就可以实现各种功能.

linxinit的功能可以作为一个module加进去.

大致流程已经搞清楚, 链接脚本&符号等问题可能还得分析一下.
大概明天可以着手移植.

20230207
=======================
将reset_mmio的patch基于linx-dev-acr开了个新分支: linx-dev-acr-reset0202.
做numa节点特性时测试了一下cpu唤醒功能.
bios加了测试代码, 主核会做test_wake(2), 来唤醒cpu2.

如何判断cpu2确实被唤醒, 有两个方法可以判断.

1. 在qemu启动参数加入in_asm,exec等log, 如果出现Trace-XX 2, 说明cpu2开始执行TB块, cpu2确实被唤醒.
2. 在bios C代码部分, 令其打印各自的lxlcid, 在打印0之后再次打印了2, 说明cpu2被cpu0唤醒.

但测试中发现, 被标记为halted的cpu不会被挂起, 而是跟着主核一同执行, 导致运行混乱.
这个补丁在年前是可以正常的, 移植到linx-dev-acr上出现这个问题, 说明在期间这个分支的提交导致了这个问题.

最后定位到问题在于 linx_cpu_has_work方法没有被实现, 会返回true.

主循环cpu_exec执行时, 会首先通过以下调用检查cpu是否在工作, 如果cpu没在工作会立即返回, 不执行TB块;
如果cpu已经在工作了, 则会清除halted标志.

::

    cpu_exec
        cpu_handle_halt
            cpu_has_work
                linx_cpu_has_work


将linx_cpu_has_work实现后, reset_mmio补丁可以正常工作, cpu2能被唤醒.

在期间还遇到一个bug, 在qemu执行中, 使用monitor/info registers, 打印被唤醒后cpu2的寄存器, 发现其pc始终为0.
cpu2肯定是被唤醒执行代码去了, 这个bug不算紧急, 可以晚点定位.

cpu2寄存器pc为0的原因找到了
这次测试比较hack, 实际上内核代码并没有给从核的ACR1.evbase寄存器赋值, 导致从核发生异常后直接跳到了0.



20230202
=======================
内核和bios之间service的接口, 内核使用SOFTWARE1异常来请求服务.

a7表示service的id, a0 -- a6来传递具体的参数.

多核唤醒服务, 要传递的参数有lxlcid, 内核启动地址, 私有参数地址等.
主核收到这个服务时, 要首先把从核内核启动地址记录在内存中(如数组),
等到从核执行到eret指令之前时, 使用数组中的地址来代替qemu传过来的地址放入elink寄存器中.


20230201
=======================
qemu中, 特权级发生改变时需要用到acr_check接口来判断本次转换是否合法.
acr_check需要用到acr_tree, 目前acr_tree的实现不合理, 原因如下

1. acr_tree被定义在 LINXHartArrayState 结构中, 并可以通过xxx_set_props设置值.
   且放在结构体里, 可能会让别人误解: acr_tree是可变的. (这个是不对的, acr_tree应该是写死的)

2. 每个节点的信息过多, 实际上仅需要{valid, parent_acr}两个成员就足够实现acr_check了.

对应的修改措施如下

acr_tree改为一个static const 结构放到acr_check所在文件里, 只供acr_check使用.

相关实现已提交到 linx-dev-acr-tree0118分支.

20230117
=======================
acr_check需要的接口如下,

check_acr_request        ACR 低 -> 高 是否可切换
check_acr_enter          ACR 高 -> 低 是否可切换

check_acr_valid          ACR是否是有效的
check_acr_comparable     两个ACR是否可比较


当前的实现, 存储ACR的结构设计不合理, 以后新增ACR时会修改旧ACR的信息, 不利于迭代.

节点信息应改为 {valid, parent_acr}, 上面4个接口均可实现.

且valid信息要考虑 hypervisor的介入.

比如, 虚拟化开启时, ACR2应该是无效的. 而虚拟化未开启时, ACR3/4 应该是无效的.

这个用掩码/宏之类的来协助实现. 未实现的ACR的valid是0, 而实现了的ACR的valid根据其是否受到虚拟化影响有不同的valid值.

比如 ACR0.valid = 0b11
     ACR2.valid = 0b01
	 ACR3.valid = 0b10
	 HYMASK = 0b10
	 MASK = 0b01

	if v_is_enable
		acr_is_valid = ACRxx.valid & HYMASK
	else
		acr_is_valid = ACRxx.valid & MASK

ACR结构要体现不能被改变的性质, 可将其定义为静态局部变量, 这些变量仅在函数内使用, 4个接口可以放在一个函数里.


20230116
=======================
新开个分支dev-acr-smp, 编写bios多核代码, 编写之前这里写一下要做的特性.

opensbi的汇编和C语言均区分了cool_boot和warm_boot, 汇编的cool_boot部分额外做的事情主要是是分配和初始化scratch空间,
C语言的部分主要是各种驱动的init做了区分, 主核要额外初始化/创建一些结构.

还没有驱动框架, linxinit暂时只在汇编部分区分cool/warm_boot.

启动核已经定死是0号hart, qemu可以保证只让0号启动, 其他核都进入hated状态, 不会动.
不需要写多核竞争部分, 0号hart做cool_boot, 其余hart等待唤醒做warm_boot.

cool_boot核首先要读MMIO设备确定hart的总数量, 然后按照总数量去初始化每个hart的scratch空间,
opensbi的scratch上存了一大堆信息, 目前linxinit暂时只有 a0--a2 三个gpr需要存储.

然后warm_boot核启动时, 要根据自己的hartid去算出自己scratch的位置, 去认领.

fw_end
+---------+
|         |
| hart0   |  --> scratch0
|         |
+---------+
|         |
| hart1   |  --> scratch1
|         |
+---------+
|         |
| hart2   |  --> scratch2
|         |
+---------+

有了多核后, 串口的输出打印就要加上锁了(可能暂时也不需要?), 可以考虑把内核的锁移植过来.

实施方案::
    主核(0号)启动后设置异常入口, 然后先设置自己的scratch空间, 设置好之后主核设置栈, 然后调用C函数set_scratch(),
    在set_scratch()里, 主核首先读MMIO设备得到hart总数, 然后主核初始化其他核的scratch空间.
    这里可以用for循环去初始化内存, 比较方便.
    然后退出set_scratch(), 调用init()函数. 如果栈够用, 可以不用退出直接调用init()函数.

    从核启动后设置异常入口, 然后根据自己的hartid去认领自己的scratch空间, 最后调用init函数.

    在init函数中, 只需要主核打印logo.

20221220
===============================================================================
按照wz的补丁, 在riscv_hart_realize函数里令从核初始化为halted状态.
实际上打开了CPUState->start_powered_off成员.然后 linx_cpu_reset 复位时,
根据 start_powered_off, 会将 cpu 处于 halted 状态.

如果想要激活halted的核, 应该首先关闭start_powered_off,
然后再次执行一次linx_cpu_reset,这样从核的halted状态就没了.

LXLC reset MMIO的代码可以这样写,

模仿sifive_test设备, 写出大部分代码, 对它的写操作, 会转换成操作对应的核的状态,
首先要读出命令段cpu的编号, 然后得到对应cpu的CPUState,
将它的start_powered_off成员关掉, 最后执行linx_cpu_reset函数去激活它.

LxLC_reset.c文件和sifive_test代码放同一个目录下.
另外LxLC_reset.c文件要调用linx_cpu_reset函数,而linx_cpu_reset函数是target/linx/cpu.c下的静态函数,

需要弄个全局函数把linx_cpu_reset包一下, 或者把这个linx_cpu_reset变为全局函数.

LxLC Reset  0x2000 + 0x1000

控制段[0--8]   命令段[16--31]
 1                  从核             从核被激活开始取指令执行
 1                  主核             主核被激活开始取指令执行


20221217
====================================
现在linxinit使用RV的特权级指令和CSR已经可以启动内核了, sbi服务流程也完成了,
现在可以考虑将RV特性替换成ACR的内容.
先列举一下需要改动的地方.

1. 委托机制, 可能要使用静态配置, bios可以不配置.
2. trap入口, MTVEC要使用EVBASE代替.
3. trap跳转方式, 要由直接跳转改为向量表机制.
4. MCAUSE寄存器要改为ECAUSE寄存器, 寄存器布局也有改变.
5. trap的退出, mret改为eret; mepc改为elink; 另外退出时要去读st.sz的信息.
6. sysget/set 两条指令要改成 SSR的相关指令, 行为应该不变.

拉一个分支做替换.

20221215
====================================
opensbi的time是这样工作的.
内核发起一个SBI_EXT_TIME调用, 请求改变timecmp的值, 新值通过a0传递,
opensbi去处理这个调用, 改变timecmp的值.
看起来似乎很简单, bios这边只需要判断出SBI_EXT_TIME, 然后再给timecmp的内存写值就行了.

但是实测了一下bios这边收不到SBI_EXT_TIME调用, 原来在这之前内核会发起SBI_EXT_BASE调用,
来判断bios都支持哪些extension.

内核启动时会打印以下内容, 表示opensbi已回应这些SBI_EXT_BASE调用::
    SBI TIME extension detected
    SBI IPI extension detected
    SBI RFENCE extension detected
    SBI v0.2 HSM extension detected

如果bios不支持TIME extension的话, 内核就不会再发 SBI_EXT_TIME调用了, 也可能还有其他坑,
现在bios的secall服务的流程基本上实现了, 具体的服务就先不管了.
下一步去移植opensbi的libfdt和串口驱动, 定时器暂时不弄了.

20221213
====================================
linxinit现需要使用C代码作为异常入口, 这里分析一下opensbi的相关代码.

opensbi的mscratch寄存器用途如下, 高地址方向放了一个结构体, 低地址方向当作栈使用.

CSR_MSCRATCH <---
fw_start
fw_size
next_arg1
next_addr
next_mode
warmboot_addr
platform_addr
hartid_to_scratch
trap_exit
tmp0
options
EXTRA_SPACE

mscratch结构体存了很多信息, 来决定每个hart的运行状态(不同的hart可能要执行不同的OS),
linxinit的结构体暂只存3个成员: qemu传进来的a0, a1, a2, 后续需要再加.

异常入口大概是这样

    异常入口地址
    将寄存器保存在栈
    执行sbi_trap_handler函数, 参数为sp
    从栈恢复寄存器

其中到异常入口时, 会判断该异常来自M-mode or 更低特权级, 如果来自M, 那么异常堆栈就是当前sp, 否则异常堆栈是mscratch.

确定了异常堆栈后, 会扩大栈空间, 并将包括旧的sp 的寄存器都存到栈里, 并更新sp.

寄存器数量是35, 除了32个gpr外, 还包括mepc, mstatus, mstatusH等几个寄存器.

之所以要保存一些CSR, 是因为sbi_trap_handler中可能会发生嵌套trap, 会覆盖掉这些CSR, linxinit不需要处理嵌套中断, 所以只
需要保存32个gpr就够了.

opensbi的secall服务需要事先使用sbi_ecall_init去注册(需要引入一堆数据结构), linxinit暂用switch case去调用各种服务,
后续可以考虑换成函数指针数组.



20221207
====================================
linxinit新增了fdt和驱动框架特性, 分析一下opensbi里相关的代码.

fdt
----
opensbi fdt相关的代码有两处: lib/utils/fdt 和 lib/utils/libfdt

后者是fdt库,包含了很多对fdt的操作, 是C语言代码, 直接搬过来应该就可以用.
前者是fdt库的使用示例, 展示了fdt库的用法.

驱动框架
----------
opensbi会固定执行多种驱动的代码, 支持多种单板, 每种单板所需要的驱动定义在各自的platform.c中.
opensbi执行到sbi_init后, 会在init_coldboot / init_warmboot中按照顺序初始化各种驱动.

platform.c中函数指针指向了初始化驱动的方法.

这些初始化代码执行到具体的驱动有两种方法:
1. 直接调用驱动代码, 如uart8250_init.
2. 通过qemu传进来的设备树匹配和选择驱动, generic单板用的这种方式.

linxinit也可以用类似的方法添加驱动::

    主核和从核跳转到内核之前, 执行一个C函数init().
    在这个函数里, 可以按需添加一些驱动的初始化函数, 这些函数要事先定义在driver目录下.
    直接执行, 不用通过函数指针.


测试
-------
可以把串口驱动移植过来, 使用fdt的方式初始化, 验证这两个特性.

20221202
====================================

linxinit的特性如下
1. 支持多核启动.
2. 支持向低特权级提供服务(暂只支持cpu_start服务).

服务可能有很多种, 由某个异常来发起服务, 用a7传递服务类型.
如果服务由内核发起, 那么处理服务需要保存和恢复寄存器, 需要类似mscratch
的SSR来指定hart的私有空间, 类似于opensbi的用法, 用来保存寄存器, 也能当栈用.

可以在把服务的入口地址按顺序排列, 进入异常后根据服务类型跳到不同的
入口函数去执行, 执行完后eret返回.

cpu_start服务用一个SSR去做, 往SSR指定bit置位就可以启动另一个hart.

类似opensbi, 主核执行cold boot流程, 从核执行warm boot流程.

主核要负责分配所有hart的私有空间, 然后设置异常入口, 发起cpu_start服务
去唤醒从核, 然后eret返回, 然后跳到内核所在的地址和特权级.

从核启动时处于halt状态, 等待主核发起cpu_start服务.从核启动后会去执行 cold boot
流程, 首先认领自己的私有空间, 然后设置异常入口, 跳到内核的地址和特权级.

目录组织:
start.S   主核从核 boot的代码
entry.S   异常处理入口和服务入口函数
xxx.S     每个服务都要一个S文件.

分布开发, 首先只启动一个核, 然后可以加上cpu_start服务支持多核, 最后支持更多的服务.


20221201
=====================================================

目前linxinit的一些基础设施可以先看一下::
    a. 普通内存申请释放，b. 基础数据结构，c. 锁的实现


opensbi锁的实现(主要代码在lib/sbi/riscv_locks.c)
-------------------------------------------------

定义了一个结构体用来表示锁. ::

    typedef struct {
    #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
           u16 next;
           u16 owner;
    #else
           u16 owner;
           u16 next;
    #endif
    } __aligned(4) spinlock_t;

根据端序的不同排列方式也不同, 这样做的效果是: 用带w的指令去访问(spinlock_t*)地址,
拿到的结果是next|owner这种形式(两个值各占16bit).

上锁操作: next++
解锁操作: owner++
处于锁定状态: next != owner
处于未锁定状态: next == owner

根据以上特性提供了几个函数 ::

    bool spin_lock_check(spinlock_t *lock);     检查该锁状态是被锁定?

    bool spin_lock_unlocked(spinlock_t lock);   检查该锁状态是未锁定?

    bool spin_trylock(spinlock_t *lock);       尝试上锁, 成功返回true, 失败返回false

    void spin_lock(spinlock_t *lock);          上锁, 如果失败会堵塞循环, 直到其他hart解锁

    void spin_unlock(spinlock_t *lock);        解锁

访问共享资源的场景都会用到这个锁 ,目前可以尝试用block的汇编实现这个锁.


opensbi动态内存的分配(主要函数sbi_scratch_alloc_offset)
--------------------------------------------------------
mscratch为hart指示了一段私有空间, 上下各4k, 向上的4k用来当栈, 向下增长的4k保存了一个结构体, 剩余的
空间用来做动态内存分配, 参数只有一个size, 分配规则是要多少给多少, 分配后不会回收,
如果不够了就返回0, 表示分配失败.

用一个全局变量extra_offset来表示剩余的空间还有多少. ::

    注意: 一个变量, 所有hart共用这一个变量, 说明一次能给所有hart分配这个空间.

这块空间跟通常的堆内存不一样, 并没有那种申请个堆内存去存字符串的那种用法.

真正的用法是::
    想存个东西到私有空间, 但是sbi_scratch结构体里没定义这个成员, 于是使用sbi_scratch_alloc_offset
    函数去申请一个空间去存这个东西.
    看了下用到这个函数的场景, 存放的都是"offset"类的东西, 跟sbi_scratch结构体里的成员的宏
    SBI_SCRATCH_FW_START_OFFSET差不多.

这个动态内存感觉暂时用不到, 目前linxinit不需要处理太多服务, 需要用到的东西直接定义到结构体就行了?


基础数据结构
--------------
目前看到有一些操作字符串的行为, 和fifo数据结构, 都是用C语言实现的, 应该能直接搬过来.


20221017
===================================
v0.14的指令改动,lconst和sysset/sysget要改成16bit.
参照**LinxISA SEG 2022-10-11 BlockISAv014**

sysset/sysget
--------------
原本包含在编码里的csrid改为用T寄存器去索引.
这两个指令的改动很简单,只需要改一下编码格式和翻译函数就可以了.

ppt中没有找到编码格式, 暂时认为是op8-1-reg格式, op_code保持不变.

lconst
--------
原本包含在编码里的立即数现在放到了body的下面, 而lconst的参数变成了该指令相对于
立即数的偏移.
分成了3条指令, lconst.4bs lconst.4bu 和 lconst.8b,分别用于取不同大小的立即数.

编码是一个新的格式, 其中offset是8bit, 其偏移最大值是2^8.
但是body_size的最大值是2^10, 在一些情况下(比如body较大), 8bit是不够索引的.
使用lconst指令时要保证offset在2^8以内.


20221010
===================================
用qemu-linx去测试spec2017的几个测例,发现有几个C++测例会产生abort错误,
经定位发现是C++的异常处理特性导致的.

写了一个最简单的测例,使用linx64-linux-gnu-g++静态编译,也会有相同的错误.
这个程序在x86服务器可以正常编译执行.
::

    int main()
    {
        try {
            throw 1;
        }
        catch(int x) {
            return 0;
        }

        return 0;
    }


收集exec日志,分析其调用栈大致是下面这样.

::

    main
        __cxa_throw
            _Unwind_RaiseException
                uw_init_context_1
                    uw_frame_state_for
                        _Unwind_Find_FDE
                        <中间略>
                abort


从uw_init_context_1开始,就都是glibc里的内容了, 后面的调用链也跟qemu的log对的上.
我参考的这里的代码: https://github.com/lattera/glibc

::

    uw_init_context_1 {
        ...
        if (uw_frame_state_for (context, &fs) != _URC_NO_REASON)
            abort ();
        ...
    }


是uw_frame_state_for失败导致了出错.
最后可以定位到出错的位置是_Unwind_Find_FDE函数返回了NULL值导致的这个错误.
_Unwind_Find_FDE有两个地方可以返回NULL.

主要代码

::

    _Unwind_Find_FDE {
        struct unw_eh_callback_data data;

        data.pc = (_Unwind_Ptr) pc;
        data.tbase = NULL;
        data.dbase = NULL;
        data.func = NULL;
        data.ret = NULL;

        if (dl_iterate_phdr (_Unwind_IteratePhdrCallback, &data) < 0)
            return NULL;

        return data.ret;
    }


dl_iterate_phdr函数的嫌疑很大,这个函数可以在man里查到,
data由回调函数去写, dl_iterate_phdr的返回值就是回调函数的返回值.
可能是回调返回值小于0,也可能没有正确写datat.ret的值.
回调_Unwind_IteratePhdrCallback比较长, 我没有去看.

C++的异常的throw,要用到glibc里的stack unwind功能,
就是不停栈回溯, 建立执行环境, 直到找到对应的catch.
但是这里进行stack unwind之前就挂掉了.

听编译器那边说可能是dwarf的问题.


20221008
===================================
qemu-usr信号机制大致是这样的:
发生异常,退出主循环,会根据异常类别同时设置一个信号放到队列,比如TARGET_SIGILL,然后会去处理这个信号
处理不同信号的方式保存在sigact_table静态数组里.

sigact_table数组,记录了1--64信号的处理行为.这个数组会被初始化为全0,
即按照默认处理方式处理(一般就是dump_core_and_abort退出)

如果遇到ecall执行134号系统调用rt_sigaction,就会修改sigact_table数组对应索引,即修改信号的处理方式.

现存在这样的问题:
有一个类似hello-test的rv/block混合程序,使用signal(SIGILL,handler)修改了SIGILL信号的handler.这个handler是一个rv函数.
这时, qemu会把sigact_table数组的SIGILL索引的handler改成rv函数的地址.

然后刻意制造了一个block的非法指令异常,这个异常发生后,会设置TARGET_SIGILL信号,
之后qemu会根据sigact_table数组处理这个信号.
执行setup_rt_frame,令pc等于对应的handler,并同时设置sp等几个寄存器.

这个非法指令异常发生时block并没有正常退出,bstate都有值,qemu仍然认为现在还是body.
于是riscv_tr_translate_insn里检查pc的断言就失败了.

解决方法:
在跳转signal的handler之前,清除bstate.
如果因为设置了handler而发生了跳转, 就认为之前的block已经结束了,又回到了layer1.



20220922 IOMMU (ongoing)
===================================
简介
-------
IOMMU,input–output memory management unit, 类似于MMU,
允许具有DMA能力的IO设备在虚拟内存环境下工作.
AMD的叫 IOMMU.
Intel的叫 VT-d.
RV的叫 RISC-V IOMMU, 官方文档地址: https://github.com/riscv-non-isa/riscv-iommu
目前还是0.1版本, 今年4月13号第一次commit, 是个很新的东西, 其文档100页.

目前支持IOMMU的硬件: https://en.wikipedia.org/wiki/List_of_IOMMU-supporting_hardware
目前只有Intel和AMD, 还有一些GPU.

DMA重映射
----------
IO虚拟化, 给每个设备建立一个翻译表,每个设备发起的DMA访问需要通过翻译表
将虚拟地址转化为物理地址.
这个设备的翻译表应该和相应的MMU页表相对应(位于同一个domain),约定二者访问的
是同一段地址空间.

这样的一个好处是可以提升虚拟化的性能.还可以防止DMA故障和DMA攻击的恶意访问.

如果没有IOMMU, guest运行时, 难以直接访问硬件, DMA设备无法直接访问guest的GPA,
需要host介入, 通常会增加IO操作的延迟.
有了IOMMU之后, host可以配置guest的翻译表,使其兼容guest的页表, 使二者位于同一个domain,
解决延迟问题, 设备请求地址GPA由IOMMU转换为HPA

中断重映射
-----------
同样也有一张翻译表, IOMMU会拦截IO设备产生的中断,然后根据翻译表将此中断发送给指定的CPU.
是VT-d的功能, 主要目的应该也是提升虚拟化的性能.

RV IOMMU
---------
文档中提到的一些使用场景
对于没有虚拟化的OS:
1. 保护操作系统免受错误设备的错误内存访问
2. 支持32bit的设备在64bit环境中工作
3. 支持将连续的虚拟地址映射到碎片的物理地址
4. 动态的中断重定向,比如将一个中断重定向到另一个hart
5. 共享虚拟寻址,就是设备和进程共享同一个地址空间

对于Hypervisor:
Guest可以直接控制IO设备,可以DMA重映射或者中断重映射.

对于Guest OS:
允许为每个设备配置为不同的Guest

文档的其余内容
一些数据结构的定义,各种的Directory-Table 页表等; 和转换过程的介绍; 还有mmio寄存器; 软硬件的guidelines等.

todo


20220920 分析rv的 aclint
===================================
主要分析的官方文档 https://github.com/riscv/riscv-aclint/blob/main/riscv-aclint.adoc
全名 Advanced Core Local Interruptor

提供了三个外设: MTIMER MSWI SSWI, 都直接映射到内存.

MTIMER
------------
分为 MTIME和MTIMECMP,
其中MTIME是 多个hart共享的, MTIMECMP每个hart单独有一个.


MSWI SSWI
------------
分别是 M 软中断和 S软中断.
映射了很多,每个hart都有一对,最多支持4095个hart.

RV中对应的寄存器: mip 和sip 中的  MSIP 和 SSIP bit.


20220920 分析rv的 pmu
======================
rv的pmu是 hpm(Hardware Performance Monitor)

hpm寄存器
------------

对于M-mode,
mcycle
记录该core上的时钟周期数,同一个上的hart共享同一个mcycle

minstret
记录retired的指令数量

mhpmcounter3 -- mhpmcounter31
mhpmevent3 -- mhpmevent31
计数器记录对应的事件.

mcounteren
位域如下:
HPM31 HPM30 .... .... HPH4 HPM3 IR TM CY
分别控制 S模式 对 hpmcountern  instret time cycle的访问

mcountinhibit
位域如下:
HPM31 HPM30 .... .... HPH4 HPM3 IR 0 CY
如果bit被置一,相应的寄存器停止增长


对于S-mode,
scounteren
同mcounteren, 不过控制的是U-mode对那些寄存器的访问

hpm功能的使用
-------------
手册里说每一个hpmevent寄存器都能配置成某个事件, 这个事件是由平台决定的, qemu没有定义事件,
在qemu里,这一系列寄存器没有用.

在网上看到有人用hpm统计ICache的miss次数,用于统计miss率.
https://blog.csdn.net/tugouxp/article/details/118387088

shadow寄存器
-------------
cycle instret hpmcountern 这些都是相应 mcycle minstret mhpmcountern 的shadow 寄存器

time寄存器
-------------
time 寄存器, 属于 Machine-Level Memory-Mapped Registers,在手册里被单独分了个类,没跟CSR放在一块
分为mtime和mtimecmp 寄存器, 当 mtime >= mtimecmp 时, 将触发一个 Mtime 中断
这个中断只有在 mie.MTIE 被置一时才有效.



20220919
======================
分析了plic的官方文档 https://github.com/riscv/riscv-plic-spec/blob/master/riscv-plic.adoc
qemu中对应的文件为: hw/intc/sifive_plic.c

这个设备主要负责RV的外部S和外部M中断. 中断的target一般是hart.
该设备并没有和RV平台绑定,RV手册里出现的
"mip.MEIP is read-only in mip, and is set and cleared by a platform-specific interrupt controller."
指的是相应的"IRQ_S_EXT"或"IRQ_M_EXT"引脚被置位引起的mip寄存器的变化.



其寄存器包括
------------

Interrupt Priorities registers:
每个中断源的优先级,共1--1023个优先级.

Interrupt Pending Bits registers:
每个中断源的挂起标志位 IP

Interrupt Enables registers:
对于每个hart,每个中断源的使能


Priority Thresholds registers:
对于每个hart,接受的中断源优先级的阈值, 优先级低于该阈值的中断会被过滤掉.

Interrupt Claim registers:
Interrupt Completion registers:
用于每个hart的 claim ,completion行为. 是同一个寄存器的分时复用,地址相同.

中断的执行
------------

1. 首先每个中断源都会发送中断信号到 gateway
2. 相应中断源的IP 会设置为 "挂起"
3. 最高优先级的中断请求会被发送到 plic core,
4. plic core会将这个中断请求发送给所有阈值符合的hart, 该hart的mip寄存器的相应bit会被置位.
5. hart 处理中断, 先发送一个 claim 给plic core,
6. plic core会清除对应的 中断IP
7. 当hart处理完成之后, 还会发送 completion 给 plic
8. 这时gateway可以发送新的中断请求了,
9. 这时hart可以继续处理别的中断,而不需要从上下文返回 重新trap.





20220824
======================
目前larm上fixup的特性:

寄存器的改动
------------

新增了两个委托寄存器,ubedeleg sbedeleg, 用于将特定的异常指定为fixup异常,
是csr寄存器,只能用rv的 csr指令或者 block的 sysget sysset 访问.

header的改动
------------

两个新字段
fixup_addr: 复用了 br_offset 字段, 用于 "fixup.direct/indirect" 属性. 表示此块到Fixup handler之间的偏移.
所以,对于 direct/indirect 的fixup属性, 不允许 块的跳转属性是 direct, call 或 cond,否则要报EC_FIXUP异常

fixup_attr, 使用了battr中的 [5:6]两位,表示fixup属性.

fixup属性
------------

fixup.fall , 发生fixup异常,直接跳到下一个块(下一个块就是Fixup handler)

fixup.direct , 发生fixup异常 直接跳到fixup_addr指定的Fixup handler

fixup.indirect, 发生fixup异常 按照正常异常流程处理,然后在异常处理中由软件决定是否跳到fixup_addr处

当异常发生时,首先检查相应的bedeleg , 看这个异常是否是 fixup异常

然后根据fixup的属性去处理.

+----------------+------------------------------------------+----------------------------------------+
|                |         from U mode                      |            from S mode                 |
+----------------+------------------------------------------+----------------------------------------+
| fixup.fall     | 跳到Fixup handler(不保存ctx,不改特权级)  | 跳到Fixup handler(保存ctx,不改特权级)  |
+----------------+------------------------------------------+----------------------------------------+
| fixup.direct   | 跳到Fixup handler(不保存ctx,不改特权级)  | 跳到Fixup handler(保存ctx,不改特权级)  |
+----------------+------------------------------------------+----------------------------------------+
| fixup.indirect | 正常异常处理                             |         正常异常处理                   |
+----------------+------------------------------------------+----------------------------------------+

20220822
======================
fixup新特性

目前从立福那边获得的信息如下(还未上larm)
----------------------------------------

head中包含fixup的属性(fattr)和fixup代码的地址(fixup_addr)
当发生fixup异常时,会根据这两个信息做跳转.

fixup_addr复用了br_offset的位域,所以块的跳转类型是direct,call或者cond之一的,不支持fixup属性.

fattr分为:
fixup.fall : 表示发生fixup异常时,跳转到下一个块,不需要编码fixup_addr字段
fixup.direct <br_fixup> : 跳转到fixup代码处
fixup.indirect <br_fixup> : 跳转到异常向量处,然后再异常向量中可以再跳到fixup_addr处.

新增了两个寄存器 ubedeleg 和 sbedeleg
用于表示U模式下和S模式下被委托成fixup处理的异常

在qemu中断处理函数中的处理
--------------------------

首先判断此特权级发生的异常是否被相应的bedeleg委托,
然后根据fatter做相应的处理.

S模式下发生的fixup异常,不用改变特权级,就是S-模式下的异常,跳转的pc要根据fattr变动.

U模式下, 如果属性是fixup.direct或者fixup.fall, fixup异常在U模式下不是按异常处理了,
按直接块跳转处理,没有块内异常状态。不改变特权级.
U模式下的fixup.indirect 还是按照原先的异常流程处理.

20220808
======================
GRP local Index 新特性 & qemu的实现

新特性
-------------
set和get参数含义 由"gpr号"改为了"对mask的索引".
这个特性可以实现多个head对应同一个body的场景,减少code size.

qemu的实现
-------------
主要是修改set/get的语义,不能直接用参数访问gpr了,而是要通过gpr_index从低往高
数对应mask里的'1'.
这个操作比较复杂,要右移算出gpr_num,还要访问C语言的数组,TCG码是无法实现的,只能用helper实现.
set/get指令特别多,每个都调用helper的话,效率会降低不少,
所以采用下列方法实现:

在env里面加了几个数组,qemu把它当寄存器对待.
ogpr[32]和igpr[32], 分别表示 O0 -- O31 和 I0 -- I31.
然后blk_head时,用一个helper函数来填充ogpr和igpr.

然后set/get的参数O0 I0等,直接访问o/igpr就可以了(写死),每次执行head时都会更新o/igpr,不会出错.


这个实现放在了linx-dev-sxq-sget 分支.(临时patch,可能还会变动)

这个patch只关心此特性,只关心gpr,认为sgpr不存在.
1. set/get处理的是gpr,而非sgpr.
2. 取消了sr_mask以及mask的检查等.

todo
-------------
1. 修改set/get的测试用例.
2. 和内核编译器等讨论
3. 在新patch上实现sgpr的取消.



20220803
======================
分离head body反汇编的方法:
翻译完commit之后立即打印相应head和body的反汇编,原先反汇编的地方只负责rv的反汇编.

翻译commit之后,相应的head已经执行,env里的信息都是最新的.

head反汇编内容较少,可以不用访问内存,直接使用env里的信息拼凑,如果少了那些信息,直接在翻译head时加上这个信息即可.
body的反汇编可以借助env里的信息,bpc tpc1/2之类的.

主要实现逻辑在disas/linx.c里,向外暴露一个全局函数
translate.c里调用这个全局函数,完成所有反汇编的打印.

这个实现不应该更改任何公共代码.
遇上中断异常等情况,也没有漏洞.

这个方案没有走qemu官方的in_asm框架,是非常hack的做法.
1. in_asm的调用链很长,经过了几个文件,用了很多static的内容,需要手动加入当中的必要内容.
2. rv的反汇编需要跳过head,需要保留一些原先的代码.

blockisa的0.13版本的一些特性更新了,优先做新特性.
in_asm这个问题优先级不高,而且实现繁琐,可以先放一放.


20220802
======================
把in_asm的那个问题用临时方案处理了,遇到无法访问的body直接终止in_asm的输出.
不过这样会导致用户态程序的好多body都没反汇编输出了.

彻底解决这个问题的最合适方法就是将head和body的反汇编分开,二者独立输出.

首先要知道
head和body 要分到两个独立的tb中, body可能因为跨页之类的原因分为多个tb

不考虑中断, body的执行分为这几下情况:
就算body出现跨页之类的情况,以下行为仍然不变

1. 第一次head 第一次body : 正常翻译, 翻译head 执行head, 翻译body,执行body

2. 第二次head 第二次body : 不需要翻译,直接执行, 执行head,执行body

3. 第二次head body从tpc开始执行 : head不需要翻译,直接执行,但body需要重新翻译tpc之后的部分

第3种情况可以忽略, 这些内容之前应该翻译过, 想看可以看之前的in_asm

直接看第1种情况,正常翻译时,反汇编如何分辨出body指令.

类比翻译,如何分辨出body: 翻译到head时,会设置一大堆数据:tpc1/2等,然后再翻译时,如果pc符合要求,
就认为这里要当作body翻译.
输出反汇编时,也可以参照翻译的做法:
翻译head,反汇编head,这一步要设置一些数据head_data(tpc1/tpc2之类的,可以在blk_head设置),然后反汇编时,
如果head_data存在,就当作body去输出反汇编,输出完要清除head_data,如果head_data不存在,就当成rv指令.

先不考虑中断,

取指 head
	-> 发生取指缺页异常
		-> 去处理异常, 如果遇到第一次执行的内容, 就正常翻译,正常in_asm
			-> 重新回到head地址,重新取指head,这次取到了
				->blk_head 翻译head,顺便设置head_data
					->执行head,然后取指body
						->发生取指缺页异常
							-> 同上,这里认为 这些内容不是第一次执行的,不会翻译, head_data被保留
								->然后重新取指body,并翻译tb,然后按照head_data输出body的反汇编,没有问题

现在考虑中断,看是否有冲突.

直接考虑M中断,发生时会跳到mtvec,这时只要head_data存在值,就会被当成body反汇编,这样就错了.
S中断在某些情况也有问题,这里就不展开说了.

当然这些问题肯定有办法解决,但是因为一个log功能,改动中断处理逻辑、加多余的state,这肯定是不合适的.

1. 感觉最好的做法就是把那个hack做法当成可选选项加到启动参数里.

正常情况下: 用户态的很多body都无法输出反汇编.
加上选项: 可以输出用户态body的反汇编,但是代价是body的取指异常触发的时机不对了.

2. 或者干脆用个数据结构把所有body的tb块的首地址全存起来,输出反汇编时先查表,查到了就按照body处理,
否则就按照rv处理.可以参考下qemu tb表的实现.


20220715
======================
中断处理中,只要bpc或tpc有值 ,就会认为是块内中断,
并保存块内状态,将vld置位.

这个做法忽略了原子块的情况, 原子块发生块内中断时 也同样会保存,恢复 块内状态,
但是原子块要重新执行.
恢复的sr_mask 会导致 body 中出现重复set的错误,

所以对于原子块的块内中断,不应该保存bstate.实际上原子块不置位st.vld就能保证sret
不恢复状态了.

ebstate相关的代码会进行优化,优化后如下:


    do_interrrupt {
        定义局部变量ebstate
        配置ebsate.st(函数)

        处理S
            使用linx_save_bstate(函数) 配置 ebstate其他部分
        处理M
            无ebstate的处理

        将ebstate赋值到 s/mebstate (函数)

    }

linx_save_bstate函数做的事情:

1. 保存bstate到reg寄存器
2. 给st.sz赋值
3. 给st.rmax赋值
4. 给st.vld赋值

原子块不需要做的事情 1 4
2 和 3 我就认为原子块也需要做吧.

那么"原子块的块间中断不需要保存bstate"这个语义就要求原子块不能执行save那个函数了.
save函数内的4件事要分到两个函数里.

save_bstate 1 4
set_st      2 3
整个流程变成这样

    do_interrrupt {
        定义局部变量ebstate
        配置ebsate.st_cause(函数)

        处理S
            if 不是原子块 {
                linx_save_bstate

            }
            linx_set_st
        处理M
            无ebstate的处理

        将ebstate赋值到 s/mebstate (函数)

    }

可以先把优化ebstate的patch提交,然后再处理原子中断的这个问题





20220714
======================
内核那边报出来一个问题: 测试某个测例时,
mask 检查功能检查出 重复set的错误

调试需要 ,代码做了一些改动

出问题的那个bpc, 我让它的每一次set操作都打印当前ibpc.

出问题的那个块的反汇编如下(为原子块):

1d4670:>--00004100 00008000 b9902002 8fb9dc0b >---bstart>-b.std, bnext.concat, battr:atomic.aq, bget:0x00004100, bset:0x00008000, ptr:------ size:0x8, bnext:------,
1d4680:>--fffffffe ffffffe2 00000000 00000000 >---bstart.concat>-- bnext.fall ptr:0x6420c bnext:0x1d4690

    6420c:>--0812                >---get>s0
    6420e:>--0044                >---lw>-[t#1,0]
    64210:>--0f92                >---set>a5,t#1
    64212:>--0e12                >---get>a4
    64214:>--4000                >---add>t#3,t#1
    64216:>--1fe1                >---slli>---t#1,32
    64218:>--1fc1                >---srai>---t#1,32
    6421a:>--c00e                >---sw>-t#1,[t#7,0]

现场如下: (ce-XX 0(11:33:53): 0x7fdc84f06980 [0000000000000000/00000000001d4670/00004200/ff000000]-
Trace-XX 0(11:33:53): 0x7fdc84f06b40 [0000000000000000/000000000006420c/00004200/ff300600]-

set reg_num: 15 ! pc: 0x6420c bpc: 0x1d4670 ibpc: 2-

>>>>save! sr_mask: 8000, tpc: 6420c
------------- CS_OUT_FROM_BLK(0): Addr(6420c=>ffffffff800032a0) with bpc=1d4670(6420c,6421c), Priv(0=>1) for `store_page_fault`
TREG_P:        8
SR_MASK:        8000

(中间一大堆处理,然后返回)

>>>>recovery! sr_mask: 8000, tpc: 6420c
------------- CS_IN_RECOVER(0) Addr (ffffffff815ac784=>1d4670) Priv(1=>0)

  TREG_P:        8
  SR_MASK:        8000

Trace-XX 0(11:34:07): 0x7fdc84f06980 [0000000000000000/00000000001d4670/00004200/ff000000]-
Trace-XX 0(11:34:07): 0x7fdc84f06b40 [0000000000000000/000000000006420c/00004200/ff300600]-

set reg_num: 15 ! pc: 0x6420c bpc: 0x1d4670 ibpc: 2
set dup, reg_num: 15, pc: 0x6420c bpc: 0x1d4670 ibpc: 2


可以看出: 执行最后一条sw指令时发生了缺页, 但是保存的tpc却是 body的第一条指令的地址,而不是发生异常的sw的地址.
导致返回时 重新从body开头执行, 导致 set指令和 恢复的sr_mask 重复,发生错误.

qemu回退版本,掩码检查功能去掉后,该现象仍然存在,只不过不会报掩码的错误了.

感觉跟原子块有关.



20220706
======================

qemu里有个全局变量qemu_loglevel,保存了当前输出log的信息,qemu_log_mask会根据其输出log


新实现的功能start_exec,执行到指定pc再开启exec_log,实现起来就是在cpu_exec主循环中检查pc
如果pc符合,就在qemu_loglevel中添加exec_log的flag.
这个主要用于在system下调试用户态程序.使用这个功能之前应该提前知道某个tb块的起始
pc(一般是head地址或者body的起始地址).
这个功能唯一的要求就是启动参数 -d 中不能加"exec"选项.
联系一下linx_debug中的关闭和启动log的功能.看看和start_exec有没有冲突.
linx_debug开关log的实现是这样的:使用一个tmp变量保存当前的qemu_loglevel,
关log:将qemu_loglevel保存到tmp后置0;
开log:将tmp中的值恢复到qemu_loglevel: qemu_loglevel = tmp.
linx_debug关闭log后,如果遇到start_exec_pc,应该如何处理?

1.不做处理,这时只有exec log 存在,然后linx_debug开启log,此时qemu_loglevel恢复
,exec_flag被覆盖,这显然是不符合期望的.

2.此时exec log被开启,但是其他的log仍然关闭,然后linx_debug将log开启时,恢复改为
qemu_loglevel|= tmp ,这样exec_flag被保留.

3.此时 qemu_loglevel 被恢复,且加上了exec_log.

2 和 3 两种做法都可以,3可能更符合使用习惯,exec_log被打开时肯定是希望其他的log也
有的,只看exec也看不出什么,不过3得把静态的tmp变量变成全局变量后放到公共代码区域.

再说一下用sr_mask实现的掩码检查功能.
在启动内核进入shell后,执行命令总是会出现 set 和 sr_mask 掩码错误的问题,查看反汇
编的掩码是没有问题的.

于是在保存bstate时和恢复bstate时打印了一下sr_mask的值,发现了这样的现象:

>>>>save! sr_mask : 0
>>>>recovery! sr_mask : 0
>>>>save! sr_mask : 4000
>>>>recovery! sr_mask : 4000
>>>>save! sr_mask : 0
>>>>save! sr_mask : c104
>>>>recovery! sr_mask : c104
>>>>recovery! sr_mask : c104

set dup, reg_num: 15  at cpu:0  bpc: 0x5c3340

set dup, reg_num: 2  at cpu:0  bpc: 0x5c3340
error, sr_mask: c904, smask: 8804, sr_mask != smask;  at cpu:0  bpc: 0x5c3340
>>>>save! sr_mask : 0
>>>>recovery! sr_mask : 0
>>>>save! sr_mask : 0
>>>>recovery! sr_mask : 0

连续发生了两次中断,也save了两次,然后恢复的时候发生了错误.

错误的恢复:    c104
smask:         8804
错误的sr_mask: c904

可以看出,在错误的c104上置位8804,就会得到c904,同时第2位和15位会重复置位.

内核还没加sr_mask相关的内容,这个功能等内核添加之后再测试吧.



20220704
======================
sr_mask 是ebstate里的寄存器,只有在中断并发生save_ebstate时,sr_mask才有意义.
需要在qemu本地(cpu)添加一个sr_mask变量,用于保存set操作的历史,然后发生中断时,被save到
相应的ebstate中.(以下sr_mask都是指cpu里的本地sr_mask)

如果没有发生中断,sr_mask作为qemu中添加的本地状态,其更新流程应该是这样的:
在blk_head时,应该被清0,然后执行set指令时相应的bit要被置位.
对于BRANCH_CALL 和 BRANCH_INDIRECT_CALL 跳转类型,在commit阶段还需要把ra寄存器
对应的bit置位.

如果考虑中断,sr_mask的更新流程应该是这样:
在blk_head时,如果需要恢复状态(tpc有值,且不等于bpc),此时sr_mask不应该清0,因为sr_mask
和tpc一样是需要恢复的bstate.
如果这时tpc有值且等于bpc,表示这是一个redo_ecall,马上又要跳走了,此时sr_mask已经
被清0了,无需处理.
如果此时tpc没有值,说明没有bstate被恢复,也可能没有发生中断,sr_mask应该清0
另外blk_head其他的一些情况:body_size为0、原子块,sr_mask也需要清0.




下面说一下mask动态检查.
对于get,只需要检查 get的目标寄存器有没有在bget里就可以了,对于bget比实际的get
多的情况,是不需要报错的.
对于set,不但要保证set的目标寄存器在bset里,还要保证bset里的每一个寄存器都被set,
重复set的情况也不允许.

实现起来应该是这样的:
定义一个helper函数,会在set操作、get操作以及commit时调用.
对于get,只需检查目标寄存器是否是错的
对于set,需要检查目标寄存器是否正确,还需要借助sr_mask检查有没有重复set.
在commit调用,检查sr_mask是否和bset一致.


20220701
======================
执行 立福提供的用户态程序accept01, 搭配 0625 的busybox,
会出现错误,大致流程是这样的: accept01会发起一个user_ecall, 然后进入内核态去处理
中间还夹杂了一堆s_timer 和 m_timer 中断.

最后的现场是这样的: sret redo_ecall 回到了 一个body_size为0的块头,然后按照qemu
的流程继续执行,出错.可以知道 sepc是错的.

几乎每次都会出现这个错误.
(其中redo_ecall 是通过gdb看出来的)

因为这个错误是必现的,就给qemu加了一个功能: -d start_log 0x15a090
可以指定在某个地址之后再输出所有log,调试用户态程序很有用.

0x15a090这个地址是 accept01 执行 user_ecall之前的某个块头地址, 所以可以完整地
打印出 从user_ecall 到 挂掉 的全过程(包括 -d cpu)

之前 和立福一块调试了一下 这里补一下调试的过程

首先在entry.S里 几个 特定符号处(主要 关心_save_context 和 restore_all) 加了几条
block_qemu_debug_state 指令
发现: user_ecall 发生时 和 入栈前后的 寄存器都对的上,

但是最后的sret那个块的前后 和 user_ecall相比 , sp对不上, 其他的各种寄存器也都对
不上, 导致恢复的sepc错误,跳到一个错误的块头

另外,save_context 应该和 restore_all的执行相对应,执行次数应该一样

但是 根据 block_qemu_debug_state的输出内容, restore_all 符号 多执行了一次,
最后一次执行 走的的路径和前面执行的路径不一样, 是从 resume_userspace 过去的,

另外larm 新增了SR_MASK寄存器 ,[0 -- 31]用于记录 sr寄存器是否有效. [32 -- 63]保留.
这个寄存器 可以 记录 set的历史 , 可以实现: bset的动态检查 和 检查重复set错误.
如果 [32 -- 63] 可以用于 记录 get的历史, 那么bget的动态检查也可以实现了

20220623
======================
bpc要作为一个寄存器使用了,执行时, 实时更新,
set_blk_t 和 get_blk_t  要通过ibpc的实时值 去访问t寄存器.

我这边接着昨天的 linx-dev-sxq-ibpc 分支 去定位 非法地址的问题.
可能非法地址的原因不只是 qemu T寄存器访问错误.

看到这样一种情况
save! bpc:ffffffff81480240 tpc:ffffffff821e8540    sepc:ffffffff81480240   scause:d
before move:
ffffffe002e6b9d8        0       0       d889a6d4a0d9c504
0       0       0       0
after move:
0       0       0       0
ffffffe002e6b9d8        0       0       d889a6d4a0d9c504
[    3.572683] Unable to handle kernel paging request at virtual address d889a6d4a0d9c504
[    3.591464] Oops [#1]
[    3.599166] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-gf6f9185c75be #32
[    3.616922] Hardware name: riscv-virtio,qemu (DT)
[    3.626888] epc : strcmp+0x40/0xe0
[    3.638505]  ra : parse_mboxes+0x20/0x120
[    3.651906] epc : ffffffff81480240 ra : ffffffff810ef9c0 sp : ffffffe002e6b940
[    3.666854]  gp : ffffffff82d012e8 tp : ffffffe002ed8000 t0 : ffffffe002e6b8f8
[    3.681871]  t1 : ffffffffffffffff t2 : 0000000067a4272b s0 : ffffffe002e6b950
[    3.697019]  s1 : ffffffe03fdfd398 a0 : ffffffe002e6b9d8 a1 : d889a6d4a0d9c504
[    3.711844]  a2 : 0000000000000000 a3 : ffffffe03ee011c2 a4 : d889a6d51ee0e500
[    3.726454]  a5 : ffffffff810ef9a0 a6 : d889a6d51ee0e500 a7 : 0000000000000000
[    3.741063]  s2 : 0000000000000000 s3 : ffffffff82d020a8 s4 : 0000000000000000
[    3.755477]  s5 : ffffffff82712a70 s6 : ffffffe03fdfd398 s7 : ffffffe03ee011c2
[    3.770294]  s8 : ffffffe03fdfd480 s9 : 0000000000000000 s10: ffffffe002ea8810
[    3.784886]  s11: 0000000000000000 t3 : 0000000000ff0000 t4 : 0000000000000001
[    3.799366]  t5 : d889a6d51ee0e500 t6 : ffffffff8761f2ea
[    3.811015] status: 8000000000006120 badaddr: d889a6d4a0d9c504 cause: 000000000000000d
[    3.826598] ebstate.st: 0000000000000401
[    3.835626]  bstate.r0  : ffffffff821e8540 bstate.r1  : 0000000000000000
[    3.850566]  bstate.r2  : 0000000000000000 bstate.r3  : 0000000000000000
[    3.865004]  bstate.r4  : 0000000000000000 bstate.r5  : ffffffe002e6b9d8
[    3.879604]  bstate.r6  : 0000000000000000 bstate.r7  : 0000000000000000
[    3.894038]  bstate.r8  : d889a6d4a0d9c504 bstate.r9  : 0000000000000000

对应的log,可以看出来这些tb都不是第一次运行.为了方便给这些Trace编号
no.1
Trace-XX 0(11:25:13): 0x7f2948e94980 [0000000000000000/ffffffff810ef9a0/00004201/ff080000] parse_mboxes
Trace-XX 0(11:25:13): 0x7f2948e94fc0 [0000000000000000/ffffffff81f8e002/00004201/ff080000]-
no.2
Trace-XX 0(11:25:13): 0x7f2948193c40 [0000000000000000/ffffffff81480200/00004201/ff080000] strcmp
Trace-XX 0(11:25:13): 0x7f2948193dc0 [0000000000000000/ffffffff821e851a/00004201/ff080000]-
no.3
Trace-XX 0(11:25:13): 0x7f2948193f80 [0000000000000000/ffffffff81480240/00004201/ff080000] strcmp
Trace-XX 0(11:25:13): 0x7f2948194100 [0000000000000000/ffffffff821e8538/00004201/ff080000]-
riscv_cpu_do_interrupt: hart:0, async:0, cause:000000000000000d, epc:0xffffffff821e8540, tval:0xd889a6d4a0d9c504, desc=load_page_fault

找到出错的地方,第5条微指令 访问d889a6d4a0d9c504 出现了错误,这个地址是a1传进来的,往前面看看a1是在哪被设置的
这里对应no.3 Trace.
IN: strcmp
Priv: 1; Virt: 0
0xffffffff81480240:  00000c00 0000c000 ffe01c00 682f9c0b std_block.concat next:0xffffffff81480220, ptr:0xffffffff821e8538, attr:none, out_reg(a4,a5), in_reg(a0,a1)
0xffffffff81480250:  0000000d ffffffff 00000000 00000c00 std_block.conditional
>---0xffffffff821e8538:  0a12                  get             a0
>---0xffffffff821e853a:  0084                  lbu             [t#1, 0]
>---0xffffffff821e853c:  0f92                  set             a5, t#1
>---0xffffffff821e853e:  0b12                  get             a1
>---0xffffffff821e8540:  0084                  lbu             [t#1, 0]
>---0xffffffff821e8542:  0e92                  set             a4, t#1
>---0xffffffff821e8544:  8410                  setbpc.eq       t#5, t#2

no.2 Trace 里没有出现a1,略过

no.1 Trace,head和body写在一起如下

IN: parse_mboxes
Priv: 1; Virt: 0
0xffffffff810ef9a0:  000c1f06 000c6f06 0860e400 9e607c8b aux_block.concat next:0xffffffff81480200, ptr:0xffffffff81f8dfa6, attr:none, out_reg(ra,sp,s0,s1,a0,a1,a3,a4,s2,s3), in_reg(ra,sp,s0,s1,a0,a1,a2,s2,s3)
0xffffffff810ef9b0:  0000000e 00000039 00000000 00000800 aux_block.call
>---0xffffffff81f8dfa6:  ee09                  const           -144                            # 0xffffffffffffff70
>---0xffffffff81f8dfa8:  0212                  get             sp
>---0xffffffff81f8dfaa:  0400                  add             t#1, t#2
中间略过

>---0xffffffff81f8dff4:  e001                  addi            t#8, 0
>---0xffffffff81f8dff6:  2992                  set             s1, t#2
>---0xffffffff81f8dff8:  00000000008c6b24060a  lconst          9202468                         # 0x8c6b24

>---0xffffffff81f8e002:  e001                  addi            t#8, 0
>---0xffffffff81f8e004:  2022                  addtpc          t#2
>---0xffffffff81f8e006:  e001                  addi            t#8, 0
>---0xffffffff81f8e008:  2b92                  set             a1, t#2
>---0xffffffff81f8e00a:  6001                  addi            t#4, 0
>---0xffffffff81f8e00c:  e001                  addi            t#8, 0
>---0xffffffff81f8e00e:  2a92                  set             a0, t#2
>---0xffffffff81f8e010:  0c12                  get             a2
>---0xffffffff81f8e012:  0001                  addi            t#1, 0
>---0xffffffff81f8e014:  1292                  set             s2, t#1
>---0xffffffff81f8e016:  f32f                  sd              t#5, [t#8, -56]

注意到 no.1 Trace执行的tb信息有两个,分别是head和body,但是body不是从
tpc1开始的,而是从靠后的一个位置开始的.应该是sret回到这里的.往前搜一下它是如何分
成另一个tb的.

Trace-XX 0(11:25:13): 0x7f2948e94b00 [0000000000000000/ffffffff81f8dfa6/00004201/ff080000]-
IN:-
Priv: 1; Virt: 0
(block body) 0xffffffff81f8e002+22 in (0xffffffff81f8dfa6-0xffffffff81f8e018)
这里突然就开始新翻译了,注意ffffffff81f8dfa6,它是因为跨页主动分成新tb的.

中断什么时候发生的?再搜一下
Trace-_X 0(11:25:13): 0x7f2948e94b00 [0000000000000000/ffffffff81f8dfa6/00004201/ff080000]-
Trace-_X 0(11:25:13): 0x7f2948e94fc0 [0000000000000000/ffffffff81f8e002/00004201/ff080000]-
Stopped execution of TB chain before 0x7f2948e94fc0 [ffffffff81f8e002]-
riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000007, epc:0xffffffff81f8e002, tval:0x0000000000000000, desc=m_timer
riscv_cpu_do_interrupt: hart:0, async:1, cause:0000000000000005, epc:0xffffffff81f8e002, tval:0x0000000000000000, desc=s_timer

执行ffffffff81f8e002这个tb时发生了中断.这样就解释通了.再回到a1的来源问题
从上面可看到
a1看起来是由addtpc得到的,但中间同时出现了中断和跨页
内核里一堆addtpc,addtpc不太可能出错,可能中断和跨页的处理,对T寄存器的操作出现了错误?

记录一下这个错误发生的全过程:
1.head ffffffff810ef9a0的body第一次执行(翻译)时,就被分成了两个tb,一个是tpc1开始
第二个从 0xffffffff81f8e002开始.
2.然后执行第二个tb发生了中断,epc是该tb的第一个地址.
3.中断返回,接着从第二个tb开头执行,其中使用第一个tb的T寄存器给a1赋值
4.又执行了一个block后,把a1中的值当成一个地址访问,结果出错,a1存的地址非法.


20220622
======================
内核运行会随机报出非法地址的错误,定位到的一个原因是这样的:
翻译时的t#x 被写死成某个固定的寄存器;发生中断/异常保存上下文时,会把T寄存器
移位.如果一个body被分成了多个tb,并且已经执行过了(tb被保存在hash表中),第二次执行
时,tb之间发生中断,T寄存器被移位了,和原来翻译时写死的对不上了,指令会访问错误的T
寄存器.

为了解决这个问题,就在一个body会被分成多个tb的时候(跨页,TOO_MANY),把T寄存器按照
ibpc的规则移位,并且在tb开始的时候令ibpc为0,push到了linx-dev-sxq-ibpc分支.
但是似乎没什么用,问题照旧.
加-d选项几乎每次都有非法地址错误,不加-d选项有小几率发生非法地址错误.

然后在这个版本上debug,
在linx_save_bstate函数中打印了 bpc sepc scause 和 sebstate中的T寄存器.

(为了方便错误发生,使用-d in_asm,exec选项)

>>>>save! bpc:ffffffff81480f00 tpc:ffffffff821e89ba    sepc:ffffffff81480f00   scause:f
ffffffff827be848       6b      0       ffffffffffffff50
ffffffffffffff51        0       ffffffff827be849      0
[    5.555838] Unable to handle kernel paging request at virtual address ffffffffffffff50
[    5.574514] Oops [#1]
[    5.582193] CPU: 0 PID: 1 Comm: swapper Not tainted 5.16.0-rc3-g5cf1ec61272f #31
[    5.599738] Hardware name: riscv-virtio,qemu (DT)
[    5.609583] epc : memcpy+0x40/0x80
[    5.621150]  ra : param_sysfs_init+0x280/0x720
[    5.634036] epc : ffffffff81480f00 ra : ffffffff8220c260 sp : ffffffe002e6bba0
[    5.648856]  gp : ffffffff82d012e8 tp : ffffffe002ed8000 t0 : ffffffe002e3d040
[    5.663503]  t1 : ffffffffffffffff t2 : 00000000000000a7 s0 : ffffffe002e6bbb0
[    5.677957]  s1 : ffffffff82600060 a0 : ffffffffffffff50 a1 : ffffffff827be848
[    5.692551]  a2 : ffffffffffffff57 a3 : 0000000000000002 a4 : 0000000000000001
[    5.706834]  a5 : ffffffffffffff50 a6 : ffffffe002e1b000 a7 : 0000000000000064
[    5.721318]  s2 : ffffffe002e11360 s3 : ffffffff82867ea0 s4 : 0000000000001000
[    5.735862]  s5 : ffffffff82d020a8 s6 : 0000000000000dc0 s7 : ffffffff82863de0
[    5.750506]  s8 : ffffffff800534c0 s9 : ffffffff80052dc0 s10: ffffffff827c25c8
[    5.765123]  s11: ffffffff8286ab78 t3 : 0000000000000000 t4 : ffffffff8286ab78
[    5.779573]  t5 : 000000000000003d t6 : 0000000000000000
[    5.790676] status: 8000000000006120 badaddr: ffffffffffffff50 cause: 000000000000000f
[    5.806006] ebstate.st: 0000000000000401
[    5.814859]  bstate.r0  : ffffffff821e89ba bstate.r1  : ffffffff827be848
[    5.829707]  bstate.r2  : 000000000000006b bstate.r3  : 0000000000000000
[    5.843929]  bstate.r4  : ffffffffffffff50 bstate.r5  : ffffffffffffff51
[    5.858444]  bstate.r6  : 0000000000000000 bstate.r7  : ffffffff827be849
[    5.872816]  bstate.r8  : 0000000000000000 bstate.r9  : 0000000000000000
[    5.887021]  bstate.r10 : 0000000000000000 bstate.r11 : 0000000000000000
[    5.901361]  bstate.r12 : 0000000000000000 bstate.r13 : 0000000000000000
[    5.915705]  bstate.r14 : 0000000000000000 bstate.r15 : 0000000000000000
[    5.930058]  bstate.r16 : 0000000000000000 bstate.r17 : 0000000000000000

可以看到save打印的内容和内核打印的内容是一致的,这里列出重要的信息:
scause:f Store/AMO page fault
bpc:ffffffff81480f00
tpc:ffffffff821e89ba
异常地址:ffffffffffffff50
8个T寄存器:
ffffffff827be848       6b      0       ffffffffffffff50
ffffffffffffff51        0       ffffffff827be849        0

然后看反汇编的部分
head: ffffffff81480f00     ibpc
ffffffff821e89aa:>--0b12    0            >--get>a1
ffffffff821e89ac:>--0084    1            >--lbu>[t#1,0]
ffffffff821e89ae:>--0e92    2            >--set>a4,t#1-----
ffffffff821e89b0:>--0f12    3            >--get>a5         ffffffffffffff50
ffffffff821e89b2:>--0101    4            >--addi>---t#1,1  ffffffffffffff51
ffffffff821e89b4:>--0f92    5            >--set>a5,t#1
ffffffff821e89b6:>--a101    6            >--addi>---t#6,1
ffffffff821e89b8:>--0b92    7            >--set>a1,t#1
ffffffff821e89ba:>--7bec    8            >--sb>-t#7,[t#4,-1]  t#4 表示的是  T[4] 号寄存器,


发生异常的指令是 sb t#7,[t#4,-1],ibpc正好等于8,翻译时t#4写死成T[4],
ibpc=8,发生上下文保存时,T寄存器也不会移动.
[t#4,-1] 访问到了 ffffffffffffff50,这一点qemu执行的是对的.
这个地址来源于a5,也有可能是a5传进来的地址就是错的.



20220617
======================
把sret的bug修了之后,执行内核就进不去shell了(用的还是两个月前的旧busybox),还会
出现一些问题,先记录一下.

1.目前do_recovery中,如果tpc和tpc1/2对不上,会报一个非法指令异常.
  非法指令异常没有进行委托,会在M模式中处理(不会清理tpc),然后会跳转到handle_exception
  处理异常.
  但是执行handle_exception的第一个head,发现tpc有值,且和tpc1/2对不上(一定对不上)
  然后又会触发一个非法指令异常,又会到中断的M模式,这样无限循环.无限打印
  "the bstate is not this block, tpc:xxxxxx ..."这个log.
  如果运行正常的话,是不会有这个问题的,但和我们认为的"m-mode run RV code only"不
  一样了.
2.经常出现1中的错误(应该是内核进行sret恢复时出现了问题),就把非法指令异常改回了
  断言,在save、recovery 和head_do_recovery加了一些打印,想看看为什么会恢复失败,
  发现是内核修改了sepc的值,让sret返回,但是没有修改tpc为相应的值,导致tpc和sepc的
  值对不上,导致了错误.
3.当内核执行到"[xxx] clocksource: Switched to clocksource riscv_clocksource"时
  有概率出现出现这样:

    [    0.652597] clocksource: Switched to clocksource riscv_clocksource
    >>>>save! bpc:ffffffff824224a0 tpc:ffffffff8172d412    sepc:ffffffff824224a0   scause:8000000000000005
    M-mode int! 发生了数个M模式的中断
    >>>>recovery! tpc:ffffffff8172d412     sepc:ffffffff824224a0
    >>>>head_do_re! bpc:ffffffff80003180(这个地址是处理异常函数)   tpc:ffffffff8172d412

  sepc没有被修改,但是再次执行head的时候,就跑到handle_exception了,结果tpc对不上了,
  很是怪异.
4.在2的打印基础之上增加了打印ebstate,想看看ebstate的还原会不会发生错误.
  出现这样的现象:(仅仅打印tpc!=tpc1的情况).

>>>>save! bpc:ffffffff801174e0 tpc:ffffffff815c2018    sepc:ffffffff801174e0   scause:f
 0      ffffffe003273828        192b0b0 0

ffffffffc1c5ac00        65edc75c1c5ac00 1       0

>>>>recovery! tpc:ffffffff81542a86     sepc:ffffffff800574c0
 0      0       0       0
 0       0       0       0

>>>>head_do_re! bpc:ffffffff800574c0   tpc:ffffffff81542a86

 看起来是save时的ebstate和recovery时的ebstate不一致了,但更有可能是save和recovery
 不配套.这个问题原因未知,待定位.


20220613
======================
ecall目前没有清除bstate,进入interrupt后tpc1/2有值,会被当成块内中断去处理.临时做
了补丁,以配合内核调试:把interrupt中对ecall的处理,转移到了commit阶段的helper函数
里.
目前位置commit的所有情况都需要清除bstate了,但多种情况的处理还得用上bstate,这里
得考虑重写一下了.

20220609
======================
目前riscv和block的ecall都会触发RISCV_EXCP_U_ECALL异常,然后kernel会通过st.cause
来判断ecall的来源.LINX_EXCP_U_ECALL暂时没用了.

但是user模式还是靠LINX_EXCP_U_ECALL来判断ecall的来源,现在已经失效,无法运行helloworld
程序,换成st.cause来判断后可以正常运行helloworld.这个我暂时提交到linx-dev-larm0609
分支上了.

这样写有两个问题,
1.st.cause这些寄存器是对user模式不可见的,是#ifndef CONFIG_USER_ONLY的,为什么没报错
我暂时没有想通.
2.这样写过于hack了,riscv只用trapnr就可以分辨出ecall并让pc跨过ecall,block也应该用
相似的方法实现.

另外要加一条指令:FENCE.VMA <T#M>, <T#N>,行为是在commit阶段执行SFENCE.VMA.这个使用
之前的异常延迟提交就可以实现,flag可以放在CARG里.


20220602
======================
使用helper函数代替commit里的一大堆tcg码,尝试实现了一下

1.blk_type 在blk_head中更新,是实时更新的,
2.delay_flag 要保存在SBPC, 执行ecall等指令时会更新,SBPC也是实时更新的(这个待实现)
3.tcg码使用C语言代替
4.碰到gen_helper函数 把gen去掉,直接调用原helper函数
5.tcg_gen_exit_tb(NULL, 0) 作用是no chain ,加到最后面.
6.do_jump 函数主要功能是根据跳转类型给pc赋值,直接用C语言实现,gen_go_to之类的没有管.
7.在最后加入ctx->base.is_jmp = DISAS_NORETURN;表示该tb块结束.



下面是实现:
::

    ser_sgpr_to_gpr(ctx);

    target_ulong ctx_pc =  ctx->base.pc_next;


    gen_helper_handle_exec_and_branch(env,ctx_pc) {
        if(env->blktype == HEAD_TYPE_SYS && env->sbpc) {
            switch(env->sbpc|0x11) {
                case DELAY_ECALL:
                    env->pc = env->bpc;
                    env->bpc = LINX_ILLEGAL_INSTR_ADDR;
                    env->pc = ctx_pc;
                    helper_raise_exception(env,LINX_EXCP_U_ECALL);
                    break;
                case DELAY_EBREAK:
                    env->pc = env->bpc;
                    env->bpc = LINX_ILLEGAL_INSTR_ADDR;
                    env->pc = ctx_pc;
                    helper_raise_exception(env,RISCV_EXCP_BREAKPOINT);
                    break;
    #ifndef CONFIG_USER_ONLY
                case DELAY_SRET:
                    env->pc = ctx_pc;
                    helper_sret(env->pc,env,env->pc);
                    env->bpc = LINX_ILLEGAL_INSTR_ADDR;
                    break;
                case DELAY_SRET:
                    env->pc = ctx_pc;
                    helper_mret(env->pc,env,env->pc);
                    env->bpc = LINX_ILLEGAL_INSTR_ADDR;
                    break;
    #endif
                case DELAY_WFI:
    #ifndef CONFIG_USER_ONLY
                    env->bpc = LINX_ILLEGAL_INSTR_ADDR;
                    env->pc = env->next_bpc;
                    env->next_bpc = LINX_ILLEGAL_INSTR_ADDR;
                    helper_wfi(env);
    #endif
                    break;
                default:
                    g_assert_not_reached();
                }
                env->sbpc = DELAY_NONE;
        } else {
            env->bpc = LINX_ILLEGAL_INSTR_ADDR;
            switch(env->blktype) {
                case HEAD_TYPE_STD:
                case HEAD_TYPE_AUX:
                case HEAD_TYPE_SYS:
                    switch(env->brhtype) {
                        case BRANCH_FALL_THROUGH:
                            env->pc = env->next_bpc;
                            env->next_bpc = LINX_ILLEGAL_INSTR_ADDR;
                            break;
                        case BRANCH_DIRECT_LINK:
                            env->pc = env->bpc + env->bnext;
                            break;
                        case BRANCH_CALL:
                            env->gpr[1] = env->next_bpc;
                            env->next_bpc = LINX_ILLEGAL_INSTR_ADDR;
                            env->pc = env->bpc + env->bnext;
                            break;
                        case BRANCH_CONDITIONAL:
                            if(env->sbpc) {
                                env->pc = env->bpc + env->bnext;
                            }else {
                                env->pc = env->next_bpc;
                                env->next_bpc = LINX_ILLEGAL_INSTR_ADDR;
                            }
                            break;
                        case BRANCH_INDIRECT_LINK:
                            env->pc = env->spbc;
                            break;
                        case BRANCH_INDIRECT_CALL:
                            env->gpr[1] = env->next_bpc;
                            env->next_bpc = LINX_ILLEGAL_INSTR_ADDR;
                            env->pc = env->sbpc;
                            break;
                        case BRANCH_RET:
                            env->pc = env->sbpc;
                            break;
                        default:
                            linx_debug_not_reached();
                        }
                        break;
                default:
                     linx_debug_not_reached();
                }
        return;
        }


    ctx->base.is_jmp = DISAS_NORETURN; 
    tcg_gen_exit_tb(NULL, 0);


20220530
======================
1.之前实现的mask运行时检查功能,定义了一个新的上下文变量,需要在异常跳转的时候保存,
并在异常返回的时候恢复,跟bstate.ext的地位一样,这就需要larm那边修改,添加一个这个
功能的寄存器.
2.commit里的延迟异常处理和跳转需要改成运行时判断,SBPC要代替delay_exec_ins的作用,
这种复杂的逻辑应该要用helper函数实现,
当前的状态,gen_tcg 和 C语言代码混合.它们一个是执行时的,一种是翻译时的,现在要全
部用helper函数实现,感觉很困难,目前完全没有思路.



20220526
======================
SBPC要替代原本的delay_exec_ins的作用
原本的delay_exec_ins只在sys block下起作用.现在使用sbpc代替它.
commit中有以下代码:
::

    if(是sys块 && sbpc有delay_flag) {
        就去处理那些flag,并引发异常.
        将sbpc清空
    }else {
        根据跳转类型去计算下次bpc的值,sbpc要参与.
    }

说明sbpc的delay_flag功能是和原本的sbpc功能互斥的,二者不会产生冲突.



20220525
======================
把之前推出的mask运行时检查的方案实现了,放到了linx-dev-sxq-mask新分支上.

简单地说就是每次set和get时,都会检查目标寄存器是否合法,并给tmp_mask置位,然后
commit时tmp_mask和mask会比较.

使用之前编写的SMC代码测试了一下,自修改代码修改了get的目标寄存器,可以如预期的报出错误.

但是测试kernel的时候却出现了问题,在handle_exception符号下的block中,tmp_mask总是
比mask多几个,而且每次出现的现象还不一样.

分析了一下:当发生异常时,会跳到handle_exception符号的head处,但是之前运行的tmp_mask
仍然保留,造成错误.
于是尝试性地在blk_head中把tmp_mask清零,又试了一下,还是有错误,但是出错的地方不再
是handle_exception了,而且出现地频率也比较慢.
这是因为当bstate.vld=1时,跳到handle_exception处的head处,把tmp_mask给清零了,然后
跳回去时接着当时的tpc执行,造成的结果就是tmp_mask会少一些bit.

为了应对这个情况,应该这样处理:

在blk_head中,如果vld=0,就应该把tmp_mask清零
             如果vld=1,应该把tmp_mask备份一下(ttmp_mask),然后把tmp_mask清零.
在commit中,如果vld=0,就不做处理
           如果vld=1,就把ttmp_mask复原.



20220520
======================
大概了解了一下qemu SMC机制.

使用昨天第一个a.out来分析,直接使用qemu-linx可以正常运行.

加上gdb调试,会产生SIGSEGV信号,但是摁c可以继续运行,调用栈为:
main
-> cpu_loop
--> cpu_exec
---> cpu_loop_exec_tb
----> cpu_tb_exec
-----> code_gen_buffer
------> <signal handler called> 此处产生了信号
-------> host_signal_handler
--------> handle_sigsegv_accerr_write
---------> page_unprotect
----------> tb_invalidate_phys_page

猜测tcg中的store操作会引发 segment fault,产生一个 SIGSEGV 信号,
但是qemu不会退出,而是去处理这个信号,把相应的tb舍弃,然后会返回到主循环中继续运行.
运行到被修改地址时发现相应的tb没了,就会重新翻译.

再梳理一下tb的生成和in_asm的打印

同一个block的head和body被放在了两个不同的tb中.
当翻译head时, in_asm会打印head信息,并连带着后面的body反汇编一起打印出来
当翻译body时, in_asm只会打印一行简单的信息,比如:
IN:
(block body) 0x000100b0+30 in (0x000100b0-0x000100ce)
所以我昨天的理解是错的,经确认,昨天的test确实发生了重翻译.

下面可以再推一下block的mask运行时检查

1.如果head被修改,会重新翻译head的tb,执行blk_head,顺便检查mask,之后如果body没有
  相应的修改,那么mask_verify会报错,这里可以保持现状,不用修改.
2.如果body被修改,会重新翻译body的tb,set和get被修改后检查不出来的,这里应该加上
  tcg运行时检查
3.如果一个block的head和body在一个tb内被同时修改为另一组符合block规则的.
  按照运行tb的顺序,应该是先翻译head,然后blk_head -> mask_verify -> translator_lduw
  去读head对应body中的set&get,那么这条路径读到的body是新的还是旧的?如果读到的还
  是旧的,那么mask_verify就会报错.我认为应该是新的,因为内存已经被修改了.

目前编译器不支持把head放到data段,1 3点无法验证,但可以说通.
所以可以先把set get的tcg掩码检查先做了,这个会新开一个分支去做.
这些操作比较复杂,只用tcg中间码不一定能实现,可能需要helper函数


20220519
======================
通过更改汇编代码,将待修改代码放到了可读的data段,这样qemu没有报segment fault.
然后进行了几次尝试

::

    int a = 0;

    void test() {a = a + 13;}

    void hack() {//modify 13 to 14}

    int main () {
        test();
        hack();
        test();

        printf("%d\n",a);

        return 0;
    }

先-S编译成汇编,然后修改hack,使其修改test的13变成14,最后输出的结果是27,证明了qemu
对SMC的处理机制.
联系之前在网上查找到的结果,可以认为 qemu舍弃了被修改代码对应的tb,并发生了重翻译.

但是-d in_asm 只输出了一次test的旧代码,并没有输出修改后的test的新代码.
反汇编是在translator_loop函数的末尾输出的.
说明并没有通过
translator_loop->riscv_tr_translate_insn 这条路径去翻译程序.
这表示也不会有 blk_head和commit等函数.

于是又试了一次,使用hack在test中插入了一条错误的get指令,预期的结果:发生重翻译,
执行blk_head的时候qemu发生错误.但实际上程序正常执行,-d in_asm中也没有打印出test
的新代码.

目前编译器并不支持简单地把head放到data里,暂时测试不了,不过猜测跟上面的结果一样
之前的运行时检查的修改思路
::

    假设代码执行中,head的mask和body的set&get都有可能会被修改,
    1.要有2个TCGv变量,保存head的mask,并实时更新,它的值总是当前head的mask.
    2.set和get生成的TCG码中,首先,要确保目标寄存器在mask中,否则会直接报错.
    3.另外set和get执行时要操作一个TCGv tmp_mask,将对应的寄存器在tmp_mask中置位,
      commit时,要确保tmp_mask和head mask一致,否则会报错,commit之后,tmp_mask会清零,
      便于下次使用.

这个思路要求发生重翻译并及时通过blk_head更新head mask的值.但如果能发生重翻译的
话就用不着改运行时检查了.

看来得先把qemu的SMC机制搞清楚.

20220518
======================
昨天查到qemu对自修改代码(SMC)有应对机制,今天又进一步想了一下.
目前的情况:
::

    blk_head函数翻译tb,顺便检查mask,之后如果没有SMC,执行tb时mask永远不会出错
    如果出现SMC,qemu的机制会舍弃该tb,并断开链接,然后重新翻译tb,返回上一步
    这个机制保证了mask的正确.

改为运行时检查后,每次set get和commit都会检查mask,起到的效果和上面一样,但是频繁的
检查会损失一点效率.所以没有必要运行时检查mask bit.

然后尝试编写SMC,想要验证一下qemu的机制,但是使用qemu-linx运行都因为store指令
发生了segment fault,下面记录了过程.

我猜测了一下原因:代码段是只读的,怎么可以直接去写呢,应该需要其他手段编写.

需要其他手段去验证,比如去网上搜索其他架构的SMC例程,然后使用qemu-arch运行,记录
-d in_asm 输出,只要能输出修改后的代码,就证明了qemu确实会重新翻译.

但是网上关于SMC的例程比较少,而且大部分都是x86的,由于x86比较特殊,qemu中对x86的
SMC多做了一些处理,所以不能用x86的SMC验证,并且kvm也可能会产生影响.

另外服务器上好像只有riscv的工具链.

如果能找到riscv的SMC,就上qemu验证一下,找不到的话就算了吧.



修改head
::


    int myadd(int a, int b) {

        return a + b;
    }

    void myhack() {

    }
    int main() {

        int c;
        c = myadd(1,2);
        myhack();
        int d;
        d = myadd(3,4);
        return 0;
    }

使用-S选项生成汇编,和a.out,通过反汇编获得地址.
然后在myhack的body中添加微指令,去修改myadd的head中的mask.
但是最后用qemu-linx运行时出现segment fault

微指令大概是这样的
::

    lconst 地址
    const 新值
    sd t#1,[t#2,0]    //store系列指令

又尝试修改body,手段和上面差不多.
::

    const 0x1
    const 0x1
    const 0x1

有若干的const 0x1指令,我使用微指令修改其中某一个改为const 0x5.

如果-d in_asm可以输出修改后的代码,就表示qemu发生了重翻译.
但是运行结果仍然是segment fault,而且都是因为store系列指令出错的.







20220517
======================
为了应对自修改代码的情况,bit mask需要在执行时检查.
目前是:翻译时使用C代码检查一次,现在要使用TCG码去实现这个检查.
该特性是block特有的,要放在target/linx 目录下.
这个功能不紧急,而且实现和测试比较麻烦,会新开一个分支提交.

假设代码执行中,head的mask和body的set&get都有可能会被修改,
1.要有2个TCGv变量,保存head的mask,并实时更新,它的值总是当前head的mask.
2.set和get生成的TCG码中,首先,要确保目标寄存器在mask中,否则会直接报错.
3.另外set和get执行时要操作一个TCGv tmp_mask,将对应的寄存器在tmp_mask中置位,
commit时,要确保tmp_mask和head mask一致,否则会报错,commit之后,tmp_mask会清零,便于
下次使用.

主要难点在于第1点.
TCG码的生成是由C翻译代码控制的,生成的TB块对应着一段pc,然后执行的时候是直接执行TB块,
不关心C翻译代码.
但是只有在翻译的时候,C代码才能拿到head&mask并将它保存在TCGv mask中,翻译只有一次,
所以TCGv mask并不能实时更新.

update:
qemu在翻译head的时候生成的TCG码中写死了mask,可以保证head mask是实时同步的.
通过搜索可得:qemu提供了对自修改代码的应对机制.
https://github.com/azru0512/slide/blob/master/QEMU/QEMU-handle-self-modifying-code-01.txt(这个链接说了user模式的情况,system模式也有相似的机制,但要复杂得多)

简单地说,自修改代码会导致相关的tb块从hash表中移除,与其他tb的链接也会被打断.
这个机制保证了TCGv mask是实时更新的.

梳理了一下一些"-d" log的打印顺序.
 | -d int 打印时,相关指令已经被执行,可以说是执行完指令之后打印的(仅引发异常的指令)
 | -d cpu 紧接着exec打印
 | -d exec 执行tb块前打印,打印完之后,会立即去执行tb块
 | -d mmu 翻译时打印的,暂时不关心
 | -d in_asm 翻译完之后,执行之前打印

::


   |---------|
   |  翻译   |
   |  in_asm |
   |---------|

   |---------|
   |  exec   |
   |  cpu    |
   | 执行    |
   |         |
   |---------|
    (int)


20220516
======================
今天主要修改了 "-d cpu"输出的log格式,使bstate和bstate.ext和larm一致
另外只有在singlestep模式下,才会输出sgpr.


20220513
======================
使用最新的编译器重新编译了一遍hello world程序
昨天的segment fault问题没有了,现在程序可以跑到 switch RISCV_EXCP_U_ECALL,
并执行do_syscall了.

但是执行完之后会按照riscv的方式处理,令pc+4,跳过ecall指令.这个行为在block中
是错的,当ecall指令起作用时,这个block已经结束了,如果想要跳过ecall,pc需要指到
下一个块,而不是简单的加4.简单得加4后,pc指向了一个错误的地址,然后接着执行,就
全乱了.

还要处理的一个问题:head里bit mask代码修改,要改成执行时检查,要考虑到自修改代码.
当前的情况:
掩码检查mask_verify只会发生在head翻译的时候,这个检查是用C代码实现的,而不是TCG
中间码,head只会翻译一次,也就是说只会检查一次.

但是这个块可能会执行很多次,如果在执行中,set get 等代码被修改了,这个错误就检查
不出来了.

现在要在执行中(set get指令)加入这个检查,这样不光可以规避这个错误,而且还能使错误
发生的位置更精确.

这个特性是block特有的,代码需要放在/target/linx/文件夹下,那就只能从TCG中间码入手了.

思路大概是这样的,set和get生成的TCG中间码,要包含对mask的检查,在body被运行期间,每次
遇到set或者get,都会对某个全0 TCGv tmp_mask 置位,当body提交时,要保证tmp_mask和
mask一致,否则就会报错.提交过后,tmp_mask需要被清零.

这个思路假设block head不会被修改,这个问题优先级比较低,可以先放一下.


20220512
======================
编译器目前已经可以直接编译block用户态程序.不需要额外加 start.S文件.

编写hello world程序,使用--static选项编译, 分别使用riscv工具链和block工具链编译
得到 riscv_hello 和 block_hello

使用qemu-linx 运行 ,riscv_hello可正常运行 , block_hello 会segment fault

目标是,两者都必须要正常执行,需要有一个方法来区分 user程序是 riscv 的还是 block的

RISCV_EXCP_U_ECALL = 0x8,
LINX_EXCP_U_ECALL = 0x1B ,目前并没有途径可以得到linx这个结果,
所以这两个不能用来区分当前的user程序,要找到其他方法

先梳理一下user模式的流程:
::

    for(;;) {

    trapnr = cpu_exec(cs); // 执行主循环,获得异常值

    switch (trapnr)   // 根据异常,做相应的处理,

    case RISCV_EXCP_U_ECALL:    //riscv和block共用这里,这里肯定是有问题的
    case LINX_EXCP_U_ECALL:
    }

现在要定位segment fault是在何处发生的,使用gdb在switch(trapnr)打了断点,
因为猜测 错误发生在RISCV_EXCP_U_ECALL处
结果是断点没有触发,说明产生trap之前,或者是产生trap时,就发生了segment fault,
此时的堆栈如下:
::

    #0  0x00007fffe8012a02 in code_gen_buffer ()
    #1  0x0000555555699fb1 in cpu_tb_exec (cpu=cpu@entry=0x555555ad19e0,
        itb=itb@entry=0x7fffe8012900 <code_gen_buffer+75987>, tb_exit=tb_exit@entry=0x7fffffffda94)
        at ../accel/tcg/cpu-exec.c:372
    #2  0x000055555569b3a7 in cpu_loop_exec_tb (tb_exit=0x7fffffffda94, last_tb=<synthetic pointer>,
        tb=0x7fffe8012900 <code_gen_buffer+75987>, cpu=0x555555ad19e0) at ../accel/tcg/cpu-exec.c:985
    #3  cpu_exec (cpu=cpu@entry=0x555555ad19e0) at ../accel/tcg/cpu-exec.c:1144
    #4  0x00005555555c22c8 in cpu_loop (env=env@entry=0x555555adb950) at ../linux-user/linx/cpu_loop.c:39
    #5  0x00005555555b38ba in main (argc=<optimized out>, argv=0x7fffffffe3b8, envp=<optimized out>)
        at ../linux-user/main.c:962

看样子是主循环中的某次code_gen_buffer出错了.
使用-d in_asm 启动 block_hello,得到的最后信息是:_vfiprintf_r 这个符号下的head和
body,应该是跟printf有关,大概率是陷入系统调用时出的问题

system模式在opensbi和kernel上遇到了ecall的问题,我猜测user模式的错误也是ecall引起
的.

总之,目前的首要目标是先定位发生segment fault的原因,然后再能往下进行.




20220511
======================
现在运行kernel,qemu会直接停在某个位置,结合反汇编查看,该处的符号是handle_exception,
根据kernel的说法,造成这个结果的因为是某处的round_down操作的结果错误.
::

    #define round_down(x, y) ((x) & ~__round_mask(x, y)) 作用是: x 按位与 (y-1)的反码

    #define __round_mask(x, y) ((__typeof__(x))((y)-1))  (y-1),然后将(y-1)转化成x的类型

经定位,问题出在qemu的sll指令实现上,该指令的实现,把作为形参的rs给修改了,
修改了前面某个t寄存器,正确实现 应该先定义一个tmep来保存rs的值,然后再操作.

20220510
======================
之前的tcgbp补丁里包含了user模式,但是没有实现,之前一直在关心data断点和tlb的机制,
没有测试user模式,于是今天加入并提到了主线.

使用和system模式一样的方法:
::

    if(spec!=NULL) {
        cpu->bps_sz = 0;
        tcg_bp_set_static_bp(spec);
        cpu->bps = tcg_bps_init(&cpu->bps_sz);
    }

"-tcgbp spec,spec,spec..." 可以加入多个断点,但是只有一个断点可以被触发,
因为user模式断点触发会直接退出.

20220509
======================
了解了一下qemu tlb的机制
这相关的调用为:
::

    cpu_exec
        tb_lookup 寻找tb块
            tb_htable_lookup 在h表中寻找tb块
                get_page_addr_code  获得地址
                    get_page_addr_code_hostp
                        如果tlb没有命中,就会调用tlb_fill
                        tlb_fill
                            tcg_bp_tlb_fill

    tlb_fill的注释:这个函数调用时会对tlb进行 "resize" 操作,即舍弃之前的tlb.
    重新寻找.这个操作过后,对tlb的命中做一次断言,保证tlb一定命中.

这样的话,data断点就可以对虚拟地址进行调试了.
如果没开mmu,调用栈还是跟上面的一样,也会有这样的一套流程,所以data断点同样可以
监视物理地址.经过验证确实可以.

这个分支整理一下就会合并到linx-dev分支.


20220507
======================
把国柱的两个补丁打在了linx-dev-sxq-tcgbp分支
比起之前的断点,又新定义了一种断点类型:data,待实现

关于monitor的暂停检查,我原本想的是,弹出warning.
这个补丁的实现是强制暂停.

如果只是弹出warning的话,这时调试者可能就无法继续调试了,因为qemu可能无法触发断点
进入暂停状态.
如果强制暂停的话,调试者还可以继续调试,而且暂停无任何负面影响.

关于data的断点类型,
设定了一段内存范围,如果guest对此范围进行读 写,此断点就会触发.


关于检查断点的位置,跟之前的linx_debug差不多,都是tb块执行完成之后.
data断点也应该在此处检查.

tcg_bp_tlb_fill这个函数只看名字似乎可以处理data断点,如果此地址范围发生了读写,
那么该地址一定存在于tlb中,但后者似乎并不是前者的必要条件.

这个无法处理mmu开启之前的地址.

这个函数实际上并没有真的访问tlb,或许这个函数是半成品,我也想不到这个函数要在哪
调用,这个函数的参数应该是tlb的范围.然后遍历所有data断点,如果tlb的范围在data断点
的范围之中,那么就触发.


梳理一下这些功能
断点命令:
::

    (qemu)tcgbp|tb action cpu spec

    action包含:list add remove disable enable
    cpu:cpu编号,如果是*就代表所有cpu
    spec:对于其他的命令,spec表示bp的编号
         对于add命令,spec可拆成三部分 r0 r1 r2
        r0可忽略,直接夹在r1前边,表示该断点的属性,RWON随意顺序组合
            R read
            W write
            O Once-shot
            N Non-stop
        r1不可忽略,表示一个pc值
        r2可忽略,和r1之间通过"+"连接,表示count和data断点的信息

三种断点:
::

    stop on pc断点
    pc count断点
    data断点,待实现,带有R W属性的断点就是data断点

断点结构包含:
::

    type            断点类型
    state           断点状态:激活 使能 禁用
    attr            断点属性
    ptr             pc or data pointer of bp
    hits            触发次数

    pc_count_info/data_x_info 联合体,表示这两类断点的信息


这些功能大量用到了glib,比如,断点结构使用了链表存储在了cpustate中.
对链表的各种操作也用到了glib,这样减少了cpustate的尺寸.

使用CPU_FOREACH来遍历所有cpu,然后按照cpuid去操作,避免了手动在monitor中更换cpu





20220505
======================
之前的monitor断点功能没有形成单方面的穷举.而且混乱;也没有考虑到多核.
现在重新设计一下,这个实现会先放到linx-dev-sxq-bp2分支.

monitor中的命令格式:
::

    (qemu)cpu num                  切换当前监视的cpu,下边命令都是针对于当前cpu
    (qemu)linx_debug b[reak] a[dd] address 添加一个断点
    (qemu)linx_debug b[reak] d[el] num     删除指定编号的断点
    (qemu)linx_debug b[reak] d[el] [0]     删除所有断点
    (qemu)linx_debug l[ist]                打印断点列表
    (qemu)help linx_debug                  打印linx_debug帮助

断点数据设计为:
::

    breakpoint {
        enable    		  断点使能
        pc_stop           触发暂停的pc
        pc_count_begin    从这个pc开始计数
        do_insn_count     是否正在计数?
        max_insn          最大计数值
        insn_count        当前指令计数
        triggering_count  断点触发次数
    }

断点分为启动参数中的静态断点和monitor添加的动态断点,它们共享一个结构.
工作原理如下:
::

    1.静态断点和动态断点共享一个数据结构,该数据结构会结构体数组的方式存放在
    cpustate中.每一个vcpu都会有一个该数据结构
    2.每一个cpu的断点触发后都会使整个qemu暂停(所有cpu),而且只有暂停中才可以
    使用monitor操作断点数据(非暂停会报警告),这样带来一个好处就是不需要考虑
    monitor线程与cpu线程之间的锁了.
    3.初始化时,静态断点会设置在所有cpu上,monitor中,可以自由切换cpu,并分别为
    当前cpu设置断点.




20220428
======================

昨天修改的代码注释写的不够详细,下一次提交时要写详细一点

今天要解决一下  跳转函数中Indirect Call类型的bpc更新问题
目前bpc是依赖于pc的

整体逻辑应该是这样的:
翻译tb时,生成的中间码就已经确定了pc和bpc的更新
然后在执行tb时完成了pc和bpc的更新

所以要解决这个问题应该从翻译时入手,
翻译的流程
translator_loop 翻译一个tb块
riscv_tr_translate_insn翻译一条指令
如果pc在body的范围,就去翻译微指令,最后一条微指令还会linx_blk_commit,否则就去decode_opc
decode_opc执行时碰到block_head 会去翻译head,调用trans_blk_head

执行trans_blk_head时,第一行就是 打印trace
body为空会跳转,linx_blk_commit提交之后也会跳转,跳转执行 linx_blk_do_jump函数
bpc指向block的头部,bpc应该在linx_blk_do_jump中更新

但实际上:
跳转函数中只更新了pc :tcg_gen_movi_tl(cpu_pc, xxx);
bpc的更新发生在了trans_blk_head中 : tcg_gen_movi_tl(bpc, ctx->base.pc_next)
也就是说,每次识别到 block头部,才会依赖pc去更新bpc,而执行完微指令之后,commit->jump时只会更新pc
这导致了bpc是落后于pc的

解决方法:跳转函数中,所有更新pc的地方都要顺便更新bpc



20220427
======================
暂时不用管联调那边了,只关注qemu就可以了


联系联调那边 在汇编中添加了一组宏, linx_debug_xxx ,这组宏会生成一个head,有着独特的attr,
主要作用是传递一些参数
qemu中遇到这些head 会调用  gen_helper_linx_debug 去处理那些参数

更详细的处理在 helper_linx_debug (target/linx/op_helper.c) 中

ABI更新了,把qemu中反汇编和commit 这相关的内容也更新了


20220426
======================
kernel更新后,qemu运行会出现段错误,
今天国柱找到原因了:
::

	qemu中没有使用gprs[0],将它指向了NULL,但是编译器却编译出了包含x0的二进制

另外为了方便kernel的调试,增加了两个功能:
::

	一个是BUG_ON宏,类似断言的功能,kernel可以在条件合适的情况下终止qemu的运行
	另一个是linx_debug函数,由kernel调用,参数是guest的地址,qemu收到后会打印出
	guest地址上的字符串

20220425
======================
最终确定的monitor断点功能如下:
只有在相应地址暂停的功能,由于是在tb块结束之后判断的,
所以一个tb块里只有一个断点起作用

linx_debug add address --添加一个断点
linx_debug delete num --删除一个断点
linx_debug list --查看所有断点

在暂停的时候操作断点,两个线程不会同时访问断点数据,所以没有上锁.
已经添加到linx-dev-sxq-bq2

暂时没有考虑多核的情况

20220424
======================
之前添加的monitor功能需要改进
1. 不能和linx_debug使用相同的全局变量,需要重新设计一组变量
2. 要实现类似gdb的调试功能,支持多个断点的增加和删除

设想:
(qemu)linx_debug 							    打印每个断点的情况
linx_debug a[dd] pc_stop                   添加一个断点,在某处pc停止 
linx_debug a[dd] pc_count_begin max_insn
加一个断点,在某处pc开始计数,计数到max_insn停止

linx_debug d[elete]                        删除所有断点
linx_debug d[elete] num                    删除某个序号的断点


   然后使用结构体数组保存这些断点数据,数组放在另一个结构体里,
   再把结构体指针放到cpu里

   这些数据的更新方式应该与 -linx_debug 相同,
   打算写一个新函数 linx_debug_check_tb_monitor , 和 linx_debug_check_tb 并列
   断点数据应该包含的内容: ::

		{
	enable

	pc_stop
	pc_count_begin
	max_insn
	insn_count        当前指令计数

	triggering_count  断点触发次数
		}

3. 要考虑锁,monitor中似乎没有命令可以直接修改cpu的状态,
   所以应该没现成的锁,需要设计新锁

   monitor和qemu_main_loop 是两个不同的线程,这两个线程 对cpu的写操作要上锁,
   qemu中已经包装好了QemuMutex (include/qemu/thread-posix.h) ,
   还定义了相关的宏函数 :qemu_mutex_lock 和 qemu_mutex_unlock (include/qemu/thread.h)
   直接用这个应该就可以

   具体实现方法:
   定义一个 两个线程可见的QemuMutex, 在修改cpu"断点数据"之前上锁,修改完之后解锁,
   对于monitor中,上锁的位置很明确
   在qemu_main_loop中,上锁的位置 是 新函数 linx_debug_check_tb_monitor 的内部

20220422
======================
页表的问题暂时卡住了.
今天把国柱的linx_debug移植到qemu monitor 方便后续调试

我只需要在monitor里修改linx_debug的相关参数就可以了

这些参数被设置到了 CPUState 结构体中
bool do_insn_count;             //当前是否在计数?:
uint64_t insn_count;            //计数值

uint64_t count_start_pc;        //在哪个pc开始计数? pc_count_begin
uint64_t max_insn_count;        //计数最大值 max_insn
uint64_t stop_on_pc;            //在某个地址暂停 pc_stop

下一步就是添加一个monitor命令
根据qemu的官方手册,"Implementing the HMP command"章节,步骤如下:
在hmp-commands.hx中添加相关内容
在monitor/misc.c中编写函数
然后在include/monitor/hmp.h添加函数声明

为了简便,我直接复制了一套mouse_move指令的代码在其基础上修改.


20220421
======================
接昨天,现在使用qemu调试最新的内核,-d in_asm最后翻译的内容是
get	a0
syset	satp, t#1
这个操作是设置trampoline_pg_dir作为页表,同时打开了mmu,这个块也运行完毕,
要运行下一个块(80200100),下一个块就变成了虚拟地址,而80200100在页表中没有
索引,会发生缺页异常,然后会跳转到label1的虚拟地址运行.
-d mmu的输出结果是:
address=ffffffff80000100 ret 1 physical 0000000000000000 prot 0
这说明这个虚拟地址对应的物理地址为0
trampoline_pg_dir页表的内容有问题


20220420
======================
上午把Kenneth的log看了一下,

把翻译TB和反汇编打印的逻辑整理了一下
然后问题现在还是卡在relocate那里,
就把riscv的setup_vm和relocate看了一下
setup_vm
这个函数设置了两个页表
1.early_pg_dir
内核映射:将内核自身处于的连续物理内存区域映射到位于PAGE_OFFSET的虚拟内存地址上
FIXMAP映射:映射设备树相关
2.trampoline_pg_dir
将PAGE_OFFSET后长为PMD_SIZE的区域映射到load_pa,
即内核起始被装载后的起始物理内存地址。
relocatea
调用relocate的地方:
la a0, swapper_pg_dir
XIP_FIXUP_OFFSET a0
call relocate_enable_mmu
这里传入了参数a0,存储页表的物理地址

然后relocate符号处,从上往下的顺序,依次是:
1.计算ra偏移之后的地址
2.计算label1偏移之后的地址,并存入CSR_TVEC
3.拼凑satp寄存器的值存放到a2,但暂时没有加载
拼凑的方法:高位是satp_mode,低位是a0偏移之后的结果
4.和3一样,拼凑一个satp,但是使用的页表是trampoline_pg_dir,
这个satp被加载进SATP.此时mmu被启用,下一条指令会因为异常跳到label1,
但下一条就是label1,有点意义不明
5.label1,将死循环的地址加载到CSR_TVEC
6.重新加载全局指针gp
7.把上面的a2加载到CSR_TVEC,并ret.

trampoline_pg_dir,这个表似乎没有用,实际调试运行riscv时,上面7个阶段是按照顺序
执行的.

new edit:
trampoline_pg_dir并不是没有用,mmu被启用之后,pc的地址要经过mmu转换,正常运行下去
是无法运行到label1的,会触发异常,pc会跳到CSR_TVEC的位置----也就是新的label1的
虚拟地址运行下去.
trampoline_pg_dir的作用就是开启mmu之后让pc依然能够跳回来接着往下跑,然后会更换
新的页表,并ret


20220418
======================
## 目前的问题
qemu运行最新的内核,最后的trace是:
linx_head 0x80200140
linx_mini   0x813abf54
linx_mini   0x813abf56
对应着head.S中relocate的最后一个ret块.
monitor中观察到的pc也停在了c0200100(未知的地址)

## 添加反汇编解码
在国柱的框架中添加反汇编解码,目前已添加完毕,通过编译,第二天测试一下.


20220415
======================
## 目前的问题:
"-d cpu"输出的bpc始终为0,并没有输出tpc,也不知道tpc的值是否正确,

接昨天,把断点打在riscv_tr_translate_insn函数内部,然后监视env中的值

发现env->bpc的值一直都是正常的,env->tpc一直为0.

在处理微指令的部分的最后加上了"env->tpc = env->pc;"

然后调试,tpc的也值正常了.

## 现在问题在"-d cpu"上面,要先定位到是在哪里打印的.

根据打印的内容在根目录搜索字符串"x26/s10",搜到"riscv_int_regnames[]",
最终定位到"riscv_cpu_dump_state"函数
分别两次用带"-d"和不带"-d"的参数启动gdb,
发现确实跟这个函数有关系,堆栈调用情况为:

  cpu_tb_exec
    log_cpu_exec
        log_cpu_state
            cpu_dump_state
                riscv_cpu_dump_state

找到打印bpc的地方发现了显然易见的错误:

 qemu_fprintf(f, " %-8s " TARGET_FMT_lx,"bpc", env->tpc);
 qemu_fprintf(f, " %-8s " TARGET_FMT_lx,"sbpc", env->sbpc);
 qemu_fprintf(f, "\n");

改正即可,现在又发现了新的问题:
"-d cpu"的打印的内容 和 在monitor中使用 "info register"显示的内容的格式
是一摸一样的,
但是前者输出的bpc、sbpc、tpc全为0，后者可以显示正确的内容.

monitor的堆栈最终也会调用cpu_dump_state函数

## "-d cpu"和 monitor "info registers" 的问题还需确认,看是否是调用的时机不同,
影响了结果.

最终发现的原因:
我是使用gdb单步运行时发现的问题,opensbi的内容过多,其中包含着循环,我错当成
在head.S中添加的死循环,所以看到的寄存器都对不上.


20220414
======================
昨天的问题:加入死循环会出现 掩码确认 mask_verify 的错误,

解决方法:head中 加入bset ra, 因为call需要改变ra的值

现在需要确认的内存:

[0x830ea0b8] = 0
[0x830ea000]~~[0x831330e0] 之间都是 0
[0x830e8a38] = 1                         

在qemu monitor中 观察这些地址不需要再 往后偏移2M,
因为: 在qemu中,整个image往后偏移了2M, 在head.S中,已经把相关地址做过 +2M 修改了  


在setup_vm之前 上面那些地址 都是 guest 中的 物理地址 了,所以不需要转换,直接使用xp命令就可以查看.
经查看上述的地址的值 都正确无误



为了更进一步验证,修改head.S,在[0x830ea000]~~[0x831330e0] 之间写入 0x222 11111111, 

观察到的部分结果如下:
(qemu) xp/100 0x830ea000
00000000830ea000: 0x11111111 0x00000222 0x11111111 0x00000222
00000000830ea010: 0x11111111 0x00000222 0x11111111 0x00000222
00000000830ea020: 0x11111111 0x00000222 0x11111111 0x00000222
00000000830ea030: 0x11111111 0x00000222 0x11111111 0x00000222

可见被正确写入了,
另外 [0x830ea0b8] = 0 也在范围之内 ,所以中间会出现一段0
目前 loop 被加在了 setup_vm之前 



现在要解决 寄存器 bpc 和 tpc 始终显示为0 的问题

bpc和tpc的位置:
target/linx/cpu.h    CPURISCVState结构体中   //被重定义为 CPUArchState

pc指向 head 地址时 , 应该让 bpc = pc
pc指向 mini 地址时 , 应该让 tpc = t寄存器的


pc值被用来 查找tb块
生成新的tb块 也需要用到tb值

逻辑应该是这样的:
	->运行tb块会更新pc值
	->翻译新的tb块时,pc值是最新值
	->要翻译的中间码中包含更新pc值的指令,可以从那里入手同时更新bpc和tpc的值



使用gdb启动 qemu system系统模式,查看函数调用情况 ::

  qemu_thread_start
    mttcg_cpu_thread_fn
      tcg_cpus_exec
        cpu_exec(../accel/tcg/cpu-exec.c)(和用户模式一样的主循环)

cpu_exec逻辑: ::

	tb_lookup/tb_gen_code 	            (查找或生成tb块)
		//申请tb块的内存,然后执行下面的函数
		gen_intermediate_code            (每一种架构都要调用不同的xyz_tr_ops)
			translator_loop
	cpu_loop_exec_tb(执行tb块)
		cpu_tb_exec

		
translator_loop分析: ::

  //这个函数功能是 完成一个tb块的翻译 ,每次翻译的tb块都包含固定数量的指令
  translator_loop(linx的翻译操作集合 *ops, 反汇编上下文 *db,
                       CPUState *cpu, TranslationBlock *tb, int max_insns)
  
  //初始化反汇编上下文,使用db来管理tb
  ops->init_disas_context(db, cpu);
  //tb块开始
  gen_tb_start(db->tb);
  ops->tb_start(db, cpu);
  
  while(true)
  {
      db->num_insns++;  //看得出来当反汇编上下文翻译完,这个循环就会结束
          ops->insn_start(db, cpu);

          /*.........*/

      ops->translate_insn(db, cpu); //这里是翻译中间码的地方
  }
  
  //tb块结束
  ops->tb_stop(db, cpu);
  gen_tb_end(db->tb, db->num_insns);

  /************************************/

明天把tcg中间码 相关的看一下, 让bpc tpc的值正确
屏幕上打印的log 未包含 tpc, 需要找到打印的地方 并加上tpc



20220413
======================
目前block的head.S运行到setup_vm时,qemu会出现 "gen_exception_illegal"的错误,
在这个错误解决之前,先检查一下setup_vm之前代码的正确性

根据 header/mini trace 和 反汇编 检查了每一个寄存器的值,发现的问题如下:
1.bpc始终为0
2.mie和mip写入失败

其余的寄存器都未出现问题,
在微指令执行途中,t寄存器会被覆盖,不过终值是对的,被覆盖掉的值应该也没问题

然后微指令中 使用了数次s系列指令,修改了内存,阅读汇编代码可得到一些内存的值(在qemu中,这些地址需要偏移2M):
[0x830ea0b8] = 0
[0x830ea000]~~[0x831330e0] 之间都是 0
[0x830e8a38] = 0x830e8a39

使用qemu monitor可以读guest内存,开始尝试:

1. 直接使用参数 "-d nochain" 启动qemu-system,qemu会很快error挂掉,来不及进入monitor

2. 加上"in_asm" "cpu"参数,使用"-D ./qemu.log"可进入monitor, 不使用log,终端会打印巨量内容无法进入monitor
   使用"info registers"发现pc值被固定到了某个值(比较小,start_kernel刚运行),此时qemu无法正常退出,猜测可能是巨量信息导致

3. kernel是在运行setup_vm时发生错误的,于是打算在运行这个之前让程序进入死循环,这样就不会error了,
   死循环中 就可以使用monitor查看内存了 
   具体做法: 注释掉 call setup_vm,在这个块下面新增: ::

    loop:
    block_std_head _start_kerne1l6
    bnext.call loop     //该块会循环
    block_text_begin _start_kerne1l6
        const 1
        const 0
    block_text_end _start_kerne1l6

qemu 运行 新内核出现 "mask_verify"的错误,最后一个head是0x802002e0(call setup_vm的地方),用上面两点的方法也是相同的结果.


明天试着把死循环加到其他地方看看效果.




20220407
======================
今天阅读学习了blockISA相关文档
使用gdb调试学习了qemu riscv64 user模式,主要记录了从main 到 tcg_gen_xxxx 之间的堆栈调用关系
明天把qemu指令添加编码、TCG这块内容再看一下
