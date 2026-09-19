// Namespace support: //@cse regions that contain (or are) a namespace must have
// the namespace's functions/structs optimized and emitted, not silently dropped.

//@cse
namespace nsx {

int cse_ns(int a, int b, int c) {
    int x = a * b;
    int y = a * b + c;
    return x + y + a * b;
}

struct Point {
    double x;
    double y;
};

}  // namespace nsx
