/**
 * @file vector_config_test.cpp
 * @brief vsetvli/vsetivli/vsetvl, vtype handling and the vector CSRs.
 */
#include "vector_test_support.h"

#include "processor/vector/vtype.h"

using namespace Kites;
using namespace vtest;
namespace csr = Kites::csr_address;

namespace
{
uint64_t vtypeOf(unsigned sew, int lmulLog2, bool ta, bool ma)
{
    unsigned vsew = 0;
    while ((8U << vsew) < sew)
        ++vsew;
    const unsigned vlmul = static_cast<unsigned>(lmulLog2 < 0 ? lmulLog2 + 8 : lmulLog2);
    return (uint64_t{ma} << 7) | (uint64_t{ta} << 6) | (vsew << 3) | vlmul;
}
} // namespace

// VS-04: reset state.
TEST(VecConfig, VS04_reset_state)
{
    Machine m(".text\nnop\n");
    EXPECT_EQ(m.csr(csr::kVtype), rvv::kVillBit);
    EXPECT_EQ(m.csr(csr::kVl), 0U);
    EXPECT_EQ(m.csr(csr::kVstart), 0U);
    EXPECT_EQ(m.csr(csr::kVlenb), kVlenb);
    EXPECT_EQ(m.getV(0, 32), std::vector<uint8_t>(32 * kVlenb, 0));
}

// VC-01: vl = min(AVL, VLMAX) for every legal SEW/LMUL and a range of AVLs.
TEST(VecConfig, VC01_vsetvli_sets_vl_and_vtype)
{
    for (const auto &[sew, lmul] : legalConfigs())
    {
        const uint64_t vlmax = vlmaxOf(sew, lmul);
        for (uint64_t avl : {uint64_t{0}, uint64_t{1}, vlmax - 1, vlmax, vlmax + 1, 2 * vlmax,
                             ~uint64_t{0}})
        {
            for (int policy = 0; policy < 4; ++policy)
            {
                const bool ta = (policy & 1) != 0;
                const bool ma = (policy & 2) != 0;
                SCOPED_TRACE("e" + std::to_string(sew) + " " + lmulName(lmul) +
                             " avl=" + std::to_string(avl));
                Machine m(".text\nvsetvli t0, a0, e" + std::to_string(sew) + ", " +
                          lmulName(lmul) + (ta ? ", ta" : ", tu") + (ma ? ", ma" : ", mu") + "\n");
                m.setX(10, avl);
                m.run();
                const uint64_t expected = std::min(avl, vlmax);
                EXPECT_EQ(m.x(5), expected);
                EXPECT_EQ(m.csr(csr::kVl), expected);
                EXPECT_EQ(m.csr(csr::kVtype), vtypeOf(sew, lmul, ta, ma));
            }
        }
    }
}

TEST(VecConfig, VC01_vsetivli_and_vsetvl)
{
    Machine a(".text\nvsetivli t0, 7, e16, m1, ta, ma\n");
    a.run();
    EXPECT_EQ(a.x(5), 7U);
    EXPECT_EQ(a.csr(csr::kVl), 7U);

    Machine b(".text\nvsetivli t0, 31, e64, m1, ta, ma\n"); // VLMAX = 2
    b.run();
    EXPECT_EQ(b.x(5), 2U);

    // vsetvl takes vtype from a register.
    Machine c(".text\nvsetvl t0, a0, a1\n");
    c.setX(10, 100);
    c.setX(11, vtypeOf(8, 2, true, false)); // e8, m4 -> VLMAX 64
    c.run();
    EXPECT_EQ(c.x(5), 64U);
    EXPECT_EQ(c.csr(csr::kVtype), vtypeOf(8, 2, true, false));
}

// VC-02: rs1 = x0 requests VLMAX; rs1 = rd = x0 keeps vl.
TEST(VecConfig, VC02_x0_special_cases)
{
    Machine a(".text\nvsetvli t0, x0, e32, m2, ta, ma\n");
    a.setX(10, 3);
    a.run();
    EXPECT_EQ(a.x(5), 8U);

    // Same SEW/LMUL ratio: vl is kept.
    Machine b(".text\nvsetvli t0, a0, e32, m1, ta, ma\nvsetvli x0, x0, e16, mf2, ta, ma\n");
    b.setX(10, 3);
    b.run();
    EXPECT_EQ(b.csr(csr::kVl), 3U);
    EXPECT_EQ(b.csr(csr::kVtype), vtypeOf(16, -1, true, true));

    // Different ratio would change VLMAX: reserved, so vill is set (D4).
    Machine c(".text\nvsetvli t0, a0, e32, m1, ta, ma\nvsetvli x0, x0, e8, m1, ta, ma\n");
    c.setX(10, 3);
    c.run();
    EXPECT_EQ(c.csr(csr::kVtype), rvv::kVillBit);
    EXPECT_EQ(c.csr(csr::kVl), 0U);
}

// VC-03: unsupported vtype values set vill and vl = 0.
TEST(VecConfig, VC03_unsupported_vtype_sets_vill)
{
    struct Case
    {
        uint64_t vtype;
        const char *why;
    };
    const Case cases[] = {
        {vtypeOf(64, -3, false, false), "SEW 64 with LMUL 1/8"},
        {vtypeOf(32, -2, false, false), "SEW 32 with LMUL 1/4"},
        {vtypeOf(16, -3, false, false), "SEW 16 with LMUL 1/8"},
        {4, "reserved vlmul"},
        {4U << 3, "reserved vsew (SEW 128)"},
        {1U << 8, "reserved bit 8"},
        {rvv::kVillBit, "vill requested"},
    };
    for (const Case &c : cases)
    {
        SCOPED_TRACE(c.why);
        Machine m(".text\nvsetivli t0, 4, e8, m1, ta, ma\nvsetvl t1, a0, a1\n");
        m.setX(10, 4);
        m.setX(11, c.vtype);
        m.run();
        EXPECT_EQ(m.csr(csr::kVtype), rvv::kVillBit);
        EXPECT_EQ(m.csr(csr::kVl), 0U);
        EXPECT_EQ(m.x(6), 0U);
    }
}

// VS-02 / VS-03: CSR names, read-only CSRs and the vcsr composite.
TEST(VecConfig, VS02_VS03_vector_csrs)
{
    Machine m(".text\n"
              "vsetvli t0, a0, e16, m2, ta, ma\n"
              "csrr s1, vl\n"
              "csrr s2, vtype\n"
              "csrr s3, vlenb\n"
              "csrwi vxrm, 3\n"
              "csrwi vxsat, 1\n"
              "csrr s4, vcsr\n"
              "csrwi vcsr, 2\n"
              "csrr s5, vxrm\n"
              "csrr s6, vxsat\n");
    m.setX(10, 5);
    m.run();
    EXPECT_EQ(m.x(9), 5U);
    EXPECT_EQ(m.x(18), vtypeOf(16, 1, true, true));
    EXPECT_EQ(m.x(19), kVlenb);
    EXPECT_EQ(m.x(20), 7U);
    EXPECT_EQ(m.x(21), 1U);
    EXPECT_EQ(m.x(22), 0U);

    for (const char *ro : {"csrw vl, a0", "csrw vtype, a0", "csrw vlenb, a0", "csrrwi a0, vl, 1",
                           "csrrs a0, vl, a1"})
    {
        SCOPED_TRACE(ro);
        Machine bad(std::string(".text\n") + ro + "\n");
        bad.setX(11, 1);
        EXPECT_THROW(bad.run(), std::runtime_error);
        EXPECT_EQ(bad.vm->program_counter_, 0U); // PC stays on the faulting instruction
    }
}

// VP-02: vstart is reset to 0 by every vector instruction.
TEST(VecConfig, VP02_vstart_reset_after_vector_instruction)
{
    Machine m(".text\nvsetivli t0, 4, e8, m1, ta, ma\ncsrwi vstart, 2\nvadd.vi v1, v1, 1\n");
    m.run();
    EXPECT_EQ(m.csr(csr::kVstart), 0U);
    // Elements below the old vstart were not written.
    const auto v1 = m.getV(1);
    EXPECT_EQ(v1[0], 0);
    EXPECT_EQ(v1[1], 0);
    EXPECT_EQ(v1[2], 1);
    EXPECT_EQ(v1[3], 1);
    EXPECT_EQ(v1[4], 0); // tail undisturbed
}
