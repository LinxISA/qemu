supertest 测例分析
*******************

介绍
=====

本篇主要是记录并分析 supertest 用 QEMU 运行时产生的错误，以供后来者了解情况。

记录人：贾文杰

20230227
=========

编译器给出了 0.16 的 supertest，用来测试编译器或 QEMU 的问题。因为 0.16 版本是跟 0.15
一起调试的，架构上相对于 0.14 变化很大，glibc 改动也挺多的，所以这次给的 supertest 是
glibc 版本的。

跑了下 supertest 出现了非法指令异常，从日志上看，它是跑了一个 sysget 指令，获取的 csr
编号是 0x800，跟编译器沟通后，0x800 csr 是 UTPR(User Thread Pointer register)，而
QEMU 没有实现这个，导致出现了这么个异常。

UTPR 的引入是因为 tp 寄存器从 gpr 中移出，但为了能记录线程的信息也就弄了这么个 csr，但
是，PTR 不只有 UTPR，还有 S/MTPR，这个搞不明白，之前就能用 tp，三个特权级共用，为啥
现在得为每个特权级弄个 tp 呢？

QEMU 将 TPR 实现后，再次跑 supertest 出现了 segment fault。从日志上看，是下面这个块体
出现了问题::

    1043c:   002a                    lw.ip   0x0 # 0  ==> 对应地址上的数据是 0x0
    1043e:   020a                    lwu.ip  0x4 # 4  ==> 对应地址上的数据是 0x800
    10440:   001e                    sysget  t#1
    10442:   4000                    add t#3,t#1
    10444:   0d32                    set a5,t#1
    10446:   2064                    ld  [t#2,0]
    10448:   0c32                    set a4,t#1
    1044a:   0112                    get sp
    1044c:   1021                    subi    t#1,16
    1044e:   0132                    set sp,t#1

开启 singlestep 后，确认到是 0x10446 这条指令出现了问题。从 cpu 上看 UTPR 的值是 0，
也就出现了访问 0 地址的异常，也就是 segment fault。搜索了整个日志，只有获取 UTPR 的操作，
没有设置的操作。

20230228
=========

用了 0.16 glibc 的编译器，编了个 hello world 测例，可以运行起来。从这个测例的日志上看，
glibc 对 tp 的初始化是在 __libc_setup_tls，但是这个是在 glibc 的 start files 中，然
而 supertest 之前是用来跑 function model 的，function model 不支持系统调用那些东西，
所以，编译器团队在编 supertest 的时候，是用自己写 start files 来编译的，也就是编译时加
上 -nostartfiles 参数。

现在问题清楚了，编译器那边就得重新再编一版 supertest。等待ing

20230301
=========

今早问了下王鑫，supertest 编译的情况。编了一个晚上了，还没有编译完成。因为加上了 glibc
的 start files，又用的是静态链接，每个 supertest 测例相比之前，体积大了很多，大概是从
455K 变到了 12M。编出来的 supertest 达到了 200G，解压都不知道要解压多久。还在解压中。。

王涛提出后面可以加上 -ffunction-sections 将每个函数单独放到一个 section 中，再加上
--gc-sections 将未使用的 section 给去掉，这样来减少 elf 的大小，对静态链接尤其有用。

supertest 已经给了出来，跑了一下，有以下测例出现了问题::

    超时的测例：
    ./suite/iec60559/denormal/tf2d.c/tf2d
    ./suite/C99/7/12/6/4/t1.c/t1
    ./suite/depth/c99opmix/t0828.0.c/t0828.0
    ./suite/depth/c99opmix/t0826.0.c/t0826.0
    ./suite/depth/major/struct/t0036.0.c/t0036.0
    ./suite/depth/major/extern/t0019.0.c/t0019.0
    ./suite/depth/c99major/struct/t0045.0.c/t0045.0
    ./suite/depth/c99major/extern/t0019.0.c/t0019.0
    ./suite/depth/minor/extern/t0019.0.c/t0019.0
    ./suite/depth/opmix/opmix/t0637.0.c/t0637.0
    ./suite/Cxx14/5/1/2/tgl4variadic2.C/tgl4variadic2

    abort 的测例：
    ./suite/Cxx11/5/1/2/t08.C/t08

超时的测例从日志上看，最后面执行到的是一条 ecall 指令，查看了 a7 寄存器，对应的系统调用
号是 0x62，也就是 futex，出错的函数是 __lll_lock_wait_private。

这个明天再看下是啥原因。

20230302
=========

花了个上午把 0.16 的 QEMU 反汇编实现了下，主要是修改了下解码头相关的东西。

昨天那个问题，找王鑫要了对应测例的 0.14 版本。用 QEMU 跑了下，从日志上看，这个函数在
0.14 上并不会执行到，那只能看看为什么会走到了这个函数。

看了下 lock wait 的调用栈，大概是这样的::

    __libc_start_main
        -> main
        -> exit
            -> __run_exit_handlers
                -> call_fini
                    -> __do_global_dtors_aux
                        -> deregister_tm_clones
                        -> __deregister_frame_info
                            -> __deregister_frame_info_bases
                -> __lll_lock_wait_private --> 这里调用 futex

lock wait 是在函数 __run_exit_handlers 中调用的，这一部分是在 glibc 中的。搞了份
glibc 代码过来看，在进入 exit handler 这个函数的时候，会先尝试获取锁，并进入互斥区，
当这个锁已经被使用了，将会一直等待::

    void
    attribute_hidden
    __run_exit_handlers (int status, struct exit_function_list **listp,
                 bool run_list_atexit, bool run_dtors)
    {
      /* First, call the TLS destructors.  */
    #ifndef SHARED
      if (&__call_tls_dtors != NULL)
    #endif
        if (run_dtors)
          __call_tls_dtors ();

      __libc_lock_lock (__exit_funcs_lock);  <--- 这个是一堆的宏，最后调 lock wait
      ...
    }

    #define __libc_lock_lock(NAME) ({ lll_lock (NAME, LLL_PRIVATE); 0; })

    #define lll_lock(futex, private) __lll_lock (&(futex), private)

    #define __lll_lock(futex, private)                                       \
      ((void)                                                                \
       ({                                                                    \
         int *__futex = (futex);                                             \
         if (__glibc_unlikely                                                \
             (atomic_compare_and_exchange_bool_acq (__futex, 1, 0)))         \
           {                                                                 \
             if (__builtin_constant_p (private) && (private) == LLL_PRIVATE) \
               __lll_lock_wait_private (__futex);   <----这个函数             \
             else                                                            \
               __lll_lock_wait (__futex, private);                           \
           }                                                                 \
       }))


目前的情况看，0.14 版本没有调用 lock wait 函数，那这个锁 __exit_funcs_lock 在这里是
没有使用的，而 0.16 版本有调用 lock wait，那这个锁应该是有使用的。

20230303
=========

为了确认下那个锁在 0.16 是被使用的，通过 QEMU 的日志，看看锁的值，从 lock wait 函数的
入参着手，不过这个入参的值是一个地址，还得跑两遍，第一遍记录下锁的地址::

    ----------------
    IN: __lll_lock_wait_private
    0x0000000000036410:  00000420 20000000 std_block.fall_through next:0x36418, ptr:0x366c8, attr:none, out_reg(), in_reg()

    Trace 0: 0x7ff274155fc0 [0000000000000000/0000000000036410/00004200/00000000] __lll_lock_wait_private
     pc       0000000000036410
     r0/ra    0000000000021cd0 r1/sp    0000004000800220 r2/a0    0000000000110aa0 r3/a1    0000000000000000   ----> a0 为 lock wait 的入参，即锁的地址

第二遍在 QEMU 中的 load/store 对这个地址就行监控，当 load/store 的地址为锁的地址时，
将 load/store 的值输出到日志里。从日志上看，lock wait 读到这个锁的值为 1::

    ----------------
    IN: __lll_lock_wait_private
    0x0000000000036420:  00000420 20000000 std_block.fall_through next:0x36428, ptr:0x366c8, attr:none, out_reg(a4), in_reg(a0)
        0x00000000000366c8:  0212              get             a0
        0x00000000000366ca:  0044              lw              [t#1, 0]
        0x00000000000366cc:  0c32              set             a4, t#1

    Trace 0: 0x7f7230156240 [0000000000000000/0000000000036420/00004200/00000200] __lll_lock_wait_private
    ----------------
    IN:
    (block body) 0x000366c8+6 in (0x000366c8-0x000366ce)

    Trace 0: 0x7f7230156400 [0000000000000000/00000000000366c8/00004200/00000200]
    linx-exec-log(110aa0): cflags_next_tb=ffffffff, tpc1/2=366c8/366ce  ---> 0x110aa0 是 lw 指令要读取的内存地址
    linx-exec-log(1): cflags_next_tb=ffffffff, tpc1/2=366c8/366ce  ---> 1 是 lw 指令读取到的值

在日志里再往上翻翻，找到它最近设置的地方，能看的出来它是往里面置 0，但是在 lock wait 里
却读到的是 1::

    ----------------
    IN: __run_exit_handlers
    0x0000000000021c38:  fffffd68 00007f72 3de0ac80 20202000 std_block.fall_through next:0x21c48, ptr:0x21ff8, attr:relaxed, out_reg(a5), in_reg(s0,a5)
        0x0000000000021ff8:  0512              get             s0
        0x0000000000021ffa:  0044              lw              [t#1, 0]
        0x0000000000021ffc:  0d12              get             a5
        0x0000000000021ffe:  400e              sw              t#1, [t#3, 0]
        0x0000000000022000:  4001              addi            t#3, 0
        0x0000000000022002:  0d32              set             a5, t#1

    Trace 0: 0x7f7230148c00 [0000000000000000/0000000000021c38/00004200/00000200] __run_exit_handlers
    ----------------
    IN:
    (block body) 0x00021ff8+8 in (0x00021ff8-0x00022004)

    Trace 0: 0x7f7230148dc0 [0000000000000000/0000000000021ff8/00004200/00300600]
    linx-exec-log(110aa0): cflags_next_tb=ffffffff, tpc1/2=21ff8/22004  ---> lw
    linx-exec-log(1): cflags_next_tb=ffffffff, tpc1/2=21ff8/22004
    linx-exec-log(110aa0): cflags_next_tb=ffffffff, tpc1/2=21ff8/22004  ---> sw
    linx-exec-log(0): cflags_next_tb=ffffffff, tpc1/2=21ff8/22004
    ----------------
    IN:
    (block body) 0x00022000+4 in (0x00021ff8-0x00022004)

    Trace 0: 0x7f7230148fc0 [0000000000000000/0000000000022000/00004200/00000200]

这有点奇怪，再把中间码打出来看看，看看有没有写成功::

    ----------------
    IN:
    (block body) 0x00021ff8+8 in (0x00021ff8-0x00022004)

    OP after optimization and liveness analysis:

     ---- 0000000000021ff8
     mov_i64 TR1,lg-r5/s0                     sync: 0  dead: 1  pref=%rsi
     mov_i32 ibpc,$0x1                        sync: 0  dead: 0  pref=0xffff

     ---- 0000000000021ffa
     mov_i64 tmp5,TR1                         pref=0xf038
     call log,$0x0,$0,env,TR1                 dead: 0 1
     call load_data,$0x0,$1,tmp4,env,tmp5,$0xa  dead: 1 2 3  pref=none
     call log,$0x0,$0,env,tmp4                dead: 0
     mov_i64 TR2,tmp4                         sync: 0  dead: 0 1  pref=0xffff
     add_i32 ibpc,ibpc,$0x1                   dead: 1  pref=0xffff

     ---- 0000000000021ffc
     mov_i64 TR3,lg-r13/a5                    sync: 0  dead: 0 1  pref=0xffff
     add_i32 ibpc,ibpc,$0x1                   sync: 0  dead: 0 1  pref=0xffff

     ---- 0000000000021ffe
     mov_i64 tmp4,TR1                         pref=0xf038
     call log,$0x0,$0,env,TR1                 dead: 0 1
     call log,$0x0,$0,env,TR3                 dead: 0 1
     call store_data,$0x0,$0,env,tmp4,TR3,$0x4  dead: 0 2 3
     mov_i64 TR4,tmp4                         sync: 0  dead: 0 1  pref=0xffff
     add_i32 ibpc,ibpc,$0x1                   sync: 0  dead: 0 1 2  pref=0xffff
     mov_i64 pc,$0x22000                      sync: 0  dead: 0 1  pref=0xffff
     exit_tb $0x0

     ----------------
     IN:
     (block body) 0x00022000+4 in (0x00021ff8-0x00022004)

     OP after optimization and liveness analysis:
      ld_i32 tmp1,env,$0xfffffffffffffff0      pref=0xffff
      brcond_i32 tmp1,$0x0,lt,$L0              dead: 0

      ---- 0000000000022000
      mov_i64 TR5,TR2                          sync: 0  dead: 0 1  pref=0xffff
      add_i32 ibpc,ibpc,$0x1                   sync: 0  dead: 0 1  pref=0xffff

      ---- 0000000000022002
      call mask_check,$0x0,$0,env,$0xd,$0x2    dead: 0 1 2
      mov_i64 lg-r13/a5,TR5                    sync: 0  dead: 0 1  pref=0xffff
      add_i32 ibpc,ibpc,$0x1                   sync: 0  dead: 0 1 2  pref=0xffff
      call mask_check,$0x0,$0,env,$0x0,$0x3    dead: 0 1 2
      mov_i64 r13/a5,lg-r13/a5                 sync: 0  dead: 0 1  pref=0xffff
      mov_i64 carg,$0x0                        sync: 0  dead: 0  pref=0xffff
      mov_i32 ibpc,$0xffffffff                 sync: 0  dead: 0 1  pref=0xffff
      mov_i64 tpc,$0xffffffffffffffff          sync: 0  dead: 0  pref=0xffff
      mov_i64 bpc,$0xffffffffffffffff          sync: 0  dead: 0  pref=0xffff
      mov_i64 next_bpc,$0xffffffffffffffff     sync: 0  dead: 0  pref=0xffff
      mov_i64 tpc1,$0xffffffffffffffff         sync: 0  dead: 0  pref=0xffff
      mov_i64 tpc2,$0xffffffffffffffff         sync: 0  dead: 0 1  pref=0xffff
      mov_i64 set_mask,$0x0                    sync: 0  dead: 0 1  pref=0xffff
      mov_i64 pc,$0x21c48                      sync: 0  dead: 0 1  pref=0xffff
      call lookup_tb_ptr,$0x6,$1,tmp3,env      dead: 1  pref=none
      goto_ptr tmp3                            dead: 0
      set_label $L0
      exit_tb $0x7f3d28147c83

这里看出问题来了，当前的块是一个原子块，而原子块的实现是，当写值时，先往缓存里写，缓存的
大小为一个 cache line 的大小，等到块提交时，再将缓存的数据以一个 cache line 为单位，整
个写到内存中，而这里块提交阶段没有将缓存中的数据写回到内存中(正常情况下，块提交的中间码
会有个 call write_store_buf_to_mem)，导致后面读取到的值仍然为 1。

这个块有个特点，它的块身因为跨页而被拆成了两个 TB，第一个 TB 中的 load/store 用到了
原子块实现，而第二个 TB 没有用到。从 QEMU 的源码上看，只有第一个 TB 执行的时候会带有
CF_ATOMIC 的标记位，并进入个函数 cpu_exec_multi_steps_atomic，在这里面会先进入互斥区，
将其他 cpu 停下来，只有当前原子块的 cpu 可以执行。但是当第一个 TB 执行完，就会退出这个
函数，第二个 TB 就按照正常流程走，进入 cpu_exec，不会带有 CF_ATOMIC，cpu 间也是并行运
行，同时，因为第二个 TB 不带有 CF_ATOMIC，它翻译出来的中间码也就不会有 call
write_store_buf_to_mem。

之后，就得修改下 QEMU 的原子块实现，来适配跨页原子块的情况。不过，话说回来，原子块跨页
不会导致性能变差嘛？
