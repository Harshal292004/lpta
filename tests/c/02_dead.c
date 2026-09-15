// 02 — dead weight.
// Compile O0 vs O2, same flags as 01.
// Look at: `waste`, `unused`, and the `if (n < 0)` branch in the O2 output.
// Question: your code is gone in O2. Which pass do you blame — and how would
//           you prove it wasn't one of the other 150?
int compute(int n) {
  int waste = n * 42 + 7;
  int unused = waste * waste;
  int r = n + 1;
  if (n < 0)
    r = -r;
  return r;
}
