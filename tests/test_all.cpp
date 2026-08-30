// Consolidated test file for CSE optimization
#include <cmath>

//@cse
double compute(double a, double x, double y, double result, int n) {
    double t1 = a * x + a * y;
    double t2 = a * x * a * x;
    result = t1 + t2;
    return result;
}

//@cse
double recombine(double a, double b, double x, double y, int n) {
    double sum = 0;
    double t = a * x + a * y;
    sum = sum + t;
    return sum;
}

//@cse
double conditional(double a, double x, int n) {
    double result = 0;
    double val = a * x;
    if (val > 0) {
        result = result + val * val;
    } else {
        result = result - val * val;
    }
    return result;
}

//@cse
void process(double a, double* x, double* y, double* result, int n) {
    for (int i = 0; i < n; i = i + 1) {
        double t = a * x[i] + a * y[i];
        result[i] = t * t;
    }
}

//@cse
double debug(double a, double x, double y) {
    double t = a * x + a * y;
    return t;
}
