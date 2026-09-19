#pragma once
#include <memory>
#include <string>

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
// When `alias`/`setName` carry a per-latset instantiation context, a templated
// alias such as `latset::c<LatSet>(k)` resolves against the concrete `setName`.
std::unique_ptr<Pass> createLatticeResolvePass(const std::string& alias = "",
                                               const std::string& setName = "");

}  // namespace freelb
}  // namespace cse
