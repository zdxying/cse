// A member call carries only the method's *bare* name, so the builtin math
// whitelist (`sin`/`max`/`abs`/...) must not decide its purity. `obj.sin(...)`
// is not the global `sin`: it may write the object. A user method that happens
// to share a builtin's name was treated as referentially transparent, so CSE
// merged two calls and dropped one side effect.
//
// The twin-style check is a value check: `member_sin_twice` must add `a` to
// `o.s` twice.

struct MemberPurityS {
  double s;
  double sin(double x) {
    s = s + x;
    return x;
  }
};

//@cse
double member_sin_twice(MemberPurityS o, double a) {
  o.sin(a);
  o.sin(a);
  return o.s;
}
//@cse
