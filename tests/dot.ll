; Dot-product loop. Chosen to exercise mem2reg (the %sum alloca),
; loop transforms (vectorize/unroll), and instcombine/GVN (the loads).
;
; Compile from C:  clang -O0 -S -emit-llvm dot.c
; or run directly:  opt tests/fibonacci.ll -passes='default<O2>'

define i32 @dot(ptr %a, ptr %b, i32 %n) {
entry:
  %sum = alloca i32, align 4
  store i32 0, ptr %sum, align 4
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %inc, %body ]
  %cmp = icmp slt i32 %i, %n
  br i1 %cmp, label %body, label %exit

body:
  %ai = getelementptr i32, ptr %a, i32 %i
  %av = load i32, ptr %ai, align 4
  %bi = getelementptr i32, ptr %b, i32 %i
  %bv = load i32, ptr %bi, align 4
  %mul = mul i32 %av, %bv
  %cur = load i32, ptr %sum, align 4
  %next = add i32 %cur, %mul
  store i32 %next, ptr %sum, align 4
  %inc = add i32 %i, 1
  br label %header

exit:
  %result = load i32, ptr %sum, align 4
  ret i32 %result
}