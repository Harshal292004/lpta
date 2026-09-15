// 04 — computed twice, kept once.
// Compile O0 vs O2, same flags as 01.
// Look at: how many times `a * b + c` is computed in each version.
// Question: two identical computations became one. What did the compiler have
//           to *prove* before it was allowed to merge them?
int combine(int a, int b, int c, int flag) {
  int x = a * b + c;
  int y;
  if (flag)
    y = a * b + c + 1;
  else
    y = x + 2;
  return x + y;
}
