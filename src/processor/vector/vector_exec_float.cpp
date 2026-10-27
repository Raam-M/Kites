/**
 * @file vector_exec_float.cpp
 * @brief Floating-point vector instructions (OPFVV / OPFVF) for SEW = 16 (Zvfh), SEW = 32 (F)
 * and SEW = 64 (D).
 *
 * Element arithmetic uses the host's IEEE 754 operations with the rounding mode taken from frm,
 * and the host exception flags are accrued into fflags using the RISC-V bit layout. RISC-V rules
 * that differ from plain host behaviour are applied explicitly: canonical NaN results,
 * minimumNumber/maximumNumber for min/max, saturating float-to-integer conversion, round-to-odd
 * narrowing, and the bit-exact 7-bit reciprocal (square-root) estimates.
 *
 * Half precision has no portable host type. A half operation is computed in double under the
 * same rounding mode (exactly for add, subtract, multiply and conversions; once rounded for
 * divide, square root and fused multiply-add) and then rounded to half in software, which
 * also detects tininess after rounding as RISC-V requires. Because double carries more than
 * 2 * 11 + 2 significand bits, this double rounding gives the correctly rounded half result.
 *
 * Known limitation: the host has no round-to-nearest-max-magnitude mode, so frm = RMM rounds
 * ties to even for arithmetic (conversions to integer do honour RMM).
 */
#include "processor/registers.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_exec.h"
#include "processor/vector/vector_format.h"

#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>

#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

namespace Kites
{
namespace rvv
{
namespace detail
{
namespace
{

using u64 = uint64_t;

// fflags bits (RISC-V layout).
constexpr uint32_t kNX = 1U << 0; // inexact
constexpr uint32_t kUF = 1U << 1; // underflow
constexpr uint32_t kOF = 1U << 2; // overflow
constexpr uint32_t kDZ = 1U << 3; // divide by zero
constexpr uint32_t kNV = 1U << 4; // invalid

constexpr unsigned kRne = 0;
constexpr unsigned kRtz = 1;
constexpr unsigned kRdn = 2;
constexpr unsigned kRup = 3;
constexpr unsigned kRmm = 4;

// ------------------------------------------------------------------ host environment

int hostRounding(unsigned rm)
{
    switch (rm)
    {
    case kRtz:
        return FE_TOWARDZERO;
    case kRdn:
        return FE_DOWNWARD;
    case kRup:
        return FE_UPWARD;
    case kRne:
    default:
        return FE_TONEAREST; // RNE, and RMM approximated by RNE
    }
}

uint32_t hostFlags()
{
    const int e = std::fetestexcept(FE_ALL_EXCEPT);
    uint32_t f = 0;
    if (e & FE_INEXACT)
        f |= kNX;
    if (e & FE_UNDERFLOW)
        f |= kUF;
    if (e & FE_OVERFLOW)
        f |= kOF;
    if (e & FE_DIVBYZERO)
        f |= kDZ;
    if (e & FE_INVALID)
        f |= kNV;
    return f;
}

/// Sets the host rounding mode for one instruction and restores it afterwards.
class HostRounding
{
  public:
    explicit HostRounding(unsigned rm) : saved_(std::fegetround())
    {
        std::fesetround(hostRounding(rm));
    }
    ~HostRounding() { std::fesetround(saved_); }
    HostRounding(const HostRounding &) = delete;
    HostRounding &operator=(const HostRounding &) = delete;

  private:
    int saved_;
};

// ------------------------------------------------------------------ format traits

template <typename T>
struct Format;

template <>
struct Format<float>
{
    using Bits = uint32_t;
    static constexpr unsigned kBits = 32;
    static constexpr unsigned kExpBits = 8;
    static constexpr unsigned kSigBits = 23;
    static constexpr u64 kCanonicalNaN = 0x7FC00000ULL;
};

template <>
struct Format<double>
{
    using Bits = uint64_t;
    static constexpr unsigned kBits = 64;
    static constexpr unsigned kExpBits = 11;
    static constexpr unsigned kSigBits = 52;
    static constexpr u64 kCanonicalNaN = 0x7FF8000000000000ULL;
};

/// IEEE 754 binary16, stored as bits; arithmetic goes through double (see the file comment).
struct Half
{
    uint16_t bits;
};

template <>
struct Format<Half>
{
    using Bits = uint16_t;
    static constexpr unsigned kBits = 16;
    static constexpr unsigned kExpBits = 5;
    static constexpr unsigned kSigBits = 10;
    static constexpr u64 kCanonicalNaN = 0x7E00ULL;
};

template <typename T>
inline constexpr bool kIsHalf = std::is_same_v<T, Half>;

/**
 * @brief Rounds a double to half under the host rounding mode and raises the host flags
 * (inexact, overflow, and underflow with tininess detected after rounding).
 */
u64 roundToHalf(double v)
{
    if (std::isnan(v))
        return Format<Half>::kCanonicalNaN;
    const u64 sign = std::signbit(v) ? 0x8000U : 0U;
    const double a = std::fabs(v);
    if (std::isinf(a))
        return sign | 0x7C00U;
    if (a == 0)
        return sign;

    const int mode = std::fegetround();
    // Rounds the integer-and-fraction split of a scaled value; returns the rounded integer.
    auto roundScaled = [&](double scaled, bool &inexact)
    {
        const double whole = std::floor(scaled);
        const double fraction = scaled - whole; // exact
        inexact = fraction != 0;
        double r = whole;
        if (mode == FE_TONEAREST)
        {
            if (fraction > 0.5 || (fraction == 0.5 && std::fmod(whole, 2.0) != 0))
                r += 1;
        }
        else if (mode == FE_UPWARD)
        {
            if (inexact && sign == 0)
                r += 1;
        }
        else if (mode == FE_DOWNWARD)
        {
            if (inexact && sign != 0)
                r += 1;
        }
        return r;
    };

    int e = 0;
    std::frexp(a, &e);
    const int exp = e - 1; // a is in [2^exp, 2^(exp + 1))
    const bool priorInexact = std::fetestexcept(FE_INEXACT) != 0;

    if (exp >= -14)
    {
        bool inexact = false;
        double m = roundScaled(std::ldexp(a, 10 - exp), inexact); // in [1024, 2048]
        int outExp = exp;
        if (m == 2048)
        {
            m = 1024;
            ++outExp;
        }
        if (inexact)
            std::feraiseexcept(FE_INEXACT);
        if (outExp > 15)
        {
            std::feraiseexcept(FE_OVERFLOW | FE_INEXACT);
            const bool toInfinity = mode == FE_TONEAREST || (mode == FE_UPWARD && sign == 0) ||
                                    (mode == FE_DOWNWARD && sign != 0);
            return sign | (toInfinity ? 0x7C00U : 0x7BFFU);
        }
        return sign | (static_cast<u64>(outExp + 15) << 10) | (static_cast<u64>(m) - 1024);
    }

    // Below the normal range: the result is a multiple of 2^-24.
    bool inexact = false;
    const double m = roundScaled(std::ldexp(a, 24), inexact); // in [0, 1024]
    // Tininess after rounding: round to 11 significant bits with an unbounded exponent range.
    bool unboundedInexact = false;
    const double unbounded = std::ldexp(roundScaled(std::ldexp(a, 10 - exp), unboundedInexact),
                                        exp - 10);
    const bool tiny = unbounded < std::ldexp(1.0, -14);
    if (inexact)
        std::feraiseexcept(FE_INEXACT);
    if (tiny && (inexact || priorInexact))
        std::feraiseexcept(FE_UNDERFLOW);
    return sign | static_cast<u64>(m); // m = 1024 encodes the smallest normal
}

/// Raises invalid for signaling NaN operands of a half operation (the conversion to double
/// is done in software and does not raise it).
void raiseForSignalingHalf(std::initializer_list<u64> operands);

template <typename T>
T fromBits(u64 raw)
{
    typename Format<T>::Bits bits = static_cast<typename Format<T>::Bits>(raw);
    T value;
    std::memcpy(&value, &bits, sizeof(T));
    return value;
}

template <typename T>
u64 toBits(T value)
{
    typename Format<T>::Bits bits;
    std::memcpy(&bits, &value, sizeof(T));
    return bits;
}

template <typename T>
bool isNaN(u64 raw)
{
    const u64 expMask = maskBits(Format<T>::kExpBits) << Format<T>::kSigBits;
    return (raw & expMask) == expMask && (raw & maskBits(Format<T>::kSigBits)) != 0;
}

template <typename T>
bool isSignalingNaN(u64 raw)
{
    return isNaN<T>(raw) && ((raw >> (Format<T>::kSigBits - 1)) & 1U) == 0;
}

void raiseForSignalingHalf(std::initializer_list<u64> operands)
{
    for (u64 raw : operands)
    {
        if (isSignalingNaN<Half>(raw))
            std::feraiseexcept(FE_INVALID);
    }
}

/// Result of an arithmetic operation: NaNs become the canonical NaN.
template <typename T>
u64 result(T value)
{
    return std::isnan(value) ? Format<T>::kCanonicalNaN : toBits(value);
}

/// Value of a float, double or half element as a double (exact).
template <typename T>
double valueOf(u64 raw)
{
    if constexpr (kIsHalf<T>)
        return halfToDouble(raw);
    else
        return static_cast<double>(fromBits<T>(raw));
}

/// Runs one element operation with cleared host flags and accrues the raised flags.
template <typename Fn>
u64 withFlags(ExecState &s, Fn &&fn)
{
    std::feclearexcept(FE_ALL_EXCEPT);
    const u64 r = fn();
    s.fflags |= hostFlags();
    return r;
}

// ------------------------------------------------------------------ element operations

template <typename T>
u64 minMax(ExecState &s, u64 a, u64 b, bool isMax)
{
    if (isSignalingNaN<T>(a) || isSignalingNaN<T>(b))
        s.fflags |= kNV;
    const bool aNaN = isNaN<T>(a);
    const bool bNaN = isNaN<T>(b);
    if (aNaN && bNaN)
        return Format<T>::kCanonicalNaN;
    if (aNaN)
        return b;
    if (bNaN)
        return a;
    const double x = valueOf<T>(a);
    const double y = valueOf<T>(b);
    if (x == y) // equal, including +0 / -0
    {
        const bool aNegative = std::signbit(x);
        return isMax ? (aNegative ? b : a) : (aNegative ? a : b);
    }
    return (isMax ? x > y : x < y) ? a : b;
}

template <typename T>
u64 classify(u64 raw)
{
    const bool negative = ((raw >> (Format<T>::kBits - 1)) & 1U) != 0;
    const u64 exp = (raw >> Format<T>::kSigBits) & maskBits(Format<T>::kExpBits);
    const u64 sig = raw & maskBits(Format<T>::kSigBits);
    const u64 expMax = maskBits(Format<T>::kExpBits);
    if (exp == expMax)
    {
        if (sig == 0)
            return negative ? 1U << 0 : 1U << 7; // -inf / +inf
        return ((sig >> (Format<T>::kSigBits - 1)) & 1U) != 0 ? 1U << 9 : 1U << 8; // qNaN / sNaN
    }
    if (exp == 0)
    {
        if (sig == 0)
            return negative ? 1U << 3 : 1U << 4; // -0 / +0
        return negative ? 1U << 2 : 1U << 5;     // subnormal
    }
    return negative ? 1U << 1 : 1U << 6; // normal
}

/// Bit-exact 7-bit reciprocal square-root estimate (vfrsqrt7.v).
template <typename T>
u64 rsqrt7(ExecState &s, u64 raw)
{
    constexpr unsigned e = Format<T>::kExpBits;
    constexpr unsigned sb = Format<T>::kSigBits;
    static const uint8_t table[128] = {
        52,  51,  50,  48,  47,  46,  44,  43,  42,  41,  40,  39,  38,  36,  35,  34,
        33,  32,  31,  30,  30,  29,  28,  27,  26,  25,  24,  23,  23,  22,  21,  20,
        19,  19,  18,  17,  16,  16,  15,  14,  14,  13,  12,  12,  11,  10,  10,  9,
        9,   8,   7,   7,   6,   6,   5,   4,   4,   3,   3,   2,   2,   1,   1,   0,
        127, 125, 123, 121, 119, 118, 116, 114, 113, 111, 109, 108, 106, 105, 103, 102,
        100, 99,  97,  96,  95,  93,  92,  91,  90,  88,  87,  86,  85,  84,  83,  82,
        80,  79,  78,  77,  76,  75,  74,  73,  72,  71,  70,  70,  69,  68,  67,  66,
        65,  64,  63,  63,  62,  61,  60,  59,  59,  58,  57,  56,  56,  55,  54,  53};
    const u64 cls = classify<T>(raw);
    const u64 signBit = 1ULL << (e + sb);
    const u64 inf = maskBits(e) << sb;
    switch (cls)
    {
    case 1U << 0: // -inf
    case 1U << 1: // -normal
    case 1U << 2: // -subnormal
    case 1U << 8: // sNaN
        s.fflags |= kNV;
        return Format<T>::kCanonicalNaN;
    case 1U << 9: // qNaN
        return Format<T>::kCanonicalNaN;
    case 1U << 3: // -0
        s.fflags |= kDZ;
        return signBit | inf;
    case 1U << 4: // +0
        s.fflags |= kDZ;
        return inf;
    case 1U << 7: // +inf
        return 0;
    default:
        break;
    }
    u64 exp = (raw >> sb) & maskBits(e);
    u64 sig = raw & maskBits(sb);
    if (cls == (1U << 5)) // positive subnormal: normalise
    {
        while (((sig >> (sb - 1)) & 1U) == 0)
        {
            --exp;
            sig <<= 1;
        }
        sig = (sig << 1) & maskBits(sb);
    }
    const unsigned index = static_cast<unsigned>(((exp & 1U) << 6) | (sig >> (sb - 6)));
    const u64 outSig = static_cast<u64>(table[index]) << (sb - 7);
    const u64 outExp = ((3 * maskBits(e - 1) + ~exp) / 2) & maskBits(e);
    return (outExp << sb) | outSig;
}

/// Bit-exact 7-bit reciprocal estimate (vfrec7.v).
template <typename T>
u64 rec7(ExecState &s, u64 raw, unsigned rm)
{
    constexpr unsigned e = Format<T>::kExpBits;
    constexpr unsigned sb = Format<T>::kSigBits;
    static const uint8_t table[128] = {
        127, 125, 123, 121, 119, 117, 116, 114, 112, 110, 109, 107, 105, 104, 102, 100,
        99,  97,  96,  94,  93,  91,  90,  88,  87,  85,  84,  83,  81,  80,  79,  77,
        76,  75,  74,  72,  71,  70,  69,  68,  66,  65,  64,  63,  62,  61,  60,  59,
        58,  57,  56,  55,  54,  53,  52,  51,  50,  49,  48,  47,  46,  45,  44,  43,
        42,  41,  40,  40,  39,  38,  37,  36,  35,  35,  34,  33,  32,  31,  31,  30,
        29,  28,  28,  27,  26,  25,  25,  24,  23,  23,  22,  21,  21,  20,  19,  19,
        18,  17,  17,  16,  15,  15,  14,  14,  13,  12,  12,  11,  11,  10,  9,   9,
        8,   8,   7,   7,   6,   5,   5,   4,   4,   3,   3,   2,   2,   1,   1,   0};
    const u64 cls = classify<T>(raw);
    const u64 signBit = 1ULL << (e + sb);
    const u64 inf = maskBits(e) << sb;
    switch (cls)
    {
    case 1U << 0: // -inf
        return signBit;
    case 1U << 7: // +inf
        return 0;
    case 1U << 3: // -0
        s.fflags |= kDZ;
        return signBit | inf;
    case 1U << 4: // +0
        s.fflags |= kDZ;
        return inf;
    case 1U << 8: // sNaN
        s.fflags |= kNV;
        return Format<T>::kCanonicalNaN;
    case 1U << 9: // qNaN
        return Format<T>::kCanonicalNaN;
    default:
        break;
    }
    const u64 sign = (raw >> (e + sb)) & 1U;
    u64 exp = (raw >> sb) & maskBits(e);
    u64 sig = raw & maskBits(sb);
    if (cls == (1U << 2) || cls == (1U << 5)) // subnormal: normalise
    {
        while (((sig >> (sb - 1)) & 1U) == 0)
        {
            --exp;
            sig <<= 1;
        }
        sig = (sig << 1) & maskBits(sb);
        if (exp != 0 && exp != ~0ULL)
        {
            // The reciprocal overflows: result depends on the rounding mode.
            s.fflags |= kNX | kOF;
            const bool toMaxFinite =
                rm == kRtz || (rm == kRdn && sign == 0) || (rm == kRup && sign != 0);
            const u64 infinity = (sign << (e + sb)) | inf;
            return toMaxFinite ? infinity - 1 : infinity;
        }
    }
    const unsigned index = static_cast<unsigned>(sig >> (sb - 7));
    u64 outSig = static_cast<u64>(table[index]) << (sb - 7);
    u64 outExp = 2 * maskBits(e - 1) + ~exp;
    if (outExp == 0 || outExp == ~0ULL)
    {
        outSig = (outSig >> 1) | (1ULL << (sb - 1));
        if (outExp == ~0ULL)
        {
            outSig >>= 1;
            outExp = 0;
        }
    }
    return (sign << (e + sb)) | ((outExp & maskBits(e)) << sb) | outSig;
}

/// Rounds an (exactly represented) value to an integer under a RISC-V rounding mode.
double roundToInteger(double v, unsigned rm)
{
    switch (rm)
    {
    case kRtz:
        return std::trunc(v);
    case kRdn:
        return std::floor(v);
    case kRup:
        return std::ceil(v);
    case kRmm:
        return std::round(v); // ties away from zero
    default:
    {
        const double lower = std::floor(v);
        const double diff = v - lower;
        if (diff > 0.5)
            return lower + 1;
        if (diff < 0.5)
            return lower;
        return std::fmod(lower, 2.0) == 0.0 ? lower : lower + 1; // tie: to even
    }
    }
}

/// RISC-V float-to-integer conversion: saturates out-of-range values and NaN (to the maximum).
u64 floatToInteger(ExecState &s, double v, unsigned bits, bool isSigned, unsigned rm)
{
    const double limit = std::ldexp(1.0, static_cast<int>(isSigned ? bits - 1 : bits));
    const u64 maxBits = isSigned ? maskBits(bits - 1) : maskBits(bits);
    const u64 minBits = isSigned ? (1ULL << (bits - 1)) : 0; // two's complement minimum
    if (std::isnan(v))
    {
        s.fflags |= kNV;
        return maxBits;
    }
    const double r = roundToInteger(v, rm);
    if (r >= limit)
    {
        s.fflags |= kNV;
        return maxBits;
    }
    if ((isSigned && r < -limit) || (!isSigned && r < 0))
    {
        s.fflags |= kNV;
        return minBits;
    }
    if (r != v)
        s.fflags |= kNX;
    if (isSigned)
        return static_cast<u64>(static_cast<int64_t>(r)) & maskBits(bits);
    return static_cast<u64>(r);
}

/// Integer (bits wide, signed or not) to floating-point T under the current host rounding mode.
template <typename T>
u64 integerToFloat(ExecState &s, u64 raw, unsigned bits, bool isSigned)
{
    if constexpr (kIsHalf<T>)
    {
        // Integers converted to half are at most 32 bits wide, so exact in double.
        return withFlags(s,
                         [&]
                         {
                             const double value =
                                 isSigned ? static_cast<double>(signExtend(raw, bits))
                                          : static_cast<double>(raw & maskBits(bits));
                             return roundToHalf(value);
                         });
    }
    else
    {
        return withFlags(s,
                         [&]
                         {
                             volatile T value = isSigned
                                                    ? static_cast<T>(signExtend(raw, bits))
                                                    : static_cast<T>(raw & maskBits(bits));
                             return toBits(static_cast<T>(value));
                         });
    }
}

template <typename T>
double asDouble(u64 raw)
{
    return static_cast<double>(fromBits<T>(raw));
}

// ------------------------------------------------------------------ operations by kind

enum class Kind
{
    Binary,     // vd = op(vs2, op1)
    FusedMulAdd,
    Unary,      // vd = op(vs2)
    Compare,    // mask = op(vs2, op1)
    Merge,      // vfmerge.vfm / vfmv.v.f
    Widening,   // vfwadd, vfwsub, vfwmul, vfw*macc
    Convert,    // vfcvt / vfwcvt / vfncvt
    Reduction,  // vfred*, vfwred*
    ScalarMove, // vfmv.f.s / vfmv.s.f
    Slide,      // vfslide1up / vfslide1down
};

const std::unordered_map<std::string, Kind> &kinds()
{
    static const std::unordered_map<std::string, Kind> table = {
        {"vfadd", Kind::Binary},       {"vfsub", Kind::Binary},       {"vfrsub", Kind::Binary},
        {"vfmul", Kind::Binary},       {"vfdiv", Kind::Binary},       {"vfrdiv", Kind::Binary},
        {"vfmin", Kind::Binary},       {"vfmax", Kind::Binary},       {"vfsgnj", Kind::Binary},
        {"vfsgnjn", Kind::Binary},     {"vfsgnjx", Kind::Binary},     {"vfmacc", Kind::FusedMulAdd},
        {"vfnmacc", Kind::FusedMulAdd}, {"vfmsac", Kind::FusedMulAdd}, {"vfnmsac", Kind::FusedMulAdd},
        {"vfmadd", Kind::FusedMulAdd}, {"vfnmadd", Kind::FusedMulAdd}, {"vfmsub", Kind::FusedMulAdd},
        {"vfnmsub", Kind::FusedMulAdd}, {"vfsqrt", Kind::Unary},      {"vfrsqrt7", Kind::Unary},
        {"vfrec7", Kind::Unary},       {"vfclass", Kind::Unary},      {"vmfeq", Kind::Compare},
        {"vmfne", Kind::Compare},      {"vmflt", Kind::Compare},      {"vmfle", Kind::Compare},
        {"vmfgt", Kind::Compare},      {"vmfge", Kind::Compare},      {"vfmerge", Kind::Merge},
        {"vfwadd", Kind::Widening},    {"vfwsub", Kind::Widening},    {"vfwmul", Kind::Widening},
        {"vfwmacc", Kind::Widening},   {"vfwnmacc", Kind::Widening},  {"vfwmsac", Kind::Widening},
        {"vfwnmsac", Kind::Widening},  {"vfcvt", Kind::Convert},      {"vfwcvt", Kind::Convert},
        {"vfncvt", Kind::Convert},     {"vfredosum", Kind::Reduction}, {"vfredusum", Kind::Reduction},
        {"vfredmin", Kind::Reduction}, {"vfredmax", Kind::Reduction}, {"vfwredosum", Kind::Reduction},
        {"vfwredusum", Kind::Reduction}, {"vfslide1up", Kind::Slide}, {"vfslide1down", Kind::Slide},
    };
    return table;
}

bool findKind(const InsnInfo &info, Kind &kind)
{
    if (info.base == "vfmv")
    {
        kind = info.suffix == "v.f" ? Kind::Merge : Kind::ScalarMove;
        return true;
    }
    auto it = kinds().find(info.base);
    if (it == kinds().end())
        return false;
    kind = it->second;
    return true;
}

unsigned roundingFromFrm(const ExecState &s)
{
    const unsigned frm = static_cast<unsigned>(s.csr(csr_address::kFrm) & 7U);
    s.require(frm <= kRmm, "frm holds a reserved rounding mode (" + std::to_string(frm) + ")");
    return frm;
}

void requireFloatSew(const ExecState &s, unsigned sew)
{
    s.require(config().supportsFloat(sew),
              "floating-point element width " + std::to_string(sew) + " is not supported by " +
                  config().describe());
}

template <typename T>
u64 binaryOp(ExecState &s, const std::string &base, u64 a, u64 b)
{
    const u64 sign = 1ULL << (Format<T>::kBits - 1);
    if (base == "vfmin" || base == "vfmax")
        return minMax<T>(s, a, b, base == "vfmax");
    if (base == "vfsgnj")
        return (a & ~sign) | (b & sign);
    if (base == "vfsgnjn")
        return (a & ~sign) | (~b & sign);
    if (base == "vfsgnjx")
        return a ^ (b & sign);
    if constexpr (kIsHalf<T>)
    {
        // Sums, differences and products of halves are exact in double.
        return withFlags(s,
                         [&]
                         {
                             raiseForSignalingHalf({a, b});
                             volatile double x = halfToDouble(a);
                             volatile double y = halfToDouble(b);
                             double r = 0;
                             if (base == "vfadd")
                                 r = x + y;
                             else if (base == "vfsub")
                                 r = x - y;
                             else if (base == "vfrsub")
                                 r = y - x;
                             else if (base == "vfmul")
                                 r = x * y;
                             else if (base == "vfdiv")
                                 r = x / y;
                             else // vfrdiv
                                 r = y / x;
                             return roundToHalf(r);
                         });
    }
    else
    {
        const T x = fromBits<T>(a);
        const T y = fromBits<T>(b);
        return withFlags(s,
                         [&]
                         {
                             volatile T vx = x;
                             volatile T vy = y;
                             T r{};
                             if (base == "vfadd")
                                 r = vx + vy;
                             else if (base == "vfsub")
                                 r = vx - vy;
                             else if (base == "vfrsub")
                                 r = vy - vx;
                             else if (base == "vfmul")
                                 r = vx * vy;
                             else if (base == "vfdiv")
                                 r = vx / vy;
                             else // vfrdiv
                                 r = vy / vx;
                             return result<T>(r);
                         });
    }
}

/// vd = fma per the eight RVV forms; s1 = vs1/rs1, a = vs2, d = old vd.
template <typename T>
u64 fusedOp(ExecState &s, const std::string &base, u64 s1, u64 a, u64 d)
{
    if constexpr (kIsHalf<T>)
    {
        // The product of two halves is exact in double; the fused sum is rounded once to
        // double and once to half, which cannot change the half result (file comment).
        return withFlags(s,
                         [&]
                         {
                             raiseForSignalingHalf({s1, a, d});
                             const double x = halfToDouble(s1);
                             const double v2 = halfToDouble(a);
                             const double vd = halfToDouble(d);
                             volatile double r = 0;
                             if (base == "vfmacc")
                                 r = std::fma(x, v2, vd);
                             else if (base == "vfnmacc")
                                 r = std::fma(-x, v2, -vd);
                             else if (base == "vfmsac")
                                 r = std::fma(x, v2, -vd);
                             else if (base == "vfnmsac")
                                 r = std::fma(-x, v2, vd);
                             else if (base == "vfmadd")
                                 r = std::fma(x, vd, v2);
                             else if (base == "vfnmadd")
                                 r = std::fma(-x, vd, -v2);
                             else if (base == "vfmsub")
                                 r = std::fma(x, vd, -v2);
                             else // vfnmsub
                                 r = std::fma(-x, vd, v2);
                             return roundToHalf(r);
                         });
    }
    else
    {
        return withFlags(s,
                         [&]
                         {
                             const T x = fromBits<T>(s1);
                             const T v2 = fromBits<T>(a);
                             const T vd = fromBits<T>(d);
                             T r{};
                             if (base == "vfmacc" || base == "vfwmacc")
                                 r = std::fma(x, v2, vd);
                             else if (base == "vfnmacc" || base == "vfwnmacc")
                                 r = std::fma(-x, v2, -vd);
                             else if (base == "vfmsac" || base == "vfwmsac")
                                 r = std::fma(x, v2, -vd);
                             else if (base == "vfnmsac" || base == "vfwnmsac")
                                 r = std::fma(-x, v2, vd);
                             else if (base == "vfmadd")
                                 r = std::fma(x, vd, v2);
                             else if (base == "vfnmadd")
                                 r = std::fma(-x, vd, -v2);
                             else if (base == "vfmsub")
                                 r = std::fma(x, vd, -v2);
                             else // vfnmsub
                                 r = std::fma(-x, vd, v2);
                             return result<T>(r);
                         });
    }
}

template <typename T>
bool compareOp(ExecState &s, const std::string &base, u64 a, u64 b)
{
    const bool anyNaN = isNaN<T>(a) || isNaN<T>(b);
    if (base == "vmfeq" || base == "vmfne")
    {
        if (isSignalingNaN<T>(a) || isSignalingNaN<T>(b))
            s.fflags |= kNV; // quiet comparison
        const bool equal = !anyNaN && valueOf<T>(a) == valueOf<T>(b);
        return base == "vmfeq" ? equal : !equal;
    }
    if (anyNaN)
    {
        s.fflags |= kNV; // signaling comparison
        return false;
    }
    const double x = valueOf<T>(a);
    const double y = valueOf<T>(b);
    if (base == "vmflt")
        return x < y;
    if (base == "vmfle")
        return x <= y;
    if (base == "vmfgt")
        return x > y;
    return x >= y; // vmfge
}

template <typename T>
u64 unaryOp(ExecState &s, const std::string &base, u64 a, unsigned rm)
{
    if (base == "vfclass")
        return classify<T>(a);
    if (base == "vfrsqrt7")
        return rsqrt7<T>(s, a);
    if (base == "vfrec7")
        return rec7<T>(s, a, rm);
    if constexpr (kIsHalf<T>)
    {
        return withFlags(s,
                         [&]
                         {
                             raiseForSignalingHalf({a});
                             volatile double x = halfToDouble(a);
                             return roundToHalf(std::sqrt(static_cast<double>(x)));
                         });
    }
    else
    {
        return withFlags(s,
                         [&]
                         {
                             volatile T x = fromBits<T>(a);
                             return result<T>(std::sqrt(static_cast<T>(x)));
                         });
    }
}

/// Calls fn with a value of the element type for SEW (Half, float or double).
template <typename Fn>
u64 bySew(unsigned sew, Fn &&fn)
{
    switch (sew)
    {
    case 16:
        return fn(Half{});
    case 32:
        return fn(float{});
    default:
        return fn(double{});
    }
}

/// Converts a narrow element exactly to the next wider format (half to single, single to
/// double). A signaling NaN raises invalid; NaNs become the canonical NaN of the wide format.
u64 widenExact(ExecState &s, u64 raw, unsigned narrowEew)
{
    if (narrowEew == 16)
    {
        if (isSignalingNaN<Half>(raw))
            s.fflags |= kNV;
        if (isNaN<Half>(raw))
            return Format<float>::kCanonicalNaN;
        return toBits(static_cast<float>(halfToDouble(raw)));
    }
    return withFlags(s, [&] { return result<double>(asDouble<float>(raw)); });
}

// ------------------------------------------------------------------ handlers

/// Scalar FP operand: the low SEW bits of f[rs1] (Kites keeps single values zero-extended).
u64 scalarFloat(const ExecState &s)
{
    return s.fpr(s.field(kRs1)) & maskBits(s.sew);
}

u64 fpSrc1(const ExecState &s, uint64_t i, unsigned eew)
{
    return s.hasVectorSrc1() ? s.read(s.vs1(), i, eew) : (scalarFloat(s) & maskBits(eew));
}

void elementwise(ExecState &s, Kind kind, unsigned rm)
{
    requireFloatSew(s, s.sew);
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    s.checkGroup(s.group(s.vs2(), s.sew), "vs2");
    if (s.hasVectorSrc1())
        s.checkGroup(s.group(s.vs1(), s.sew), "vs1");
    s.checkMaskOverlap(d);
    const std::string &base = s.info.base;
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        const u64 a = s.read(s.vs2(), i, s.sew);
        u64 r = 0;
        if (kind == Kind::Binary)
        {
            const u64 b = fpSrc1(s, i, s.sew);
            r = bySew(s.sew, [&](auto t) { return binaryOp<decltype(t)>(s, base, a, b); });
        }
        else if (kind == Kind::FusedMulAdd)
        {
            const u64 b = fpSrc1(s, i, s.sew);
            const u64 old = s.read(d.reg, i, s.sew);
            r = bySew(s.sew, [&](auto t) { return fusedOp<decltype(t)>(s, base, b, a, old); });
        }
        else // Unary
        {
            r = bySew(s.sew, [&](auto t) { return unaryOp<decltype(t)>(s, base, a, rm); });
        }
        s.write(d.reg, i, s.sew, r);
    }
    s.tail(d);
}

void compare(ExecState &s)
{
    requireFloatSew(s, s.sew);
    const Group a = s.group(s.vs2(), s.sew);
    s.checkGroup(a, "vs2");
    const Group d = ExecState::maskGroup(s.vd());
    s.checkOverlap(d, a, "vs2");
    if (s.hasVectorSrc1())
    {
        const Group b = s.group(s.vs1(), s.sew);
        s.checkGroup(b, "vs1");
        s.checkOverlap(d, b, "vs1");
    }
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactiveMaskBit(d.reg, i);
            continue;
        }
        const u64 av = s.read(s.vs2(), i, s.sew);
        const u64 bv = fpSrc1(s, i, s.sew);
        const bool r =
            bySew(s.sew, [&](auto t) -> u64
                  { return compareOp<decltype(t)>(s, s.info.base, av, bv) ? 1 : 0; }) != 0;
        s.writeMaskBit(d.reg, i, r);
    }
    s.maskTail(d.reg, s.vl);
}

void merge(ExecState &s)
{
    requireFloatSew(s, s.sew);
    const bool isMerge = s.info.base == "vfmerge";
    const Group d = s.group(s.vd(), s.sew);
    s.checkGroup(d, "vd");
    if (isMerge)
    {
        s.checkGroup(s.group(s.vs2(), s.sew), "vs2");
        s.require(!overlaps(d, ExecState::maskGroup(0)),
                  "vfmerge destination overlaps the selector register v0");
    }
    const u64 scalar = scalarFloat(s);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        const bool takeScalar = !isMerge || s.maskBit(0, i);
        s.write(d.reg, i, s.sew, takeScalar ? scalar : s.read(s.vs2(), i, s.sew));
    }
    s.tail(d);
}

void widening(ExecState &s, unsigned rm)
{
    (void)rm;
    s.require(config().supportsFloat(s.sew) && config().supportsFloat(2 * s.sew),
              "widening floating-point operations need SEW and 2*SEW floating point (" +
                  config().describe() + ")");
    const std::string &base = s.info.base;
    const bool wideA = !s.info.suffix.empty() && s.info.suffix[0] == 'w';
    const bool fused = base.find("macc") != std::string::npos || base.find("msac") != std::string::npos;
    const unsigned narrowEew = s.sew;
    const unsigned wideEew = 2 * s.sew;
    const Group d = s.group(s.vd(), wideEew);
    s.checkGroup(d, "vd");
    const Group a = s.group(s.vs2(), wideA ? wideEew : narrowEew);
    s.checkGroup(a, "vs2");
    s.checkOverlap(d, a, "vs2");
    if (s.hasVectorSrc1())
    {
        const Group b = s.group(s.vs1(), narrowEew);
        s.checkGroup(b, "vs1");
        s.checkOverlap(d, b, "vs1");
    }
    s.checkMaskOverlap(d);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        const u64 rawA = s.read(s.vs2(), i, wideA ? wideEew : narrowEew);
        const u64 rawB = fpSrc1(s, i, narrowEew);
        // Widen the narrow operands exactly (an sNaN raises invalid here, as it would in the op).
        const u64 wa = wideA ? rawA : widenExact(s, rawA, narrowEew);
        const u64 wb = widenExact(s, rawB, narrowEew);
        const std::string op = fused                ? base
                               : base == "vfwadd"   ? "vfadd"
                               : base == "vfwsub"   ? "vfsub"
                                                    : "vfmul";
        const u64 old = fused ? s.read(d.reg, i, wideEew) : 0;
        const u64 r = bySew(wideEew,
                            [&](auto t)
                            {
                                using W = decltype(t);
                                if (fused)
                                    return fusedOp<W>(s, op, wb, wa, old);
                                return binaryOp<W>(s, op, wa, wb);
                            });
        s.write(d.reg, i, wideEew, r);
    }
    s.tail(d);
}

void convert(ExecState &s, unsigned rm)
{
    const std::string &base = s.info.base;
    const std::string &suffix = s.info.suffix; // e.g. "rtz.xu.f.v", "f.x.w", "rod.f.f.w"
    const bool widen = base == "vfwcvt";
    const bool narrow = base == "vfncvt";
    const bool rtz = suffix.rfind("rtz.", 0) == 0;
    const bool rod = suffix.rfind("rod.", 0) == 0;
    const std::string kind = rtz || rod ? suffix.substr(4) : suffix; // "xu.f.v", "f.x.w", ...
    const bool toInt = kind[0] == 'x';
    const bool fromInt = kind[2] == 'x';
    const bool isSigned = kind.substr(0, 2) == "x." || kind.substr(2, 2) == "x.";
    const unsigned rounding = rtz ? kRtz : rod ? kRtz : rm;

    const unsigned srcEew = narrow ? 2 * s.sew : s.sew;
    const unsigned dstEew = widen ? 2 * s.sew : s.sew;
    const unsigned floatEew = toInt ? srcEew : dstEew; // width of the floating-point side
    const unsigned otherEew = toInt ? dstEew : srcEew;
    s.require(srcEew <= config().elen() && dstEew <= config().elen(),
              "conversion requires 2*SEW <= ELEN");
    s.require(config().supportsFloat(floatEew), "floating-point width " +
                                                    std::to_string(floatEew) +
                                                    " is not supported by " + config().describe());
    if (!toInt && !fromInt) // float to float
        s.require(config().supportsFloat(otherEew), "floating-point width " +
                                                        std::to_string(otherEew) +
                                                        " is not supported by " +
                                                        config().describe());

    const Group d = s.group(s.vd(), dstEew);
    const Group a = s.group(s.vs2(), srcEew);
    s.checkGroup(d, "vd");
    s.checkGroup(a, "vs2");
    s.checkOverlap(d, a, "vs2");
    s.checkMaskOverlap(d);

    HostRounding host(rounding);
    for (u64 i = s.vstart; i < s.vl; ++i)
    {
        if (!s.active(i))
        {
            s.inactive(d, i);
            continue;
        }
        const u64 raw = s.read(s.vs2(), i, srcEew);
        u64 r = 0;
        if (toInt)
        {
            const u64 valueBits = bySew(srcEew,
                                        [&](auto t)
                                        {
                                            using T = decltype(t);
                                            if (isSignalingNaN<T>(raw))
                                                s.fflags |= kNV;
                                            return toBits(valueOf<T>(raw));
                                        });
            r = floatToInteger(s, fromBits<double>(valueBits), dstEew, isSigned, rounding);
        }
        else if (fromInt)
        {
            r = bySew(dstEew, [&](auto t)
                      { return integerToFloat<decltype(t)>(s, raw, srcEew, isSigned); });
        }
        else if (widen) // f16 -> f32 or f32 -> f64, exact
        {
            r = widenExact(s, raw, srcEew);
        }
        else // f32 -> f16 or f64 -> f32
        {
            const bool toHalf = dstEew == 16;
            r = withFlags(s,
                          [&]
                          {
                              if (toHalf)
                              {
                                  volatile double value = asDouble<float>(raw); // exact
                                  return roundToHalf(value);
                              }
                              volatile double value = fromBits<double>(raw);
                              return result<float>(static_cast<float>(value));
                          });
            const bool resultIsNaN = toHalf ? isNaN<Half>(r) : isNaN<float>(r);
            if (rod && (std::fetestexcept(FE_INEXACT) != 0) && !resultIsNaN)
            {
                r |= 1U; // round to odd: jam inexactness into the least significant bit
            }
        }
        s.write(d.reg, i, dstEew, r);
    }
    s.tail(d);
}

void reduction(ExecState &s, unsigned rm)
{
    (void)rm;
    s.requireVstartZero();
    const std::string &base = s.info.base;
    const bool widen = base.rfind("vfwred", 0) == 0;
    if (widen)
        s.require(config().supportsFloat(s.sew) && config().supportsFloat(2 * s.sew),
                  "widening floating-point reductions need SEW and 2*SEW floating point (" +
                      config().describe() + ")");
    else
        requireFloatSew(s, s.sew);
    s.checkGroup(s.group(s.vs2(), s.sew), "vs2");
    if (s.vl == 0)
        return;
    const unsigned accEew = widen ? 2 * s.sew : s.sew;
    u64 acc = s.read(s.vs1(), 0, accEew);
    const bool isMin = base == "vfredmin";
    const bool isMax = base == "vfredmax";
    const bool unordered = base == "vfredusum" || base == "vfwredusum";
    bool anyActive = false;
    // Unordered sums are computed in element order, which is one of the permitted orders.
    for (u64 i = 0; i < s.vl; ++i)
    {
        if (!s.active(i))
            continue;
        anyActive = true;
        u64 x = s.read(s.vs2(), i, s.sew);
        if (widen)
        {
            x = widenExact(s, x, s.sew);
            acc = bySew(accEew, [&](auto t) { return binaryOp<decltype(t)>(s, "vfadd", acc, x); });
        }
        else if (isMin || isMax)
        {
            acc = bySew(s.sew, [&](auto t) { return minMax<decltype(t)>(s, acc, x, isMax); });
        }
        else
        {
            acc = bySew(s.sew, [&](auto t) { return binaryOp<decltype(t)>(s, "vfadd", acc, x); });
        }
    }
    if (unordered && !anyActive)
    {
        // With no active element, the result is vs1[0] plus an additive identity, which the
        // spec allows for unordered sums: a NaN becomes the canonical NaN (invalid for a
        // signaling NaN). Spike makes the same choice.
        const bool nan = accEew == 16 ? isNaN<Half>(acc) : accEew == 32 ? isNaN<float>(acc)
                                                                       : isNaN<double>(acc);
        const bool signaling = accEew == 16   ? isSignalingNaN<Half>(acc)
                               : accEew == 32 ? isSignalingNaN<float>(acc)
                                              : isSignalingNaN<double>(acc);
        if (signaling)
            s.fflags |= kNV;
        if (nan)
            acc = accEew == 16   ? Format<Half>::kCanonicalNaN
                  : accEew == 32 ? Format<float>::kCanonicalNaN
                                 : Format<double>::kCanonicalNaN;
    }
    s.write(s.vd(), 0, accEew, acc);
    s.tailFrom(Group{s.vd(), accEew, 0}, 1);
}

void scalarMove(ExecState &s)
{
    requireFloatSew(s, s.sew);
    if (s.info.suffix == "f.s")
    {
        // vfmv.f.s ignores vl and vstart. Single values are kept zero-extended, like Kites' F.
        s.fprWrite = std::make_pair(s.field(kRd), s.read(s.vs2(), 0, s.sew));
        return;
    }
    if (s.vstart < s.vl) // vfmv.s.f
    {
        s.write(s.vd(), 0, s.sew, scalarFloat(s));
        s.tailFrom(Group{s.vd(), s.sew, 0}, 1);
    }
}

} // namespace

void executeFloat(ExecState &s)
{
    Kind kind{};
    if (!findKind(s.info, kind))
    {
        s.illegal("instruction is not implemented");
    }
    if (kind == Kind::ScalarMove)
    {
        s.require(!s.vtype.vill(), "vtype is illegal (vill = 1)");
        scalarMove(s);
        return;
    }
    s.requireVtype();
    if (kind == Kind::Slide)
    {
        requireFloatSew(s, s.sew);
        executeInteger(s); // same data movement as vslide1up/vslide1down
        return;
    }
    if (kind == Kind::Compare || kind == Kind::Merge)
    {
        kind == Kind::Compare ? compare(s) : merge(s);
        return;
    }
    const unsigned rm = roundingFromFrm(s);
    HostRounding host(rm);
    switch (kind)
    {
    case Kind::Binary:
    case Kind::FusedMulAdd:
    case Kind::Unary:
        elementwise(s, kind, rm);
        break;
    case Kind::Widening:
        widening(s, rm);
        break;
    case Kind::Convert:
        convert(s, rm);
        break;
    case Kind::Reduction:
        reduction(s, rm);
        break;
    default:
        s.illegal("instruction is not implemented");
    }
}

bool isImplementedFloat(const InsnInfo &info)
{
    Kind kind{};
    return findKind(info, kind);
}

} // namespace detail
} // namespace rvv
} // namespace Kites
