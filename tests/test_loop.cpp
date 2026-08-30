// Test file for CSE with if-else
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
