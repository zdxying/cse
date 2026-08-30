// Comprehensive test for CSE with for loops
//@cse
void process(double a, double* x, double* y, double* result, int n) {
    for (int i = 0; i < n; i = i + 1) {
        double t = a * x[i] + a * y[i];
        result[i] = t * t;
    }
}
