// @cse
template <typename CELLTYPE, bool WriteToField>
struct rhoImpl {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  using GenericRho = typename CELL::GenericRho;

  __any__ static inline void apply(CELL& cell, T& rho_value) {
    rho_value = T{};
    for (unsigned int i = 0; i < LatSet::q; ++i) rho_value += cell[i];
    if constexpr (WriteToField) cell.template get<GenericRho>() = rho_value;
  }
};

// @cse
template <typename CELLTYPE, typename SOURCE, bool WriteToField>
struct sourcerhoImpl {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  using GenericRho = typename CELL::GenericRho;

  __any__ static inline void apply(CELL& cell, const T source, T& rho_value) {
    rho_value = T{};
    for (unsigned int i = 0; i < LatSet::q; ++i) rho_value += cell[i];
    // fOmega: avoid lattice artifact
    rho_value += source * T{0.5};
    if constexpr (WriteToField) cell.template get<GenericRho>() = rho_value;
  }
};

// @cse
template <typename CELLTYPE, bool WriteToField>
struct UImpl {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  using GenericRho = typename CELL::GenericRho;

  __any__ static inline void apply(CELL& cell, Vector<T, LatSet::d>& u_value) {
    u_value.clear();
    T rho_value{};
    for (unsigned int i = 0; i < LatSet::q; ++i) {
      rho_value += cell[i];
      u_value += latset::c<LatSet>(i) * cell[i];
    }
    u_value /= rho_value;
    if constexpr (WriteToField) cell.template get<VELOCITY<T, LatSet::d>>() = u_value;
  }
};

// @cse
template <typename CELLTYPE, typename ForceScheme, bool WriteToField>
struct forceUImpl {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;

  __any__ static inline void apply(CELL& cell, const Vector<T, LatSet::d>& f_alpha, Vector<T, LatSet::d>& u_value) {
    u_value.clear();
    T rho_value{};
    for (unsigned int i = 0; i < LatSet::q; ++i) {
      rho_value += cell[i];
      u_value += latset::c<LatSet>(i) * cell[i];
    }
    u_value += f_alpha * T{0.5};
    u_value /= rho_value;
    if constexpr (WriteToField) cell.template get<VELOCITY<T, LatSet::d>>() = u_value;
  }
  // for scalar force
  __any__ static inline void apply(CELL& cell, const T f, Vector<T, LatSet::d>& u_value) {
    u_value.clear();
    T rho_value{};
    for (unsigned int i = 0; i < LatSet::q; ++i) {
      rho_value += cell[i];
      u_value += latset::c<LatSet>(i) * cell[i];
    }
    u_value[ForceScheme::scalardir] += f * T{0.5};
    u_value /= rho_value;
    if constexpr (WriteToField) cell.template get<VELOCITY<T, LatSet::d>>() = u_value;
  }
};

// @cse
template <typename CELLTYPE, bool WriteToField>
struct rhoUImpl {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  using GenericRho = typename CELL::GenericRho;

  __any__ static void apply(CELL& cell, T& rho_value, Vector<T, LatSet::d>& u_value) {
    rho_value = T{};
    u_value.clear();
    for (unsigned int i = 0; i < LatSet::q; ++i) {
      rho_value += cell[i];
      u_value += latset::c<LatSet>(i) * cell[i];
    }
    u_value /= rho_value;
    if constexpr (WriteToField) {
      cell.template get<GenericRho>() = rho_value;
      cell.template get<VELOCITY<T, LatSet::d>>() = u_value;
    }
  }
};

// @cse
template <typename CELLTYPE, typename ForceScheme, bool WriteToField>
struct forcerhoUImpl {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  using GenericRho = typename CELL::GenericRho;

  __any__ static inline void apply(CELL& cell, const Vector<T, LatSet::d>& f_alpha, T& rho_value,
                           Vector<T, LatSet::d>& u_value) {
    rho_value = T{};
    u_value.clear();
    for (unsigned int i = 0; i < LatSet::q; ++i) {
      rho_value += cell[i];
      u_value += latset::c<LatSet>(i) * cell[i];
    }
    u_value += f_alpha * T{0.5};
    u_value /= rho_value;
    if constexpr (WriteToField) {
      cell.template get<GenericRho>() = rho_value;
      cell.template get<VELOCITY<T, LatSet::d>>() = u_value;
    }
  }
  // for scalar force
  __any__ static inline void apply(CELL& cell, const T f, T& rho_value, Vector<T, LatSet::d>& u_value) {
    rho_value = T{};
    u_value.clear();
    for (unsigned int i = 0; i < LatSet::q; ++i) {
      rho_value += cell[i];
      u_value += latset::c<LatSet>(i) * cell[i];
    }
    u_value[ForceScheme::scalardir] += f * T{0.5};
    u_value /= rho_value;
    if constexpr (WriteToField) {
      cell.template get<GenericRho>() = rho_value;
      cell.template get<VELOCITY<T, LatSet::d>>() = u_value;
    }
  }
};

// @cse
template <typename CELLTYPE>
struct Pi_ab_neq {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  // f_i^(1) = f_i - f_i^eq
  // PI_ab^neq = SUM_i((f_i - f_i^eq)*c_ia*c_ib)
  // PI_ab^eq = SUM_i(f_i^eq*c_ia*c_ib) = rho*U_a*U_b + rho*Cs^2*delta_ab
  __any__ static inline void apply(CELL& cell, const T rho, const Vector<T, LatSet::d>& u, 
  std::array<T, util::SymmetricMatrixSize<LatSet::d>()>& tensor) {
    unsigned int i{};
    for (unsigned int alpha = 0; alpha < LatSet::d; ++alpha) {
      for (unsigned int beta = alpha; beta < LatSet::d; ++beta) {
        T value{};
        for (unsigned int k = 0; k < LatSet::q; ++k) {
          value += latset::c<LatSet>(k)[alpha] * latset::c<LatSet>(k)[beta] * cell[k];
        }
        // remove the equilibrium part: PI_ab^eq
        value -= rho * u[alpha] * u[beta];
        if (alpha == beta) value -= rho * LatSet::cs2;
        tensor[i] = value;
        ++i;
      }
    }
  }
};

// @cse
template <typename CELLTYPE>
struct forcePi_ab_neq {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  // f_i^(1) = f_i - f_i^eq
  // PI_ab^neq = SUM_i((f_i - f_i^eq)*c_ia*c_ib)
  // PI_ab^eq = SUM_i(f_i^eq*c_ia*c_ib) = rho*U_a*U_b + rho*Cs^2*delta_ab
  __any__ static inline void apply(CELL& cell, const T rho, const Vector<T, LatSet::d>& u, const Vector<T, LatSet::d>& f_alpha,
                         std::array<T, util::SymmetricMatrixSize<LatSet::d>()>& tensor) {
    unsigned int i{};
    // remove force term in u
    Vector<T, LatSet::d> unew = u - f_alpha * T{0.5}; //(T{0.5} / rho);

    for (unsigned int alpha = 0; alpha < LatSet::d; ++alpha) {
      for (unsigned int beta = alpha; beta < LatSet::d; ++beta) {
        T value{};
        const T force = T{0.5} * (f_alpha[alpha] * unew[beta] + f_alpha[beta] * unew[alpha]);  //* rho
        for (unsigned int k = 0; k < LatSet::q; ++k) {
          value += latset::c<LatSet>(k)[alpha] * latset::c<LatSet>(k)[beta] * cell[k];
        }
        value += force;
        // remove the equilibrium part: PI_ab^eq
        value -= rho * unew[alpha] * unew[beta];
        if (alpha == beta) value -= rho * LatSet::cs2;
        tensor[i] = value;
        ++i;
      }
    }
  }
};

// @cse
template <typename CELLTYPE>
struct stress {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  // sigma_ab = -(1 - 1/(2*tau))*SUM_i(f_i^(1)*c_ia*c_ib)
  // f_i^(1) = f_i - f_i^eq
  // PI_ab^eq = SUM_i(f_i^eq*c_ia*c_ib) = rho*U_a*U_b + rho*Cs^2*delta_ab
  __any__ static inline void apply(CELL& cell, const T rho, const Vector<T, LatSet::d>& u, 
  std::array<T, util::SymmetricMatrixSize<LatSet::d>()>& stress_tensor) {
    unsigned int i{};
    const T coeff = T{0.5} * cell.getOmega() - T{1};
    for (unsigned int alpha = 0; alpha < LatSet::d; ++alpha) {
      for (unsigned int beta = alpha; beta < LatSet::d; ++beta) {
        T value{};
        for (unsigned int k = 0; k < LatSet::q; ++k) {
          value += latset::c<LatSet>(k)[alpha] * latset::c<LatSet>(k)[beta] * cell[k];
        }
        // remove the equilibrium part: PI_ab^eq
        value -= rho * u[alpha] * u[beta];
        if (alpha == beta) value -= rho * LatSet::cs2;
        // multiply by the coefficient
        value *= coeff;
        stress_tensor[i] = value;
        ++i;
      }
    }
  }
};

// @cse
template <typename CELLTYPE>
struct strainRate {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;
  // sigma_ab = -(1 - delta_T/(2*tau)) * SUM_i(f_i^(1)*c_ia*c_ib)
  // sigma_ab = 2 * mu * S_ab = 2 * rho * nu * S_ab
  // S_ab = simga_ab / (2 * rho * Cs^2 * (tau - 0.5))
  // S_ab = (1/rho)(-Cs^2/(2*tau))*SUM_i(f_i^(1)*c_ia*c_ib)
  // f_i^(1) = f_i - f_i^eq
  // PI_ab^eq = SUM_i(f_i^eq*c_ia*c_ib) = rho*U_a*U_b + rho*Cs^2*delta_ab
  __any__ static inline void apply(CELL& cell, const T rho, const Vector<T, LatSet::d>& u, 
  std::array<T, util::SymmetricMatrixSize<LatSet::d>()>& strain_rate_tensor) {
    unsigned int i{};
    T omega{};
    if constexpr(cell.template hasField<OMEGA<T>>()) omega = cell.template get<OMEGA<T>>();
    else omega = cell.getOmega();
    const T coeff = T{-1.5} * omega / cell.template get<typename CELL::GenericRho>();
    for (unsigned int alpha = 0; alpha < LatSet::d; ++alpha) {
      for (unsigned int beta = alpha; beta < LatSet::d; ++beta) {
        T value{};
        for (unsigned int k = 0; k < LatSet::q; ++k) {
          value += latset::c<LatSet>(k)[alpha] * latset::c<LatSet>(k)[beta] * cell[k];
        }
        // remove the equilibrium part: PI_ab^eq
        value -= rho * u[alpha] * u[beta];
        if (alpha == beta) value -= rho * LatSet::cs2;
        // multiply by the coefficient
        value *= coeff;
        strain_rate_tensor[i] = value;
        ++i;
      }
    }
  }
};

// @cse
template <typename CELLTYPE>
struct shearRateMagImpl {
  using CELL = CELLTYPE;
  using T = typename CELL::FloatType;
  using LatSet = typename CELL::LatticeSet;

  __any__ static inline T get(const std::array<T, util::SymmetricMatrixSize<LatSet::d>()>& strain_rate_tensor) {
    T value{};
    unsigned int i{};
    for (unsigned int alpha = 0; alpha < LatSet::d; ++alpha) {
      for (unsigned int beta = alpha; beta < LatSet::d; ++beta) {
        T sq = strain_rate_tensor[i] * strain_rate_tensor[i];
        if (alpha != beta) sq *= T{2};
        value += sq;
        ++i;
      }
    }
    return std::sqrt(T{2} * value);
  }
};

