// Pins the numeric-kind class: a rewrite may not drop the only floating
// operand, and a constant that replaces a whole expression must carry the
// expression's int/float kind. Each case is chosen so the pre-fix rewrite
// produced a different value, not just a different spelling.
#include "harness.h"

#include "numeric_kind.cpp.cse"

int main() {
  // i * 2.0 is a double multiply. The old x*2.0 -> x+x fired for an integral
  // operand too, so 3e9 became 3e9+3e9 in 32-bit arithmetic.
  check("uint*2.0", sf_uint_times_2f(3000000000u), ref_uint_times_2f(3000000000u));
  check("int*2.0", sf_int_times_2f(1073741825), ref_int_times_2f(1073741825));

  // Peeling a floating -1.0 turned the double product into an unsigned one.
  check("neg_one", sf_neg_one(3u, 2u), ref_neg_one(3u, 2u));

  // 1.0 * i, i / 1.0 and i - 0.0 are double expressions; dropping the literal
  // made the following / 2 an integer division.
  check("unit_factor", sf_unit_factor(1), ref_unit_factor(1));
  check("unit_divisor", sf_unit_divisor(1), ref_unit_divisor(1));
  check("zero_sub", sf_zero_sub(1), ref_zero_sub(1));

  // x/x is 1.0 for a double x; emitting the integer 1 turned /2 into 0.
  check("self_div", sf_self_div(2.0), ref_self_div(2.0));
  check("self_div_chain", sf_self_div_chain(2.0), ref_self_div_chain(2.0));

  // Under -r the unsigned case must not be factored; the value check also
  // covers the default profile, where nothing is factored.
  check("recomb_unsigned", sf_recomb_unsigned(1.0, 4294967295u),
        ref_recomb_unsigned(1.0, 4294967295u));

  return finish("NUMERIC-KIND");
}
