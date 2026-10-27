/**
 * @file vector_profiler_test.cpp
 * @brief Vector statistics (VS-01) and the analytic timing model (VT-01): counts, element and
 * byte totals, cycle estimates checked against hand-computed schedules, and undo/redo.
 */
#include "vector_test_support.h"

#include "config/config.h"
#include "processor/vector/vector_isa.h"
#include "processor/vector/vector_profiler.h"

using namespace Kites;
using namespace vtest;

namespace
{

class TimingGuard
{
  public:
    TimingGuard() : saved_(rvv::timingConfig()) {}
    ~TimingGuard() { rvv::timingConfig() = saved_; }

  private:
    rvv::TimingConfig saved_;
};

std::unique_ptr<Machine> runProgram(const std::string &body)
{
    auto m = std::make_unique<Machine>(".text\n" + body);
    const uint64_t data = vm_config::config.getDataSectionStart();
    m->setX(10, data);
    m->setX(11, data + 64);
    m->setX(12, data + 128);
    m->run();
    return m;
}

const char *kKernel = "vsetivli t0, 8, e32, m2, ta, ma\n"
                      "vle32.v v8, (a0)\n"
                      "vle32.v v16, (a1)\n"
                      "vmul.vv v24, v8, v16\n"
                      "vmv.v.i v0, 5\n"
                      "vadd.vv v24, v24, v8, v0.t\n"
                      "vredsum.vs v2, v24, v2\n"
                      "vse32.v v24, (a2)\n";

} // namespace

TEST(VecProfiler, VS01_statistics)
{
    auto m = runProgram(kKernel);
    const rvv::VectorStatistics &s = m->vm->vector_profiler_.statistics();
    EXPECT_EQ(s.instructions, 8U);
    EXPECT_EQ(s.byClass[static_cast<size_t>(rvv::InsnClass::Config)], 1U);
    EXPECT_EQ(s.byClass[static_cast<size_t>(rvv::InsnClass::Load)], 2U);
    EXPECT_EQ(s.byClass[static_cast<size_t>(rvv::InsnClass::Store)], 1U);
    EXPECT_EQ(s.byClass[static_cast<size_t>(rvv::InsnClass::Integer)], 3U);
    EXPECT_EQ(s.byClass[static_cast<size_t>(rvv::InsnClass::Reduction)], 1U);
    EXPECT_EQ(s.byMnemonic.at("vle32.v"), 2U);
    EXPECT_EQ(s.bytesLoaded, 64U);
    EXPECT_EQ(s.bytesStored, 32U);
    // v0 = 5 in every 32-bit element: mask bits 0..7 come from byte 0 = 0b101.
    EXPECT_EQ(s.inactiveElements, 6U);
    EXPECT_EQ(s.vlInstructions, 7U);
    EXPECT_DOUBLE_EQ(s.averageVl(), 8.0);
    EXPECT_DOUBLE_EQ(s.utilisation(), 1.0); // VLMAX at e32, m2 = 8
    EXPECT_EQ(s.bySew[2], 7U);
    EXPECT_EQ(s.byLmul[4], 7U); // m2
    EXPECT_NE(m->vm->vector_profiler_.report().find("Vector instructions: 8"), std::string::npos);
    EXPECT_NE(m->vm->vector_profiler_.toJson().find("\"bytes_loaded\": 64"), std::string::npos);
}

TEST(VecProfiler, VS01_classification)
{
    auto cls = [](const char *m) { return rvv::classify(*rvv::findByMnemonic(m)); };
    EXPECT_EQ(cls("vsaddu.vv"), rvv::InsnClass::FixedPoint);
    EXPECT_EQ(cls("vfmacc.vf"), rvv::InsnClass::Float);
    EXPECT_EQ(cls("vfmv.f.s"), rvv::InsnClass::Permutation);
    EXPECT_EQ(cls("vmand.mm"), rvv::InsnClass::Mask);
    EXPECT_EQ(cls("vcpop.m"), rvv::InsnClass::Mask);
    EXPECT_EQ(cls("vrgather.vv"), rvv::InsnClass::Permutation);
    EXPECT_EQ(cls("vfwredosum.vs"), rvv::InsnClass::Reduction);
    EXPECT_EQ(cls("vclz.v"), rvv::InsnClass::BitManip);
    EXPECT_EQ(rvv::unitOf(*rvv::findByMnemonic("vfnmsub.vv")), rvv::Unit::FpMul);
    EXPECT_EQ(rvv::unitOf(*rvv::findByMnemonic("vfsqrt.v")), rvv::Unit::FpDiv);
    EXPECT_EQ(rvv::unitOf(*rvv::findByMnemonic("vremu.vx")), rvv::Unit::Div);
    EXPECT_EQ(rvv::unitOf(*rvv::findByMnemonic("vwmaccsu.vv")), rvv::Unit::Mul);
    EXPECT_EQ(rvv::unitOf(*rvv::findByMnemonic("vlse64.v")), rvv::Unit::Load);
}

// vsetivli issues at cycle 0, the load at 1, the dependent add at 2. With 4 lanes and vl = 8
// each instruction streams for 2 cycles. Load: start 1, first result 1 + 12 = 13, last at
// 15. Add: starts at 13 with chaining (15 without) and finishes 6 + 2 cycles later.
TEST(VecProfiler, VT01_hand_computed_schedule)
{
    TimingGuard guard;
    const char *chain = "vsetivli x0, 8, e32, m2, ta, ma\nvle32.v v8, (a0)\nvadd.vv v16, v8, v8\n";
    struct Case
    {
        unsigned lanes;
        bool chaining;
        uint64_t cycles;
        uint64_t convoys;
    };
    for (const Case c : {Case{4, true, 21, 1}, Case{4, false, 23, 2}, Case{8, true, 20, 1},
                         Case{1, true, 27, 1}, Case{1, false, 35, 2}})
    {
        rvv::timingConfig().lanes = c.lanes;
        rvv::timingConfig().chaining = c.chaining;
        auto m = runProgram(chain);
        const auto &p = m->vm->vector_profiler_;
        EXPECT_EQ(p.estimatedCycles(), c.cycles) << c.lanes << " lanes, chaining " << c.chaining;
        EXPECT_EQ(p.state().timing.convoys, c.convoys) << c.lanes << " lanes, chaining " << c.chaining;
    }
}

TEST(VecProfiler, VT01_structural_hazard_and_more_lanes)
{
    TimingGuard guard;
    rvv::timingConfig().chaining = true;
    // Two independent multiplies share the multiply unit: the second waits for the first to
    // stream (a new convoy); an add can run alongside.
    const char *body = "vsetivli x0, 16, e32, m4, ta, ma\nvmul.vv v8, v16, v24\n"
                       "vmul.vv v4, v16, v24\nvadd.vv v12, v16, v24\n";
    std::vector<uint64_t> cycles;
    for (unsigned lanes : {1U, 2U, 4U, 8U, 16U})
    {
        rvv::timingConfig().lanes = lanes;
        auto m = runProgram(body);
        cycles.push_back(m->vm->vector_profiler_.estimatedCycles());
        EXPECT_EQ(m->vm->vector_profiler_.state().timing.convoys, 2U) << lanes;
    }
    for (size_t i = 1; i < cycles.size(); ++i)
        EXPECT_LT(cycles[i], cycles[i - 1]) << "more lanes must be faster";
    // 1 lane: vmul #1 at 1 streams 16 cycles; vmul #2 starts at 17, ends 17 + 7 + 16 = 40.
    EXPECT_EQ(cycles[0], 40U);
}

TEST(VecProfiler, VS01_undo_redo_restores_statistics)
{
    Machine m(std::string(".text\n") + kKernel);
    m.setX(10, vm_config::config.getDataSectionStart());
    m.setX(11, vm_config::config.getDataSectionStart());
    m.setX(12, vm_config::config.getDataSectionStart() + 128);
    for (int i = 0; i < 4; ++i)
        m.step();
    const std::string afterFour = m.vm->vector_profiler_.toJson();
    m.step();
    m.step();
    EXPECT_EQ(m.vm->vector_profiler_.statistics().instructions, 6U);
    {
        QuietOutput quiet;
        m.vm->Undo();
        m.vm->Undo();
    }
    EXPECT_EQ(m.vm->vector_profiler_.toJson(), afterFour);
    {
        QuietOutput quiet;
        m.vm->Redo();
    }
    EXPECT_EQ(m.vm->vector_profiler_.statistics().instructions, 5U);
    {
        QuietOutput quiet;
        m.vm->Reset();
    }
    EXPECT_EQ(m.vm->vector_profiler_.statistics().instructions, 0U);
}
