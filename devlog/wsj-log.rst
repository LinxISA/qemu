
.. Wang Shijie 版权所有 2023

:Authors: Wang Shijie
:Version: 1.0

Wang Shijie的开发日志
**********************

20230507
=========

1. 首先对f.entry和f.exit指令进行了理解：
a). f.entry（函数入口）将localgpr的值减去立即数更新到全局寄存器；
将几个寄存器的值依次写入目标寄存器所指的连续内存空间中

b). f.exit指令（函数出口）将localgpr的值加上立即数并更新到全局寄存器中；
将从源地址寄存器所指的内存空间连续读取数据写回到指定的寄存器中；
将获取的ra寄存器更新到块提交参数寄存器的CARG.TGT域段

2. 其次是对于测试的一些想法
a). f.entry和f.exit指令具有一定程度上的相似性，即
函数入口涉及到将寄存器的值写入到一片连续的地址空间，
而函数出口是读取一片连续地址空间的值写回到指定的地址空间

b). 联想到之前测试load/store指令时，将二者联合在一起进行测试，
即先用store指令存入数据，再用load指令读数据
此处可能可以参考以上形式对f.entry和f.exit指令进行测试

3. 学习整理测试用例的blockISA语言的语法（以示例做展示）
微指令的汇编文件格式/语法（以cmp.eq指令为例）
.global blk_cmp
.section .text
.align 3
blk_cmp:
block_std_head blk_cmp     //描述body块指令的类型，此处为标准块指令
bget ra,a0,a1             //body块指令会读取的架构寄存器GPR
bset a0                   //body块会修改的架构寄存器
bnext.ret                 //返回
block_text_begin blk_cmp  //此处body是由若干条微指令组成，块体开始
get a0                    //get指令从块内私有寄存器读取双字的数据写到目的T寄存器中
get a1
cmp.eq t#2, t#1          //比较a0和a1是否相等
set t#1, a0              //set指令将左源寄存器的值写入块内私有寄存中，输出寄存器的值未定义
get ra
setc.tgt t#1
block_text_end blk_cmp   //块体结束
块指令的汇编文件格式/语法

.global blk_fentry
.section .text
.align 3
blk_fentry:
# block_mem_head blk_fentry   //不需要调用宏去描述块头类型什么的，因为指令本身是一个块头指令
f.entry [a0, a1], a2!, 15   //F.ENTRY语法

4. 学习如何最新的编译器（编译器下载地址http://10.175.104.61:8888/other/bisa_share/temp/llvm_v0.20.3_0506/linx_blockisa_llvm_glibc.tar.gz）
   wget http://10.175.104.61:8888/other/bisa_share/temp/llvm_v0.20.3_0506/linx_blockisa_llvm_glibc.tar.gz  //下载文件
   tar xf linx_blockisa_llvm_glibc.tar.gz  //解压文件
   在主目录下
   vim .bashrc
   修改/添加环境变量
   export PATH=/home/wangshijie/linx_blockisa_llvm/bin:$PATH
   如何生效呢？ 运行~
   source ~/.bashrc
   利用clang --version查看编译器版本

5. 如何对单独的汇编文件进行编译
   clang --target=linx64 -march=linx64im -mabi=lp64 -O3 -static -c fentry.S -o fentry.o

6. 提交
   git status //查看文件状态
   git add filename //将文件从工作区加入暂存区
   git commit -s //将暂存区的内容添加到本地仓库
   git commit --amend //如果push之前暂存区的文件有所改动（增加or减少or修改），使用此条指令
   git push

7. git branch //查看分支 git checkout branch_name //切换到指定分支

8. 学习linx BlockISA两个层级的指令集架构
   块指令：对架构状态的变动，读写架构寄存器/内存地址，当前块指令执行完成后执行哪一条块指令
   General Purpose Register：R0-R15；16个宽度为64位的通用寄存器
   Block Program Counter 当前Block Header的地址
   EBSTATE 块指令非正常提交时产生的额外块内状态
   System Status Register系统寄存器

9. 为什么会有块内私有寄存器和架构寄存器的概念？
   块指令定义了寄存器的申请和释放
   每条指令的块头上定义了BGET和BSET的寄存器掩码，将一个块指令最多可以读写16个不同的寄存器输入，寄存器输入输出按照bitmask掩码方式编码
   每条块指令内部被分配了私有的块内寄存器——Local GPR
   在块指令初始化时，块处理器按照BGET掩码将Global GPR中对应的寄存器拷贝到块内自身的Local GPR上
   在块指令提交时，块处理器按照BSET掩码将Local GPR中的对应寄存器拷贝到GSTATE上的Global GPR上，同时Local GPR被释放

10.块内架构状态BSTATE的定义
   Local GPR
   相对索引T寄存器：TR0-TR7，当前微指令执行前的八条微指令结果
   块指令提交参数CARG：块指令提交参数寄存器
   块指令内部指针TPC

11.回归对F.ENTRY指令/F.EXIT指令/F.TEXIT指令的测试
   通过对汇编文件的编译发现，编译器无法识别上述指令，目前在和编译器的同事沟通中...


20230508
=========

1. 首先确认了内存块指令的汇编写法，不需要使用宏去定义块头；
.global blk_fentry
.section .text
.align 3
blk_fentry:
f.entry [a0, a1], a2!, 15

2. 修改完成后进行编译，clang --target=linx64 -march=linx64im -mabi=lp64 -O3 -static -c fentry.S -o fentry.o

3. 测试的思路是我们要看fentry是否能够成功写入到内存地址的一片空间；
   首先，需要找到一片可用的空间去写，那么我定义一个数组，自然就分配了一个地址空间；
   其次，设置两个变量待写入（先测试写两个寄存器的情况）；
   其次，就是传参的问题，在test.c文件中调用那个汇编，blk_fentry(val1, val2, addr);

4. 测试出错，在文杰师兄的帮助下，发现原因，数组的地址空间是向后延伸的，而F.ENTRY指令的作用是向前写入，修改方法是现将数组的地址加上常数，这样就可以写到这个数组里

5. 查找bug的工具：
   反汇编：xobjdump -d executable_file_name > Assembled_file_name   (如test.asm)
   打印日志：xqemu -d exec,in_asm,nochain -D log_filename executable_file_name
   目前对于F.ENTRY指令的往指定地址中写入的操作验证完成

5. 下一步验证其是否将目标寄存器的值进行修改并更新到共享状态寄存器
   这个问题估计有两种解决办法：其一是使用调试工具查看，其二是在源码中打印共享状态寄存器的值；
   第一种方法：目前还不会，有待更新；
   下面采用第二种办法：
   首先，找到该指令的翻译代码，
   在/LinxBlockModel/target/linx下的xxxx.decode文件中查找指令F.ENTRY对应的函数blk_fentry
   在/LinxBlockModel/target/linx/insn_trans下的文件中查找该函数名得到gen_helper_block_fentry
   在/LinxBlockModel/target/linx下搜索helper_block_fentry，找到对应的guest instructions 转换成qemu中间码的代码
   其次，加上打印，输出共享架构寄存器的值，修改之后要重新编译
   通过结果比较发现，F.ENRTY指令更新了共享架构寄存器的值；

6. 如何使用GDB进行调试？
   使用gdb qemu-linx进入调试程序
   set args ./test


20230509/20230510
==================

1. 首先对F.EXIT指令的功能进行梳理和理解
Function Exit函数出口, 指令功能主要有三点
a). 从源地址寄存器指向的地址空间中连续读取数据写入bset掩码指定的寄存器之中

b). 将源地址寄存器中的值加上立即数更新到共享架构寄存器中

c). 将获取的ra寄存器的值更新到CRAG.TGT域

2. 前两点功能可以利用F.ENRTY指令辅助测试
   具体思路：用F.EXIT指令将连续地址空间的数据写入寄存器，再使用F.ENTRY将寄存器中的值写入另一段连续的地址空间；

3. 对F.EXIT指令单独进行测试
   定义并初始化一个数组a，将数组地址作为fexit的参数，于是将数组中数据写入指定的寄存器中
   在gen_helper_block_fexit中加入了对寄存器内容的打印，结果发现fexit指令成功将内存中内容写入寄存器

3. 与F.ENTRY联合进行测试
   前面的步骤和单独测试相同
   定义一个另外的数组b，将数组地址作为fexit_fentry的参数，目的是将fexit写入寄存器中的内容再写入内存
   执行完成后在test.c文件中比较数组a和数组b的值可以完成验证

4. 单独测试F.ENRTY和F.EXIT指令都能完成，但是将两条指令放在一起进行测试时候出现段错误的bug

5. 记录修改bug的一点体会：
a). 关注提示信息，在提示信息的帮助下可以迅速找到问题并解决，可以直接查提示信息揭露的错误

b). 对生成的可执行文件进行反汇编

c). 查看qemu的日志

6. tips如果在VSCode中误删了文件可以Ctrl+C来进行恢复！！！

7. 学习TCG_Module
a). Tina Code Generator/微小代码生成器: 目标指令流->前端解码器+中端分析优化器+后端翻译器->主机指令流 //对目标二进制文件进行动态翻译

b). 前端解码器对二进制指令的编码格式进行分析完成二进制指令的反汇编，解码后生成中间码
   中端分析优化器对中间码进行优化分析
   后端翻译器实现了从中间码到主机机器码的转化

c). 翻译过程

8. 关于GDB的使用 以测试fexit指令为例

.. code-block:: c
 gdb --args ../../build/qemu-linx test exit   //进入调试程序，调试exit函数
 b helper_block_fexit  //设置断点，此处设置为当执行到fexit时
 r //运行
 la n   //打开，全称为layout next
 p *env  //打印所有env中的值，env是什么？
 n    //执行到下一行
 p env->carg   //打印carg寄存器的值
 p env->blk_lgpr[0]   //打印本地寄存器R0的值
 P env->gpr[0]  //打印共享架构寄存器R0的值

9. 对于F.EXIT的第三点功能，可以通过GDB的方式来进行测试
