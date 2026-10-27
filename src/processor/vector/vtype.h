/**
 * @file vtype.h
 * @brief Decoding, encoding and validation of the vtype CSR (RVV 1.0).
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Kites
{
namespace rvv
{

/// vtype.vill is bit XLEN-1 (bit 63 on RV64).
inline constexpr uint64_t kVillBit = 1ULL << 63;

/**
 * @brief Read-only view of a vtype value.
 */
struct VType
{
    uint64_t raw = kVillBit;

    [[nodiscard]] bool vill() const { return (raw & kVillBit) != 0; }
    [[nodiscard]] bool vma() const { return ((raw >> 7) & 1U) != 0; }
    [[nodiscard]] bool vta() const { return ((raw >> 6) & 1U) != 0; }
    [[nodiscard]] unsigned vsew() const { return static_cast<unsigned>((raw >> 3) & 7U); }
    [[nodiscard]] unsigned vlmul() const { return static_cast<unsigned>(raw & 7U); }

    /// Selected element width in bits.
    [[nodiscard]] unsigned sew() const { return 8U << vsew(); }
    /// log2(SEW).
    [[nodiscard]] int sewLog2() const { return 3 + static_cast<int>(vsew()); }
    /// log2(LMUL), in -3..3 (vlmul = 4 is reserved and maps to 4).
    [[nodiscard]] int lmulLog2() const
    {
        const int v = static_cast<int>(vlmul());
        return v >= 5 ? v - 8 : v;
    }
};

/**
 * @brief Returns true if this implementation supports the given vtype value.
 *
 * Unsupported values (reserved bits set, SEW > ELEN, reserved LMUL, or SEW > LMUL * ELEN for
 * fractional LMUL) make vset{i}vl{i} set vill.
 */
bool isSupportedVtype(uint64_t raw);

/// VLMAX = LMUL * VLEN / SEW for the given parameters.
uint64_t vlmax(unsigned vlen, int sewLog2, int lmulLog2);

/// Builds a vtype value from its fields.
uint64_t encodeVtype(unsigned sew, int lmulLog2, bool tailAgnostic, bool maskAgnostic);

/// Formats a vtype immediate as assembly text, e.g. "e32, m1, ta, ma".
/// Values that cannot be expressed symbolically are printed as a number.
std::string vtypeToString(uint64_t raw);

/// True if the token is a vtype field keyword (e8, m1, mf2, ta, tu, ma, mu, ...).
bool isVtypeKeyword(std::string_view token);

} // namespace rvv
} // namespace Kites
