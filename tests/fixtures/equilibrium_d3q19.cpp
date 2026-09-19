// D3Q19 second-order equilibrium benchmark (loop form, FreeLB syntax).
//
// Mirrors equilibrium.h SecondOrderImpl::apply:
//   feq[k] = w[k] * rho * (1 + 3*uc + 4.5*uc^2 - 1.5*u^2),  uc = u . c[k]
// The hand-written reference (equilibrium.ur.h) expands the loop and shares
// the weight products, uc^2 terms and the u^2 term.
//
// A minimal, self-contained lattice stub is provided so the optimized output
// (which emits declared weight accessors such as latset::w<D3Q19<double>>(1))
// compiles without the FreeLB headers.

template <typename T, unsigned int D>
struct Vector {
    T v[D];
    T& operator[](unsigned int i) { return v[i]; }
    const T& operator[](unsigned int i) const { return v[i]; }
    T getnorm2() const {
        T s = T(0);
        for (unsigned int i = 0; i < D; ++i) s += v[i] * v[i];
        return s;
    }
};

template <typename T>
T operator*(const Vector<T, 3>& a, const Vector<int, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Minimal D3Q19 lattice (mirrors tests/lattice_set.h).
template <typename T>
struct D3Q19 {
    using FloatType = T;
    static constexpr unsigned int d = 3;
    static constexpr unsigned int q = 19;
    static constexpr int c[q][d] = {
        {0, 0, 0},  {1, 0, 0},   {-1, 0, 0}, {0, 1, 0},   {0, -1, 0}, {0, 0, 1},   {0, 0, -1},
        {1, 1, 0},  {-1, -1, 0}, {1, 0, 1},  {-1, 0, -1}, {0, 1, 1},  {0, -1, -1}, {1, -1, 0},
        {-1, 1, 0}, {1, 0, -1},  {-1, 0, 1}, {0, 1, -1},  {0, -1, 1}};
    static constexpr T w[q] = {
        T(1) / T(3),  T(1) / T(18), T(1) / T(18), T(1) / T(18),
        T(1) / T(18), T(1) / T(18), T(1) / T(18), T(1) / T(36),
        T(1) / T(36), T(1) / T(36), T(1) / T(36), T(1) / T(36),
        T(1) / T(36), T(1) / T(36), T(1) / T(36), T(1) / T(36),
        T(1) / T(36), T(1) / T(36), T(1) / T(36)};
};

namespace latset {

template <typename LatSet>
constexpr typename LatSet::FloatType w(unsigned int i) {
    return LatSet::w[i];
}

template <typename LatSet>
constexpr Vector<int, LatSet::d> c(unsigned int i) {
    Vector<int, LatSet::d> r{};
    for (unsigned int k = 0; k < LatSet::d; ++k) r[k] = LatSet::c[i][k];
    return r;
}

}  // namespace latset

//@cse
void equilibrium_d3q19(double* feq, double rho, const Vector<double, 3>& u) {
    const double u2 = u.getnorm2();
    for (unsigned int k = 0; k < 19; ++k) {
        const double uc = u * latset::c<D3Q19<double>>(k);
        feq[k] = latset::w<D3Q19<double>>(k) * rho *
                 (1.0 + 3.0 * uc + uc * uc * 0.5 * 9.0 - 3.0 * u2 * 0.5);
    }
}
