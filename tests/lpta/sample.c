static int square(int x) { return x * x; }

int sum_squares(const int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += square(a[i]);
  return s;
}

int dead_branch(int x) {
  if (x > 0)
    return x + 1;
  return 0;
}
