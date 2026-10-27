/**
 * @file vector_integer_test.cpp
 * @brief Integer and fixed-point vector arithmetic checked against the reference model for every
 * legal SEW/LMUL pair, operand form, masking mode and a spread of vl values (VA-01, VA-02,
 * VA-03, VP-01).
 *
 * Register layout used by every case: vs2 = v8, vs1 = v16, vd = v24, mask = v0. These groups are
 * aligned for LMUL up to 8 and never overlap.
 */
#include "vector_reference.h"
#include "vector_test_support.h"

#include "processor/vector/vector_isa.h"

using namespace Kites;
using namespace vtest;
namespace csr = Kites::csr_address;

namespace
{

struct Inputs
{
    std::vector<uint8_t> vs2, vs1, vd, mask;
    uint64_t scalar = 0;
    int64_t imm = 0;
    uint64_t avl = 0;
    unsigned xrm = 0;
};

Inputs randomInputs(std::mt19937_64 &rng, unsigned vs2Regs, unsigned vs1Regs, unsigned vdRegs,
                    uint64_t vlmax, bool signedImm)
{
    Inputs in;
    in.vs2 = randomBytes(rng, vs2Regs * kVlenb);
    in.vs1 = randomBytes(rng, vs1Regs * kVlenb);
    in.vd = randomBytes(rng, vdRegs * kVlenb);
    in.mask = randomBytes(rng, kVlenb);
    in.scalar = rng();
    in.imm = signedImm ? static_cast<int64_t>(rng() % 32) - 16 : static_cast<int64_t>(rng() % 32);
    switch (rng() % 4)
    {
    case 0:
        in.avl = vlmax;
        break;
    case 1:
        in.avl = vlmax + 3; // vl = VLMAX
        break;
    default:
        in.avl = rng() % (vlmax + 1); // includes 0
        break;
    }
    in.xrm = static_cast<unsigned>(rng() % 4);
    return in;
}

/// Second operand of element i for the given form.
uint64_t operand1(const std::string &form, const Inputs &in, size_t i, unsigned eew)
{
    if (form == "vv" || form == "wv" || form == "vvm" || form == "vs")
        return elem(in.vs1, i, eew);
    if (form == "vx" || form == "wx" || form == "vxm")
        return truncate(in.scalar, eew);
    return truncate(static_cast<uint64_t>(in.imm), eew); // vi, wi, vim
}

std::string operand1Text(const std::string &form, const Inputs &in)
{
    if (form[1] == 'v' || form == "vs")
        return "v16";
    if (form[1] == 'x')
        return "x11";
    return std::to_string(in.imm);
}

/// Loads the inputs into a machine, runs it and returns it.
std::unique_ptr<Machine> runCase(const std::string &body, unsigned sew, int lmul, const Inputs &in)
{
    auto m = std::make_unique<Machine>(".text\nvsetvli x0, x10, e" + std::to_string(sew) + ", " +
                                       lmulName(lmul) + ", tu, mu\n" + body + "\n");
    m->setV(0, in.mask);
    m->setV(8, in.vs2);
    m->setV(16, in.vs1);
    m->setV(24, in.vd);
    m->setX(10, in.avl);
    m->setX(11, in.scalar);
    m->setCsr(csr::kVxrm, in.xrm);
    m->run();
    return m;
}

std::string trace(const std::string &mnemonic, unsigned sew, int lmul, bool masked,
                  const Inputs &in)
{
    return mnemonic + " e" + std::to_string(sew) + " " + lmulName(lmul) +
           (masked ? " masked" : "") + " avl=" + std::to_string(in.avl) +
           " xrm=" + std::to_string(in.xrm);
}

std::vector<std::string> formsOf(const std::string &op, std::initializer_list<const char *> forms)
{
    std::vector<std::string> out;
    for (const char *f : forms)
        if (rvv::findByMnemonic(op + "." + f) != nullptr)
            out.emplace_back(f);
    return out;
}

bool usesSignedImmediate(const std::string &mnemonic)
{
    const rvv::InsnInfo *info = rvv::findByMnemonic(mnemonic);
    return info != nullptr && info->hasField(rvv::kSimm5);
}

} // namespace

// VA-01, VA-02, VA-03, VP-01: single-width integer and fixed-point operations.
TEST(VecInteger, VA01_single_width_operations)
{
    const std::vector<std::string> ops = {
        "vadd",  "vsub",   "vrsub",  "vand",   "vor",    "vxor",   "vsll",  "vsrl",
        "vsra",  "vminu",  "vmin",   "vmaxu",  "vmax",   "vmul",   "vmulh", "vmulhu",
        "vmulhsu", "vdivu", "vdiv",  "vremu",  "vrem",   "vsaddu", "vsadd", "vssubu",
        "vssub", "vaaddu", "vaadd",  "vasubu", "vasub",  "vsmul",  "vssrl", "vssra"};
    std::mt19937_64 rng(20261027);
    size_t cases = 0;
    for (const std::string &op : ops)
    {
        for (const std::string &form : formsOf(op, {"vv", "vx", "vi"}))
        {
            const std::string mnemonic = op + "." + form;
            for (const auto &[sew, lmul] : legalConfigs())
            {
                for (bool masked : {false, true})
                {
                    const unsigned regs = groupRegs(lmul);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const Inputs in =
                        randomInputs(rng, regs, regs, regs, vlmax, usesSignedImmediate(mnemonic));
                    SCOPED_TRACE(trace(mnemonic, sew, lmul, masked, in));
                    const auto m = runCase(mnemonic + " v24, v8, " + operand1Text(form, in) +
                                               (masked ? ", v0.t" : ""),
                                           sew, lmul, in);
                    const uint64_t vl = std::min(in.avl, vlmax);
                    std::vector<uint8_t> expected = in.vd;
                    bool saturated = false;
                    for (size_t i = 0; i < vl; ++i)
                    {
                        if (masked && !bit(in.mask, i))
                            continue;
                        const ref::Result r = ref::binary(op, elem(in.vs2, i, sew),
                                                          operand1(form, in, i, sew), sew, in.xrm);
                        setElem(expected, i, sew, r.value);
                        saturated |= r.saturated;
                    }
                    ASSERT_EQ(m->getV(24, regs), expected);
                    ASSERT_EQ(m->csr(csr::kVxsat), saturated ? 1U : 0U);
                    ++cases;
                }
            }
        }
    }
    EXPECT_GT(cases, 2500U);
}

// VA-02: division edge cases are exercised explicitly (not left to random data).
TEST(VecInteger, VA02_division_edge_cases)
{
    for (unsigned sew : {8U, 16U, 32U, 64U})
    {
        SCOPED_TRACE("e" + std::to_string(sew));
        const uint64_t minValue = 1ULL << (sew - 1);
        const uint64_t minusOne = truncate(~0ULL, sew);
        Inputs in;
        in.vs2 = std::vector<uint8_t>(kVlenb, 0);
        in.vs1 = std::vector<uint8_t>(kVlenb, 0);
        in.vd = std::vector<uint8_t>(kVlenb, 0);
        in.mask = std::vector<uint8_t>(kVlenb, 0);
        in.avl = 2;
        setElem(in.vs2, 0, sew, 77);       // 77 / 0
        setElem(in.vs1, 0, sew, 0);
        setElem(in.vs2, 1, sew, minValue); // MIN / -1
        setElem(in.vs1, 1, sew, minusOne);
        for (const char *op : {"vdiv", "vdivu", "vrem", "vremu"})
        {
            SCOPED_TRACE(op);
            const auto m = runCase(std::string(op) + ".vv v24, v8, v16", sew, 0, in);
            const auto out = m->getV(24);
            const std::string name = op;
            if (name == "vdiv" || name == "vdivu")
                EXPECT_EQ(elem(out, 0, sew), minusOne);
            else
                EXPECT_EQ(elem(out, 0, sew), 77U);
            if (name == "vdiv")
                EXPECT_EQ(elem(out, 1, sew), minValue);
            if (name == "vrem")
                EXPECT_EQ(elem(out, 1, sew), 0U);
        }
    }
}

// Multiply-add family: vd = f(vd, vs1/rs1, vs2).
TEST(VecInteger, VA01_multiply_add)
{
    std::mt19937_64 rng(11);
    for (const char *op : {"vmacc", "vnmsac", "vmadd", "vnmsub"})
    {
        for (const std::string &form : formsOf(op, {"vv", "vx"}))
        {
            for (const auto &[sew, lmul] : legalConfigs())
            {
                for (bool masked : {false, true})
                {
                    const unsigned regs = groupRegs(lmul);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const Inputs in = randomInputs(rng, regs, regs, regs, vlmax, true);
                    const std::string mnemonic = std::string(op) + "." + form;
                    SCOPED_TRACE(trace(mnemonic, sew, lmul, masked, in));
                    const auto m = runCase(mnemonic + " v24, " + operand1Text(form, in) +
                                               ", v8" + (masked ? ", v0.t" : ""),
                                           sew, lmul, in);
                    std::vector<uint8_t> expected = in.vd;
                    for (size_t i = 0; i < std::min(in.avl, vlmax); ++i)
                    {
                        if (masked && !bit(in.mask, i))
                            continue;
                        const ref::u128 d = elem(in.vd, i, sew);
                        const ref::u128 b = operand1(form, in, i, sew);
                        const ref::u128 a = elem(in.vs2, i, sew);
                        const std::string name = op;
                        ref::u128 r = 0;
                        if (name == "vmacc")
                            r = b * a + d;
                        else if (name == "vnmsac")
                            r = d - b * a;
                        else if (name == "vmadd")
                            r = b * d + a;
                        else
                            r = a - b * d;
                        setElem(expected, i, sew, static_cast<uint64_t>(r));
                    }
                    ASSERT_EQ(m->getV(24, regs), expected);
                }
            }
        }
    }
}

// Widening arithmetic: 2*SEW results, including the .wv/.wx forms with a wide vs2.
TEST(VecInteger, VA01_widening)
{
    std::mt19937_64 rng(12);
    const std::vector<std::string> ops = {"vwaddu", "vwadd",  "vwsubu",   "vwsub",
                                          "vwmulu", "vwmul",  "vwmulsu",  "vwmaccu",
                                          "vwmacc", "vwmaccsu", "vwmaccus"};
    for (const std::string &op : ops)
    {
        const bool macc = op.rfind("vwmacc", 0) == 0;
        for (const std::string &form : formsOf(op, {"vv", "vx", "wv", "wx"}))
        {
            const bool wideA = form[0] == 'w';
            for (const auto &[sew, lmul] : legalConfigs())
            {
                if (sew > 32 || lmul > 2)
                    continue;
                for (bool masked : {false, true})
                {
                    const unsigned wide = 2 * sew;
                    const unsigned narrowRegs = groupRegs(lmul);
                    const unsigned wideRegs = groupRegs(lmul + 1);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const Inputs in = randomInputs(rng, wideA ? wideRegs : narrowRegs, narrowRegs,
                                                   wideRegs, vlmax, true);
                    const std::string mnemonic = op + "." + form;
                    SCOPED_TRACE(trace(mnemonic, sew, lmul, masked, in));
                    const std::string src1 = operand1Text(form, in);
                    const std::string body =
                        macc ? mnemonic + " v24, " + src1 + ", v8" : mnemonic + " v24, v8, " + src1;
                    const auto m = runCase(body + (masked ? ", v0.t" : ""), sew, lmul, in);
                    std::vector<uint8_t> expected = in.vd;
                    for (size_t i = 0; i < std::min(in.avl, vlmax); ++i)
                    {
                        if (masked && !bit(in.mask, i))
                            continue;
                        const uint64_t a = elem(in.vs2, i, wideA ? wide : sew);
                        const uint64_t b = operand1(form, in, i, sew);
                        const ref::i128 sa = wideA ? ref::sval(a, wide) : ref::sval(a, sew);
                        const ref::i128 ua = a;
                        const ref::i128 sb = ref::sval(b, sew);
                        const ref::i128 ub = b;
                        const ref::i128 d = ref::sval(elem(in.vd, i, wide), wide);
                        ref::i128 r = 0;
                        if (op == "vwaddu") r = ua + ub;
                        else if (op == "vwadd") r = sa + sb;
                        else if (op == "vwsubu") r = ua - ub;
                        else if (op == "vwsub") r = sa - sb;
                        else if (op == "vwmulu") r = ua * ub;
                        else if (op == "vwmul") r = sa * sb;
                        else if (op == "vwmulsu") r = sa * ub;
                        else if (op == "vwmaccu") r = d + ub * ua;
                        else if (op == "vwmacc") r = d + sb * sa;
                        else if (op == "vwmaccsu") r = d + sb * ua;
                        else r = d + ub * sa; // vwmaccus
                        setElem(expected, i, wide, static_cast<uint64_t>(r));
                    }
                    ASSERT_EQ(m->getV(24, wideRegs), expected);
                }
            }
        }
    }
}

// Narrowing shifts and clips (fixed point): vd (SEW) from a 2*SEW vs2.
TEST(VecInteger, VA01_VA03_narrowing)
{
    std::mt19937_64 rng(13);
    for (const char *opName : {"vnsrl", "vnsra", "vnclipu", "vnclip"})
    {
        const std::string op = opName;
        for (const std::string &form : formsOf(op, {"wv", "wx", "wi"}))
        {
            for (const auto &[sew, lmul] : legalConfigs())
            {
                if (sew > 32 || lmul > 2)
                    continue;
                for (bool masked : {false, true})
                {
                    const unsigned wide = 2 * sew;
                    const unsigned regs = groupRegs(lmul);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const Inputs in =
                        randomInputs(rng, groupRegs(lmul + 1), regs, regs, vlmax, false);
                    const std::string mnemonic = op + "." + form;
                    SCOPED_TRACE(trace(mnemonic, sew, lmul, masked, in));
                    const auto m = runCase(mnemonic + " v24, v8, " + operand1Text(form, in) +
                                               (masked ? ", v0.t" : ""),
                                           sew, lmul, in);
                    std::vector<uint8_t> expected = in.vd;
                    bool saturated = false;
                    for (size_t i = 0; i < std::min(in.avl, vlmax); ++i)
                    {
                        if (masked && !bit(in.mask, i))
                            continue;
                        const uint64_t a = elem(in.vs2, i, wide);
                        const unsigned shift =
                            static_cast<unsigned>(operand1(form, in, i, sew) % wide);
                        ref::i128 r = 0;
                        if (op == "vnsrl")
                            r = static_cast<ref::i128>(a) >> shift;
                        else if (op == "vnsra")
                            r = ref::sval(a, wide) >> shift;
                        else if (op == "vnclipu")
                        {
                            r = ref::roundShift(static_cast<ref::i128>(a), shift, in.xrm);
                            if (r > static_cast<ref::i128>(ref::umask(sew)))
                            {
                                r = static_cast<ref::i128>(ref::umask(sew));
                                saturated = true;
                            }
                        }
                        else
                        {
                            r = ref::roundShift(ref::sval(a, wide), shift, in.xrm);
                            const ref::i128 hi = (ref::i128{1} << (sew - 1)) - 1;
                            const ref::i128 lo = -(ref::i128{1} << (sew - 1));
                            if (r > hi || r < lo)
                            {
                                r = r > hi ? hi : lo;
                                saturated = true;
                            }
                        }
                        setElem(expected, i, sew, static_cast<uint64_t>(r));
                    }
                    ASSERT_EQ(m->getV(24, regs), expected);
                    ASSERT_EQ(m->csr(csr::kVxsat), saturated ? 1U : 0U);
                }
            }
        }
    }
}

// Integer extension vzext/vsext.vf2/vf4/vf8.
TEST(VecInteger, VA01_extension)
{
    std::mt19937_64 rng(14);
    for (const char *op : {"vzext", "vsext"})
    {
        for (unsigned factor : {2U, 4U, 8U})
        {
            for (const auto &[sew, lmul] : legalConfigs())
            {
                const unsigned srcEew = sew / factor;
                const int factorLog2 = factor == 2 ? 1 : factor == 4 ? 2 : 3;
                if (srcEew < 8 || lmul - factorLog2 < -3)
                    continue;
                for (bool masked : {false, true})
                {
                    const unsigned regs = groupRegs(lmul);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const Inputs in = randomInputs(rng, regs, regs, regs, vlmax, true);
                    const std::string mnemonic = std::string(op) + ".vf" + std::to_string(factor);
                    SCOPED_TRACE(trace(mnemonic, sew, lmul, masked, in));
                    const auto m =
                        runCase(mnemonic + " v24, v8" + (masked ? ", v0.t" : ""), sew, lmul, in);
                    std::vector<uint8_t> expected = in.vd;
                    for (size_t i = 0; i < std::min(in.avl, vlmax); ++i)
                    {
                        if (masked && !bit(in.mask, i))
                            continue;
                        const uint64_t v = elem(in.vs2, i, srcEew);
                        setElem(expected, i, sew,
                                std::string(op) == "vsext" ? static_cast<uint64_t>(sext(v, srcEew))
                                                           : v);
                    }
                    ASSERT_EQ(m->getV(24, regs), expected);
                }
            }
        }
    }
}

// Add/subtract with carry/borrow, including mask-producing vmadc/vmsbc.
TEST(VecInteger, VA01_carry_and_borrow)
{
    std::mt19937_64 rng(15);
    for (const char *opName : {"vadc", "vsbc", "vmadc", "vmsbc"})
    {
        const std::string op = opName;
        const bool maskDest = op[1] == 'm';
        for (const std::string &form : formsOf(op, {"vvm", "vxm", "vim", "vv", "vx", "vi"}))
        {
            const bool carryIn = form.size() == 3;
            for (const auto &[sew, lmul] : legalConfigs())
            {
                const unsigned regs = groupRegs(lmul);
                const uint64_t vlmax = vlmaxOf(sew, lmul);
                const Inputs in = randomInputs(rng, regs, regs, regs, vlmax, true);
                const std::string mnemonic = op + "." + form;
                SCOPED_TRACE(trace(mnemonic, sew, lmul, false, in));
                const auto m = runCase(mnemonic + " v24, v8, " + operand1Text(form, in) +
                                           (carryIn ? ", v0" : ""),
                                       sew, lmul, in);
                std::vector<uint8_t> expected = in.vd;
                for (size_t i = 0; i < std::min(in.avl, vlmax); ++i)
                {
                    const ref::i128 a = elem(in.vs2, i, sew);
                    const ref::i128 b = operand1(form, in, i, sew);
                    const ref::i128 c = carryIn && bit(in.mask, i) ? 1 : 0;
                    const bool subtract = op == "vsbc" || op == "vmsbc";
                    const ref::i128 r = subtract ? a - b - c : a + b + c;
                    if (maskDest)
                        setBit(expected, i,
                               subtract ? r < 0 : r > static_cast<ref::i128>(ref::umask(sew)));
                    else
                        setElem(expected, i, sew, static_cast<uint64_t>(r));
                }
                ASSERT_EQ(m->getV(24, regs), expected);
            }
        }
    }
}

// Integer compares produce masks; inactive and tail bits are left undisturbed.
TEST(VecInteger, VA01_compares)
{
    std::mt19937_64 rng(16);
    const std::vector<std::string> ops = {"vmseq", "vmsne",  "vmsltu", "vmslt",
                                          "vmsleu", "vmsle", "vmsgtu", "vmsgt"};
    for (const std::string &op : ops)
    {
        for (const std::string &form : formsOf(op, {"vv", "vx", "vi"}))
        {
            for (const auto &[sew, lmul] : legalConfigs())
            {
                for (bool masked : {false, true})
                {
                    const unsigned regs = groupRegs(lmul);
                    const uint64_t vlmax = vlmaxOf(sew, lmul);
                    const std::string mnemonic = op + "." + form;
                    Inputs in =
                        randomInputs(rng, regs, regs, 1, vlmax, usesSignedImmediate(mnemonic));
                    // Make equality likely for some elements.
                    for (size_t i = 0; i < vlmax; i += 3)
                        setElem(in.vs1, i, sew, elem(in.vs2, i, sew));
                    SCOPED_TRACE(trace(mnemonic, sew, lmul, masked, in));
                    const auto m = runCase(mnemonic + " v24, v8, " + operand1Text(form, in) +
                                               (masked ? ", v0.t" : ""),
                                           sew, lmul, in);
                    std::vector<uint8_t> expected = in.vd;
                    for (size_t i = 0; i < std::min(in.avl, vlmax); ++i)
                    {
                        if (masked && !bit(in.mask, i))
                            continue;
                        const uint64_t ua = elem(in.vs2, i, sew);
                        const uint64_t ub = operand1(form, in, i, sew);
                        const int64_t sa = sext(ua, sew);
                        const int64_t sb = sext(ub, sew);
                        bool r = false;
                        if (op == "vmseq") r = ua == ub;
                        else if (op == "vmsne") r = ua != ub;
                        else if (op == "vmsltu") r = ua < ub;
                        else if (op == "vmslt") r = sa < sb;
                        else if (op == "vmsleu") r = ua <= ub;
                        else if (op == "vmsle") r = sa <= sb;
                        else if (op == "vmsgtu") r = ua > ub;
                        else r = sa > sb;
                        setBit(expected, i, r);
                    }
                    ASSERT_EQ(m->getV(24), expected);
                }
            }
        }
    }
}

// vmerge and vmv.v.*: v0 selects between vs2 and operand 1 (not a mask).
TEST(VecInteger, VA01_merge_and_move)
{
    std::mt19937_64 rng(17);
    for (const auto &[sew, lmul] : legalConfigs())
    {
        const unsigned regs = groupRegs(lmul);
        const uint64_t vlmax = vlmaxOf(sew, lmul);
        for (const std::string form : {"vvm", "vxm", "vim"})
        {
            const Inputs in = randomInputs(rng, regs, regs, regs, vlmax, true);
            SCOPED_TRACE(trace("vmerge." + form, sew, lmul, false, in));
            const auto m =
                runCase("vmerge." + form + " v24, v8, " + operand1Text(form, in) + ", v0", sew,
                        lmul, in);
            std::vector<uint8_t> expected = in.vd;
            for (size_t i = 0; i < std::min(in.avl, vlmax); ++i)
                setElem(expected, i, sew,
                        bit(in.mask, i) ? operand1(form, in, i, sew) : elem(in.vs2, i, sew));
            ASSERT_EQ(m->getV(24, regs), expected);
        }
        for (const std::string form : {"vv", "vx", "vi"})
        {
            const Inputs in = randomInputs(rng, regs, regs, regs, vlmax, true);
            const std::string src = form == "vv" ? "v16" : operand1Text(form, in);
            SCOPED_TRACE(trace("vmv.v." + form.substr(1), sew, lmul, false, in));
            const auto m = runCase("vmv.v." + form.substr(1) + " v24, " + src, sew, lmul, in);
            std::vector<uint8_t> expected = in.vd;
            for (size_t i = 0; i < std::min(in.avl, vlmax); ++i)
                setElem(expected, i, sew, operand1(form, in, i, sew));
            ASSERT_EQ(m->getV(24, regs), expected);
        }
    }
}
