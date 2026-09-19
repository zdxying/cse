// Input fixture for the FreeLB .ur.h generator (csegen): force.h shapes.
// Mirrors src/lbm/force.h's `// @cse` structs; parsed only.

// @cse
template <typename T, typename LatSet>
struct ForcePopImpl {
  __any__ static inline void compute(std::array<T, LatSet::q> &Fi, const Vector<T, LatSet::d> &u, const Vector<T, LatSet::d> &F) {
    for (unsigned int i = 0; i < LatSet::q; ++i) {
      Fi[i] = latset::w<LatSet>(i) * F * ((latset::c<LatSet>(i) - u) * LatSet::InvCs2 + (latset::c<LatSet>(i) * u * LatSet::InvCs4) * latset::c<LatSet>(i));
    }
  }
};

// @cse
template <typename T, typename LatSet, unsigned int d>
struct ScalarForcePopImpl {
  __any__ static inline void compute(std::array<T, LatSet::q> &Fi, const Vector<T, LatSet::d> &u, const T F) {
    for (unsigned int i = 0; i < LatSet::q; ++i) {
      const T v1 = (latset::c<LatSet>(i)[d] - u[d]) * LatSet::InvCs2;
      const T v2 = (latset::c<LatSet>(i) * u * LatSet::InvCs4) * latset::c<LatSet>(i)[d];
      Fi[i] = latset::w<LatSet>(i) * F * (v1 + v2);
    }
  }
};
