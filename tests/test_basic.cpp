// Test file for CSE optimization
#include <cmath>

//@cse
double compute(double a, double x, double y, double result, int n) {
    double t1 = a * x + a * y;
    double t2 = a * x * a * x;
    result = t1 + t2;
    return result;
}
