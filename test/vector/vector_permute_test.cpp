/**
 * @file vector_permute_test.cpp
 * @brief Reductions, mask instructions and permutations against simple reference loops
 * (VA-01, VP-01).
 */
#include "vector_reference.h"
#include "vector_test_support.h"

using namespace Kites;
using namespace vtest;

namespace
{

struct Case
{
    unsigned sew;
    int lmul;
    bool masked;
    uint64_t avl;
    uint64_t vl;
    uint64_t vlmax;
    std::vector<uint8_t> v8, v16, v24, v0;
    uint64_t scalar;
};

Case makeCase(std::mt19937_64 &rng, unsigned sew, int lmul, bool masked)
{
    Case c;
    c.sew = sew;
    c.lmul = lmul;
    c.masked = masked;
    c.vlmax = vlmaxOf(sew, lmul);
    c.avl = rng() % 3 == 0 ? c.vlmax : rng() % (c.vlmax + 1);
    c.vl = std::min(c.avl, c.vlmax);
    c.v8 = randomBytes(rng, 8 * kVlenb);
    c.v16 = randomBytes(rng, 8 * kVlenb);
    c.v24 = randomBytes(rng, 8 * kVlenb);
    c.v0 = randomBytes(rng, kVlenb);
    c.scalar = rng();
    return c;
}

std::unique_ptr<Machine> run(const Case &c, const std::string &body)
{
    auto m = std::make_unique<Machine>(".text\nvsetvli x0, x10, e" + std::to_string(c.sew) + ", " +
                                       lmulName(c.lmul) + ", tu, mu\n" + body +
                                       (c.masked ? ", v0.t" : "") + "\n");
    m->setV(0, c.v0);
    m->setV(8, c.v8);
    m->setV(16, c.v16);
    m->setV(24, c.v24);
    m->setX(10, c.avl);
    m->setX(11, c.scalar);
    m->run();
    return m;
}

bool active(const Case &c, size_t i)
{
    return !c.masked || bit(c.v0, i);
}

std::string name(const std::string &op, const Case &c)
{
    return op + " e" + std::to_string(c.sew) + " " + lmulName(c.lmul) +
           (c.masked ? " masked" : "") + " vl=" + std::to_string(c.vl);
}

} // namespace

TEST(VecPermute, VA01_integer_reductions)
{
    std::mt19937_64 rng(21);
    const std::vector<std::string> ops = {"vredsum", "vredand",  "vredor",   "vredxor", "vredminu",
                                          "vredmin", "vredmaxu", "vredmax", "vwredsumu", "vwredsum"};
    for (const std::string &op : ops)
    {
        const bool widen = op[1] == 'w';
        for (const auto &[sew, lmul] : legalConfigs())
        {
            if (widen && sew > 32)
                continue;
            for (bool masked : {false, true})
            {
                const Case c = makeCase(rng, sew, lmul, masked);
                SCOPED_TRACE(name(op, c));
                const auto m = run(c, op + ".vs v24, v8, v16");
                std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + kVlenb);
                if (c.vl > 0)
                {
                    const unsigned accBits = widen ? 2 * sew : sew;
                    ref::i128 acc = widen && op == "vwredsum" ? ref::sval(elem(c.v16, 0, accBits), accBits)
                                                              : elem(c.v16, 0, accBits);
                    for (size_t i = 0; i < c.vl; ++i)
                    {
                        if (!active(c, i))
                            continue;
                        const uint64_t x = elem(c.v8, i, sew);
                        const ref::i128 sx = ref::sval(x, sew);
                        const ref::i128 sacc = ref::sval(static_cast<uint64_t>(acc), accBits);
                        const ref::i128 uacc = static_cast<ref::i128>(
                            static_cast<uint64_t>(acc) & static_cast<uint64_t>(ref::umask(accBits)));
                        if (op == "vredsum" || op == "vwredsumu") acc = uacc + x;
                        else if (op == "vwredsum") acc = sacc + sx;
                        else if (op == "vredand") acc = uacc & x;
                        else if (op == "vredor") acc = uacc | x;
                        else if (op == "vredxor") acc = uacc ^ x;
                        else if (op == "vredminu") acc = std::min<ref::i128>(uacc, x);
                        else if (op == "vredmaxu") acc = std::max<ref::i128>(uacc, x);
                        else if (op == "vredmin") acc = std::min(sacc, sx);
                        else acc = std::max(sacc, sx);
                    }
                    setElem(expected, 0, accBits, static_cast<uint64_t>(acc));
                }
                ASSERT_EQ(m->getV(24), expected);
            }
        }
    }
}

TEST(VecPermute, VA01_mask_logical_and_population)
{
    std::mt19937_64 rng(22);
    for (const auto &[sew, lmul] : legalConfigs())
    {
        for (const char *op : {"vmand", "vmnand", "vmandn", "vmxor", "vmor", "vmnor", "vmorn",
                               "vmxnor"})
        {
            Case c = makeCase(rng, sew, lmul, false);
            SCOPED_TRACE(name(op, c));
            const auto m = run(c, std::string(op) + ".mm v24, v8, v16");
            std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + kVlenb);
            for (size_t i = 0; i < c.vl; ++i)
            {
                const bool a = bit(c.v8, i);
                const bool b = bit(c.v16, i);
                const std::string o = op;
                bool r = false;
                if (o == "vmand") r = a && b;
                else if (o == "vmnand") r = !(a && b);
                else if (o == "vmandn") r = a && !b;
                else if (o == "vmxor") r = a != b;
                else if (o == "vmor") r = a || b;
                else if (o == "vmnor") r = !(a || b);
                else if (o == "vmorn") r = a || !b;
                else r = a == b;
                setBit(expected, i, r);
            }
            ASSERT_EQ(m->getV(24), expected);
        }
        for (bool masked : {false, true})
        {
            const Case c = makeCase(rng, sew, lmul, masked);
            SCOPED_TRACE(name("vcpop/vfirst", c));
            int64_t count = 0;
            int64_t first = -1;
            for (size_t i = 0; i < c.vl; ++i)
            {
                if (active(c, i) && bit(c.v8, i))
                {
                    if (first < 0)
                        first = static_cast<int64_t>(i);
                    ++count;
                }
            }
            EXPECT_EQ(run(c, "vcpop.m x12, v8")->x(12), static_cast<uint64_t>(count));
            EXPECT_EQ(run(c, "vfirst.m x12, v8")->x(12), static_cast<uint64_t>(first));
        }
    }
}

TEST(VecPermute, VA01_set_first_iota_and_id)
{
    std::mt19937_64 rng(23);
    for (const auto &[sew, lmul] : legalConfigs())
    {
        for (bool masked : {false, true})
        {
            for (const char *op : {"vmsbf", "vmsif", "vmsof"})
            {
                Case c = makeCase(rng, sew, lmul, masked);
                if (rng() % 3 == 0) // sometimes no bit is set
                    c.v8 = std::vector<uint8_t>(8 * kVlenb, 0);
                SCOPED_TRACE(name(op, c));
                const auto m = run(c, std::string(op) + ".m v24, v8");
                std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + kVlenb);
                bool found = false;
                for (size_t i = 0; i < c.vl; ++i)
                {
                    if (!active(c, i))
                        continue;
                    const bool b = bit(c.v8, i);
                    const std::string o = op;
                    bool r = false;
                    if (!found && b)
                    {
                        found = true;
                        r = o != "vmsbf";
                    }
                    else if (!found)
                    {
                        r = o != "vmsof";
                    }
                    setBit(expected, i, r);
                }
                ASSERT_EQ(m->getV(24), expected);
            }

            const unsigned regs = groupRegs(lmul);
            {
                const Case c = makeCase(rng, sew, lmul, masked);
                SCOPED_TRACE(name("viota.m", c));
                const auto m = run(c, "viota.m v24, v8");
                std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + regs * kVlenb);
                uint64_t sum = 0;
                for (size_t i = 0; i < c.vl; ++i)
                {
                    if (!active(c, i))
                        continue;
                    setElem(expected, i, sew, sum);
                    sum += bit(c.v8, i) ? 1 : 0;
                }
                ASSERT_EQ(m->getV(24, regs), expected);
            }
            {
                const Case c = makeCase(rng, sew, lmul, masked);
                SCOPED_TRACE(name("vid.v", c));
                const auto m = run(c, "vid.v v24");
                std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + regs * kVlenb);
                for (size_t i = 0; i < c.vl; ++i)
                    if (active(c, i))
                        setElem(expected, i, sew, i);
                ASSERT_EQ(m->getV(24, regs), expected);
            }
        }
    }
}

TEST(VecPermute, VA01_scalar_moves)
{
    std::mt19937_64 rng(24);
    for (const auto &[sew, lmul] : legalConfigs())
    {
        Case c = makeCase(rng, sew, lmul, false);
        SCOPED_TRACE(name("vmv.x.s / vmv.s.x", c));
        // vmv.x.s sign-extends element 0 and ignores vl.
        EXPECT_EQ(run(c, "vmv.x.s x12, v8")->x(12),
                  static_cast<uint64_t>(sext(elem(c.v8, 0, sew), sew)));
        const auto m = run(c, "vmv.s.x v24, x11");
        std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + kVlenb);
        if (c.vl > 0)
            setElem(expected, 0, sew, c.scalar);
        ASSERT_EQ(m->getV(24), expected);
    }
}

TEST(VecPermute, VA01_slides)
{
    std::mt19937_64 rng(25);
    for (const auto &[sew, lmul] : legalConfigs())
    {
        for (bool masked : {false, true})
        {
            const unsigned regs = groupRegs(lmul);
            for (const std::string op :
                 {"vslideup.vx", "vslideup.vi", "vslidedown.vx", "vslidedown.vi", "vslide1up.vx",
                  "vslide1down.vx"})
            {
                Case c = makeCase(rng, sew, lmul, masked);
                const bool immediate = op.back() == 'i';
                const uint64_t offset = immediate ? rng() % 32 : rng() % (c.vlmax + 3);
                if (!immediate && op.find('1') == std::string::npos)
                    c.scalar = offset;
                SCOPED_TRACE(name(op, c) + " offset=" + std::to_string(offset));
                const auto m = run(c, op + " v24, v8, " +
                                          (immediate ? std::to_string(offset) : std::string("x11")));
                std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + regs * kVlenb);
                for (size_t i = 0; i < c.vl; ++i)
                {
                    if (!active(c, i))
                        continue;
                    if (op.rfind("vslideup", 0) == 0)
                    {
                        if (i >= offset)
                            setElem(expected, i, sew, elem(c.v8, i - offset, sew));
                    }
                    else if (op.rfind("vslidedown", 0) == 0)
                    {
                        setElem(expected, i, sew,
                                i + offset < c.vlmax ? elem(c.v8, i + offset, sew) : 0);
                    }
                    else if (op == "vslide1up.vx")
                    {
                        setElem(expected, i, sew, i == 0 ? c.scalar : elem(c.v8, i - 1, sew));
                    }
                    else
                    {
                        setElem(expected, i, sew, i + 1 == c.vl ? c.scalar : elem(c.v8, i + 1, sew));
                    }
                }
                ASSERT_EQ(m->getV(24, regs), expected);
            }
        }
    }
}

TEST(VecPermute, VA01_gather_and_compress)
{
    std::mt19937_64 rng(26);
    for (const auto &[sew, lmul] : legalConfigs())
    {
        const unsigned regs = groupRegs(lmul);
        for (bool masked : {false, true})
        {
            for (const std::string form : {"vv", "vx", "vi", "ei16"})
            {
                Case c = makeCase(rng, sew, lmul, masked);
                const unsigned indexEew = form == "ei16" ? 16 : sew;
                if (form == "ei16")
                {
                    int sewLog2 = 3;
                    while ((1U << sewLog2) < sew)
                        ++sewLog2;
                    const int emul = 4 - sewLog2 + lmul;
                    if (emul < -3 || emul > 3)
                        continue;
                }
                // Indices mostly in range, some out of range (which yield 0).
                for (size_t i = 0; i < 8 * kVlenb * 8 / indexEew; ++i)
                    setElem(c.v16, i, indexEew,
                            truncate(rng() % (c.vlmax + c.vlmax / 4 + 2), indexEew));
                const uint64_t imm = rng() % 32;
                c.scalar = rng() % 2 ? rng() % (c.vlmax + 1) : rng();
                const std::string body =
                    form == "ei16" ? "vrgatherei16.vv v24, v8, v16"
                    : form == "vv" ? "vrgather.vv v24, v8, v16"
                    : form == "vx" ? "vrgather.vx v24, v8, x11"
                                   : "vrgather.vi v24, v8, " + std::to_string(imm);
                SCOPED_TRACE(name(body, c));
                const auto m = run(c, body);
                std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + regs * kVlenb);
                for (size_t i = 0; i < c.vl; ++i)
                {
                    if (!active(c, i))
                        continue;
                    const uint64_t index = form == "vx" ? c.scalar
                                           : form == "vi" ? imm
                                                          : elem(c.v16, i, indexEew);
                    setElem(expected, i, sew, index < c.vlmax ? elem(c.v8, index, sew) : 0);
                }
                ASSERT_EQ(m->getV(24, regs), expected);
            }
        }
        {
            Case c = makeCase(rng, sew, lmul, false);
            SCOPED_TRACE(name("vcompress.vm", c));
            const auto m = run(c, "vcompress.vm v24, v8, v16");
            std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + regs * kVlenb);
            size_t packed = 0;
            for (size_t i = 0; i < c.vl; ++i)
                if (bit(c.v16, i))
                    setElem(expected, packed++, sew, elem(c.v8, i, sew));
            ASSERT_EQ(m->getV(24, regs), expected);
        }
    }
}

TEST(VecPermute, VA01_whole_register_moves)
{
    std::mt19937_64 rng(27);
    for (unsigned count : {1U, 2U, 4U, 8U})
    {
        Case c = makeCase(rng, 32, 0, false);
        SCOPED_TRACE("vmv" + std::to_string(count) + "r.v");
        const auto m = run(c, "vmv" + std::to_string(count) + "r.v v24, v8");
        std::vector<uint8_t> expected(c.v24.begin(), c.v24.begin() + 8 * kVlenb);
        std::copy(c.v8.begin(), c.v8.begin() + count * kVlenb, expected.begin());
        ASSERT_EQ(m->getV(24, 8), expected);
    }
}
