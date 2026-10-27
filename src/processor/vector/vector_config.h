/**
 * @file vector_config.h
 * @brief Implementation parameters of the RISC-V Vector (RVV 1.0) extension.
 */
#pragma once

namespace Kites
{
namespace rvv
{

/// Maximum element width in bits (ELEN). Kites implements the full V extension (Zve64d).
inline constexpr unsigned kElen = 64;

/// Default vector register length in bits (VLEN). 128 is the minimum allowed by V (Zvl128b).
inline constexpr unsigned kDefaultVlen = 128;

/// Number of architectural vector registers.
inline constexpr unsigned kNumVectorRegisters = 32;

/**
 * @brief Runtime parameters of the vector unit.
 *
 * VLEN is read when the register file is reset, so a change takes effect on the next reset.
 */
struct VectorConfig
{
    unsigned vlen = kDefaultVlen; ///< Vector register length in bits (power of two, 128..1024).

    /// When true, agnostic tail/inactive elements are overwritten with all ones.
    /// When false (default), they are left undisturbed. Both are legal per the spec.
    bool agnosticFillOnes = false;
};

/// Global vector configuration shared by the register file and the vector unit.
VectorConfig &config();

} // namespace rvv
} // namespace Kites
