supertest 测例分析
*******************

介绍
=====

本篇主要是记录 supertest 用 QEMU 运行时产生的错误，以供后来者了解情况。

记录人：韩志林 30043474

20230403
========

20230320-0322 定位到的有几个qemu 指令功能实现的问题，这里回顾一下：

1. 确认 eret块指令+bsize==0作为supertest结束标志。
   出现该问题主要是和编译器未对齐。
2. 确认 call/indirect call的情况下，块内对ra寄存器不能set.g或者set.gl。另外，只
   用set不会影响到函数返回。出现这个问题是qemu在做set mask掩码检查时报非法指令，
   较容易定位，和架构定义对齐即可。
3. 修复 lw.ip, lwu.ip 一个是有符号32bit，一个是无符号32bit；
4. 修复 sw/sd rsd, rsb 指令实现问题；
5. 修复 ld_f, lbu_f, lhu_f, lwu_f 指令中imm编码为有符号数；
6. 修复 qemu中concat块高64bit为0时触发的bug。supertest中会存在concat块的 head[127:64] 全为0的情况。

20230404
========

supertest完整版测试
-------------------

之前编译器同事提供的supertest是一个缩减版的二进制文件，编译过程中指定的start.s
文件是一个很小，无特殊功能的的文件，测试结果是supertest所有测例均通过。由于当前
specint仅剩520子项功能验证不通过，问题较难定位，考虑到supertest问题定位较为容易，
于是贾文杰考虑用完整版的supertest做进一步的测试。文杰编译出完整版的supertest可
执行文件后，该任务交由杨继钦主导。

目前用clang++编译出来的supertest执行上总共有三类问题:

1. 超时，执行的时候加了timeout时间限制，还是挂了，初步怀疑是出现了死循环
2. abort，这个需要细看
3. terminate called after throwing an instance of XXX

先看第三类，首先这类的报错都很明显，都是执行某种类型的实例出现的报错，查询这类问题，
通常有这些原因：

1. 访问了不存在的内存空间。
2. 使用了未经初始化的变量。
3. 使用了已经释放的内存空间。
4. 使用了不合法的指针。 建议检查程序中是否存在上述问题，并进行修复。

所以看报错，这类问题都是出现在异常处理的代码中，怀疑是异常处理有问题(try catch),
所以先跑一版0.16的进行确认，发现0.16的llvm也是挂的,这样就需要找个能通过的版本，确认
try catch生成的汇编代码的问题。

首先尝试x86,把编译器改成x86，尝试编译x86的版本，看看能否通过(不过这个大概率是
可以通过的)，所以考虑编译通过之后，比对相应的汇编逻辑，查看llvm编译器的问题.

20230406
========
文杰去确认了一下，llvm确实还不支持异常处理，那么这个问题也就可以先放放，那么考虑了
一下，先编译gcc的，gcc支持异常处理，并且gcc和llvm的qemu逻辑一致，先忽略llvm关于异常
处理的supertest测例。

结果发现，gcc没有问题，之前出现supertest的部分问题，原因是用的llvm编译器，这个编
译器还不支持异常处理，导致关于异常处理的测例出现报错，这样就可以确认，0.20上，
supertest没有问题，测试通过

20230411
========
4月10日新给的一版supertest有三个测例未通过，今天定位一下它的问题。

先看./suite/FIRST/test0.c/test0这个测例，这个测例的结果是test fail，程序结束了，但
没有正常结束，看qemu的执行看不出来问题，按照预期结束，但是汇编的流程，会set a0为13，
然后函数结束，先去确认之前版本的程序执行流程，发现0.20在4月10号之前没有跑过这个测例

先考虑跑一版0.16的测例，看看这个程序的执行流程。

结果发现，0.16也是这样，去看看main函数的c代码(不允许拷贝supertest源代码),可以看到
按照逻辑来说，这个程序有三个退出的地方。CVAL_EXIT(retval)和CVAL_EXIT(12)和 return 13
这三种方式。光看c应该用CVAL_EXIT(retval)进行结束的(程序正常执行的时候)，现在变成
无论程序是否正常执行，都用return 13进行函数返回。

所以是走了一个错误的返回路径，去确认了一遍函数在返回之前的执行，是符合预期的，所以
这个测例就只需要关注为什么CVAL_EXIT这个函数没有被编译进去，导致程序无法正确结束的
问题，已经反馈给了将将，后续等待确认，有可能是编译选项的问题？

再看第二个问题，./suite/iso14882/18/7/t03.C/t03这个测例。

这个测例的执行超时，初步怀疑是程序跑飞了，陷入死循环.

这里先确认一下test0这个测例，因为这个测例只有return 13返回的事情，还是需要关注一下
的，看看是否是编译选项的原因。

王鑫确认了一下这个测例的汇编，以及之前的编译测试记录，发现这个测例是属于已经发现
的失败测例，所以这个问题可以关闭了，确认这个测例就是失败的。

再看看剩下的两个测例，iso14882/18/7/t03.C/t03和Cxx11/20/8/12/t_0002ld.C/t_0002ld，
把编译器已经确认的fail_list拿过来，发现这两个也是属于已知错误，t_0002ld属于运行
时间很长的测例，才导致了timeout的问题，所以这几个测例的问题可以确认了，问题关闭。

supertest编译
-------------

环境: 10.175.104.61
账号: supertest, supertest
如果需要单独编译或者分析supertest的用例，可登录上面的账号，里面有个README文件，
参考该README文件操作即可。

..code::

    timeout 100 /home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/linx64-linux-gnu-gcc -I /home3/supertest/QEMU/workpace/SuperTest_LINX64/report/h -fconvert -iconvertdir=/home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/llvm-mc -O3 -static -ffunction-sections -fdata-sections -Wl,--gc-sections -ffixed-point -std=c90 -o tmaxpar.s /home3/supertest/QEMU/workpace/SuperTest_LINX64/suite/2/2/4/1/tmaxpar.c -S
    cc1: warning: unknown register name: point

    timeout 100 /home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/linx64-linux-gnu-gcc -fconvert -iconvertdir=/home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/llvm-mc -o tmaxpar.o tmaxpar.s -c

    timeout 100 /home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/linx64-linux-gnu-gcc -fconvert -iconvertdir=/home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/llvm-mc -o tmaxpar tmaxpar.o -L /home3/supertest/QEMU/workpace/SuperTest_LINX64/report/lib -lst
    /home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/../lib64/gcc/linx64-linux-gnu/11.0.0/../../../../linx64-linux-gnu/bin/ld: /home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/../lib64/gcc/linx64-linux-gnu/11.0.0/libgcc.a(unwind-dw2-fde-dip.o): in function `_Unwind_IteratePhdrCallback':
    /home2/wangxin/PureRelease/linx-dataflow/LinxToolchain/build/linx64-linux-gnu/../../src/linx64_linux_build_src/LinxGCC/libgcc/unwind-dw2-fde.c:350: undefined reference to `dl_iterate_phdr'
    /home3/supertest/QEMU/block_toolchain/gcc-v0.20/linx64-linux-gnu/bin/../lib64/gcc/linx64-linux-gnu/11.0.0/../../../../linx64-linux-gnu/bin/ld: /home2/wangxin/PureRelease/linx-dataflow/LinxToolchain/build/linx64-linux-gnu/../../src/linx64_linux_build_src/LinxGCC/libgcc/unwind-dw2-fde.c:350: undefined reference to `dl_iterate_phdr'
    collect2: error: ld returned 1 exit status

最终确认是编译的时候需要加 -static 参数。
