// `f(void)` has to parse, and so does everything around it.
#include <cmath>
#include <cstdio>

#include "void_param.cpp.cse"
#include "harness.h"

int main() {
  // x = 6, so the result is 12 -- and it is only reachable if the region was
  // optimized rather than passed through.
  check("no_params()", no_params(), 12.0);

  // An ordinary parameter list still parses, and still gets CSE'd.
  check("void_list_with_params(2,3)", void_list_with_params(2.0, 3.0), 10.0);
  check("void_list_with_params(-1,4)", void_list_with_params(-1.0, 4.0), 6.0);

  // A `void` return type with a `(void)` parameter list.
  g_calls = 0.0;
  void_return_and_void_list();
  void_return_and_void_list();
  void_return_and_void_list();
  check("void_return_and_void_list x3", g_calls, 3.0);

  return finish("VOID-PARAM");
}
