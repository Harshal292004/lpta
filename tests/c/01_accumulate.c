// 01 — the accumulator.
// Compile: clang -S -emit-llvm -O0 01_accumulate.c -o acc_O0.ll
//          clang -S -emit-llvm -O2 01_accumulate.c -o acc_O2.ll
// Look at: the local `s` — where does it live in each version?
// Question: count the instructions that disappeared between O0 and O2.
//           What would you need to measure to say *which pass* removed each one?
int sum(int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += i;
  return s;
}
