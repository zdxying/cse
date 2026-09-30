// A region whose text contains braces inside comments must be extracted whole
// and optimized, not truncated and skipped.
//
// The values are the source's values either way; what the FLOP pin in
// run_tests.sh and the `x + x` shape below catch is that the region was
// recognized at all. Before the fix the extractor cut the region off at the
// comment's `}` and the fragment failed to parse.
#include <cmath>
#include <cstdio>

#include "comment_braces.cpp.cse"

static int failures = 0;

static void check(const char* name, double got, double want) {
  bool ok = std::fabs(got - want) < 1e-12;
  if (!ok) failures++;
  std::printf("%-26s got=%.6f want=%.6f  %s\n", name, got, want, ok ? "OK" : "FAIL");
}

int main() {
  // x = y = a*b, so the result is 2*a*b. The source and the optimized form
  // agree; a wrongly shared or truncated region would not have a value at all.
  check("line_comment(2,3)", braces_in_line_comment(2.0, 3.0), 12.0);
  check("line_comment(-1,4)", braces_in_line_comment(-1.0, 4.0), -8.0);
  check("line_comment(0,5)", braces_in_line_comment(0.0, 5.0), 0.0);

  check("block_comment(2,3)", braces_in_block_comment(2.0, 3.0), 10.0);
  check("block_comment(-1,4)", braces_in_block_comment(-1.0, 4.0), 6.0);

  std::printf(failures == 0 ? "\nALL COMMENT-BRACE CHECKS PASSED\n"
                            : "\n%d COMMENT-BRACE CHECK(S) FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}
