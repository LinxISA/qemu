620.omnetpp_s 0.13beta测例分析
**********************************

介绍
======

SpecInt 2017 测例中，620 测例在用 0.13beta 的 gcc 编译器(20230118_B003)编译后，用
QEMU 运行后产生了 aborted，输出内容::

    OMNeT++ Discrete Event Simulation  (C) 1992-2008 Andras Varga, OpenSim Ltd.
    Version: 4.0, build: 090310-10709, edition: Academic Public License -- NOT FOR COMMERCIAL USE
    See the license for distribution terms and warranty disclaimer
    Setting up Cmdenv...
    Loading NED files from a SPEC directory 13

    Preparing for running configuration General, run #0...
    Scenario: $repetition=0
    Assigned runID=speccpu-runid
    Setting up network `largeNet'...
    Initializing...

    Running simulation...
    ** Event #1   T=0   Elapsed: 0.000s (0m 00s)  0% completed   ev/sec=0
    Aborted

执行的命令是::

    wenjie@kl-dev:~/ca_spec_test/620.omnetpp_s/build/build_base_mytest-m64.0000$ qemu-linx ./omnetpp_linx ../../run/run_base_refspeed_mytest-m64.0000/omnetpp.ini


分析
======

20230207
----------

分析员：贾文杰

这个测例挺早之前就分析出来了，但是没有写分析文档，为了之后再有同样问题能够更好的去查，
在这里复盘下过程。

首先，blockisa 输出的跟 riscv 的不同地方，riscv 在最后是这样的::

    ** Event #1   T=0   Elapsed: 0.000s (0m 00s)  0% completed   ev/sec=0
    ** Event #467312603   T=2.25   Elapsed: 0.000s (0m 00s)  100% completed   ev/sec=0

    <!> Simulation time limit reached -- simulation stopped.


    Calling finish() at end of Run #0...

    End.

能想到的着手点就是看看最后面的几个输出在代码中的位置。

"** Event # ..." 的是由函数 Cmdenv::doStatusUpdate 来输出，在程序中有 5 处，这 5 处
都在 Cmdenv::simulate 函数里，函数大体样子是这样的:

.. code-block:: C

    void Cmdenv::simulate()
    {
        ...
        try
        {
            if (!opt_expressmode) { // 程序不走这里 }
            else
            {
                disable_tracing = true;
                speedometer.start(simulation.getSimTime());
                doStatusUpdate(speedometer);  --> 1

                while (true)
                {
                    cSimpleModule *mod = simulation.selectNextModule();

                    if (!mod) {
                        throw cTerminationException("scheduler interrupted while waiting");
                    }

                    speedometer.addEvent(simulation.getSimTime());

    #if !defined(SPEC) || defined(SPEC_DEBUG)
                    // print event banner from time to time
                    if ((simulation.getEventNumber()&0xff)==0 && elapsed(opt_status_frequency_ms, last_update))
                        doStatusUpdate(speedometer);  --> 2
    #endif
                    simulation.doOneEvent(mod);

                    checkTimeLimits();
                    if (sigint_received) {
                        throw cTerminationException("SIGINT or SIGTERM received, exiting");
                    }
                }
            }
        }
        catch (cTerminationException& e)
        {
            if (opt_expressmode)
                doStatusUpdate(speedometer);  --> 3
            disable_tracing = false;
            stopClock();
            deinstallSignalHandler();

            stoppedWithTerminationException(e);
            displayMessage(e);
            return;
        }
        catch (std::exception& e)
        {
            if (opt_expressmode)
                doStatusUpdate(speedometer);  --> 4
            disable_tracing = false;
            stopClock();
            deinstallSignalHandler();
            throw;
        }

        if (opt_expressmode)
            doStatusUpdate(speedometer);  --> 5
        disable_tracing = false;
        stopClock();
        deinstallSignalHandler();
    }

代码中标记 2 那两个宏都没有定义，所以 "100% complete" 不会在这里输出。剩下的 3 4 5
都有可能。opt_expressmode 为 false，标记 5 不会走到，在标记 3 和 4 的位置上加上打印，
跑一下 riscv 的，验证出来是走到标记 3。之后就得看看 blockisa 的是不是也要走到这个标记
3，但因为它是在 catch 中，blockisa 走到这里就会 abort，所以得看它抛异常的地方是不是会
产生这个 cTerminationException 异常。

cTerminationException 没有子类，那应该只能通过抛出对应异常，标记 3 才会走到。那只要在
所有抛出 cTerminationException 异常的地方加上打印，应该就能确认 blockisa 抛出异常的地
方跟 riscv 的是否是一致的。(还好该种异常抛出的地方在程序中数量不多)

重新再跑一遍后，riscv 和 blockisa 都会在 checkTimeLimits 这个函数中抛出异常::

    void EnvirBase::checkTimeLimits()
    {
        if (opt_simtimelimit!=0 && simulation.getSimTime()>=opt_simtimelimit) {
            throw cTerminationException(eSIMTIME);  ===> 在这里产生的异常
        }
        ...

        timeval now;
    #if !defined(SPEC) || defined(SPEC_DEBUG)
        gettimeofday(&now, NULL);
    #else
        now.tv_sec = now.tv_usec = 0;
    #endif
        long elapsedsecs = now.tv_sec - laststarted.tv_sec + elapsedtime.tv_sec;
        if (elapsedsecs>=opt_cputimelimit)
            throw cTerminationException(eREALTIME);
    }

有点奇怪，opt_simtimelimit 和 getSimTime 打印出来的值都是 0.000000，但不等于 0。查了
下，浮点数判等还不能这样子判断，它还涉及到精度的问题。这段 if 判断涉及到知识盲区了，不
太清楚这个是做啥。后面再看看。

现在能确定 blockisa 和 riscv 都会走到同一个异常点，但中间是否有没有问题，不能保证，也
不能确定 blockisa 是 100% 的时候出问题，应该要把中间状态打出来，不是让它从 0% 直接到
100%。

将标记 2 去除那两个宏，能将中间状态打印出来了，看到 blockisa 是从 0% 走到 99% 后，在
走到了那个 throw 去，产生异常进而 abort。

现在看来，这个程序本身的逻辑是通过异常来结束程序的运行，通过比较 blockisa 和 riscv 测例
之间的日志输出可以看到所走的是一样的，但是 blockisa 因为不支持 dwarf 而导致程序运行失败，
但是这个抛异常的地方是在切片后面，所以不影响。
