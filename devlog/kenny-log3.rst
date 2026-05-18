.. Kenneth Lee 版权所有 2022

:Authors: Kenneth Lee
:Version: 1.0

Kenneth的开发日志3
*******************

这里记录linx-block方案的qemu开发日志。从2022年10月以后开始。

20230202
========

今天继续分析qemu性能问题。


20230201
========

今天开始定位qemu性能不高的问题，在这里跟踪：
:doc:`issues/model_perf`
。

20221124
========

今天给王州确认一下opensbi的scratch空间的用途。

opensbi的入口在firmware/fw_base.S:_start，一开始就做主从核的选举，然后才做重定
位，清BSS，设置fdt指针（fdt包含在二进制映像中）等操作，这之后，就是
_scratch_init。

在代码这个地方有注释，它认为进入BIOS的时候，t1是firmware的首地址，s7是Hart ID，
s8是堆栈大小。这说明硬件到BIOS的接口并没有包括scratch space。scratch space是用
tp做基址的，tp的基址算法是：::

  tp = _fw_end + s7 * s8

这个意思很直接了，就是靠着固件往下，每个Hart一个，放一个堆栈，栈底就是这个
scratch空间。所以这个空间肯定不是硬件（Qemu）给BIOS的，要不是BIOS自己用的，
要不是BIOS给内核用的。

我们往下找，汇编初始化完成以后，剩下的全部是sbi_init在起作用，这个函数走到最后
就是这个：sbi_hart_switch_mode()->mret。而在这之前，它把CSR_SSCRATCH寄存器设置
成了0，也就是说，这个变量不会传递给内核。

这样依赖，我们认为BIOS传递给内核的大型内存参数就只有dtb了。

20221011
========

今天给石晓强分析一下spec2017测试例没有通过的问题。

从他昨天的日志上看，错误是一个C++代码导致的，是在一个try catch序列中发生了异常，
需要从try序列中跳出到catch序列中，这需要unwind堆栈，所以调用了
sysdeps/generic/unwind.h:_Unwind_RaiseException()。

但我在当前的glibc代码中找不到这个函数的实现，只有一个头文件声明。从石晓强跟踪到
的堆栈中可以看到它的实现中调用了uw_init_context_1()，这个东西只有
uw_init_context宏使用了，但这个宏也没有使用者。我不知道怎么调过去的。

咨询和OS和编译器两个团队的意见，他们说这个地方用的是gcc自己的libunwind，我手上
没有这个代码，没法继续查了，这个问题转给编译器的同学吧。我只是比较奇怪：这样说
起来，gcc C++(try/catch语法）对drawf是强依赖了？没有编译信息还不给我用这个语法？
这说不过去。我猜是gcc编译的时候带了drawf信息，所以unwind的库走了这个分支，也许
只要我们编译的时候不要带这个信息，这个问题可以规避过去？

.. MARK: NEW LOG IS HERE


综合分析
============

none
