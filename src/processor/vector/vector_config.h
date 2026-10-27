/**
 * @file vector_config.h
 * @brief Implementation parameters of the RISC-V Vector extension (RVV 1.0): VLEN, the agnostic
 * policy, the vector profile (V or one of the Zve* subsets) and optional vector extensions.
 */
#pragma once

#include <optional>
#include <string>

namespace Kites
{
namespace rvv
{

/// Largest ELEN supported by any profile (V and Zve64*).
inline constexpr unsigned kElen = 64;

/// Default vector register length in bits (VLEN). 128 is the minimum allowed by V (Zvl128b).
inline constexpr unsigned kDefaultVlen = 128;
inline constexpr unsigned kMinVlen = 128;
inline constexpr unsigned kMaxVlen = 1024;

/// Number of architectural vector registers.
inline constexpr unsigned kNumVectorRegisters = 32;

/**
 * @brief Vector profile: the full V extension or one of the embedded subsets (spec chapter 18).
 *
 * | Profile | ELEN | FP SEW         | 64-bit vmulh/vsmul |
 * |---------|------|----------------|--------------------|
 * | V       | 64   | 32, 64         | yes                |
 * | Zve64d  | 64   | 32, 64         | no                 |
 * | Zve64f  | 64   | 32             | no                 |
 * | Zve64x  | 64   | none           | no                 |
 * | Zve32f  | 32   | 32             | -                  |
 * | Zve32x  | 32   | none           | -                  |
 */
enum class Profile
{
    V,
    Zve64d,
    Zve64f,
    Zve64x,
    Zve32f,
    Zve32x,
};

/**
 * @brief Runtime parameters of the vector unit.
 *
 * VLEN is read when the register file is reset, so a change takes effect on the next reset.
 * The other fields apply to the next instruction executed.
 */
struct VectorConfig
{
    unsigned vlen = kDefaultVlen; ///< Vector register length in bits (power of two, 128..1024).

    /// When true, agnostic tail/inactive elements are overwritten with all ones.
    /// When false (default), they are left undisturbed. Both are legal per the spec.
    bool agnosticFillOnes = false;

    Profile profile = Profile::V;

    bool zvfh = false; ///< half-precision (SEW = 16) floating point; needs an F profile
    bool zvbb = false; ///< vector basic bit manipulation (includes Zvkb)
    bool zvkb = false; ///< vector cryptography bit manipulation (subset of Zvbb)
    bool zvbc = false; ///< vector carry-less multiply (SEW = 64; needs ELEN = 64)

    [[nodiscard]] unsigned elen() const;
    [[nodiscard]] unsigned elenLog2() const { return elen() == 64 ? 6 : 5; }
    /// True if floating-point vector instructions are legal at this SEW.
    [[nodiscard]] bool supportsFloat(unsigned sew) const;
    /// False for Zve64*: vmulh*, vsmul at SEW = 64 are not included.
    [[nodiscard]] bool supportsMulHigh64() const { return profile == Profile::V; }

    /// ISA-string style summary, e.g. "v, zvbb, zvbc".
    [[nodiscard]] std::string describe() const;
};

/// Global vector configuration shared by the register file and the vector unit.
VectorConfig &config();

/// True for a power of two in [kMinVlen, kMaxVlen].
bool isSupportedVlen(unsigned vlen);

/// "v", "zve64d", ... (lower case).
std::string profileName(Profile profile);
std::optional<Profile> parseProfile(const std::string &name);

/**
 * @brief Applies a comma-separated list such as "zve64f,zvfh,zvbb" to cfg.
 *
 * Items are a profile name or an extension name (zvfh, zvbb, zvkb, zvbc); the profile is left
 * unchanged if none is given and listed extensions are enabled. Returns an error message, or an
 * empty string on success (cfg is unchanged on error).
 */
std::string applyExtensionList(VectorConfig &cfg, const std::string &list);

/// Checks combinations the spec forbids (Zvfh without F, Zvbc without ELEN = 64).
std::string validate(const VectorConfig &cfg);

} // namespace rvv
} // namespace Kites
