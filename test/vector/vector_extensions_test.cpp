/**
 * @file vector_extensions_test.cpp
 * @brief Configurable parts of the vector unit: VLEN (VC-03), the agnostic-ones policy
 * (VC-04), the Zve* profiles (VX-04), and the Zvbb/Zvkb (VX-01), Zvbc (VX-02) and Zvfh (VX-03)
 * extensions.
 */
#include "vector_test_support.h"

#include "config/config.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_unit.h"
#include "processor/vector/vtype.h"

#include <cmath>
#include <cstring>
#include <functional>

using namespace Kites;
using namespace vtest;
namespace csr = Kites::csr_address;

namespace
{

/// Restores the global vector configuration when a test ends.
class ConfigGuard
{
  public:
    ConfigGuard() : saved_(rvv::config()) {}
    ~ConfigGuard() { rvv::config() = saved_; }
    ConfigGuard(const ConfigGuard &) = delete;
    ConfigGuard &operator=(const ConfigGuard &) = delete;

  private:
    rvv::VectorConfig saved_;
};

std::string vset(unsigned sew, const char *lmul = "m1", const char *policy = "ta, ma")
{
    return "vsetvli x0, x10, e" + std::to_string(sew) + ", " + lmul + ", " + policy + "\n";
}

/// Runs `.text` + body on a fresh single-cycle machine; v8, v16, v24 and v0 are preset.
std::unique_ptr<Machine> run(const std::string &body, uint64_t avl,
                             const std::vector<uint8_t> &v8, const std::vector<uint8_t> &v16,
                             const std::vector<uint8_t> &v24 = {},
                             const std::vector<uint8_t> &v0 = {}, uint64_t x11 = 0)
{
    auto m = std::make_unique<Machine>(".text\n" + body);
    m->setX(10, avl);
    m->setX(11, x11);
    if (!v8.empty())
        m->setV(8, v8);
    if (!v16.empty())
        m->setV(16, v16);
    if (!v24.empty())
        m->setV(24, v24);
    if (!v0.empty())
        m->setV(0, v0);
    m->run();
    return m;
}

bool throwsIllegal(const std::string &body, uint64_t avl = 4)
{
    try
    {
        run(body, avl, {}, {});
    }
    catch (const rvv::IllegalInstruction &)
    {
        return true;
    }
    return false;
}

// Independent reference implementations of the Zvbb/Zvbc element operations.
uint64_t refBrev(uint64_t v, unsigned bits)
{
    uint64_t r = 0;
    for (unsigned i = 0; i < bits; ++i)
        if ((v >> i) & 1U)
            r |= 1ULL << (bits - 1 - i);
    return r;
}

uint64_t refRotl(uint64_t v, unsigned s, unsigned sew)
{
    s %= sew;
    v = truncate(v, sew);
    return s == 0 ? v : truncate((v << s) | (v >> (sew - s)), sew);
}

uint64_t refClmul(uint64_t a, uint64_t b, bool high)
{
    uint64_t lo = 0;
    uint64_t hi = 0;
    for (unsigned i = 0; i < 64; ++i)
    {
        if ((b >> i) & 1U)
        {
            lo ^= a << i;
            if (i != 0)
                hi ^= a >> (64 - i);
        }
    }
    return high ? hi : lo;
}

} // namespace

// ---------------------------------------------------------------------------------- Zvbb

TEST(VecExtensions, VX01_zvbb_element_operations)
{
    ConfigGuard guard;
    rvv::config().zvbb = true;
    rvv::config().zvkb = true;
    std::mt19937_64 rng(0x2bb);

    struct Case
    {
        const char *op;
        std::function<uint64_t(uint64_t a, uint64_t b, unsigned sew)> ref;
    };
    const Case binary[] = {
        {"vandn", [](uint64_t a, uint64_t b, unsigned sew) { return truncate(a & ~b, sew); }},
        {"vrol", [](uint64_t a, uint64_t b, unsigned sew) { return refRotl(a, b % sew, sew); }},
        {"vror",
         [](uint64_t a, uint64_t b, unsigned sew) { return refRotl(a, (sew - b % sew) % sew, sew); }},
    };
    const Case unary[] = {
        {"vbrev", [](uint64_t a, uint64_t, unsigned sew) { return refBrev(a, sew); }},
        {"vbrev8",
         [](uint64_t a, uint64_t, unsigned sew)
         {
             uint64_t r = 0;
             for (unsigned b = 0; b < sew / 8; ++b)
                 r |= refBrev((a >> (8 * b)) & 0xFF, 8) << (8 * b);
             return r;
         }},
        {"vrev8",
         [](uint64_t a, uint64_t, unsigned sew)
         {
             uint64_t r = 0;
             for (unsigned b = 0; b < sew / 8; ++b)
                 r = (r << 8) | ((a >> (8 * b)) & 0xFF);
             return r;
         }},
        {"vclz",
         [](uint64_t a, uint64_t, unsigned sew) -> uint64_t
         {
             a = truncate(a, sew);
             return a == 0 ? sew : static_cast<uint64_t>(__builtin_clzll(a)) - (64 - sew);
         }},
        {"vctz",
         [](uint64_t a, uint64_t, unsigned sew) -> uint64_t
         {
             a = truncate(a, sew);
             return a == 0 ? sew : static_cast<uint64_t>(__builtin_ctzll(a));
         }},
        {"vcpop",
         [](uint64_t a, uint64_t, unsigned sew) -> uint64_t
         { return static_cast<uint64_t>(__builtin_popcountll(truncate(a, sew))); }},
    };

    for (unsigned sew : {8U, 16U, 32U, 64U})
    {
        const uint64_t vl = kVlen / sew;
        const auto vs2 = randomBytes(rng, kVlenb);
        const auto vs1 = randomBytes(rng, kVlenb);
        const auto old = randomBytes(rng, kVlenb);
        const auto mask = randomBytes(rng, kVlenb);
        const uint64_t scalar = rng();
        const unsigned imm = static_cast<unsigned>(rng() % 64);
        for (bool masked : {false, true})
        {
            const std::string vm = masked ? ", v0.t" : "";
            auto expectAll = [&](const std::string &insn, const std::function<uint64_t(uint64_t, size_t)> &ref)
            {
                auto m = run(vset(sew) + insn + vm + "\n", vl, vs2, vs1, old, mask, scalar);
                const auto vd = m->getV(24);
                for (size_t i = 0; i < vl; ++i)
                {
                    const uint64_t expected =
                        masked && !bit(mask, i) ? elem(old, i, sew) : ref(elem(vs2, i, sew), i);
                    ASSERT_EQ(elem(vd, i, sew), expected) << insn << vm << " e" << sew << " [" << i << "]";
                }
            };
            for (const Case &c : binary)
            {
                expectAll(std::string(c.op) + ".vv v24, v8, v16",
                          [&](uint64_t a, size_t i) { return c.ref(a, elem(vs1, i, sew), sew); });
                expectAll(std::string(c.op) + ".vx v24, v8, a1",
                          [&](uint64_t a, size_t) { return c.ref(a, truncate(scalar, sew), sew); });
            }
            expectAll("vror.vi v24, v8, " + std::to_string(imm),
                      [&](uint64_t a, size_t) { return refRotl(a, (sew - imm % sew) % sew, sew); });
            for (const Case &c : unary)
                expectAll(std::string(c.op) + ".v v24, v8",
                          [&](uint64_t a, size_t) { return c.ref(a, 0, sew); });
        }
    }
}

TEST(VecExtensions, VX01_zvbb_widening_shift)
{
    ConfigGuard guard;
    rvv::config().zvbb = true;
    std::mt19937_64 rng(0x5011);
    for (unsigned sew : {8U, 16U, 32U})
    {
        const uint64_t vl = kVlen / sew;
        const auto vs2 = randomBytes(rng, kVlenb);
        const auto vs1 = randomBytes(rng, kVlenb);
        const uint64_t scalar = rng();
        for (const std::string form : {"vv", "vx", "vi"})
        {
            const std::string src = form == "vv" ? "v16" : form == "vx" ? "a1" : "13";
            auto m = run(vset(sew) + "vwsll." + form + " v24, v8, " + src + "\n", vl, vs2, vs1, {},
                         {}, scalar);
            const auto vd = m->getV(24, 2);
            for (size_t i = 0; i < vl; ++i)
            {
                const uint64_t amount = form == "vv" ? elem(vs1, i, sew)
                                        : form == "vx" ? scalar
                                                       : 13;
                const uint64_t expected =
                    truncate(elem(vs2, i, sew) << (amount % (2 * sew)), 2 * sew);
                ASSERT_EQ(elem(vd, i, 2 * sew), expected) << form << " e" << sew << " [" << i << "]";
            }
        }
    }
}

TEST(VecExtensions, VX02_zvbc_carryless_multiply)
{
    ConfigGuard guard;
    rvv::config().zvbc = true;
    std::mt19937_64 rng(0xbc);
    const auto vs2 = randomBytes(rng, kVlenb);
    const auto vs1 = randomBytes(rng, kVlenb);
    const uint64_t scalar = rng();
    for (bool high : {false, true})
    {
        const std::string op = high ? "vclmulh" : "vclmul";
        auto vv = run(vset(64) + op + ".vv v24, v8, v16\n", 2, vs2, vs1);
        auto vx = run(vset(64) + op + ".vx v24, v8, a1\n", 2, vs2, vs1, {}, {}, scalar);
        for (size_t i = 0; i < 2; ++i)
        {
            EXPECT_EQ(elem(vv->getV(24), i, 64), refClmul(elem(vs2, i, 64), elem(vs1, i, 64), high));
            EXPECT_EQ(elem(vx->getV(24), i, 64), refClmul(elem(vs2, i, 64), scalar, high));
        }
    }
    EXPECT_EQ(refClmul(0x8000000000000001ULL, 3, false), 0x8000000000000003ULL); // a ^ (a << 1)
    EXPECT_EQ(refClmul(0x8000000000000001ULL, 3, true), 1U);
    EXPECT_EQ(refClmul(0x8000000000000000ULL, 2, true), 1U);
    EXPECT_TRUE(throwsIllegal(vset(32) + "vclmul.vv v24, v8, v16\n"));
}

TEST(VecExtensions, VX01_disabled_extensions_are_illegal)
{
    ConfigGuard guard;
    rvv::config().zvbb = rvv::config().zvkb = rvv::config().zvbc = false;
    EXPECT_TRUE(throwsIllegal(vset(32) + "vandn.vv v24, v8, v16\n"));
    EXPECT_TRUE(throwsIllegal(vset(64) + "vclmul.vv v24, v8, v16\n"));

    rvv::config().zvkb = true; // Zvkb alone: the rotate/and-not/byte-reverse subset only
    EXPECT_FALSE(throwsIllegal(vset(32) + "vandn.vv v24, v8, v16\n"));
    EXPECT_FALSE(throwsIllegal(vset(32) + "vror.vi v24, v8, 33\n"));
    EXPECT_TRUE(throwsIllegal(vset(32) + "vclz.v v24, v8\n"));
    EXPECT_TRUE(throwsIllegal(vset(16) + "vwsll.vi v24, v8, 3\n"));
}

// ---------------------------------------------------------------------------------- Zve*

TEST(VecExtensions, VX04_zve_profiles_restrict_sew_and_float)
{
    ConfigGuard guard;
    auto setProfile = [](rvv::Profile p)
    {
        rvv::config().profile = p;
        rvv::config().zvfh = false;
    };
    auto vill = [](const std::string &vtype)
    {
        auto m = run("vsetvli t0, x10, " + vtype + "\n", 4, {}, {});
        return (m->csr(csr::kVtype) >> 63) != 0;
    };

    setProfile(rvv::Profile::Zve32x);
    EXPECT_TRUE(vill("e64, m1, ta, ma"));
    EXPECT_TRUE(vill("e8, mf8, ta, ma")); // LMUL >= SEW_MIN / ELEN = 1/4
    EXPECT_FALSE(vill("e32, m1, ta, ma"));
    EXPECT_FALSE(vill("e8, mf4, ta, ma"));
    EXPECT_TRUE(throwsIllegal(vset(32) + "vle64.v v8, (x0)\n"));
    EXPECT_TRUE(throwsIllegal(vset(32) + "vfadd.vv v24, v8, v16\n"));
    EXPECT_TRUE(throwsIllegal(vset(16) + "vwadd.vv v24, v8, v16\n") == false);
    EXPECT_TRUE(throwsIllegal(vset(32) + "vwadd.vv v24, v8, v16\n")); // 2*SEW > ELEN

    setProfile(rvv::Profile::Zve32f);
    EXPECT_FALSE(throwsIllegal(vset(32) + "vfadd.vv v24, v8, v16\n"));

    setProfile(rvv::Profile::Zve64x);
    EXPECT_TRUE(throwsIllegal(vset(32) + "vfadd.vv v24, v8, v16\n"));
    EXPECT_TRUE(throwsIllegal(vset(64) + "vmulh.vv v24, v8, v16\n"));
    EXPECT_TRUE(throwsIllegal(vset(64) + "vsmul.vv v24, v8, v16\n"));
    EXPECT_FALSE(throwsIllegal(vset(32) + "vmulh.vv v24, v8, v16\n"));
    EXPECT_FALSE(throwsIllegal(vset(64) + "vmul.vv v24, v8, v16\n"));

    setProfile(rvv::Profile::Zve64f);
    EXPECT_FALSE(throwsIllegal(vset(32) + "vfadd.vv v24, v8, v16\n"));
    EXPECT_TRUE(throwsIllegal(vset(64) + "vfadd.vv v24, v8, v16\n"));
    EXPECT_TRUE(throwsIllegal(vset(32) + "vfwadd.vv v24, v8, v16\n"));

    setProfile(rvv::Profile::Zve64d);
    EXPECT_FALSE(throwsIllegal(vset(64) + "vfadd.vv v24, v8, v16\n"));
    EXPECT_TRUE(throwsIllegal(vset(64) + "vmulh.vv v24, v8, v16\n"));

    setProfile(rvv::Profile::V);
    EXPECT_FALSE(throwsIllegal(vset(64) + "vmulh.vv v24, v8, v16\n"));
    EXPECT_TRUE(throwsIllegal(vset(16) + "vfadd.vv v24, v8, v16\n")); // no Zvfh
}

TEST(VecExtensions, VX04_extension_list_parsing)
{
    rvv::VectorConfig cfg;
    EXPECT_EQ(rvv::applyExtensionList(cfg, "zve64f, zvfh, zvbb"), "");
    EXPECT_EQ(cfg.profile, rvv::Profile::Zve64f);
    EXPECT_TRUE(cfg.zvfh && cfg.zvbb && cfg.zvkb && !cfg.zvbc);
    EXPECT_EQ(cfg.describe(), "zve64f, zvfh, zvbb");
    EXPECT_NE(rvv::applyExtensionList(cfg, "zve32x,zvbc"), ""); // Zvbc needs ELEN = 64
    EXPECT_NE(rvv::applyExtensionList(cfg, "zve64x,zvfh"), ""); // Zvfh needs F
    EXPECT_NE(rvv::applyExtensionList(cfg, "v,zve64d"), "");
    EXPECT_NE(rvv::applyExtensionList(cfg, "zvxyz"), "");
    EXPECT_EQ(cfg.profile, rvv::Profile::Zve64f); // unchanged after errors
    EXPECT_EQ(rvv::applyExtensionList(cfg, "v"), "");
    EXPECT_FALSE(cfg.zvfh || cfg.zvbb || cfg.zvkb || cfg.zvbc);
    EXPECT_TRUE(rvv::isSupportedVlen(256));
    EXPECT_FALSE(rvv::isSupportedVlen(64));
    EXPECT_FALSE(rvv::isSupportedVlen(384));
    EXPECT_FALSE(rvv::isSupportedVlen(2048));
}

// ---------------------------------------------------------------------------------- Zvfh

namespace
{
uint16_t h(double v)
{
#if defined(__FLT16_MANT_DIG__)
    const _Float16 x = static_cast<_Float16>(v);
    uint16_t bits;
    std::memcpy(&bits, &x, 2);
    return bits;
#else
    (void)v;
    return 0;
#endif
}

std::vector<uint8_t> halves(std::initializer_list<uint16_t> values)
{
    std::vector<uint8_t> out(kVlenb, 0);
    size_t i = 0;
    for (uint16_t v : values)
        setElem(out, i++, 16, v);
    return out;
}
} // namespace

TEST(VecExtensions, VX03_zvfh_known_values_and_flags)
{
    ConfigGuard guard;
    rvv::config().zvfh = true;
    // 1 + 2 = 3; 65504 + 16 ties to even (odd significand) and overflows; min subnormal * 0.5
    // ties to 0 and underflows; sNaN + 1 is invalid.
    const auto a = halves({0x3C00, 0x7BFF, 0x0001, 0x7C01, 0x4100, 0xC100, 0x4400, 0x3C01});
    const auto b = halves({0x4000, 0x4C00, 0x3800, 0x3C00, 0x0000, 0x0000, 0x0000, 0x3C01});
    auto add = run(vset(16) + "vfadd.vv v24, v8, v16\n", 4, a, b);
    EXPECT_EQ(elem(add->getV(24), 0, 16), 0x4200U);
    EXPECT_EQ(elem(add->getV(24), 1, 16), 0x7C00U);
    auto mul = run(vset(16) + "vfmul.vv v24, v8, v16\n", 3, a, b);
    EXPECT_EQ(elem(mul->getV(24), 2, 16), 0x0000U);
    EXPECT_EQ(mul->csr(0x001) & 0x2U, 0x2U); // underflow
    EXPECT_EQ(add->csr(0x001), 0x15U);        // NV | OF | NX

    // Round toward zero: (1 + 2^-10)^2 = 1 + 2^-9 + 2^-20 -> 1 + 2^-9.
    auto rtz = run("csrwi frm, 1\n" + vset(16) + "vfmul.vv v24, v8, v8\n", 8, a, b);
    EXPECT_EQ(elem(rtz->getV(24), 7, 16), 0x3C02U);

    auto sqrt = run(vset(16) + "vfsqrt.v v24, v8\n", 8, a, b);
    EXPECT_EQ(elem(sqrt->getV(24), 6, 16), 0x4000U);

    // Conversions: f16 <-> f32 and f16 -> integer (ties to even).
    auto widen = run(vset(16, "mf2") + "vfwcvt.f.f.v v24, v8\n", 4, halves({0x3555, 0xFC00}), {});
    EXPECT_EQ(elem(widen->getV(24), 0, 32), 0x3EAAA000U);
    EXPECT_EQ(elem(widen->getV(24), 1, 32), 0xFF800000U);
    std::vector<uint8_t> singles(kVlenb, 0);
    setElem(singles, 0, 32, 0x3EAAAAABU);
    setElem(singles, 1, 32, 0x47800000U); // 65536 overflows half
    auto narrow = run(vset(16, "mf2") + "vfncvt.f.f.w v24, v8\n", 2, singles, {});
    EXPECT_EQ(elem(narrow->getV(24), 0, 16), 0x3555U);
    EXPECT_EQ(elem(narrow->getV(24), 1, 16), 0x7C00U);
    auto toInt = run(vset(16) + "vfcvt.x.f.v v24, v8\n", 8, a, b);
    EXPECT_EQ(elem(toInt->getV(24), 4, 16), 2U);
    EXPECT_EQ(elem(toInt->getV(24), 5, 16), 0xFFFEU);
    auto wideAdd = run(vset(16, "mf2") + "vfwadd.vv v24, v8, v16\n", 1, halves({0x3C00}),
                       halves({0x3C00}));
    EXPECT_EQ(elem(wideAdd->getV(24), 0, 32), 0x40000000U);

    rvv::config().zvfh = false;
    EXPECT_TRUE(throwsIllegal(vset(16) + "vfadd.vv v24, v8, v16\n"));
}

#if defined(__FLT16_MANT_DIG__)
// Random half operands (including subnormals, infinities and NaNs) against the compiler's
// _Float16 arithmetic, which is correctly rounded (round to nearest even).
TEST(VecExtensions, VX03_zvfh_matches_host_half_arithmetic)
{
    ConfigGuard guard;
    rvv::config().zvfh = true;
    std::mt19937_64 rng(0xf16);
    for (const char *op : {"vfadd", "vfsub", "vfmul", "vfdiv"})
    {
        for (int round = 0; round < 40; ++round)
        {
            std::vector<uint8_t> a(kVlenb), b(kVlenb);
            for (size_t i = 0; i < 8; ++i)
            {
                setElem(a, i, 16, rng() & 0xFFFF);
                setElem(b, i, 16, (rng() % 4 == 0) ? elem(a, i, 16) ^ 0x8000 : rng() & 0xFFFF);
            }
            auto m = run(vset(16) + std::string(op) + ".vv v24, v8, v16\n", 8, a, b);
            for (size_t i = 0; i < 8; ++i)
            {
                _Float16 x, y;
                const uint16_t ab = static_cast<uint16_t>(elem(a, i, 16));
                const uint16_t bb = static_cast<uint16_t>(elem(b, i, 16));
                std::memcpy(&x, &ab, 2);
                std::memcpy(&y, &bb, 2);
                const _Float16 r = std::string(op) == "vfadd"   ? static_cast<_Float16>(x + y)
                                   : std::string(op) == "vfsub" ? static_cast<_Float16>(x - y)
                                   : std::string(op) == "vfmul" ? static_cast<_Float16>(x * y)
                                                                : static_cast<_Float16>(x / y);
                uint16_t expected;
                std::memcpy(&expected, &r, 2);
                if ((expected & 0x7C00) == 0x7C00 && (expected & 0x3FF) != 0)
                    expected = 0x7E00; // RISC-V returns the canonical NaN
                ASSERT_EQ(elem(m->getV(24), i, 16), expected)
                    << op << " " << std::hex << ab << " " << bb;
            }
        }
    }
    EXPECT_EQ(h(1.0), 0x3C00U);
}
#endif

// ---------------------------------------------------------------------------------- VLEN

TEST(VecExtensions, VC03_vlen_sets_vlenb_and_vlmax)
{
    ConfigGuard guard;
    for (unsigned vlen : {128U, 256U, 512U, 1024U})
    {
        rvv::config().vlen = vlen;
        auto m = run("vsetvli t0, x10, e8, m8, ta, ma\ncsrr t1, vlenb\n"
                     "vsetvli t2, x10, e32, mf2, ta, ma\n",
                     100000, {}, {});
        EXPECT_EQ(m->x(5), vlen) << vlen;       // VLMAX at e8, m8 = 8 * VLEN / 8
        EXPECT_EQ(m->x(6), vlen / 8) << vlen;   // vlenb
        EXPECT_EQ(m->x(7), vlen / 64) << vlen;  // VLEN / 32 / 2
        EXPECT_EQ(m->vm->registers_.ReadVr(31).size(), vlen / 8) << vlen;
    }
}

TEST(VecExtensions, VC03_examples_give_the_same_results_at_every_vlen)
{
    ConfigGuard guard;
    const char *examples[] = {"vvadd_i32.s",   "memcpy.s",     "dot_product.s",    "masked_abs.s",
                              "gather_scatter.s", "prefix_sum.s", "matmul_i32.s", "saxpy_f32.s",
                              "strlen.s",      "rgb_to_grey.s", "fir_q15.s"};
    for (const char *name : examples)
    {
        std::vector<uint64_t> reference;
        for (unsigned vlen : {128U, 256U, 512U, 1024U})
        {
            rvv::config().vlen = vlen;
            QuietOutput quiet;
            setupVmStateDirectory();
            RVSSProcessor vm;
            vm.LoadProgram(assemble(std::string("../examples/vector/") + name));
            for (int steps = 0; vm.program_counter_ < vm.program_size_ && steps < 100000; ++steps)
                vm.Step();
            // Data memory and the result registers (a0, s0) and vxsat; other registers hold
            // loop state such as vl, which depends on VLEN.
            std::vector<uint64_t> state = {vm.registers_.ReadGpr(10), vm.registers_.ReadGpr(8),
                                           vm.registers_.ReadCsr(csr::kVxsat)};
            const uint64_t base = vm_config::config.getDataSectionStart();
            for (uint64_t addr = 0; addr < 1024; addr += 8)
                state.push_back(vm.memory_controller_.readDoubleWord(base + addr));
            if (vlen == 128)
                reference = state;
            else
                EXPECT_TRUE(state == reference) << name << " differs at VLEN = " << vlen;
        }
    }
}

// ---------------------------------------------------------------------------------- agnostic

TEST(VecExtensions, VC04_agnostic_elements_filled_with_ones)
{
    ConfigGuard guard;
    const std::vector<uint8_t> a(kVlenb, 0x01), b(kVlenb, 0x02), old(kVlenb, 0x55);
    std::vector<uint8_t> mask(kVlenb, 0);
    mask[0] = 0b01; // element 0 active, element 1 inactive
    const uint32_t ones = 0xFFFFFFFFU;

    for (bool fill : {false, true})
    {
        rvv::config().agnosticFillOnes = fill;
        auto agnostic = run(vset(32, "m1", "ta, ma") + "vadd.vv v24, v8, v16, v0.t\n", 2, a, b, old, mask);
        auto undisturbed =
            run(vset(32, "m1", "tu, mu") + "vadd.vv v24, v8, v16, v0.t\n", 2, a, b, old, mask);
        const auto vd = agnostic->getV(24);
        EXPECT_EQ(elem(vd, 0, 32), 0x03030303U);
        for (size_t i = 1; i < 4; ++i)
        {
            EXPECT_EQ(elem(vd, i, 32), fill ? ones : 0x55555555U) << "fill=" << fill << " [" << i << "]";
            EXPECT_EQ(elem(undisturbed->getV(24), i, 32), 0x55555555U) << i;
        }

        // Mask results are always tail-agnostic: bits from vl up become ones when filling.
        auto cmp = run(vset(32, "m1", "tu, mu") + "vmseq.vv v24, v8, v8\n", 2, a, b, old);
        EXPECT_EQ(cmp->getV(24)[0], fill ? 0xFFU : 0x57U); // bits 0,1 = 1; then old 0b0101...
        EXPECT_EQ(cmp->getV(24)[1], fill ? 0xFFU : 0x55U);

        // Loads with ta.
        auto load = run(vset(32, "m1", "ta, mu") + "vle32.v v24, (a1)\n", 1, {}, {}, old, {},
                        vm_config::config.getDataSectionStart());
        EXPECT_EQ(elem(load->getV(24), 3, 32), fill ? ones : 0x55555555U);
    }
}
