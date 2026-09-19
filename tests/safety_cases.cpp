// Safety test cases for the general-purpose CSE mode.
// Each //@cse function exercises a correctness hazard:
//   - impure_calls : side-effecting calls must not be merged
//   - load_store   : a load must not be reused across a store
//   - branch_hoist : a subexpression inside a branch must not be hoisted
//   - shadowing    : shadowed variables must keep their own identity

extern int g_calls;

double side_effect(double x) {
    g_calls += 1;
    return x + (double)g_calls;
}

//@cse
double impure_calls(double x) {
    return side_effect(x) + side_effect(x);
}

//@cse
double load_store(double* a, int i) {
    double t = a[i];
    a[i] = t + 1.0;
    double z = a[i];
    return z + t;
}

//@cse
double branch_hoist(double a, double b, int c) {
    double r = 0.0;
    if (c > 0) {
        r = a * b + a * b;
    }
    return r + a * b;
}

//@cse
int comparisons(int a, int b) {
    int r = 0;
    if (a == b) {
        r = r + 1;
    }
    if (a <= b) {
        r = r + 2;
    }
    if (a != b) {
        r = r + 4;
    }
    if (a >= b) {
        r = r + 8;
    }
    return r;
}

//@cse
double shadowing(double x) {
    double y = x + 1.0;
    {
        double y = x * 2.0;
        x = y + 1.0;
    }
    return y;
}
