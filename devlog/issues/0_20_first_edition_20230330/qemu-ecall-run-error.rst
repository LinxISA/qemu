ecall指令qemu实现有误溯源
*************************

介绍
====

本篇主要是记录 v0.20 执行specint子项遇到ecall块指令出现的问题，
以供后来者了解情况。qemu分支：linx-dev-performance-0.20

记录人：韩志林 30043474

现象
====

开始联调specint子项的时候，只要对应的子项编译通过，用qemu执行的时候都会报
segment fault的问题，这种大面积的报错，一般是指令定义未理解。和架构定义对齐之后
确认以下信息：

1. 块head trap字段相关的定义如下：
   1.trap==1 && branch type == indcall，不管size是否为0，均表示ecall指令。
   qemu实现可以这么考虑：bsize==0，产生ecall异常，完成系统调用后，按照fall
   though 继续往下取下一个块执行（不按照indcall的逻辑跳转，可以理解成ecall
   已经完成了"call"的功能，块body不再有"call"的功能，所以不用按照indcall
   的方式跳转）；bsize!=0，块body在commit之后执行ecall功能，并按照fall though
   继续往下取下一个块执行(fixme: 当前qemu未实现bsize!=0的情况)。
   2.trap==1 && branch type == ret，不管bsize是否为0，均表示eret指令。eret
   仅存在system模式中，user模式不会执行到该指令。执行逻辑是按照eret指令完
   成跳转，跳转的地方由内核提供的sepc地址决定。(附：执行supertest的时候，
   若bsize==0，表示supertest的结束标志)。
2. 0.16虽然添加了ecall块指令，但是还保留了ecall微指令。qemu对于ecall块指令的处理
   存在问题，但是编译器编出的ecall功能都是通过微指令实现的，所以该问题在0.16未测
   出来。到0.20删掉了ecall微指令，编译器会编出ecall微指令该问题才暴露出来。

修复ecall块指令之后，specint可正常往下debug。

..note::

    当前qemu仅实现了bsize==0的ecall块指令(indcall && 块head的trap==1)功能，
    bsize!=0的情况待实现。
