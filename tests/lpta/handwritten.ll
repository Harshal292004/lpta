; Hand-written IR for the opt route.  The loop should survive -O1.
define i32 @dot(ptr %a, ptr %b, i32 %n) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %ai = getelementptr i32, ptr %a, i32 %i
  %bi = getelementptr i32, ptr %b, i32 %i
  %av = load i32, ptr %ai, align 4
  %bv = load i32, ptr %bi, align 4
  %mul = mul i32 %av, %bv
  %acc.next = add i32 %acc, %mul
  %i.next = add i32 %i, 1
  %cmp = icmp slt i32 %i.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %acc.next
}
