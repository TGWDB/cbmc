#include <assert.h>

void main()
{
  unsigned A, x[64];
  __CPROVER_assume(0 <= A && A < 64);
  __CPROVER_assume(__CPROVER_forall { int i ; (0 <= i && i < A) ==> x[i] >= 1 });
  assert(x[0] > 0);
}
