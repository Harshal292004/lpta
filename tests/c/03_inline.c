// 03 — the vanishing function.
// Compile O0 vs O2, same flags as 01.
// Look at: `helper` in the O2 output. Where did it go?
// Question: the call disappeared but the work remains. If you had to name the
//           single pass responsible, what evidence would convince you?
static int helper(int x) { return x * 3 + 1; }

int apply(int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += helper(i);
  return s;
}
