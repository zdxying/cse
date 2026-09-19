// Input fixture for the FreeLB .ur.h generator (csegen).
// Mirrors src/lbm/equilibrium.h's `// @cse` SecondOrderImpl; parsed only, so no
// definitions of Vector/Cell are needed.

// @cse
template <typename CELL>
struct SecondOrderImpl {
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  using CELLTYPE = CELL;
  using GenericRho = typename CELL::GenericRho;

  __any__ static void apply(std::array<T, LatSet::q> &feq, const T rho, const Vector<T, LatSet::d> &u) {
    const T u2 = u.getnorm2();
    for (unsigned int k = 0; k < LatSet::q; ++k) {
      const T uc = u * latset::c<LatSet>(k);
      feq[k] = latset::w<LatSet>(k) * rho *
      (T{1} + LatSet::InvCs2 * uc + uc * uc * T{0.5} * LatSet::InvCs4 - LatSet::InvCs2 * u2 * T{0.5});
    }
  }
};
