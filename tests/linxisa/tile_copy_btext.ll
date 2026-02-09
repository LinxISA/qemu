target triple = "linx64"

declare <1024 x i32> @llvm.linx.tma.tload(ptr, i32)
declare void @llvm.linx.tma.tstore(ptr, <1024 x i32>, i32)

@src_buf = internal global [1024 x i32] zeroinitializer, align 4
@dst_buf = internal global [1024 x i32] zeroinitializer, align 4

define void @virt_exit(i32 %code) noreturn {
entry:
  %exit_reg = inttoptr i64 268435460 to ptr
  store volatile i32 %code, ptr %exit_reg, align 4
  br label %loop

loop:
  br label %loop
}

define void @_start() noreturn {
entry:
  br label %init

init:
  %i = phi i32 [ 0, %entry ], [ %i_next, %init ]
  %srcp = getelementptr inbounds [1024 x i32], ptr @src_buf, i32 0, i32 %i
  %dstp = getelementptr inbounds [1024 x i32], ptr @dst_buf, i32 0, i32 %i
  %pat = add i32 305397760, %i
  store i32 %pat, ptr %srcp, align 4
  store i32 0, ptr %dstp, align 4
  %i_next = add i32 %i, 1
  %i_more = icmp ult i32 %i_next, 1024
  br i1 %i_more, label %init, label %copy

copy:
  %src0 = getelementptr inbounds [1024 x i32], ptr @src_buf, i32 0, i32 0
  %dst0 = getelementptr inbounds [1024 x i32], ptr @dst_buf, i32 0, i32 0
  %t = call <1024 x i32> @llvm.linx.tma.tload(ptr %src0, i32 12)
  call void @llvm.linx.tma.tstore(ptr %dst0, <1024 x i32> %t, i32 12)
  br label %check

check:
  %j = phi i32 [ 0, %copy ], [ %j_next, %check_ok ]
  %srcj = getelementptr inbounds [1024 x i32], ptr @src_buf, i32 0, i32 %j
  %dstj = getelementptr inbounds [1024 x i32], ptr @dst_buf, i32 0, i32 %j
  %sv = load i32, ptr %srcj, align 4
  %dv = load i32, ptr %dstj, align 4
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
