// Consolidated test file for CSE optimization
#include <cmath>

//@cse
double compute(double a, double b, double c, double d, double e) {
    double t1 = a * b * c * d * e;
    double t2 = b * c * d;
    double t3 = c * d;
    double t4 = a * b;
    return t1 + t2 + t3 + t4;
}
