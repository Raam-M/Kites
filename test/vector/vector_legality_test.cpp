/**
 * @file vector_legality_test.cpp
 * @brief Illegal vector usage is rejected with an error and leaves all state unchanged (VL-01),
 * plus system-level behaviour: undo/redo (SI-02) and the pipeline guard (SI-04).
 */
#include "vector_test_support.h"

#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "processor/vector/vector_unit.h"
#include "processor/vector/vtype.h"

using namespace Kites;
using namespace vtest;
namespace csr = Kites::csr_address;

namespace
{

struct Snapshot
{
    std::vector<uint8_t> vregs;
    std::vector<uint64_t> gprs;
    uint64_t vl, vtype, vstart, pc;

    bool operator==(const Snapshot &o) const
    {
        return vregs == o.vregs && gprs == o.gprs && vl == o.vl && vtype == o.vtype &&
               vstart == o.vstart && pc == o.pc;
    }
};

Snapshot snapshot(const Machine &m)
{
    return {m.getV(0, 32), m.vm->registers_.GetGprValues(), m.csr(csr::kVl), m.csr(csr::kVtype),
            m.csr(csr::kVstart), m.vm->program_counter_};
}

/// Runs a vsetvli prefix, then expects the last instruction to raise IllegalInstruction
/// without changing any state.
void expectIllegal(const std::string &setup, const std::string &insn, const char *why)
{
    SCOPED_TRACE(std::string(why) + ": " + insn);
    Machine m(".text\n" + setup + "\n" + insn + "\n");
    std::mt19937_64 rng(7);
    m.setV(0, randomBytes(rng, 32 * kVlenb));
    m.step(); // setup
    const Snapshot before = snapshot(m);
    EXPECT_THROW(m.step(), rvv::IllegalInstruction);
    EXPECT_TRUE(snapshot(m) == before) << "state changed after an illegal instruction";
}

} // namespace

// VL-01: register-group alignment, EMUL range and segment limits.
TEST(VecLegality, VL01_register_group_rules)
{
    const std::string m2 = "vsetvli t0, x0, e32, m2, ta, ma";
    expectIllegal(m2, "vadd.vv v1, v2, v4", "vd not aligned to LMUL");
    expectIllegal(m2, "vadd.vv v2, v3, v4", "vs2 not aligned to LMUL");
    expectIllegal(m2, "vadd.vv v2, v4, v5", "vs1 not aligned to LMUL");
    expectIllegal("vsetvli t0, x0, e8, m4, ta, ma", "vle64.v v8, (a0)", "EMUL = 32");
    expectIllegal("vsetvli t0, x0, e64, mf8, ta, ma", "vadd.vv v1, v2, v3", "vill after mf8/e64");
    expectIllegal("vsetvli t0, x0, e8, m4, ta, ma", "vlseg3e8.v v8, (a0)", "NFIELDS*EMUL > 8");
    expectIllegal("vsetvli t0, x0, e8, m1, ta, ma", "vlseg8e8.v v28, (a0)", "segment past v31");
    expectIllegal("vsetvli t0, x0, e8, m1, ta, ma", "vl2re8.v v3, (a0)", "whole-register align");
    expectIllegal("vsetvli t0, x0, e8, m1, ta, ma", "vmv2r.v v2, v5", "vmv2r alignment");
    expectIllegal("vsetvli t0, x0, e32, m8, ta, ma", "vwadd.vv v8, v16, v24", "widening EMUL 16");
    expectIllegal("vsetvli t0, x0, e64, m1, ta, ma", "vwadd.vv v8, v16, v17", "2*SEW > ELEN");
    expectIllegal("vsetvli t0, x0, e8, m1, ta, ma", "vzext.vf2 v8, v16", "source EEW < 8");
}

// VL-01: source/destination overlap rules.
TEST(VecLegality, VL01_overlap_rules)
{
    const std::string m1 = "vsetvli t0, x0, e16, m1, ta, ma";
    expectIllegal(m1, "vadd.vv v0, v2, v4, v0.t", "masked destination overlaps v0");
    expectIllegal(m1, "vwadd.vv v2, v2, v4", "widening: source in low half of destination");
    expectIllegal(m1, "vnsrl.wv v3, v2, v4", "narrowing: destination in high half of source");
    expectIllegal("vsetvli t0, x0, e16, m2, ta, ma", "vmseq.vv v3, v2, v4",
                  "mask destination in high part of source group");
    expectIllegal(m1, "vrgather.vv v2, v2, v4", "vrgather vd overlaps vs2");
    expectIllegal(m1, "vrgather.vv v2, v4, v2", "vrgather vd overlaps vs1");
    expectIllegal(m1, "vslideup.vx v2, v2, a0", "vslideup vd overlaps vs2");
    expectIllegal(m1, "vslide1up.vx v2, v2, a0", "vslide1up vd overlaps vs2");
    expectIllegal(m1, "vcompress.vm v2, v2, v4", "vcompress vd overlaps vs2");
    expectIllegal(m1, "vcompress.vm v2, v4, v2", "vcompress vd overlaps vs1");
    expectIllegal(m1, "viota.m v2, v2", "viota vd overlaps vs2");
    expectIllegal(m1, "vmsbf.m v2, v2", "vmsbf vd overlaps vs2");
    expectIllegal(m1, "vadc.vvm v0, v2, v4, v0", "vadc destination is v0");
    expectIllegal(m1, "vmerge.vvm v0, v2, v4, v0", "vmerge destination is v0");
    expectIllegal("vsetvli t0, x0, e8, m1, ta, ma", "vluxseg2ei8.v v2, (a0), v3",
                  "segment indexed load overlaps the index");
}

// VL-01 / VL-02: vill and vstart restrictions.
TEST(VecLegality, VL02_vill_and_vstart)
{
    // vill is set at reset: any vtype-dependent instruction is illegal.
    expectIllegal("nop", "vadd.vv v1, v2, v3", "vill at reset");
    expectIllegal("nop", "vle8.v v1, (a0)", "vill at reset (load)");
    expectIllegal("nop", "vmv.x.s a0, v1", "vill at reset (vmv.x.s)");
    // Reductions and some mask instructions require vstart = 0.
    const std::string setup = "vsetvli t0, x0, e8, m1, ta, ma";
    for (const char *insn : {"vredsum.vs v1, v2, v3", "vcpop.m a1, v2", "vfirst.m a1, v2",
                             "viota.m v1, v2", "vcompress.vm v1, v2, v3", "vmsof.m v1, v2"})
    {
        SCOPED_TRACE(insn);
        Machine m(".text\n" + setup + "\ncsrwi vstart, 1\n" + insn + "\n");
        m.step();
        m.step();
        EXPECT_THROW(m.step(), rvv::IllegalInstruction);
        EXPECT_EQ(m.csr(csr::kVstart), 1U); // unchanged on an illegal instruction
    }
}

// Whole-register loads/stores and moves do not depend on vtype, so they work with vill set.
TEST(VecLegality, whole_register_ops_ignore_vill)
{
    Machine m(".text\nvmv2r.v v4, v2\n");
    std::mt19937_64 rng(3);
    const auto src = randomBytes(rng, 2 * kVlenb);
    m.setV(2, src);
    ASSERT_EQ(m.csr(csr::kVtype), rvv::kVillBit);
    EXPECT_NO_THROW(m.run());
    EXPECT_EQ(m.getV(4, 2), src);
}

// SI-02: undoing every step restores the initial state; redoing restores the final state.
TEST(VecSystem, SI02_undo_redo_round_trip)
{
    const std::string program = R"(.data
buf: .dword 1, 2, 3, 4, 5, 6, 7, 8
out: .dword 0, 0, 0, 0, 0, 0, 0, 0
.text
    la a1, buf
    la a2, out
    li a0, 8
    vsetvli t0, a0, e64, m4, ta, ma
    vle64.v v8, (a1)
    vadd.vv v8, v8, v8
    vredsum.vs v1, v8, v8
    vmv.x.s s1, v1
    vsse64.v v8, (a2), x0
    vse64.v v8, (a2)
    csrwi vxrm, 2
    vsetvli t0, x0, e8, m1, tu, mu
)";
    Machine m(program);
    auto memoryImage = [&](uint64_t base)
    {
        std::vector<uint8_t> bytes;
        for (uint64_t i = 0; i < 128; ++i)
            bytes.push_back(m.vm->memory_controller_.peekByte(base + i));
        return bytes;
    };
    const uint64_t data = 0x10000000;
    auto state = [&]()
    {
        return std::make_tuple(m.getV(0, 32), m.vm->registers_.GetGprValues(), m.csr(csr::kVl),
                               m.csr(csr::kVtype), m.csr(csr::kVxrm), memoryImage(data),
                               m.vm->program_counter_);
    };
    const auto initial = state();
    std::vector<decltype(state())> history;
    while (m.vm->program_counter_ < m.vm->program_size_)
    {
        m.step();
        history.push_back(state());
    }
    EXPECT_EQ(m.x(9), 2U * (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8) + 2); // sum of 2*buf plus v8[0]
    const auto final = state();
    {
        QuietOutput quiet;
        for (size_t i = 0; i < history.size(); ++i)
            m.vm->Undo();
    }
    EXPECT_TRUE(state() == initial) << "undo did not restore the initial state";
    {
        QuietOutput quiet;
        for (size_t i = 0; i < history.size(); ++i)
        {
            m.vm->Redo();
            EXPECT_TRUE(state() == history[i]) << "redo mismatch at step " << i;
        }
    }
    EXPECT_TRUE(state() == final);
}

// SI-04: the 5-stage models reject vector instructions instead of mis-executing them.
template <typename Processor>
void expectPipelineRejects()
{
    QuietOutput quiet;
    setupVmStateDirectory();
    Processor vm;
    std::istringstream in(".text\naddi x1, x0, 1\nvsetvli t0, x0, e8, m1, ta, ma\naddi x2, x0, 2\n");
    vm.LoadProgram(assemble(in));
    EXPECT_THROW(vm.DebugRun(), std::runtime_error);
}

TEST(VecSystem, SI04_pipelines_reject_vector_instructions)
{
    expectPipelineRejects<RV5StageProcessorNHNF>();
    expectPipelineRejects<RV5StageProcessorHNF>();
    expectPipelineRejects<RV5StageProcessorNHF>();
    expectPipelineRejects<RV5StageProcessorHF>();
}

// SI-01: one vector instruction retires as one instruction in one cycle.
TEST(VecSystem, SI01_retirement_counting)
{
    Machine m(".text\nvsetivli t0, 4, e32, m1, ta, ma\nvadd.vv v1, v2, v3\nvle32.v v4, (a0)\n");
    m.setX(10, 0x10000000);
    m.run();
    EXPECT_EQ(m.vm->instructions_retired_, 3U);
    EXPECT_EQ(m.vm->cycle_s_, 3U);
}
