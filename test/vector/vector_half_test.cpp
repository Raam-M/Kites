/**
 * @file vector_half_test.cpp
 * @brief Half precision (Zvfh, VX-03) under every rounding mode, against the compiler's
 * _Float16 conversions as an independent oracle, plus element formatting (OB-04).
 */
#include "vector_test_support.h"

#include "processor/vector/vector_config.h"
#include "processor/vector/vector_format.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

using namespace Kites;
using namespace vtest;

namespace
{

class ZvfhGuard
{
  public:
    ZvfhGuard() : saved_(rvv::config()) { rvv::config().zvfh = true; }
    ~ZvfhGuard() { rvv::config() = saved_; }

  private:
    rvv::VectorConfig saved_;
};

std::vector<uint8_t> halves(std::mt19937_64 &rng, bool finiteOnly)
{
    std::vector<uint8_t> out(kVlenb);
    for (size_t i = 0; i < kVlenb / 2; ++i)
    {
        uint64_t h = rng() & 0xFFFF;
        if (finiteOnly && (h & 0x7C00) == 0x7C00)
            h &= 0xBFFF; // clear the top exponent bit: finite
        setElem(out, i, 16, h);
    }
    return out;
}

#if defined(__FLT16_MANT_DIG__)
const int kHostModes[] = {FE_TONEAREST, FE_TOWARDZERO, FE_DOWNWARD, FE_UPWARD};

/// The value rounded to half by the host under a host rounding mode (hardware conversion).
uint16_t hostToHalf(double v, int mode)
{
    const int saved = std::fegetround();
    std::fesetround(mode);
    volatile double in = v;
    volatile _Float16 h = static_cast<_Float16>(in);
    std::fesetround(saved);
    const _Float16 copy = h;
    uint16_t bits;
    std::memcpy(&bits, &copy, 2);
    if ((bits & 0x7C00) == 0x7C00 && (bits & 0x3FF) != 0)
        bits = 0x7E00; // canonical NaN
    return bits;
}
#endif

} // namespace

#if defined(__FLT16_MANT_DIG__)
// Exact sums and products of halves fit in double, so rounding the double with the hardware's
// double-to-half conversion gives the correctly rounded result in every mode.
TEST(VecHalf, VX03_arithmetic_in_every_rounding_mode)
{
    ZvfhGuard guard;
    std::mt19937_64 rng(0x16);
    const char *modeNames[] = {"rne", "rtz", "rdn", "rup"};
    for (unsigned frm = 0; frm < 4; ++frm)
    {
        for (int round = 0; round < 25; ++round)
        {
            const auto a = halves(rng, true);
            const auto b = halves(rng, true);
            for (const char *op : {"vfadd", "vfsub", "vfmul"})
            {
                auto m = Machine(".text\ncsrwi frm, " + std::to_string(frm) +
                                 "\nvsetivli x0, 8, e16, m1, ta, ma\n" + op + ".vv v24, v8, v16\n");
                m.setV(8, a);
                m.setV(16, b);
                m.run();
                for (size_t i = 0; i < 8; ++i)
                {
                    const double x = rvv::halfToDouble(elem(a, i, 16));
                    const double y = rvv::halfToDouble(elem(b, i, 16));
                    const double exact = std::string(op) == "vfadd"   ? x + y
                                         : std::string(op) == "vfsub" ? x - y
                                                                      : x * y;
                    ASSERT_EQ(elem(m.getV(24), i, 16), hostToHalf(exact, kHostModes[frm]))
                        << op << " " << modeNames[frm] << " " << std::hex << elem(a, i, 16) << " "
                        << elem(b, i, 16);
                }
            }
        }
    }
}

// The product of two halves is exact in double; the fused sum is rounded once to double and
// then to half, which gives the correctly rounded result (design document, half precision).
TEST(VecHalf, VX03_fused_multiply_add_forms)
{
    ZvfhGuard guard;
    std::mt19937_64 rng(0xf3a);
    struct Form
    {
        const char *name;
        double (*value)(double vs1, double vs2, double vd);
    };
    const Form forms[] = {
        {"vfmacc", [](double s1, double v2, double d) { return std::fma(s1, v2, d); }},
        {"vfnmacc", [](double s1, double v2, double d) { return std::fma(-s1, v2, -d); }},
        {"vfmsac", [](double s1, double v2, double d) { return std::fma(s1, v2, -d); }},
        {"vfnmsac", [](double s1, double v2, double d) { return std::fma(-s1, v2, d); }},
        {"vfmadd", [](double s1, double v2, double d) { return std::fma(s1, d, v2); }},
        {"vfnmadd", [](double s1, double v2, double d) { return std::fma(-s1, d, -v2); }},
        {"vfmsub", [](double s1, double v2, double d) { return std::fma(s1, d, -v2); }},
        {"vfnmsub", [](double s1, double v2, double d) { return std::fma(-s1, d, v2); }},
    };
    for (const Form &f : forms)
    {
        for (int round = 0; round < 10; ++round)
        {
            const auto vs1 = halves(rng, true);
            const auto vs2 = halves(rng, true);
            const auto vd = halves(rng, true);
            Machine m(std::string(".text\nvsetivli x0, 8, e16, m1, ta, ma\n") + f.name +
                      ".vv v24, v16, v8\n");
            m.setV(8, vs2);
            m.setV(16, vs1);
            m.setV(24, vd);
            m.run();
            for (size_t i = 0; i < 8; ++i)
            {
                const double expected =
                    f.value(rvv::halfToDouble(elem(vs1, i, 16)), rvv::halfToDouble(elem(vs2, i, 16)),
                            rvv::halfToDouble(elem(vd, i, 16)));
                ASSERT_EQ(elem(m.getV(24), i, 16), hostToHalf(expected, FE_TONEAREST))
                    << f.name << " [" << i << "]";
            }
        }
    }
}

TEST(VecHalf, VX03_integer_to_half_conversions)
{
    ZvfhGuard guard;
    std::mt19937_64 rng(0x12f);
    for (unsigned frm = 0; frm < 4; ++frm)
    {
        const auto ints = randomBytes(rng, kVlenb);
        // int16 -> f16 (same width) and int8 -> f16 (widening), signed and unsigned
        for (const std::string op : {"vfcvt.f.x.v", "vfcvt.f.xu.v", "vfwcvt.f.x.v", "vfwcvt.f.xu.v"})
        {
            const bool widen = op[2] == 'w';
            const bool isSigned = op.find(".x.v") != std::string::npos;
            Machine m(".text\ncsrwi frm, " + std::to_string(frm) + "\nvsetivli x0, 8, " +
                      (widen ? "e8, mf2" : "e16, m1") + ", ta, ma\n" + op + " v24, v8\n");
            m.setV(8, ints);
            m.run();
            for (size_t i = 0; i < 8; ++i)
            {
                const unsigned srcBits = widen ? 8 : 16;
                const uint64_t raw = elem(ints, i, srcBits);
                const double value = isSigned ? static_cast<double>(sext(raw, srcBits))
                                              : static_cast<double>(raw);
                ASSERT_EQ(elem(m.getV(24), i, 16), hostToHalf(value, kHostModes[frm]))
                    << op << " frm " << frm << " " << raw;
            }
        }
    }
}
#endif

TEST(VecHalf, VX03_overflow_and_underflow_flags_by_mode)
{
    ZvfhGuard guard;
    // 65504 + 32 overflows; the result depends on the direction.
    const std::vector<uint8_t> big = [] {
        std::vector<uint8_t> v(kVlenb, 0);
        setElem(v, 0, 16, 0x7BFF); // 65504
        setElem(v, 1, 16, 0xFBFF); // -65504
        return v;
    }();
    const std::vector<uint8_t> step = [] {
        std::vector<uint8_t> v(kVlenb, 0);
        setElem(v, 0, 16, 0x5000); // 32
        setElem(v, 1, 16, 0xD000); // -32
        return v;
    }();
    struct Expect
    {
        unsigned frm;
        uint64_t positive, negative;
    };
    for (const Expect e : {Expect{0, 0x7C00, 0xFC00}, Expect{1, 0x7BFF, 0xFBFF},
                           Expect{2, 0x7BFF, 0xFC00}, Expect{3, 0x7C00, 0xFBFF}})
    {
        Machine m(".text\ncsrwi frm, " + std::to_string(e.frm) +
                  "\nvsetivli x0, 2, e16, m1, ta, ma\nvfadd.vv v24, v8, v16\n");
        m.setV(8, big);
        m.setV(16, step);
        m.run();
        EXPECT_EQ(elem(m.getV(24), 0, 16), e.positive) << "frm " << e.frm;
        EXPECT_EQ(elem(m.getV(24), 1, 16), e.negative) << "frm " << e.frm;
        EXPECT_EQ(m.csr(0x001) & 0x5U, 0x5U) << "OF and NX, frm " << e.frm;
    }
    // Min subnormal * 0.5 rounds up to the min subnormal under RUP: tiny and inexact.
    std::vector<uint8_t> tiny(kVlenb, 0), half(kVlenb, 0);
    setElem(tiny, 0, 16, 0x0001);
    setElem(half, 0, 16, 0x3800);
    Machine m(".text\ncsrwi frm, 3\nvsetivli x0, 1, e16, m1, ta, ma\nvfmul.vv v24, v8, v16\n");
    m.setV(8, tiny);
    m.setV(16, half);
    m.run();
    EXPECT_EQ(elem(m.getV(24), 0, 16), 0x0001U);
    EXPECT_EQ(m.csr(0x001) & 0x3U, 0x3U); // UF and NX
}

TEST(VecHalf, OB04_element_formatting)
{
    EXPECT_EQ(rvv::parseElementFormat("hex"), rvv::ElementFormat::Hex);
    EXPECT_EQ(rvv::parseElementFormat("u"), rvv::ElementFormat::Unsigned);
    EXPECT_EQ(rvv::parseElementFormat("signed"), rvv::ElementFormat::Signed);
    EXPECT_EQ(rvv::parseElementFormat("f"), rvv::ElementFormat::Float);
    EXPECT_FALSE(rvv::parseElementFormat("octal").has_value());

    EXPECT_EQ(rvv::formatElement(0xFF, 8, rvv::ElementFormat::Hex), "0xff");
    EXPECT_EQ(rvv::formatElement(0xFF, 8, rvv::ElementFormat::Unsigned), "255");
    EXPECT_EQ(rvv::formatElement(0xFF, 8, rvv::ElementFormat::Signed), "-1");
    EXPECT_EQ(rvv::formatElement(0x8000000000000000ULL, 64, rvv::ElementFormat::Signed),
              "-9223372036854775808");
    EXPECT_EQ(rvv::formatElement(0x3E00, 16, rvv::ElementFormat::Float), "1.5");
    EXPECT_EQ(rvv::formatElement(0x40490FDB, 32, rvv::ElementFormat::Float), "3.14159274");
    EXPECT_EQ(rvv::formatElement(0x4000000000000000ULL, 64, rvv::ElementFormat::Float), "2");
    EXPECT_EQ(rvv::formatElement(0x7F, 8, rvv::ElementFormat::Float), "0x7f"); // no 8-bit float
    EXPECT_EQ(rvv::formatElement(0x0003, 16, rvv::ElementFormat::Hex), "0x0003");

    EXPECT_EQ(rvv::halfToDouble(0x7C00), INFINITY);
    EXPECT_EQ(rvv::halfToDouble(0xFC00), -INFINITY);
    EXPECT_TRUE(std::isnan(rvv::halfToDouble(0x7E00)));
    EXPECT_EQ(rvv::halfToDouble(0x0001), std::ldexp(1.0, -24));
    EXPECT_EQ(rvv::halfToDouble(0x8000), 0.0);
    EXPECT_TRUE(std::signbit(rvv::halfToDouble(0x8000)));

    const std::vector<uint8_t> bytes = {0x01, 0x00, 0xFF, 0xFF};
    EXPECT_EQ(rvv::splitElements(bytes, 16), (std::vector<uint64_t>{1, 0xFFFF}));
    EXPECT_EQ(rvv::formatRegister(bytes, 16, rvv::ElementFormat::Signed), "[0] 1  [1] -1");
}
