// Pins the value side of the preprocessor-directive contract.
//
// A directive line is outside the language the IR models. The lexer emits it as
// a PreprocessorDirective token (it used to drop the line outright), the region
// extractor records it on `CSERegion::hasDirectives`, and the driver passes such
// a region through verbatim. Regenerating from the AST would delete the guard
// and resurrect the code it protects -- `#if 0 return -1.0;` becoming the
// function's result, which is exactly what these checks would report.
//
// This is the value half. The shape half (which regions are flagged, in both
// directions) lives in verify_pp_extraction.cpp, because it must link the
// library and drive RegionExtractor directly: a value check cannot distinguish
// "passed through" from "optimized correctly" -- both compute the same number.
#include "harness.h"

#include "pp_directives.cpp.cse"

int main() {
  check("guarded_0", sf_guarded(2.0), ref_guarded(2.0));
  check("guarded_1", sf_guarded(-0.75), ref_guarded(-0.75));
  check("scale_twice_0", sf_scale_twice(1.5), ref_scale_twice(1.5));
  check("scale_twice_1", sf_scale_twice(0.0), ref_scale_twice(0.0));
  check("comment_then_directive_0", sf_comment_then_directive(2.0),
        ref_comment_then_directive(2.0));
  check("comment_then_directive_1", sf_comment_then_directive(-3.0),
        ref_comment_then_directive(-3.0));
  check("comment_only_0", sf_comment_only(2.0), ref_comment_only(2.0));
  check("comment_only_1", sf_comment_only(-1.5), ref_comment_only(-1.5));
  return finish("PP-DIRECTIVES");
}
