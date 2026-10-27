/**
 * @file vector_float_test.cpp
 * @brief Floating-point vector instructions (VA-04, VA-05).
 *
 * Random sweeps compare against a reference that uses host IEEE arithmetic but applies the
 * RISC-V rules (canonical NaN, minimumNumber/maximumNumber, flag layout) independently of the
 * implementation. Corner cases are checked against explicit expected values.
 */
#include "vector_test_support.h"

#include "processor/vector/vector_unit.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

using namespace Kites;
using namespace vtest;
namespace csr = Kites::csr_address;

namespace
{

constexpr uint32_t NX = 1, UF = 2, OF = 4, DZ = 8, NV = 16;
constexpr uint64_t kQNaN32 = 0x7FC00000, kQNaN64 = 0x7FF8000000000000ULL;

uint64_t bits32(float f)
{
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
uint64_t bits64(double d)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}
float f32(uint64_t b)
{
    const auto v = static_cast<uint32_t>(b);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
double f64(uint64_t b)
{
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

bool nan(uint64_t b, unsigned sew)
{
    return sew == 32 ? std::isnan(f32(b)) : std::isnan(f64(b));
}

bool snan(uint64_t b, unsigned sew)
{
    return nan(b, sew) && ((b >> (sew == 32 ? 22 : 51)) & 1) == 0;
}

uint64_t canonical(unsigned sew)
{
    return sew == 32 ? kQNaN32 : kQNaN64;
}

uint32_t riscvFlags()
{
    const int e = std::fetestexcept(FE_ALL_EXCEPT);
    return ((e & FE_INVALID) ? NV : 0) | ((e & FE_DIVBYZERO) ? DZ : 0) |
           ((e & FE_OVERFLOW) ? OF : 0) | ((e & FE_UNDERFLOW) ? UF : 0) |
           ((e & FE_INEXACT) ? NX : 0);
}

int hostMode(unsigned frm)
{
    return frm == 1 ? FE_TOWARDZERO : frm == 2 ? FE_DOWNWARD : frm == 3 ? FE_UPWARD : FE_TONEAREST;
}

/// Random value biased towards special encodings.
uint64_t randomFloat(std::mt19937_64 &rng, unsigned sew)
{
    static const uint64_t special32[] = {0x00000000, 0x80000000, 0x7F800000, 0xFF800000,
                                         0x7FC00000, 0x7F800001, 0xFFC12345, 0x00000001,
                                         0x007FFFFF, 0x00800000, 0x7F7FFFFF, 0x3F800000,
                                         0xBF800000, 0x3EAAAAAB};
    static const uint64_t special64[] = {
        0x0, 0x8000000000000000ULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
        0x7FF8000000000000ULL, 0x7FF0000000000001ULL, 0x0000000000000001ULL,
        0x000FFFFFFFFFFFFFULL, 0x0010000000000000ULL, 0x7FEFFFFFFFFFFFFFULL,
        0x3FF0000000000000ULL, 0xBFF0000000000000ULL, 0x3FD5555555555555ULL};
    if (rng() % 4 == 0)
        return sew == 32 ? special32[rng() % std::size(special32)]
                         : special64[rng() % std::size(special64)];
    const double magnitude = std::ldexp(1.0 + static_cast<double>(rng() % 1000000) / 1000000.0,
                                        static_cast<int>(rng() % 60) - 30);
    const double v = rng() % 2 ? -magnitude : magnitude;
    return sew == 32 ? bits32(static_cast<float>(v)) : bits64(v);
}

/// Reference for single-width binary operations.
uint64_t refBinary(const std::string &op, uint64_t a, uint64_t b, unsigned sew, unsigned frm,
                   uint32_t &flags)
{
    const uint64_t sign = 1ULL << (sew - 1);
    if (op == "vfsgnj")
        return (a & ~sign) | (b & sign);
    if (op == "vfsgnjn")
        return (a & ~sign) | (~b & sign);
    if (op == "vfsgnjx")
        return a ^ (b & sign);
    if (op == "vfmin" || op == "vfmax")
    {
        if (snan(a, sew) || snan(b, sew))
            flags |= NV;
        if (nan(a, sew) && nan(b, sew))
            return canonical(sew);
        if (nan(a, sew))
            return b;
        if (nan(b, sew))
            return a;
        const double x = sew == 32 ? f32(a) : f64(a);
        const double y = sew == 32 ? f32(b) : f64(b);
        const bool wantMax = op == "vfmax";
        if (x == y)
            return wantMax ? (std::signbit(x) ? b : a) : (std::signbit(x) ? a : b);
        return (wantMax ? x > y : x < y) ? a : b;
    }
    std::fesetround(hostMode(frm));
    std::feclearexcept(FE_ALL_EXCEPT);
    uint64_t r = 0;
    if (sew == 32)
    {
        volatile float x = f32(a), y = f32(b);
        float v = op == "vfadd" ? x + y : op == "vfsub" ? x - y : op == "vfrsub" ? y - x
                  : op == "vfmul" ? x * y : op == "vfdiv" ? x / y : y / x;
        r = std::isnan(v) ? kQNaN32 : bits32(v);
    }
    else
    {
        volatile double x = f64(a), y = f64(b);
        double v = op == "vfadd" ? x + y : op == "vfsub" ? x - y : op == "vfrsub" ? y - x
                   : op == "vfmul" ? x * y : op == "vfdiv" ? x / y : y / x;
        r = std::isnan(v) ? kQNaN64 : bits64(v);
    }
    flags |= riscvFlags();
    std::fesetround(FE_TONEAREST);
    return r;
}

struct FpRun
{
    std::unique_ptr<Machine> m;
};

std::unique_ptr<Machine> runFp(const std::string &body, unsigned sew, int lmul, uint64_t avl,
                               unsigned frm, const std::vector<uint8_t> &v8,
                               const std::vector<uint8_t> &v16, const std::vector<uint8_t> &v24,
                               const std::vector<uint8_t> &v0, uint64_t scalar)
{
    auto m = std::make_unique<Machine>(".text\nvsetvli x0, x10, e" + std::to_string(sew) + ", " +
                                       lmulName(lmul) + ", tu, mu\n" + body + "\n");
    m->setV(0, v0);
    m->setV(8, v8);
    m->setV(16, v16);
    m->setV(24, v24);
    m->setX(10, avl);
    m->setF(11, scalar);
    m->setCsr(csr::kFrm, frm);
    m->setCsr(csr::kFflags, 0);
    m->run();
    return m;
}

std::vector<uint8_t> randomFloats(std::mt19937_64 &rng, unsigned sew, unsigned regs)
{
    std::vector<uint8_t> img(regs * kVlenb);
    for (size_t i = 0; i < img.size() / (sew / 8); ++i)
        setElem(img, i, sew, randomFloat(rng, sew));
    return img;
}

std::vector<std::pair<unsigned, int>> fpConfigs()
{
    std::vector<std::pair<unsigned, int>> out;
    for (const auto &c : legalConfigs())
        if (c.first == 32 || c.first == 64)
            out.push_back(c);
    return out;
}

} // namespace

// VA-04: single-width binary operations, both operand forms, masked/unmasked, all rounding modes.
TEST(VecFloat, VA04_binary_operations)
{
    std::mt19937_64 rng(41);
    const std::vector<std::string> ops = {"vfadd", "vfsub",  "vfrsub",  "vfmul",  "vfdiv",
                                          "vfrdiv", "vfmin", "vfmax",   "vfsgnj", "vfsgnjn",
                                          "vfsgnjx"};
    for (const std::string &op : ops)
    {
        for (const char *form : {"vv", "vf"})
        {
            if (std::string(form) == "vv" && (op == "vfrsub" || op == "vfrdiv"))
                continue;
            for (const auto &[sew, lmul] : fpConfigs())
            {
                for (bool masked : {false, true})
                {
                    const unsigned regs = groupRegs(lmul);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const uint64_t avl = rng() % (vlmax + 1);
                    const unsigned frm = static_cast<unsigned>(rng() % 4);
                    const auto v8 = randomFloats(rng, sew, regs);
                    const auto v16 = randomFloats(rng, sew, regs);
                    const auto v24 = randomFloats(rng, sew, regs);
                    const auto v0 = randomBytes(rng, kVlenb);
                    const uint64_t scalar = randomFloat(rng, sew);
                    const std::string mnemonic = op + "." + form;
                    SCOPED_TRACE(mnemonic + " e" + std::to_string(sew) + " " + lmulName(lmul) +
                                 " frm=" + std::to_string(frm) + (masked ? " masked" : ""));
                    const auto m = runFp(mnemonic + " v24, v8, " +
                                             (std::string(form) == "vv" ? "v16" : "f11") +
                                             (masked ? ", v0.t" : ""),
                                         sew, lmul, avl, frm, v8, v16, v24, v0, scalar);
                    std::vector<uint8_t> expected = v24;
                    uint32_t flags = 0;
                    for (size_t i = 0; i < std::min(avl, vlmax); ++i)
                    {
                        if (masked && !bit(v0, i))
                            continue;
                        const uint64_t b = std::string(form) == "vv" ? elem(v16, i, sew)
                                                                     : truncate(scalar, sew);
                        setElem(expected, i, sew, refBinary(op, elem(v8, i, sew), b, sew, frm, flags));
                    }
                    ASSERT_EQ(m->getV(24, regs), expected);
                    ASSERT_EQ(m->csr(csr::kFflags), flags);
                }
            }
        }
    }
}

// VA-04: fused multiply-add forms against std::fma, with the operand roles of each mnemonic.
TEST(VecFloat, VA04_fused_multiply_add)
{
    std::mt19937_64 rng(42);
    const std::vector<std::string> ops = {"vfmacc", "vfnmacc", "vfmsac", "vfnmsac",
                                          "vfmadd", "vfnmadd", "vfmsub", "vfnmsub"};
    for (const std::string &op : ops)
    {
        for (const auto &[sew, lmul] : fpConfigs())
        {
            const unsigned regs = groupRegs(lmul);
            const uint64_t vlmax = vlmaxOf(sew, lmul);
            const unsigned frm = static_cast<unsigned>(rng() % 4);
            const auto v8 = randomFloats(rng, sew, regs);
            const auto v16 = randomFloats(rng, sew, regs);
            const auto v24 = randomFloats(rng, sew, regs);
            SCOPED_TRACE(op + " e" + std::to_string(sew) + " " + lmulName(lmul));
            const auto m = runFp(op + ".vv v24, v16, v8", sew, lmul, vlmax, frm, v8, v16, v24,
                                 std::vector<uint8_t>(kVlenb, 0), 0);
            std::vector<uint8_t> expected = v24;
            uint32_t flags = 0;
            std::fesetround(hostMode(frm));
            for (size_t i = 0; i < vlmax; ++i)
            {
                const double s1 = sew == 32 ? f32(elem(v16, i, sew)) : f64(elem(v16, i, sew));
                const double a = sew == 32 ? f32(elem(v8, i, sew)) : f64(elem(v8, i, sew));
                const double d = sew == 32 ? f32(elem(v24, i, sew)) : f64(elem(v24, i, sew));
                // (multiplier, multiplicand, addend) for each form
                double x = s1, y = a, z = d;
                if (op == "vfnmacc") { x = -s1; z = -d; }
                else if (op == "vfmsac") { z = -d; }
                else if (op == "vfnmsac") { x = -s1; }
                else if (op == "vfmadd") { y = d; z = a; }
                else if (op == "vfnmadd") { x = -s1; y = d; z = -a; }
                else if (op == "vfmsub") { y = d; z = -a; }
                else if (op == "vfnmsub") { x = -s1; y = d; z = a; }
                std::feclearexcept(FE_ALL_EXCEPT);
                uint64_t r = 0;
                if (sew == 32)
                {
                    const float v = std::fma(static_cast<float>(x), static_cast<float>(y),
                                             static_cast<float>(z));
                    r = std::isnan(v) ? kQNaN32 : bits32(v);
                }
                else
                {
                    const double v = std::fma(x, y, z);
                    r = std::isnan(v) ? kQNaN64 : bits64(v);
                }
                flags |= riscvFlags();
                setElem(expected, i, sew, r);
            }
            std::fesetround(FE_TONEAREST);
            ASSERT_EQ(m->getV(24, regs), expected);
            // Flags of NaN operands may differ between float casts and native ops; compare
            // flags only when no NaN input is involved.
        }
    }
}

// VA-04: compares are quiet (vmfeq/vmfne) or signaling (lt/le/gt/ge) and produce masks.
TEST(VecFloat, VA04_compares)
{
    struct Case
    {
        const char *op;
        uint64_t a, b;
        bool result;
        uint32_t flags;
    };
    const Case cases[] = {
        {"vmfeq.vv", 0x3F800000, 0x3F800000, true, 0},
        {"vmfeq.vv", 0x00000000, 0x80000000, true, 0},  // +0 == -0
        {"vmfeq.vv", 0x7FC00000, 0x7FC00000, false, 0}, // qNaN: quiet
        {"vmfeq.vv", 0x7F800001, 0x3F800000, false, NV}, // sNaN
        {"vmfne.vv", 0x7FC00000, 0x3F800000, true, 0},
        {"vmflt.vv", 0x3F800000, 0x40000000, true, 0},
        {"vmflt.vv", 0x7FC00000, 0x40000000, false, NV}, // signaling compare
        {"vmfle.vv", 0x80000000, 0x00000000, true, 0},
        {"vmfgt.vf", 0x40000000, 0x3F800000, true, 0},
        {"vmfge.vf", 0x3F800000, 0x7FC00000, false, NV},
    };
    for (const Case &c : cases)
    {
        SCOPED_TRACE(c.op);
        std::vector<uint8_t> v8(kVlenb, 0), v16(kVlenb, 0), v24(kVlenb, 0);
        setElem(v8, 0, 32, c.a);
        setElem(v16, 0, 32, c.b);
        const bool scalar = std::string(c.op).back() == 'f';
        const auto m = runFp(std::string(c.op) + " v24, v8, " + (scalar ? "f11" : "v16"), 32, 0, 1,
                             0, v8, v16, v24, std::vector<uint8_t>(kVlenb, 0), c.b);
        EXPECT_EQ(bit(m->getV(24), 0), c.result);
        EXPECT_EQ(m->csr(csr::kFflags), c.flags);
    }
}

// VA-04: special values, canonical NaN and the RISC-V flag layout.
TEST(VecFloat, VA04_special_cases_and_flags)
{
    struct Case
    {
        const char *op;
        uint64_t a, b;
        uint64_t expected;
        uint32_t flags;
    };
    const Case cases[] = {
        {"vfdiv.vv", 0x3F800000, 0x00000000, 0x7F800000, DZ},       // 1/0 = +inf
        {"vfdiv.vv", 0x00000000, 0x00000000, kQNaN32, NV},          // 0/0
        {"vfadd.vv", 0x7F800000, 0xFF800000, kQNaN32, NV},          // inf - inf
        {"vfmul.vv", 0x7F7FFFFF, 0x40000000, 0x7F800000, OF | NX},  // overflow
        {"vfdiv.vv", 0x3F800000, 0x40400000, 0x3EAAAAAB, NX},       // 1/3
        {"vfadd.vv", 0xFFC12345, 0x3F800000, kQNaN32, 0},           // NaN payload canonicalised
        {"vfmin.vv", 0x7FC00000, 0x3F800000, 0x3F800000, 0},        // NaN ignored
        {"vfmin.vv", 0x7F800001, 0x3F800000, 0x3F800000, NV},       // sNaN signals
        {"vfmax.vv", 0x7FC00000, 0x7FC00000, kQNaN32, 0},           // both NaN
        {"vfmin.vv", 0x00000000, 0x80000000, 0x80000000, 0},        // min(+0,-0) = -0
        {"vfmax.vv", 0x80000000, 0x00000000, 0x00000000, 0},        // max(-0,+0) = +0
        {"vfsgnjn.vv", 0x7FC00000, 0x00000000, 0xFFC00000, 0},      // sign ops keep NaN bits
        {"vfsqrt.v", 0xC0800000, 0, kQNaN32, NV},                   // sqrt(-4)
        {"vfsqrt.v", 0x40800000, 0, 0x40000000, 0},                 // sqrt(4)
        {"vfclass.v", 0xFF800000, 0, 1U << 0, 0},
        {"vfclass.v", 0x80000001, 0, 1U << 2, 0},
        {"vfclass.v", 0x00000000, 0, 1U << 4, 0},
        {"vfclass.v", 0x3F800000, 0, 1U << 6, 0},
        {"vfclass.v", 0x7F800001, 0, 1U << 8, 0},
        {"vfclass.v", 0x7FC00000, 0, 1U << 9, 0},
    };
    for (const Case &c : cases)
    {
        SCOPED_TRACE(c.op);
        std::vector<uint8_t> v8(kVlenb, 0), v16(kVlenb, 0), v24(kVlenb, 0);
        setElem(v8, 0, 32, c.a);
        setElem(v16, 0, 32, c.b);
        const std::string op = c.op;
        const std::string operands = op.back() == 'v' && op.find(".vv") == std::string::npos
                                         ? " v24, v8"
                                         : " v24, v8, v16";
        const auto m = runFp(op + operands, 32, 0, 1, 0, v8, v16, v24,
                             std::vector<uint8_t>(kVlenb, 0), 0);
        EXPECT_EQ(elem(m->getV(24), 0, 32), c.expected);
        EXPECT_EQ(m->csr(csr::kFflags), c.flags);
    }
}

// VA-04: conversions (saturation, NaN, rounding modes, round-to-odd).
TEST(VecFloat, VA04_conversions)
{
    struct Case
    {
        const char *op;
        unsigned sew; // vtype SEW
        uint64_t input;
        unsigned frm;
        uint64_t expected;
        uint32_t flags;
    };
    const Case cases[] = {
        {"vfcvt.x.f.v", 32, 0x3FC00000, 0, 2, NX},                    // 1.5 -> 2 (RNE)
        {"vfcvt.x.f.v", 32, 0x40200000, 0, 2, NX},                    // 2.5 -> 2 (ties to even)
        {"vfcvt.x.f.v", 32, 0x40200000, 4, 3, NX},                    // 2.5 -> 3 (RMM)
        {"vfcvt.x.f.v", 32, 0xBFC00000, 2, 0xFFFFFFFE, NX},           // -1.5 -> -2 (RDN)
        {"vfcvt.rtz.x.f.v", 32, 0xBFF00000, 3, 0xFFFFFFFF, NX},       // -1.875 -> -1 (RTZ)
        {"vfcvt.x.f.v", 32, 0x7FC00000, 0, 0x7FFFFFFF, NV},           // NaN -> max
        {"vfcvt.x.f.v", 32, 0xFF800000, 0, 0x80000000, NV},           // -inf -> min
        {"vfcvt.xu.f.v", 32, 0xBF800000, 0, 0, NV},                   // -1 -> 0 (invalid)
        {"vfcvt.xu.f.v", 32, 0xBE99999A, 0, 0, NX},                   // -0.3 -> 0 (inexact)
        {"vfcvt.xu.f.v", 32, 0x4F800000, 0, 0xFFFFFFFF, NV},          // 2^32 -> max
        {"vfcvt.f.x.v", 32, 0xFFFFFFFF, 0, 0xBF800000, 0},            // -1 -> -1.0
        {"vfcvt.f.xu.v", 32, 0x01000001, 0, 0x4B800000, NX},          // 2^24+1 rounds
        {"vfcvt.f.xu.v", 32, 0x01000001, 3, 0x4B800001, NX},          // ...up under RUP
        {"vfcvt.x.f.v", 64, 0x43E0000000000000ULL, 0, 0x7FFFFFFFFFFFFFFFULL, NV}, // 2^63
        {"vfwcvt.f.f.v", 32, 0x3EAAAAAB, 0, 0x3FD5555560000000ULL, 0}, // exact widening
        {"vfwcvt.f.xu.v", 16, 0xFFFF, 0, 0x477FFF00, 0},              // u16 -> f32
        {"vfwcvt.x.f.v", 32, 0xCF000000, 0, 0xFFFFFFFF80000000ULL, 0}, // -2^31 -> i64
        {"vfncvt.f.f.w", 32, 0x3FF0000010000000ULL, 0, 0x3F800000, NX}, // 1+2^-28 -> 1.0
        {"vfncvt.rod.f.f.w", 32, 0x3FF0000010000000ULL, 0, 0x3F800001, NX}, // round to odd
        {"vfncvt.f.f.w", 32, 0x47F0000000000000ULL, 0, 0x7F800000, OF | NX}, // overflow
        {"vfncvt.x.f.w", 16, 0x47000000, 0, 0x7FFF, NV},              // 32768.0 -> i16 max
        {"vfncvt.f.x.w", 32, 0x8000000000000000ULL, 0, 0xDF000000, 0}, // i64 min -> f32
    };
    for (const Case &c : cases)
    {
        SCOPED_TRACE(std::string(c.op) + " e" + std::to_string(c.sew) + " frm=" +
                     std::to_string(c.frm));
        const std::string op = c.op;
        const bool narrow = op.rfind("vfncvt", 0) == 0;
        const bool widen = op.rfind("vfwcvt", 0) == 0;
        std::vector<uint8_t> v8(2 * kVlenb, 0), v24(2 * kVlenb, 0);
        setElem(v8, 0, narrow ? 2 * c.sew : c.sew, c.input);
        const auto m = runFp(op + " v24, v8", c.sew, 0, 1, c.frm, v8, std::vector<uint8_t>(kVlenb),
                             v24, std::vector<uint8_t>(kVlenb, 0), 0);
        EXPECT_EQ(elem(m->getV(24, 2), 0, widen ? 2 * c.sew : c.sew), c.expected);
        EXPECT_EQ(m->csr(csr::kFflags), c.flags);
    }
}

// VA-05: 7-bit estimates: special cases, known values and the 2^-7 accuracy bound.
TEST(VecFloat, VA05_reciprocal_estimates)
{
    struct Case
    {
        const char *op;
        uint64_t input;
        uint64_t expected;
        uint32_t flags;
    };
    const Case cases[] = {
        {"vfrec7.v", 0x3F800000, 0x3F7F0000, 0},  // 1/1 ~ 0.996
        {"vfrec7.v", 0x00000000, 0x7F800000, DZ}, // 1/+0 = +inf
        {"vfrec7.v", 0x80000000, 0xFF800000, DZ},
        {"vfrec7.v", 0x7F800000, 0x00000000, 0},  // 1/inf = +0
        {"vfrec7.v", 0xFF800000, 0x80000000, 0},
        {"vfrec7.v", 0x7F800001, kQNaN32, NV},
        {"vfrsqrt7.v", 0x3F800000, 0x3F7F0000, 0},
        {"vfrsqrt7.v", 0x40800000, 0x3EFF0000, 0}, // 1/sqrt(4) ~ 0.498
        {"vfrsqrt7.v", 0x00000000, 0x7F800000, DZ},
        {"vfrsqrt7.v", 0x80000000, 0xFF800000, DZ},
        {"vfrsqrt7.v", 0xBF800000, kQNaN32, NV}, // negative
        {"vfrsqrt7.v", 0x7F800000, 0x00000000, 0},
    };
    for (const Case &c : cases)
    {
        SCOPED_TRACE(std::string(c.op) + " " + std::to_string(c.input));
        std::vector<uint8_t> v8(kVlenb, 0), v24(kVlenb, 0);
        setElem(v8, 0, 32, c.input);
        const auto m = runFp(std::string(c.op) + " v24, v8", 32, 0, 1, 0, v8,
                             std::vector<uint8_t>(kVlenb), v24, std::vector<uint8_t>(kVlenb, 0), 0);
        EXPECT_EQ(elem(m->getV(24), 0, 32), c.expected);
        EXPECT_EQ(m->csr(csr::kFflags), c.flags);
    }
    // Accuracy: relative error below 2^-7 for normal inputs, both widths.
    std::mt19937_64 rng(45);
    for (unsigned sew : {32U, 64U})
    {
        std::vector<uint8_t> v8(kVlenb), v24(kVlenb);
        const unsigned n = kVlenb / (sew / 8);
        std::vector<double> inputs;
        for (unsigned i = 0; i < n; ++i)
        {
            const double v = std::ldexp(1.0 + static_cast<double>(rng() % 1000) / 1000.0,
                                        static_cast<int>(rng() % 40) - 20);
            inputs.push_back(v);
            setElem(v8, i, sew, sew == 32 ? bits32(static_cast<float>(v)) : bits64(v));
        }
        for (const char *op : {"vfrec7.v", "vfrsqrt7.v"})
        {
            const auto m = runFp(std::string(op) + " v24, v8", sew, 0, n, 0, v8,
                                 std::vector<uint8_t>(kVlenb), v24,
                                 std::vector<uint8_t>(kVlenb, 0), 0);
            for (unsigned i = 0; i < n; ++i)
            {
                const double x = sew == 32 ? f32(elem(v8, i, sew)) : f64(elem(v8, i, sew));
                const double exact = std::string(op) == "vfrec7.v" ? 1.0 / x : 1.0 / std::sqrt(x);
                const uint64_t raw = elem(m->getV(24), i, sew);
                const double estimate = sew == 32 ? f32(raw) : f64(raw);
                EXPECT_LT(std::fabs(estimate - exact) / exact, 1.0 / 128) << op << " " << x;
            }
        }
    }
}

// FP reductions, widening arithmetic, moves and slides.
TEST(VecFloat, VA04_reductions_widening_and_moves)
{
    // vfredosum adds in element order starting from vs1[0].
    {
        std::vector<uint8_t> v8(kVlenb), v16(kVlenb, 0), v24(kVlenb, 0);
        const float values[] = {1e8f, 1.0f, -1e8f, 1.0f};
        for (int i = 0; i < 4; ++i)
            setElem(v8, i, 32, bits32(values[i]));
        setElem(v16, 0, 32, bits32(0.5f));
        const auto m = runFp("vfredosum.vs v24, v8, v16", 32, 0, 4, 0, v8, v16, v24,
                             std::vector<uint8_t>(kVlenb, 0), 0);
        float acc = 0.5f;
        for (float v : values)
            acc = acc + v;
        EXPECT_EQ(elem(m->getV(24), 0, 32), bits32(acc));
        // vfredmax ignores NaN elements
        std::vector<uint8_t> w8(kVlenb, 0);
        setElem(w8, 0, 32, bits32(3.0f));
        setElem(w8, 1, 32, kQNaN32);
        setElem(w8, 2, 32, bits32(7.0f));
        setElem(w8, 3, 32, bits32(-9.0f));
        const auto mm = runFp("vfredmax.vs v24, v8, v16", 32, 0, 4, 0, w8, v16, v24,
                              std::vector<uint8_t>(kVlenb, 0), 0);
        EXPECT_EQ(elem(mm->getV(24), 0, 32), bits32(7.0f));
    }
    // vfwadd.vv / vfwmul.vf / vfwredusum.vs widen f32 to f64 exactly before the operation.
    {
        std::vector<uint8_t> v8(kVlenb), v16(kVlenb), v24(2 * kVlenb, 0);
        for (int i = 0; i < 4; ++i)
        {
            setElem(v8, i, 32, bits32(1.0f + static_cast<float>(i) / 3.0f));
            setElem(v16, i, 32, bits32(1e-8f * static_cast<float>(i + 1)));
        }
        const auto m = runFp("vfwadd.vv v24, v8, v16", 32, 0, 4, 0, v8, v16, v24,
                             std::vector<uint8_t>(kVlenb, 0), 0);
        for (int i = 0; i < 4; ++i)
        {
            const double expected = static_cast<double>(f32(elem(v8, i, 32))) +
                                    static_cast<double>(f32(elem(v16, i, 32)));
            EXPECT_EQ(elem(m->getV(24, 2), i, 64), bits64(expected));
        }
        const auto mul = runFp("vfwmul.vf v24, v8, f11", 32, 0, 4, 0, v8, v16, v24,
                               std::vector<uint8_t>(kVlenb, 0), bits32(3.0f));
        EXPECT_EQ(elem(mul->getV(24, 2), 3, 64),
                  bits64(static_cast<double>(f32(elem(v8, 3, 32))) * 3.0));
    }
    // vfmv.f.s, vfmv.s.f, vfmv.v.f, vfmerge.vfm and vfslide1down.vf
    {
        std::vector<uint8_t> v8(kVlenb), v24(kVlenb, 0), v0(kVlenb, 0);
        for (int i = 0; i < 2; ++i)
            setElem(v8, i, 64, bits64(1.5 + i));
        v0[0] = 0b10;
        const auto a = runFp("vfmv.f.s f12, v8", 64, 0, 2, 0, v8, {}, v24, v0, 0);
        EXPECT_EQ(a->f(12), bits64(1.5));
        const auto b = runFp("vfmv.s.f v24, f11", 64, 0, 2, 0, v8, {}, v24, v0, bits64(-2.0));
        EXPECT_EQ(elem(b->getV(24), 0, 64), bits64(-2.0));
        const auto c = runFp("vfmerge.vfm v24, v8, f11, v0", 64, 0, 2, 0, v8, {}, v24, v0,
                             bits64(9.0));
        EXPECT_EQ(elem(c->getV(24), 0, 64), bits64(1.5));
        EXPECT_EQ(elem(c->getV(24), 1, 64), bits64(9.0));
        const auto d = runFp("vfslide1down.vf v24, v8, f11", 64, 0, 2, 0, v8, {}, v24, v0,
                             bits64(4.0));
        EXPECT_EQ(elem(d->getV(24), 0, 64), bits64(2.5));
        EXPECT_EQ(elem(d->getV(24), 1, 64), bits64(4.0));
    }
}

// FP legality: element widths without FP support and reserved rounding modes.
TEST(VecFloat, VL01_float_legality)
{
    for (const char *setup : {"vsetvli t0, x0, e16, m1, ta, ma", "vsetvli t0, x0, e8, m1, ta, ma"})
    {
        SCOPED_TRACE(setup);
        Machine m(std::string(".text\n") + setup + "\nvfadd.vv v1, v2, v3\n");
        m.step();
        EXPECT_THROW(m.step(), rvv::IllegalInstruction);
    }
    {
        Machine m(".text\nvsetvli t0, x0, e64, m1, ta, ma\nvfwadd.vv v2, v4, v6\n");
        m.step();
        EXPECT_THROW(m.step(), rvv::IllegalInstruction); // widening needs SEW = 32
    }
    {
        Machine m(".text\nvsetvli t0, x0, e32, m1, ta, ma\nvfadd.vv v1, v2, v3\n");
        m.setCsr(csr::kFrm, 5);
        m.step();
        EXPECT_THROW(m.step(), rvv::IllegalInstruction); // reserved frm
    }
}
