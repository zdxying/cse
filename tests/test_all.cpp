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

//@cse
double arrowTest(double* px, double* py) {
    double a = px->x + px->y;
    double b = py->x + py->y;
    return a + b;
}

struct Vec2 {
    double x;
    double y;
};

struct Matrix {
    double m00;
    double m01;
    double m10;
    double m11;
};

//@cse
double dot(struct Vec2* a, struct Vec2* b) {
    return a->x * b->x + a->y * b->y;
}

//@cse
double transform(struct Matrix* m, struct Vec2* v) {
    double rx = m->m00 * v->x + m->m01 * v->y;
    double ry = m->m10 * v->x + m->m11 * v->y;
    return rx + ry;
}

//@cse
struct Point {
    double x;
    double y;
    double length() {
        return x * x + y * y;
    }
    double distanceTo(struct Point* other) {
        double dx = x - other->x;
        double dy = y - other->y;
        return dx * dx + dy * dy;
    }
    double dotWith(struct Point* other) {
        return x * other->x + y * other->y;
    }
    double sumCoords() {
        return x + y;
    }
    double scaleAndAdd(double a) {
        return a * x + a * y;
    }
};

//@cse
double dotPoints(struct Point* a, struct Point* b) {
    return a->x * b->x + a->y * b->y;
}
