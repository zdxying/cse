// Test file for expression recombination
//@cse
double recombine(double a, double b, double x, double y, int n) {
    double sum = 0;
    double t = a * x + a * y;
    sum = sum + t;
    return sum;
}
