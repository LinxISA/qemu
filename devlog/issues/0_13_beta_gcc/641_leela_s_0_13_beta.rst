641.leela_s 0.13beta测例分析
**********************************

介绍
======

在用 0.13beta 的 gcc 编译器(20230118_B003)编译并运行后，用 QEMU 运行后产生 aborted。
执行的输入文件是在 run/run_base_refspeed_mytest-m64.0000/ref.sgf，执行命令为::

    wenjie@kl-dev:~/ca_spec_test/641.leela_s/build/build_base_mytest-m64.0000$ qemu-linx ./leela_s ../../run/run_base_refspeed_mytest-m64.0000/ref.sgf

本篇就是用来记录 641 测例的分析过程。

分析
======

20230206
----------

分析人：贾文杰

之前 QEMU 跑了大概 6 天都没有运行完成，先挂起。现在 QEMU 已经优化了，应该可以较快的复现。

先整体跑了下，大概需要 194min 完成，相比之前 6 天快了许多了(riscv 需要 93min)。对比了
下两边程序输出出来的日志，两边的内容都是一样的，只是最后 linx 这边多了个 aborted。因为
之前分析 620 的时候，分析出来是程序跑的时候，会陷入一个死循环，在循环的内部运行完成后，
通过抛出异常来结束这个循环。现在 641 两边跑出来的结果都是相同，怀疑是跟 620 类似的问题，
我先看下源码吧。

先直接从 main 开始看，主要执行就是一个 try-catch，在这里面塞入了一个死循环，程序大概是
这样的::

    int main (int argc, char *argv[]) {
        ...
        try {
            for (;;) {  ==> 一个死循环被 try-catch 包裹着
                std::auto_ptr<SGFTree> sgftree(new SGFTree);

                sgftree->load_from_file(filename, counter++);

                *maingame = sgftree->get_mainline();

	        int move;
                do {
                    std::auto_ptr<UCTSearch> search(new UCTSearch(*maingame));

                    move = search->think(maingame->get_to_move(), UCTSearch::NORMAL);

                    maingame->play_move(move);
                    maingame->display_state();

                } while (maingame->get_passes() < 2 && move != FastBoard::RESIGN);
            }
        } catch (std::exception & e) {
        };

        return 0;
    }

从上面的程序上看，这个正常执行都是需要在循环内抛出异常来结束这个循环，最后再结束程序运行。
对于这些会报 aborted 的程序，最简单的方法就是在程序里所有 throw 的地方加个桩，因为这些
aborted 大概率就是当前 blockisa 编译器不支持 dwarf，同时会产生异常导致的。通过在所有
throw 的地方加桩可以知道程序结束的地方是不是一致的。如果不一致，可以顺着执行流逐步的往上
查，如果一致再另说。

641 测例里总共有 7 处有 throw 语句，往每个 throw 前加上打印，程序跑出来是在
load_from_file 函数触发异常：

.. code-block:: C++

    void SGFTree::load_from_file(std::string filename, int index) {
        std::string gamebuff = SGFParser::chop_from_file(filename, index);

        if (gamebuff.empty()) {
            std::cout << "SGFTree::load_from_file: empty:" << std::endl;
            throw std::exception();
        }

        load_from_string(gamebuff);
    }

其中 chop_from_file 函数是从输入文件中读取一个测例，在输入文件中，每个测例用左右括号分
隔，下面是输入文件中的一个测例::

    (;
    FF[4]
    EV[14th Computer Olympiad]
    PC[Pamplona, Spain]
    RU[Chinese]SZ[9]KM[7.5]TM[1800]DT[2009-05-13]
    RO[10 (playoff)]
    PB[MoGo]
    PW[Fuego]
    RE[W+]
    ;B[ee]BL[1799.583]
    ;W[dg]WL[1799.803]
    ;B[cf]BL[1799.315]
    ;W[ff]WL[1799.548]
    ;B[fe]BL[1608.436]
    ;W[gf]WL[1798.568]
    )

当输入文件中所有测例都执行完成后，gamebuff 即为空，那么也就会触发异常，进而结束测例程序。
根据 SpecInt 测例输出来看，它是最后一个测例跑完后，才触发了 aborted。那这个问题其实没有
必要在继续往下看了，之后就需要白晓瑞那边，看看他是否是要修改源码绕过这个异常，进而生成
切片。

总结
======

该测例之所以 aborted 是因为程序本身的逻辑是要通过异常来结束程序。因为切片的 kernel 函数
是在循环内，也就是在异常前面，切片的生成不会受影响，不用修改源码绕过异常。
