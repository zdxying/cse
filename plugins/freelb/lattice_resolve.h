#pragma once
#include <memory>

#include "frontend/cse_config.h"
#include "passes/pass.h"

namespace cse {
namespace freelb {

// Creates a pass that resolves FreeLB lattice intrinsic calls such as
//   latset::c<D3Q19<T>>(k)        -> direction vector components
//   latset::w<D3Q19<T>>(k)        -> constant weight
//   u * latset::c<D3Q19<T>>(k)    -> scalar dot product
// The index argument must be a compile-time constant (typically produced by
// LoopUnrollPass). Lattice tables are hardcoded, matching lattice_set.h.
//
// When `config` carries a per-latset instantiation context (latsetAlias/
// latsetName), a templated alias such as `latset::c<LatSet>(k)` resolves
// against the configured concrete set.
std::unique_ptr<Pass> createLatticeResolvePass(const CSEConfig& config = CSEConfig());

}  // namespace freelb
}  // namespace cse
