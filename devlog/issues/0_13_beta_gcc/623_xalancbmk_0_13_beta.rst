623.xalancbmk_s 0.13beta测例分析
**********************************

介绍
======

SpecInt 2017 测例中，623 测例在用 0.13beta 的 gcc 编译器(20230118_B003)编译后，用
QEMU 运行后产生了 aborted，没有其他内容输出。

本篇就是用来记录 623 分析的情况。

分析
======

20230201
----------

分析人：贾文杰

运行后，只会出现 Aborted 问题。
之前排查 SpecInt 测例问题的时候，也有出现 aborted，是因为现在 block 还不支持 dwarf
格式，当运行 C++ 程序过程中，有 throw 异常，就会导致 aborted。

然后，现在这个 623 就打算从这方面着手。先用 QEMU 输出下日志，然后从最后面（可以从
_cxa_throw 的地方开始）往上找，找到第一个与测例相关的函数::

    _ZN11xercesc_2_724DecimalDatatypeValidator12checkContentEPKtPNS_17ValidationContextEbPNS_13MemoryManagerE

用 c++filt 给它翻译了下，结果如下::

    xercesc_2_7::DecimalDatatypeValidator::checkContent(unsigned short const*, xercesc_2_7::ValidationContext*, bool, xercesc_2_7::MemoryManager*)

在这个函数里加上几个桩，看看它走到了哪里挂掉的。再跑了一次，能看到它挂在了下面这个地方::

    printf("DecimalDatatypeValidator::checkContent: 5\n");
    if (getRegex()->matches(content, manager) ==false)
    {
        printf("DecimalDatatypeValidator::checkContent: 6\n");
        ThrowXMLwithMemMgr2(InvalidDatatypeValueException
                , XMLExcepts::VALUE_NotMatch_Pattern
                , content
                , getPattern()
                , manager);
        printf("DecimalDatatypeValidator::checkContent: 7\n");
    }
    printf("DecimalDatatypeValidator::checkContent: 8\n");

ThrowXMLwithMemMgr2 这个就会抛出一个异常出去，跟 riscv 对比，它会走到 5，但 6 不会走
到，那就是 matches 返回的结果不同。

再往 matches 里面加桩，再跑一遍跟 riscv 的对比。

linx 的结果为::

    DecimalDatatypeValidator::checkContent: 5
    RegularExpression::matches: 1
    RegularExpression::matches: 4
    RegularExpression::matches: 4, matchEnd: -1,1
    RegularExpression::matches: 6
    DecimalDatatypeValidator::checkContent: 6

riscv 的结果为::

    DecimalDatatypeValidator::checkContent: 5
    RegularExpression::matches: 1
    RegularExpression::matches: 4
    RegularExpression::matches: 4, matchEnd: 1,1
    RegularExpression::matches: 5
    DecimalDatatypeValidator::checkContent: 8

matches 4 那个打印如下::

    if (isSet(fOptions, XMLSCHEMA_MODE)) {
        printf("RegularExpression::matches: 4\n");
        int matchEnd = match(&context, fOperations, context.fStart, 1);
        printf("RegularExpression::matches: 4, matchEnd: %d,%d\n", matchEnd, context.fLimit);
        if (matchEnd == context.fLimit) {
            printf("RegularExpression::matches: 5\n");
            if (context.fMatch != 0) {
                context.fMatch->setStartPos(0, context.fStart);
                context.fMatch->setEndPos(0, matchEnd);
            }
            return true;
        }
        printf("RegularExpression::matches: 6\n");
        return false;
    }

目前看，是 match 返回的值不对。还是老方法，再往里面加桩。

linx 的运行情况::

    RegularExpression::matches: 4
    RegularExpression::match: 1
    RegularExpression::match-2: 10
    RegularExpression::match: 1
    RegularExpression::match-2: 3
    RegularExpression::match-1: 4
    RegularExpression::match: 1
    RegularExpression::match-2: 3
    RegularExpression::match-1: 4  <---
    RegularExpression::matches: 4, matchEnd: -1,1

riscv 的运行情况::

    RegularExpression::matches: 4
    RegularExpression::match: 1
    RegularExpression::match-2: 10
    RegularExpression::match: 1
    RegularExpression::match-2: 3
    RegularExpression::match-1: 4
    RegularExpression::match: 1
    RegularExpression::match-2: 3
    RegularExpression::match-2: 7  <---
    RegularExpression::match: 1
    RegularExpression::match-2: 3
    RegularExpression::match-1: 4
    RegularExpression::matches: 4, matchEnd: 1,1

RegularExpression 的 match 加的桩如下::

    case Op::O_NRANGE:
        printf("RegularExpression::match-2: 3\n");
        if (!matchRange(context, tmpOp, offset, direction, ignoreCase)) {
            printf("RegularExpression::match-1: 4\n");
            return -1;
        }
        tmpOp = tmpOp->getNextOp();
        break;
    case Op::O_CLOSURE:
        printf("RegularExpression::match-2: 7\n");


20230202
---------

接着昨天的分析。再往 matchRange 里打桩。

linx 的输出::

    RegularExpression::match-2: 3
    matchRange:1
    matchRange:2
    matchRange:3
    matchRange:4
    matchRange-1:3   <---
    RegularExpression::match-1: 4

riscv 的::

    RegularExpression::match-2: 3
    matchRange:1
    matchRange:2
    matchRange:3
    matchRange:4
    matchRange:5     <---
    RegularExpression::match-2: 7

RegularExpression 的 matchRange 插的桩如下::

    printf("matchRange:3\n");
    match = tok->match(strCh);
    printf("matchRange:4\n");
    if (!match) {
        printf("matchRange-1:3\n");
        return false;
    }
    printf("matchRange:5\n");

这里是 RangeToken 的 match，在往里看。

linx 的输出::

    RegularExpression::match-2: 3
    matchRange:1
    matchRange:2
    matchRange:3
    RangeToken::match: 1, 48
    RangeToken::match: 2
    RangeToken::match: 3
    RangeToken::match: 3-1, 0, 65536
    matchRange:4
    matchRange-1:3
    RegularExpression::match-1: 4

riscv 的输出::

    RegularExpression::match-2: 3
    matchRange:1
    matchRange:2
    matchRange:3
    RangeToken::match: 1, 48
    RangeToken::match: 2
    RangeToken::match: 3
    RangeToken::match: 3-1, 67043328, 65536
    matchRange:4
    matchRange:5
    RegularExpression::match-2: 7

matchRange 有问题的桩是这样的::

    printf("RangeToken::match: 3\n");
    if (ch < MAPSIZE) {
        printf("RangeToken::match: 3-1, %d, %d\n", fMap[ch/32], (1<<(ch&0x1f)));
        return ((fMap[ch/32] & (1<<(ch&0x1f))) != 0);
    }

根据打印信息，linx 的 fMap 中的值是空的，而 fMap 在创建的时候会赋一个初始值，这个操作
是在 doCreateMap 里，这个函数只会执行一遍。再继续往里面插桩。

linx 的输出::

    RangeToken::match: 1, 48
    RangeToken::doCreateMap: 1, 256, 4
    RangeToken::doCreateMap: 2
    RangeToken::doCreateMap: 3
    RangeToken::doCreateMap: 3-1, 43, 43
    RangeToken::doCreateMap: 3-2, 43, 1, 0
    RangeToken::doCreateMap: 3-3, 0
    RangeToken::doCreateMap: 3-1, 45, 45
    RangeToken::doCreateMap: 3-2, 45, 1, 0
    RangeToken::doCreateMap: 3-3, 0
    RangeToken::match: 2

riscv 的输出::

    RangeToken::match: 1, 48
    RangeToken::doCreateMap: 1, 256, 4
    RangeToken::doCreateMap: 2
    RangeToken::doCreateMap: 3
    RangeToken::doCreateMap: 3-1, 43, 43
    RangeToken::doCreateMap: 3-2, 43, 1, 2048
    RangeToken::doCreateMap: 3-1, 45, 45
    RangeToken::doCreateMap: 3-2, 45, 1, 8192
    RangeToken::match: 2

doCreateMap 中打的桩::

    XMLInt32 begin = fRanges[j];
    XMLInt32 end = fRanges[j+1];
    printf("RangeToken::doCreateMap: 3-1, %d, %d\n", begin, end);
    if (begin < MAPSIZE) {
        for (int k = begin; k <= end && k < MAPSIZE; k++) {
            printf("RangeToken::doCreateMap: 3-2, %d, %d, %d\n", k, k/32, 1<<(k&0x1F));
            fMap[k/32] |= 1<<(k&0x1F);
            printf("RangeToken::doCreateMap: 3-3, %d\n", fMap[k/32]);
        }
    }

这有点奇怪，怎么 1<<(k&0x1F) 计算出来的值对不上的。汇编一下，看看这个计算的指令是怎样的。
不过在汇编前，先在这个 printf 前面加个内嵌汇编作为一个桩，免得找不着地。

编译器似乎对于内嵌汇编会在汇编文件里，加上 '#APP' 和 '#NO_APP' 表示内嵌开始和结束。

block 的 printf 那段汇编::

        bstart .Ltmp351.bstart
        b.std
        bnext.call  printf
        bget s10, s4, s5
        bset a0, a1, a2, a3, ra, s0, s11
        bstop .Ltmp351.bstop
        .pushsection .text.body
    .Ltmp351.bstart:
        get s10         #通过下面 set a1 可以知道, s10 即为 k, k 为 43
        slli    t#1, 32
        srai    t#1, 31
        srai    t#1, 32
        slli    t#1, 32
        srli    t#1, 27
        srai    t#1, 32
        add t#1, t#7
        slli    t#1, 32
        srai    t#1, 32
        get s5
        slli    t#1, 32 # s5 << 32
        get s10         # k
        sll t#2, t#1    # ((s5 << 32) << k) => (s5 << 75) = 0
        srai    t#1, 32 # 0 >> 32 = 0
        set s11, t#1
        slli    t#7, 32
        srai    t#1, 5
        srai    t#1, 32
        set s0, t#1
        addi    t#8, 0
        set a1, t#1
        copy    t#8     # 0
        addi    t#5, 0
        set a2, t#1
        addi    t#3, 0  # 0
        set a3, t#1     # a3 = 0
        get s4
        addi    t#1, 0
        set a0, t#1
    .Ltmp351.bstop:

从汇编上来看，只要 k 大于了32，那计算的值就会为 0。这个问题我也不知道咋整了，得问问编译器
为啥编出了这样的结果。

找了编译器的盈盈咨询了下，她那边拿着 riscv 来看了下（因为 blockisa 的 gcc 是基于 riscv
gcc 来修改的，前端逻辑是一模一样的，生成出来的汇编能在 riscv 中找到匹配的），riscv 这块
的逻辑用的是字操作的指令，但，0.13beta 就是添加了这些指令，搞不太明白。现在就等编译器那
边修改了。

20230214
----------

从盈盈那边拿到了修改后的编译器，重新验证了下，程序可以正常运行并结束。那一块的汇编修改后
为::

        bstart .Ltmp347.bstart
        b.std
        bnext.call  printf
        bget s1, s3, s4
        bset a0, a1, a2, a3, ra, s0, s8
        bstop .Ltmp347.bstop
        .pushsection .text.body
    .Ltmp347.bstart:
        get s1
        sraiw   t#1, 31
        srliw   t#1, 27
        addw    t#1, t#3
        get s4
        sllw    t#1, t#5
        set s8, t#1
        sraiw   t#4, 5
        set s0, t#1
        get s1
        set a1, t#1
        set a3, t#6
        set a2, t#5
        get s3
        set a0, t#1
    .Ltmp347.bstop:

看起来主要是将字操作指令实现了，然后将一些无意义的指令删掉了，就比如说是 "addi t#1,0"

