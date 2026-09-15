// 05 — the loop that stops looking like a loop.
// Compile O0 vs O2, same flags as 01.
// Look at: the O2 body — how much work one iteration now covers, and what
//           happened to the loop counter.
// Question: several different transformations had to cooperate to produce the
//           O2 form. How would you split the credit between them?
void vadd(float *a, float *b, float *c, int n) {
  for (int i = 0; i < n; i++)
    c[i] = a[i] + b[i];
}
