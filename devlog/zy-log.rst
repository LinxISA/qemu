张越开发日志
***************
20230509
==========
1. setc.trap指令功能实现：
将CARG的标志位置为1.
块提交时，标志位如果为1，产生一个陷出(trap)，陷出模式存在左源寄存器中。
2. carg寄存器定义需要从原来的一个变成多个具体寄存器，
但是我没有十分的理解carg寄存器划分成6个段域
3. 块提交的时候需要检查carg.trap的标志位是否为1，是1的话产生一个trap，正常检查。
4. 还需要考虑的是，上下文保存的事情。应该是在riscv_cpu_do_interrupt中进行的；
5. 标志位清零问题：我认为在commit阶段检查完trap标志位之后就可以清除
20230415
==========
1. 读取lgpr相关指令：
指令功能是将lgpr 值读取到t寄存器。使用块头中gmask变量标识本地lgpr是否被初始化，
在读取lgpr之前，应该先检查对应的gmask位被置位，否则报非法指令异常，而后读取。
大部分读取lgpr的指令实现都使用了get_blk_s函数获取lgpr寄存器，因此只需在该函数中
添加对gmask检查功能即可。

2. 设置寄存器值指令
set指令，该指令仅对local gpr进行写，无需做掩码检查，但需要设置gmask对应位标识该
寄存器已被初始化。
setg指令，包含两条指令，第一条将t寄存器值写入到全局gpr上，第二条将lgpr[src]写入到
lgpr[dst]中。

第一条指令功能仅写了gpr无需做gmask检查，但需要做smask、duplicate set检查。
第二条setg指令功能读取了lgpr，因此需要添加gmask检查，写入到lgpr中需要对gmask进行置位

设置寄存器值指令具体实现方案：

    1. 第一条，把#t里的值写到全局gpr，以及local gpr[dst]中：
    写到全局gpr时需要检查对应的smask是否置位、set_mask是否重复进行set_gl。
    如果检查结果正常，写操作前需要把set_mask从0置位成1，
    写入到全局gpr，需要将对应smask位置位为0.

    2. 第二条，lgpr[src]写入到gpr[dst],以及lgpr[dst]上：
    读local gpr的数据，需要对该寄存器进行gmask检查，然后写入全局gpr需要对相应的smask,
    set_mask进行检查，不符合预期条件则报异常。
    如果检查结果正常，写操作前需要把set_mask从0置位成1，
    写入到全局gpr，需要将对应smask位置位为0


20230413
==========
1. set global gpr的时候需要置位gmask，但是blk_get_s中添加了mask_check操作，
因此对gmask的置位顺序和置位次数还存在部分疑问。
2. blk_get_s指令中，用微指令方式实现mask_check的功能（已完成）
3. setg指令，检查setg的方法需要优化，用中间码代替helper_mask_check函数调用，需澄清。

20230412
==========
1. 经过分析,具体的get_blk_s是被其他指令调用并访问块内私有寄存器的,需要直接添加check在
get_bllk_s处。
2. 如果仅在指令调用get_blk_s时动态地检查mask是否经过了初始化,则很影响性能,正在考虑调用
时是否可以使用微指令实现mask_check。

20230411
==========
v0.20的get掩码检查
1. 检查mask的接口已经定义好了，helper_mask_check预设好了case情况，补充调用即可。
2. get类指令实现的地方，如果是读块内私有寄存器中数据，就调用helper_mask_check检查一下
掩码。

20230328
============
问题：
1. 对指令 blk_rev16 的实现过程做了分析，无法用 tcg_gen_bswap64_i64 微指令来优化，
bswap无法实现半字为单位的数据翻转。查了下别的架构中的rev16的实现，实现过程也没用
bswap相关指令。

2. bam实现的优化：希望使用deposit实现相应功能，但分析后发现deposit入参无法匹配;

3. bxu优化：希望使用extract进行优化从而实现功能，但是入参类型不符合这个微指令的格式

4. bxs优化：希望使用sextract进行优化从而实现功能，但是入参类型不符合这个微指令的格式。