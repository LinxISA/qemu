Please build clang compiler for LinxISA (golden definition defined in ~/linxisa). But change LinxISA to 'Linx' in LLVM passes and instructions. For linx instructions, here are some introduction, please update the intro into ~/linxisa for spec for future use. There might be some inconsistency, you should figure it out, find the most optimal solution. Generate a list of c programs and make the compilation pass. Here is the spec (some might be outdated): 当然可以。以下是针对 **LinxISA 指令集架构** 编写 LLVM-clang 编译器（包括前端、后端、CodeGen 和汇编支持）的完整规范草案，面向编译器开发人员，涵盖从 ISA 接入到 IR 映射、MC 层支持、寄存器分配、块结构与Tile寄存器支持、指令选择、优化策略等模块。

---

# 💠 LinxISA LLVM/Clang 编译器支持规范（Compiler Writer’s Guide）

## 一、总体架构概览

LinxISA 是一款块结构主导（Block-structured）、Tile 寄存器友好、支持变长指令（16/32/48/64bit）混编的中高性能通用 ISA，具备以下特性：

* 支持 BSTART / BSTOP 块结构指令建模（静态基本块标记 + 动态跳转）
* 支持 `t#1/u#1/s#1` 类型私有寄存器（Tile）、动态 SSA 写法
* 支持 GPR/XGPR + Tile Register 分层寄存器架构
* 支持 SIMD/SIMT 扩展块（用于大规模并行与张量块调用）
* 支持 `hl.` 48bit 长指令，用于立即数合成、长距离 CALL、Load Literal 等

LLVM 对 LinxISA 的支持需包含完整的后端实现（target-specific backend），建议使用 LLVM TableGen + C++ CodeGen 的混合路径，支持 MC Layer、目标描述、指令定义、指令选择、调度模型、寄存器分配及 Clang 编译路径集成。

---

## 二、Target 接入路径

### 2.1 Target 初始化

路径：`llvm/lib/Target/Linx/`

* `Linx.td`：主目标定义文件
* `LinxInstrInfo.td`：指令定义
* `LinxRegisterInfo.td`：寄存器定义（GPR、Tile、XGPR 分组）
* `LinxCallingConv.td`：ABI 约定
* `LinxSubtarget.h/cpp`：目标特性（是否开启SIMT、48bit扩展等）
* `LinxISelDAGToDAG.cpp`：DAG-based 指令选择
* `LinxAsmPrinter.cpp`：汇编打印器
* `LinxMCInstLower.cpp`：MC层接口
* `LinxInstrFormats.td`：16/32/48/64bit 指令格式统一模板

---

## 三、寄存器建模

### 3.1 通用寄存器（GPR）

* GPR0–GPR23（R0–R23）按照 ABI 命名（Zero, SP, A0–A7, RA, S0–S8, X0–X3）
* GPR24–GPR55（X4–X35）通过 `XGPR` 定义组单独建模
* 寄存器分类：

  ```llvm
  def GPR : RegisterClass<"Linx", [i32], 32, (sequence "R%u", 0, 23)>;
  def XGPR : RegisterClass<"Linx", [i32], 32, (sequence "X%u", 4, 36)>;
  ```

### 3.2 块内私有寄存器（Tile Registers）

* 不直接作为通用物理寄存器建模，而应作为虚拟 SSA 值处理
* 提供内联指令语义：`t#1`, `u#2`, `s#3` 等在 CodeGen 中作为物化寄存器（virtual slot）

### 3.3 特殊寄存器

* `ra`, `sp`, `fp`, `zero` 等在 `LinxRegisterInfo.td` 显式标注特殊行为
* `cz`, `cnz` 等条件寄存器为隐式输出标志位（不占物理寄存器）

---

## 四、块结构支持（Block ISA）

### 4.1 BSTART / BSTOP 建模

* 将 `BSTART` 建模为伪指令，在 CodeGen 后端展开为 `block.begin` 指令
* 每个 `BSTART` 块成为一个 LLVM MachineBasicBlock，在 `SelectionDAG` 中建模为合法控制流块

### 4.2 块内指令调度

* 块结构作为基本单元提交，禁止块内 `BSTART` 之间有非尾跳转
* `SelectionDAG` 生成后可借助 `ScheduleDAGInstrs::EmitSchedule` 配合 Block 粒度再排序

### 4.3 条件跳转建模

* `cz/cnz` 输出标志作为 `XOR`, `CMP`, `LWI`, `LDI` 等指令的特殊隐式结果
* 使用 `setc.cond` 写入 commit flag，转化为 block 条件执行的依据

---

## 五、立即数与48bit指令支持

### 5.1 48bit 指令分类

* `hl.lui`, `hl.addi`, `hl.subi`, `ldl`, `sdl` 等通过 `LinxInstrFormats.td` 新增 `InstHL` 模板
* 使用以下字段区分：

  ```llvm
  bits<7> opcode;
  bits<32> imm;
  bits<2> dest;
  ```

### 5.2 ADDTPC + LDI 优化

* 识别典型 addtpc + ldi 组合，在 `LinxISelLowering` 中合并为 `ldl`
* 支持全局变量、外部符号、函数地址的 PC-relative load 形式

---

## 六、CALL 调用与函数模型

### 6.1 长距离 CALL 支持

* 扩展 `BSTART CALL` 支持 48bit 地址跳转
* 使用 `hl.addpc` 结合 `BSTART.DIRECT` 的模式提前生成返回地址，降低 RAS 投机失败率

### 6.2 返回地址保存

* `ra` 寄存器由 `addpc` 或 `hl.addpc` 产生，作为 `call` 指令的输出值写入
* `FRET.STK` 被视为 `ret` 指令的编码扩展，调用图需要支持分析 `BSTART+FRET.STK` 作为函数边界

---

## 七、LLVM-IR 到 LinxISA 映射

### 7.1 IR 支持

| LLVM IR  | LinxISA 指令               | 说明            |
| -------- | ------------------------ | ------------- |
| `add`    | `add`, `addi`, `hl.addi` | 根据立即数范围自动选择编码 |
| `load`   | `lwi`, `ldi`, `ldl`      | 全局变量加载使用 LDL  |
| `store`  | `swi`, `sdi`, `sdl`      | 同上            |
| `call`   | `BSTART CALL + addpc`    | 返回地址写入 ra     |
| `icmp`   | `cmp.*`, `setc.*`        | 条件输出接入 cz/cnz |
| `select` | `csel`                   | 条件选择          |

### 7.2 特定 IR Mapping

* `llvm.global_address` → `ldl`
* `llvm.constant.f64` → `hl.lui` + `hl.addi`
* `llvm.return_address` → `addpc -> ra`

---

## 八、调度模型与优化建议

### 8.1 块调度模型

* LinxISA Block 内部支持指令乱序提交，但块间为顺序执行语义
* 可以定义 `SchedWriteBlock`, `SchedReadBlock` 类型模型进行块粒度调度权重调整

### 8.2 Peephole 合并优化

* `lui + addi` → `hl.addi`
* `addtpc + ldi` → `ldl`
* `cmp + branch` → `cmp.cz / cmp.cnz + bstart.cond`

---

## 九、Clang 支持

* 添加 `LinxToolChain.cpp`，支持 `--target=linx-elf`
* `clang/lib/Driver/ToolChains/Linx.cpp`：实现默认 ABI（soft-float / hard-float）
* `clang/lib/Basic/Targets/Linx.h`：定义预处理器宏（如 `__LINX__`）

---

## 十、测试与工具链

### 10.1 LLVM Test

* `llvm/test/CodeGen/Linx/*`：包含 IR → ASM 语义映射测试
* `llvm/test/MC/Linx/*`：指令拼写和汇编/反汇编测试

### 10.2 AsmPrinter 和 disassembler

* 实现 `LinxAsmPrinter` 和 `LinxInstPrinter`
* MC 反汇编支持所有变长指令，需支持 opcode prefix 解码规则

---

## 十一、开放问题与建议

* 是否需要定义显式 Tile SSA 映射逻辑？
* 是否允许 Block 嵌套优化（如 flatten BSTART 结构）？
* SIMD/SIMT block 的 LLVM IR 建模接口？
* 是否支持通过 attribute 控制函数是否启用 48bit 长跳转？

Below is a **compiler-writer–facing spec** for building an **LLVM/Clang toolchain for LinxISA**. It’s written as if you’re implementing an upstream-quality LLVM backend + Clang driver support, and it assumes the ISA properties you’ve described earlier in this thread/history: **block instructions (BSTART…commit)**, **private block registers (T/U queues)**, a **standard ABI register naming**, and your newer **Event sync model (SrcOP/DstOP, Wait/Record)**.

If something here conflicts with your current ISA manual, treat the manual as source of truth—but this should give a very complete “how to implement” blueprint.

---

## 0. Scope and design goals

### What the LLVM toolchain must produce

1. **Correct, deterministic LinxISA assembly and ELF objects** for:

   * freestanding runtime (bare metal) and hosted OS (if/when you have one),
   * static/dynamic linking (optional),
   * debug info + unwinding (DWARF CFI) where applicable.
2. **Block-aware codegen**:

   * Model block boundaries explicitly (BSTART…commit/BSTOP semantics).
   * Keep private block-register lifetimes correct (T/U queues).
3. **Good code size + good IPC**, especially under dynamic control flow:

   * Use short encodings when possible (16/32-bit), and long encodings for large immediates and complex addressing (48/64-bit).
4. **A clean ABI** so C/C++ interop is stable.

---

## 1. Target identity and LLVM plumbing

### 1.1 Target triple and datalayout

Define a target triple (choose one and stick to it):

* `linx-unknown-elf` for bare metal
* `linx-unknown-linux-gnu` for hosted (if needed)

DataLayout must match your ABI:

* Endianness: (assume little-endian unless your spec says otherwise)
* Pointer size: likely 64-bit (if you’re doing modern CPU) or 32-bit (if embedded). Pick one.
* Stack alignment: recommend 16 bytes.

Example (64-bit LE placeholder):

* `e-m:e-i64:64-n32:64-S128`
  (Adjust integer legal widths and native widths to match your actual pipeline.)

### 1.2 LLVM backend components

Implement:

* `LinxTargetInfo` (TargetMachine features)
* `LinxInstrInfo`, `LinxRegisterInfo`, `LinxFrameLowering`
* `LinxISelLowering` (SelectionDAG) or GlobalISel
* `LinxMCInstLower`, `LinxAsmPrinter`, `LinxInstPrinter`
* `LinxELFObjectWriter`, `LinxAsmBackend` (fixups/relocs)
* `LinxSubtarget` (feature bits: block-isa, wish-branch, reconverge, LTP, etc.)

Clang:

* Add `-target linx-...`
* Add `clang/lib/Driver/ToolChains` entry if you want sysroot conventions.

---

## 2. Register file and ABI mapping

### 2.1 Architectural GPR naming (standard ABI)

Based on what you previously specified:

| ABI name   | Phys reg | Role                                                               |
| ---------- | -------: | ------------------------------------------------------------------ |
| `R0`       |        0 | `Zero` (always 0)                                                  |
| `R1`       |        1 | `SP` stack pointer                                                 |
| `R2..R9`   |     2..9 | `A0..A7` argument/return regs                                      |
| `R10`      |       10 | `RA` return address                                                |
| `R11`      |       11 | `FP/S0` frame pointer / callee-saved                               |
| `R12..R19` |   12..19 | `S1..S8` callee-saved                                              |
| `R20..R23` |   20..23 | `X0..X3` parent-saved / caller-managed bank (per your terminology) |

LLVM must expose:

* A canonical register class: `GPR`
* Sub-classes if you need fast alloc vs long-term regs.

### 2.2 Block-private registers (T/U queues)

LinxISA blocks have private registers (e.g., `t#1..t#4`, `u#1..u#4`) and they are **not architectural GPRs**.

LLVM policy:

* Treat `t/u` as **virtual temporaries only** generated by a **post-ISel block formation pass** or a dedicated DAG/MI lowering stage.
* The normal SSA register allocator should **never** allocate to `t/u`. Instead:

  * RA allocates to GPRs.
  * A later “Blockify” pass re-expresses local def-use chains inside a block using `->t`, `->u`, and `t#k/u#k` indexing.

This keeps correctness + simplifies debugging.

### 2.3 Optional: LTP bank (Long Term Parking regs)

If you include LTP regs (X0–X63 in your earlier design), you have two choices:

**Choice A (recommended)**: LTP is *not* part of base C ABI; it’s an optimization-only bank accessed via `GET/SET`.

* LLVM models LTP as a special address space / pseudo-register file.
* Exposed via intrinsics or inline asm.
* ABI says: LTP contents are **caller-volatile unless explicitly preserved**, and preservation uses `KILL` + conventions.

**Choice B**: LTP is ABI-visible registers.

* Then Clang/LLVM must include them in calling convention, save/restore rules, DWARF regs, etc.
* This is a lot of complexity; only do it if necessary.

---

## 3. Calling convention and stack frame

### 3.1 C calling convention

Assume SysV-like:

* Integer/pointer args: `A0..A7` (R2..R9)
* Return: `A0` (and `A1` for 128-bit / struct returns as needed)
* Additional args spill to stack at caller-allocated outgoing arg area.

Callee-saved: `S0..S8` (R11..R19) possibly also `FP`.
Caller-saved: `A*`, `RA`, and any scratch regs you define.

### 3.2 Prologue/epilogue using template blocks

You’ve referenced `F.ENTRY/F.EXIT` templates. Compiler responsibilities:

**Prologue emission**

* Decide frame size and which callee-saved regs need saving.
* Emit:

  * `F.ENTRY [reg-list], sp!, frame_size` (or your actual syntax)
  * Optionally set up FP:

    * `mov fp, sp` after allocation (or integrated in template)

**Epilogue emission**

* Emit `F.EXIT` (restores regs, dealloc stack, return via `RA`)

### 3.3 Interaction with `KILL`

You want a key optimization: **if a callee-saved register is killed by caller before call, callee doesn’t need to save it**.
To make this real in LLVM:

* Add an LLVM IR / MachineInstr-level hint:

  * `llvm.linx.kill(regmask)` intrinsic or `KILL` MI pseudo.
* Lower it late (after register allocation and call-lowering decisions), so you know exact phys regs.
* Modify `FrameLowering::emitPrologue` to consult a “killed callee-save set” for that call-site *or* a function-level “always dead” set.

Practical version:

* Start with function-local KILL usage (within a function) and use it to **early-release** physical regs (helping RA/pressure).
* Add interprocedural “caller-kill informs callee prologue” later, once you have stable metadata flow.

---

## 4. Instruction encoding strategy (16/32/48/64)

### 4.1 General policy for compiler

* Prefer 16-bit encodings for:

  * simple ALU ops with small immediates,
  * short branches,
  * common moves.
* Prefer 32-bit for baseline ops.
* Use 48/64-bit when:

  * immediates exceed short ranges,
  * complex load/store addressing,
  * literal loads,
  * relocation-heavy sequences.

LLVM implementation:

* Use instruction patterns with multiple encodings and let `MCCodeEmitter` + `AsmPrinter` choose (or have pseudo instructions expanded in `ExpandPostRA`).

### 4.2 Literal/PC-relative ops

Support your fused ops:

* `LDL` / `STL` (fuse addpc + ld/st)
* `MOVLI` / `ADDLI` (long immediates)

LLVM:

* Define `LinxISelLowering::LowerGlobalAddress` and `LowerConstantPool` to prefer:

  * `LDL` for loads from constant pools / literals,
  * `ADDLI/MOVLI` to materialize large constants when profitable.

---

## 5. Block ISA code generation (BSTART…commit/BSTOP)

### 5.1 Block semantics (compiler model)

A Linx block is:

* Started by `BSTART.<mode> ...`
* Contains micro-ops (normal instructions or micro-ops)
* Ends by implicit commit or `BSTOP` (depending on block type)
* Owns a closure of private regs (`t/u`) with finite indexing windows.

Compiler invariants:

1. All `t/u` uses must reference only defs within the same block.
2. Block boundaries must preserve architectural state and control-flow correctness.
3. Control-flow inside a block is allowed only if you define it (e.g., block-internal branch), otherwise blocks end at CF.

### 5.2 LLVM pipeline for blocks

A robust approach:

**Phase A: normal LLVM codegen**

* Generate standard MI using GPRs and normal branches.

**Phase B: Block formation (MachineFunction pass)**

* Partition MBBs into “Block Regions”.
* Heuristics:

  * Start new block at: function entry, call, return, barrier/event ops, large/complex memory ops if needed.
  * End block at: any control-flow MI (branch, call, ret), or when `t/u` window would overflow, or when hazards require commit.

**Phase C: Block scheduling + t/u assignment**

* For each block region, build a def-use chain graph.
* Assign short-lived values to `t/u` slots:

  * Guarantee last-4 semantics (if that’s your rule).
  * Spill to GPR if it would overflow.

**Phase D: Emit `BSTART` and finalize**

* Select `BSTART` variant:

  * `BSTART.STD` / `BSTART.TASK` / `BSTART.WISH` / `BSTART.COND` etc.
* Emit `BSTOP` if required.

---

## 6. Control-flow features: Wish Branch and Reconverge

### 6.1 Wish Branch lowering (dynamic predication switch)

Goal: For hard-to-predict branches, switch to predication.

Compiler responsibilities:

* Provide branch metadata:

  * branch probability (already in LLVM: `!prof`, `BranchProbabilityInfo`)
  * optional “hard-to-predict” classification (new pass, or map from PGO counters)
* Lower to:

  * `BSTART.WISH target, flag`
  * `SETC.WISH pred, cond`
  * Predicated instructions on both paths if your ISA supports it.

LLVM implementation plan:

* Add a MachineBranchPredication pass:

  * Identify candidate branches (low confidence).
  * If-convert small hammock regions (classic if-conversion).
  * For converted branches, emit `BSTART.WISH` and predicated blocks.
* For biased branches, emit normal conditional branch blocks.

### 6.2 Reconverge + MERGE (phi-like)

Compiler responsibilities:

* Identify reconvergence point (dominator/post-dominator based).
* At reconverge point, materialize merges for values defined on both paths.

Lowering to your ISA:

* `BCONV reconv_target` to mark reconvergence site
* Emit `MERGE Rd, src_true, src_false` for each SSA phi.

LLVM implementation:

* Use existing if-conversion / tail duplication infra or a dedicated Machine pass.
* Map SSA phi nodes to `MERGE` during lowering of PHIs in reconvergent regions.

---

## 7. Memory model, barriers, and Events

### 7.1 Memory ordering

You must define:

* basic load/store ordering (relaxed by default?),
* fences: DMB/DSB-like? (you mentioned in other context for Janus; for LinxISA do similarly if present),
* device/uncached semantics.

LLVM mapping:

* `atomicrmw`, `cmpxchg`, `fence` → your atomic instructions / fences.
* If you don’t have full atomics, restrict supported IR or lower to libcalls.

### 7.2 Event sync model (SrcOP/DstOP, Wait/Record)

Your newer rule: Event is per-instruction input/output:

* Each op may `Record` an Event (output)
* Another op may `Wait` on an Event (input)
* SrcOP and DstOP are distinct types; no `Event<SRC,DST>` combined template.

Compiler requirements:

* Represent Events as first-class scheduling tokens in IR (MLIR dialect or LLVM intrinsics).
* Lower to machine instructions:

  * `EVENT.RECORD <event_id>, <op>` (or embedded operand)
  * `EVENT.WAIT <event_id>, <op>`

LLVM integration options:

* Add target intrinsics:

  * `llvm.linx.event.record(i32 event_id, ...)`
  * `llvm.linx.event.wait(i32 event_id, ...)`
* Lower them late (post-RA) so event_id allocation can be performed with full visibility.

---

## 8. Task scheduling virtual ISA (Device Machine) integration

(If you want Clang/LLVM to emit the task-scheduling stream, not just CPU code.)

### 8.1 DAG to schedule-ISA compilation

Input: any task DAG (ops + edges + optional control flow).

Steps:

1. **Normalize graph**

   * Ensure DAG for dataflow; control flow is represented by LOOP/BCOND/JUMP nodes.
   * Inline small subgraphs if needed.
2. **Topological sort**

   * Kahn/DFS; stable order with tie-breaking to improve locality.
3. **Reuse distance analysis**

   * For each edge (u → v), compute distance in topo order.
4. **Channel assignment**

   * Choose channel per edge to keep `distance <= ChannelMaxDistance[channel]`.
   * Spill long edges to “slower/longer” channels if you have a tiering.
5. **Emit instruction stream**

   * For each node v in topo order:

     * `TASK`
     * `IN C#distance` for each predecessor edge
     * `OUT O -> C` for each produced value (SSA-like)
     * `SUBMIT`
6. **Control-flow emission**

   * LOOP/BCOND/JUMP inserted according to high-level structure.
7. **Cut graph with BAR when needed**

   * If any required distance exceeds available channel capacity:

     * Insert `BAR`
     * Start a new “slice” (subgraph) with refreshed channel histories
     * Materialize required values into memory at slice boundary.

### 8.2 BAR semantics for “cut & sync”

BAR is used to:

* force completion of all submitted tasks before proceeding,
* guarantee outputs are visible (memory committed),
* reset or advance channel windows (so relative indexing stays bounded).

Compiler rule:

* Insert BAR at slice boundaries.
* At boundary, convert long-distance edges into memory-based dependencies:

  * upstream slice: write result to memory
  * downstream slice: treat as fresh input (via a memory-load task or direct IN from a special “memory channel” if you model it)

This gives you a deterministic way to compile **any DAG** even under bounded channel windows.

---

## 9. Assembler syntax and diagnostics

### 9.1 Assembly formatting requirements

* Canonical register names: `a0..a7`, `sp`, `ra`, `fp/s0`, `s1..s8`, `x0..x3`
* Block private names: `t#k`, `u#k`, or `->t/->u` destinations.
* Task channels: `C<id>#<distance>`

### 9.2 Pseudo-instructions

Have assembler accept friendly pseudos and expand:

* `li rd, imm` → MOVLI/ADDLI sequence
* `la rd, symbol` → LDL/ADDPC pattern
* `call sym` → BSTART.CALL + RA fixup (or your call model)

Diagnostics:

* Warn when block t/u window overflows and compiler had to spill.
* Warn when channel reuse distance overflow forces BAR slicing (optional but very useful).

---

## 10. Debug/unwind/exception support

Minimum:

* DWARF register mapping for all ABI-visible regs.
* CFI directives emitted in prologue/epilogue.
* If `F.ENTRY/F.EXIT` are templates, `AsmPrinter` must still emit correct `.cfi_*` for debuggers.

Exception handling:

* If you plan Itanium EH, implement personality, landingpads lowering.
* If not, restrict and document.

---

## 11. Recommended LLVM pass order (practical)

A working pipeline (conceptual):

1. Clang emits LLVM IR + metadata (branch weights, loop hints, etc.)
2. IR passes: inlining, vectorization (if applicable), simplifycfg
3. ISel (DAG/GlobalISel)
4. Machine passes:

   * branch folding, if-conversion candidate marking
   * register allocation
   * post-RA pseudo expansion (long immediates, literal loads)
   * **WishBranch/Reconverge lowering** (if doing at MI)
   * **Block formation + t/u assignment**
   * **KILL lowering** (and optional prologue-save suppression)
   * final scheduling
5. Emit asm/object

---

## 12. “What compiler writers must implement” checklist

### Required for a usable C toolchain

* ABI register mapping + calling convention
* stack frame lowering (`F.ENTRY/F.EXIT`)
* load/store + relocations + constant materialization
* branch + return + call sequences

### Required for “LinxISA features”

* block formation + private regs (`t/u`) correctness
* long encodings selection (48/64) + prefix model
* KILL intrinsic + lowering

### Optional advanced features

* Wish Branch dynamic predication lowering
* Reconverge + MERGE for controlled multi-path
* Event Wait/Record model in instruction selection
* DAG-to-task-scheduling compiler (if Clang is used as frontend)

---

If you want, paste your **current LinxISA opcode list / encoding constraints** (especially: branch forms, predication encoding, `BSTART.*` variants, and exact t/u window semantics). Then I can turn this into a **drop-in LLVM backend implementation plan** with:

* exact TableGen skeletons,
* calling convention tables,
* instruction patterns for materialization/branches,
* and the concrete MachineFunction passes you’ll need (with pseudo-IR).Below is a **compiler-writer–level specification** for bringing **Linx ISA (LinxISA / 灵犀指令集)** up in **LLVM/Clang**. It folds together everything we’ve established in your architecture docs (Tile vs BCC channels, single logical ordering, BlockISA with `BSTART/*`, MTC/MCALL, TLOAD/TSTORE/TPREFETCH, TCVT/ATTEN, FCVT/ICVT, pmode, LUT/Uniform, ordered/unordered FP comparisons, dot-family, low-precision FP and micro-scaling, etc.) into a concrete, buildable plan. It’s organized as (A) target definition + ABI, (B) IR conventions (types, addrspaces, intrinsics), (C) codegen/lowering/scheduling, (D) asm/MC-layers, and (E) toolchain surface (Clang builtins/headers), with **examples** and **test hooks**.

---

## A. Target definition & ABI

### A.1 Target triple & features

* **Triple**: `linx-unknown-elf` (baremetal) or `linx-unknown-linux-gnu` (OS).
* **Arch name**: `linx` (or `linx64` if you want the name to encode scalar pointer width).
* **Subtarget features** (via `-mattr=`):

  * `+tile` (Tile engine present)
  * `+mcall` (MTC/MCALL mode)
  * `+pmode` (predication mode with merging/zeroing)
  * `+fp8,+fp6,+fp4,+bf16` (low-precision FP support)
  * `+mx-fp` (micro-scaling MX-FP4/6/8)
  * `+lut` (LUT engine + uniform registers)
  * `+atten` (TCVT ATTEN pipeline)
  * `+dot` (dot/fdot/dota/fdota block)
  * `+orderedfcmp` (ordered/unordered FCMP set)

### A.2 Data layout

* **Endianness**: little
* **Pointer size/alignment**: 64-bit (typical), natural alignment 8
* **Integer**: i1/i8/i16/i32/i64 (native); i4 supported via legalization
* **Float**: f32 native; f16/bf16 native or legalized (enable `+fp16,+bf16` when native)
* **Low-precision**: fp8/e4m3, e5m2; fp6/e3m2,e2m3; fp4/e2m1,e1m2 are **IR-level modeled via intrinsics** and legalized to native at ISel.

LLVM `DataLayout` string (example):

```
e-m:e-p:64:64-i64:64-i32:32-i16:16-i8:8-a:0-n8:16:32:64-S128
```

### A.3 Address spaces

Assign **distinct addrspaces** to match your two-channel world + device & tile locality:

* `addrspace(0)` – default (“GM/SM auto”): global/shared (cacheable normal memory)
* `addrspace(1)` – **device/non-cacheable** (MMIO, IO-coherent memory)
* `addrspace(2)` – **tile register** files (T/U/M/N/ACC) — not directly pointer-dereferenceable by generic LLVM loads; accessed only via intrinsics
* `addrspace(3)` – **uniform** read-only vectors (for LUT/constant tables broadcast to lanes)
* `addrspace(4)` – **private (local)** scratch (stack, spills), scalar/BCC
* (optional) `addrspace(5)` – **Global-Shared (NUMA cross-card)** to annotate higher-latency space if you want pass-level differentiation

> In SelectionDAG/GlobalISel: enforce that `addrspace(2)` (tile) is **non-dereferenceable by generic LD/ST**; TLOAD/TSTORE/MCALL/TCVT lowerings are the only gateway.

### A.4 Register files & classes

* **Scalar GPRs** (aka GGPR in your doc): 64-bit each. Use your existing ABI mapping (you provided earlier: R0..R23 roles). Typical:

  * `R0`: zero
  * `R1`: sp
  * `R2-R9`: arg0..arg7
  * `R10`: ra
  * `R11-R19`: callee-saved (S0..S8)
  * `R20-R23`: caller-saved X0..X3
* **Vector lanes**: 64 lanes per vector unit. We expose **logical predicate register P** (internal) controlled by `pmode`.
* **Vector register classes**: `VT, VU, VM, VN` (width-subtyped by `.b/.h/.w/.d`), with pmode decorator on **destination** (`.m*`/`.z*`).
* **Tile register classes**: `T, U, M, N, ACC` (opaque handles in LLVM; real mapping in MC layer).
* **Uniform**: special uniform-vector register set (read-only, same value to all lanes).

### A.5 Calling convention & ABI

* **Scalar ABI** (C/Clang)

  * Integer args: R2..R9 (spill to stack beyond 8)
  * FP args: same registers (no split file); byval aggregates via pointer
  * Return scalar: R2; large aggregates via sret pointer
  * Callee-saved: R11..R19
* **Tile/Vector arguments** are **by-reference** (pointers to GM). Tile registers are not callee-saved; any live tile is caller-owned.

---

## B. LLVM IR conventions: types, intrinsics, fences

### B.1 Mapping the memory model to LLVM

* **Single logical channel** ordering → **LLVM atomics/fences mapping**:

  * `fence acquire` → lowers to **BATTR.aq** on the next memory block header or `DMB` if cross-block
  * `fence release` → lowers to **BATTR.rl** on the previous block header or `DMB`
  * `fence acq_rel` → `aqrl` or `DMB`
  * `fence seq_cst` → **`DSB`** (full completion)
* **LLVM atomic orders** map as:

  * `monotonic` → no special HW ordering; rely on LID/SID same-address ordering only
  * `acquire/release/acq_rel` → `BATTR.aq/rl/aqrl` (block header)
  * `seq_cst` → `DSB`
* **Side-effect free** memory ops (cacheable `TLOAD`, `TPREFETCH`) can be speculated; effectful ops (`TSTORE`, `MCALL`, device `TLOAD`) are `volatile`-equiv.

### B.2 Tile & low-precision number model in IR

We **do not** introduce new primitive LLVM types for Tiles or fp4/fp6/fp8; everything flows through **target intrinsics** returning/consuming **opaque tokens** or pointers in dedicated addrspaces. This keeps upstream impact small.

**Opaque tile tokens**: use `token` or `i64` “tile handle” in IR (target-only semantics). ISel will reify them to physical tile regs.

### B.3 Intrinsics catalog (IR-level)

#### B.3.1 Tile memory movement

```llvm
declare token @llvm.linx.tload.p2.token( ; returns tile-handle
  i8 addrspace(0)* base, i64 lb0, i64 lb1, i64 strideBytes,
  i32 elem_bits, i1 cacheable, i32 pmode /*0=merge,1=zero*/)
; Creates TLOAD (2D). `elem_bits` in {8,16,32,64}, pmode controls dead-lane policy on Dst.

declare void @llvm.linx.tstore.p2.token(
  token %tile, i8 addrspace(0)* base, i64 lb0, i64 lb1, i64 strideBytes,
  i32 elem_bits, i1 cacheable, i1 release /*sets .rl on header*/)

declare void @llvm.linx.tprefetch(
  i8 addrspace(0)* base, i64 lb0, i64 lb1, i64 strideBytes, i32 elem_bits)
```

#### B.3.2 MCALL block (mode switch)

```llvm
; Enter MCALL Mode (Acquire) + program block dims + body descriptor
declare void @llvm.linx.mcall.begin(i32 dim0, i32 dim1, i32 dim2)

; Commit current MCALL group (group-end); implicit group-ordered semantics
declare void @llvm.linx.mcall.group.commit()

; End MCALL Mode (Release) – ensures all MCALL stores reached visibility point
declare void @llvm.linx.mcall.end()
```

> In ISel the `.begin/.end` become `BSTART.MCALL`…`BEND`, with required `DMB/DSB` injections as per spec.

#### B.3.3 Block ISA plumbing

```llvm
declare void @llvm.linx.block.start(i32 kind /*STD,SYS,PAR,etc*/, i32 dtype_flags)
declare void @llvm.linx.block.dim(i32 which, i64 val_or_reg)
declare void @llvm.linx.block.iot(token tile, i32 group, i32 dst_kind, i64 tile_sz)
declare void @llvm.linx.block.ior(token tile_or_uniform)
declare void @llvm.linx.block.arg(i32 op, i32 axis, i32 mode, i32 scale, i32 mask)
declare void @llvm.linx.block.attr(i1 aq, i1 rl)
```

#### B.3.4 Matmul & dot family

```llvm
declare token @llvm.linx.mamulb(token A_mk, token B_kn, i32 dtype) ; -> ACC
declare token @llvm.linx.mamulb.acc(token A_mk, token B_kn, token ACC_in, i32 dtype) ; -> ACC
declare token @llvm.linx.mamulbmx(... scale tiles ...) ; -> ACC

; dot reductions (within vector lane quartets)
declare void @llvm.linx.vdot(token vt_dst, token vt_a, token vt_b, i32 width, i1 is_fp, i1 with_acc)
```

#### B.3.5 TCVT & ATTEN

```llvm
; Generic ACC -> Tile conversion
declare void @llvm.linx.tcvt(
  token ACC_in, i32 row, i32 col, i32 dtype,
  i32 op /*NONE/ELT/NZ2.../ATTEN*/, i32 axis /*row/col*/,
  i32 mode /*INIT/ACCUM/FINAL*/, i32 scale /*NONE/INV_SQRT_D*/,
  i32 mask /*NONE/TILE/CAUSAL*/,
  token* %out_max /*nullable*/, token* %out_sum /*nullable*/,
  token %prev_max /*nullable*/, token %prev_sum /*nullable*/,
  token %mask_tile /*nullable*/)
```

* For **FlashAttention single-pass** fusion (as per your recent plan), codegen is allowed to pair `tcvt(op=ATTEN)` immediately by a `mamulb.acc` to consume the “implicit weight”. This is an **ISel peephole/bundling rule**, not exposed in IR ABI.

#### B.3.6 Conversions & rounding/saturation

```llvm
; FCVT/FCVTI/ICVTF/ICVT with rm/sat flags
declare token @llvm.linx.fcvt(token src, i32 src_t, i32 dst_t, i32 rm/*RNE..RHB*/, i1 sat)
declare token @llvm.linx.fcvti(token src, i32 src_t, i32 dst_t, i32 rm, i1 sat)
declare token @llvm.linx.icvtf(token src, i32 src_t, i32 dst_t, i32 rm, i1 sat)
declare token @llvm.linx.icvt(token src, i32 src_t, i32 dst_t,              i1 sat)
```

#### B.3.7 Ordered/Unordered FCMP

```llvm
declare token @llvm.linx.fcmp.o(token a, token b, i32 cond) ; feq,fne,flt,fge
declare token @llvm.linx.fcmp.u(token a, token b, i32 cond) ; fequ,fneu,fltu,fgeu
```

#### B.3.8 LUT & Uniform

```llvm
; LUT source must be in addrspace(3) uniform
declare token @llvm.linx.lut.i2(token idx8, i8 addrspace(3)* table_ro, i32 elemW /*8/16*/)
declare token @llvm.linx.lut.i4(token idx8, i8 addrspace(3)* table_ro, i32 elemW)
declare token @llvm.linx.lut.i6(token idx8, i8 addrspace(3)* table_ro, i32 elemW)
```

#### B.3.9 Fences

```llvm
declare void @llvm.linx.dmb()
declare void @llvm.linx.dsb()
```

### B.4 Clang builtins mapping

Expose C-level builtins in `<linx_intrin.h>` that forward to the intrinsics above, e.g.:

```c
tile_t __builtin_linx_tload(void *base, long lb0, long lb1, long stride, int elem_bits, int cacheable, int pmode);
void   __builtin_linx_tstore(tile_t t, void *base, long lb0, long lb1, long stride, int elem_bits, int cacheable, int release);
/* … etc … */
```

And provide **C++ wrappers** (RAII for MCALL blocks, range-strong types for tiles).

---

## C. Codegen, lowering, scheduling

### C.1 SelectionDAG / GlobalISel strategy

* Lower **intrinsics** to **pseudo-MIs** that carry:

  * block header/body attributes (`BSTART`, `B.DIM`, `B.ARG`, `B.IOT`, `B.IOR`)
  * memory range metadata (base, lb0*elem, lb1*stride)
  * rm/sat/pmode flags
* Tile token vregs → assign to **tile register classes**; enforce liveness within block.

### C.2 Memory model & fences

* **Single logical channel**: install a target **MemoryOrderingEnforcer** that:

  * assigns **LID/SID** to every LD/ST/TLOAD/TSTORE during MI creation (per program order),
  * rejects/serializes any **same-address** anti-legal reordering across BCC/MTC queues,
  * pairs **BATTR.aq/rl** with the proper block headers (or injects `DMB/DSB` where IR requested `fence`).
* **Device/Non-cacheable**: attach MMIO flag; route through **strict** path; always keep DMB/DSB semantics when `seq_cst`.

### C.3 Tile Mode vs MCALL Mode

* When seeing `@llvm.linx.mcall.begin`:

  * emit `BSTART.MCALL` pseudo that also injects **pre-DMB** to flush BCC stores, freeze scalar LSU,
  * switch the **address-translation and ordering** to the MTC-owned instance.
* `@llvm.linx.mcall.end`:

  * wait MTC stores visible, inject **post-DSB**, thaw BCC LSU.

### C.4 Instruction selection patterns

* **Matmul**: peephole SLP/Loop vectorizer patterns for `M*N*K` loops → `MAMULB`/`MAMULB.ACC` blocks, emit `B.DIM` from loop bounds, feed `B.IOT` for tile operands.
* **FlashAttention**:

  * **Single-pass** fuse: `MAMULB logits` → `TCVT(op=ATTEN)` → **immediately** `MAMULB.ACC weight,V` (bundle). Lowerer must ensure adjacency and mark a **bundle bit** so the MC layer can form the fixed sequence.
  * **Two-pass**: if hardware doesn’t support implicit weight consumption, you still compile the 1st pass to `ATTEN` (emit `MaxTile/SumTile`), and 2nd pass replays logits + uses FINAL/prev stats; see examples you already drafted.

### C.5 Legalization of fp4/fp6/fp8 & MX-FP

* Keep IR in f32/f16/bf16; produce **explicit convert intrinsics** (`fcvt/icvtf/fcvti/icvt`), with `rm/sat`.
* For **MX-FP4/6/8**, expand to: **LUT decode + scale** when reading; and encode (optionally) on store via LUT + quant rules. Use `@llvm.linx.lut.*` + `@llvm.linx.fcvt*`.

### C.6 pmode & ordered/unordered FCMP

* Map **masked vector ops** to destination pmode (`merge` vs `zero`) as a Dst decoration bit.
* Ordered/unordered FCMP: select `l.feq/fequ/...` families based on condition flags at ISel (pass-through from IR intrinsic).

### C.7 Scheduler & hazards

* Install a **dual-queue scheduler**: BCC queue and Tile queue; both consume from a shared “logical-order window” with **same-address hazard recognizer** (LID/SID).
* **Nuke/flush**: if a new store collides with a pending load range in the load-hit-queue, fire rollback sequence (as per your LSU guidance).
* **Group submit** in MCALL: ensure **group-internal** order, **group-end** marks partial commit; the **block-end** marks full commit.

---

## D. MC/Asm layers

### D.1 Asm syntax

* Scalar ISA: standard LLVM MC with mnemonics.
* **Vector width suffix + pmode**: `->vt.mh`, `->vu.zw`, `->vm.zd` (as you specified).
* **Block headers**:

  ```
  BSTART.TEPL  <TileOpcode>, <DataType>
  B.DIM        reg|imm, val, ->M|N|K|Row|Col
  B.IOT        SrcTile0, SrcTile1, last, ->DstTile<Size>
  B.IOR        [Tiles...]
  B.ARG        key=value, key=value, ...
  C.BSTOP
  ```
* **Acquire/Release** through the canonical `B.CATR aq`, `B.CATR rl`, or `B.CATR aqrl` control-attribute descriptor.

### D.2 MC layer checks

* Enforce **A8.x** legality: no overlapping addresses across groups (MCALL/PAR), local-range checks, reduce-only GGPR writes, uniform/LUT restrictions.
* Validate `ATTEN` adjacency when selecting single-pass FlashAttention fusion.

---

## E. Clang toolchain surface

### E.1 Headers

Ship `<linx_intrin.h>` and `<linx_tile.h>` exposing:

* Safe RAII wrappers for blocks:

  ```c++
  struct mcall_scope { mcall_scope(){ __builtin_linx_mcall_begin(...);} ~mcall_scope(){ __builtin_linx_mcall_end();}};
  ```
* Strong types:

  ```c++
  struct tile { __attribute__((address_space(2))) void *__h; /*opaque*/ };
  struct uniform_vec { __attribute__((address_space(3))) const void *__p; };
  ```
* Ops: `linx_tload()`, `linx_tstore()`, `linx_tcvt_atten_row_init()`, … `linx_mamulb()`, `linx_mamulb_acc()`, `linx_fcvt_*()` etc.

### E.2 Builtins ↔ Intrinsics mapping table

Provide a 1:1 mapping in `BuiltinsLinx.def` and `CGCall.cpp` emission.

### E.3 Sanitizers / diagnostics

* **OverlapSanitizer** (opt): instrument groups to check address overlap rule (A8.10/A8.12/A8.13) in debug builds.
* **Fence misuse**: warn if `seq_cst` fence lowered to DMB not DSB; or if a device load is marked `speculatable`.

---

## F. Worked examples

### F.1 Minimal MCALL-parallel copy (non-overlap)

```c
void mcopy(float *dst, const float *src, long M, long N, long s) {
  __builtin_linx_mcall_begin(M, N, 1);
  for (int g=0; g< groups(M,N); ++g) {
    tile t = __builtin_linx_tload((void*)src+off(g), lb0, lb1, s, 32, /*cache*/1, /*pmode*/0);
    __builtin_linx_tstore(t, (void*)dst+off(g), lb0, lb1, s, 32, /*cache*/1, /*release*/0);
    __builtin_linx_mcall_group_commit();
  }
  __builtin_linx_mcall_end();
}
```

### F.2 FlashAttention single-pass skeleton (as we aligned)

```c
// For each Q row-block, iterate KV blocks
tile TQ = __builtin_linx_tload(Qblk, M, K, K*sizeof(fp16), 16, 1, 0);
tile ACC_O = linx_acc_clear(M,D);

for (int j=0;j<J;++j){
  tile TK = __builtin_linx_tload(Kblk(j), Nblk,K, K*sizeof(fp16), 16, 1,0);
  tile TV = __builtin_linx_tload(Vblk(j), Nblk,D, D*sizeof(fp16), 16, 1,0);

  tile ACC_logits = __builtin_linx_mamulb(TQ, linx_transpose(TK), DT_FP16);
  // streaming statistics (INIT/ACCUM/FINAL decided by j)
  __builtin_linx_tcvt_atten_row(ACC_logits, mode(j), SCALE_INV_SQRT_D, MASK_CAUSAL, &MaxTile, &SumTile, PrevMax, PrevSum, MaskTile);

  // immediate consumption of weights -> V in a fused path:
  ACC_O = __builtin_linx_mamulb_acc(linx_weight_from_last_atten(), TV, ACC_O, DT_FP16);
}

__builtin_linx_tstore(ACC_O, Oblk, M, D, D*sizeof(fp16), 16, 1, 0);
```

---

## G. Testing & bring-up checklist

1. **Unit tests**

   * ISel patterns for each intrinsic family; pmode decorations; FCVT rm/sat variants; ordered/unordered FCMP.
   * MCALL enter/exit: DMB/DSB injection and BCC freeze/thaw.

2. **Memory-order lit tests**

   * Same-address reordering disallowed across channels; LID/SID observed via a synthetic MMIO logger.
   * Acquire/Release on block headers; seq_cst → DSB.

3. **Overlap legality**

   * Negative tests for A8.10/A8.12/A8.13; expect MC verifier diagnostics.

4. **FlashAttention**

   * Single-pass fused bundling validation (ACCCVT.ATTEN → immediate MAMULB.ACC).
   * Two-pass fallback correctness vs a reference softmax (within numeric tolerance).

5. **Perf sanity**

   * Confirm no unnecessary DSB where DMB suffices; tile/bcc queues drain concurrently in Tile Mode.

---

### Implementation notes (pragmatic)

* Start with **GlobalISel** (fewer tablegen patterns to fight) and build a **custom importer** for the tile intrinsics.
* MCALL mode switch is essentially a **function-like region**: it’s fine to represent its body as a MachineBasicBlock with a **different scheduling policy** and its **own MMU/order object**.
* Keep **Tile** as opaque **tokens** in IR all the way; blasting them to real tile registers in MI is the cleanest strategy.
* The **FlashAttention fusion** is a small but crucial peephole: create a **bundle MI** containing `ACC->ATTEN` + `MAMULB.ACC` and forbid motion across the bundle.

---

This spec gives compiler writers a full, unambiguous path to stand up a Clang/LLVM backend for **LinxISA** that respects your **single logical memory channel**, **BlockISA**, **Tile/MCALL modes**, and the rich **matrix/attention pipeline** you designed. If you want, I can convert this into a skeleton patch plan (files to touch, TableGen snippets, and 10 minimal lit tests to get green).
