target triple = "linx64"

declare <1024 x i32> @llvm.linx.tma.tload(ptr, i32)
declare void @llvm.linx.tma.tstore(ptr, <1024 x i32>, i32)

@src_buf = internal global [1024 x i32] zeroinitializer, align 4096
@dst_buf = internal global [1024 x i32] zeroinitializer, align 4096

@l0_iottbr = internal global [512 x i64] zeroinitializer, align 4096
@l1_iottbr = internal global [512 x i64] zeroinitializer, align 4096
@l2_iottbr = internal global [512 x i64] zeroinitializer, align 4096
@l3_iottbr = internal global [512 x i64] zeroinitializer, align 4096

define void @virt_exit(i32 %code) noreturn {
entry:
  %exit_reg = inttoptr i64 268435460 to ptr
  store volatile i32 %code, ptr %exit_reg, align 4
  br label %loop

loop:
  br label %loop
}

define void @trap_vector() noreturn {
entry:
  %exit_reg = inttoptr i64 268435460 to ptr
  store volatile i32 1, ptr %exit_reg, align 4
  br label %loop

loop:
  br label %loop
}

define void @_start() noreturn {
entry:
  ; EVBASE (ACR0) -> trap_vector, so unexpected faults exit.
  %tv = ptrtoint ptr @trap_vector to i64
  call void asm sideeffect "ssrset $0, 0x0f01", "r,~{memory}"(i64 %tv)

  ; Build a minimal IOMMU page table:
  ;   IOVA 0x10000 -> src_buf, IOVA 0x11000 -> dst_buf
  %l0e0 = getelementptr inbounds [512 x i64], ptr @l0_iottbr, i64 0, i64 0
  %l1p = ptrtoint ptr @l1_iottbr to i64
  %l1desc = or i64 %l1p, 3
  store volatile i64 %l1desc, ptr %l0e0, align 8

  %l1e0 = getelementptr inbounds [512 x i64], ptr @l1_iottbr, i64 0, i64 0
  %l2p = ptrtoint ptr @l2_iottbr to i64
  %l2desc = or i64 %l2p, 3
  store volatile i64 %l2desc, ptr %l1e0, align 8

  %l2e0 = getelementptr inbounds [512 x i64], ptr @l2_iottbr, i64 0, i64 0
  %l3p = ptrtoint ptr @l3_iottbr to i64
  %l3desc = or i64 %l3p, 3
  store volatile i64 %l3desc, ptr %l2e0, align 8

  %l3e16 = getelementptr inbounds [512 x i64], ptr @l3_iottbr, i64 0, i64 16
  %l3e17 = getelementptr inbounds [512 x i64], ptr @l3_iottbr, i64 0, i64 17
  %srcp = ptrtoint ptr @src_buf to i64
  %dstp = ptrtoint ptr @dst_buf to i64
  %srcdesc = or i64 %srcp, 253
  %dstdesc = or i64 %dstp, 253
  store volatile i64 %srcdesc, ptr %l3e16, align 8
  store volatile i64 %dstdesc, ptr %l3e17, align 8

  ; Enable the tile IOMMU (ACR1 bank).
  %l0p = ptrtoint ptr @l0_iottbr to i64
  call void asm sideeffect "hl.ssrset $0, 0x1f14", "r,~{memory}"(i64 %l0p)
  ; IOTCR: IME=1, SZ=16 (48-bit canonical IOVA).
  call void asm sideeffect "hl.ssrset $0, 0x1f15", "r,~{memory}"(i64 33)

  br label %init

init:
  %i = phi i32 [ 0, %entry ], [ %i_next, %init ]
  %srcp_i = getelementptr inbounds [1024 x i32], ptr @src_buf, i32 0, i32 %i
  %dstp_i = getelementptr inbounds [1024 x i32], ptr @dst_buf, i32 0, i32 %i
  %pat = add i32 305397760, %i
  store i32 %pat, ptr %srcp_i, align 4
  store i32 0, ptr %dstp_i, align 4
  %i_next = add i32 %i, 1
  %i_more = icmp ult i32 %i_next, 1024
  br i1 %i_more, label %init, label %copy

copy:
  %src_iova = inttoptr i64 65536 to ptr
  %dst_iova = inttoptr i64 69632 to ptr
  %t = call <1024 x i32> @llvm.linx.tma.tload(ptr %src_iova, i32 12)
  call void @llvm.linx.tma.tstore(ptr %dst_iova, <1024 x i32> %t, i32 12)
  br label %check

check:
  %j = phi i32 [ 0, %copy ], [ %j_next, %check_ok ]
  %srcp_j = getelementptr inbounds [1024 x i32], ptr @src_buf, i32 0, i32 %j
  %dstp_j = getelementptr inbounds [1024 x i32], ptr @dst_buf, i32 0, i32 %j
  %sv = load i32, ptr %srcp_j, align 4
  %dv = load i32, ptr %dstp_j, align 4
  %eq = icmp eq i32 %sv, %dv
  br i1 %eq, label %check_ok, label %fail

check_ok:
  %j_next = add i32 %j, 1
  %j_more = icmp ult i32 %j_next, 1024
  br i1 %j_more, label %check, label %pass

fail:
  call void @virt_exit(i32 1)
  unreachable

pass:
  call void @virt_exit(i32 0)
  unreachable
}

