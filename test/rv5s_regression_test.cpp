/**
 * @file rv5s_regression_test.cpp
 * @brief Regression tests for scalar pipeline bugs found while validating vector programs on
 * the 5-stage models: AUIPC/LUI results, the integer source of F/D instructions on the
 * forwarding models, and Run() pausing at the end-of-program breakpoint while draining.
 */
#include <gtest/gtest.h>

#include "assembler/assembler.h"
#include "config/config.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "processor/rvss/rvss_processor.h"
#include "utils/utils.h"

#include <iostream>
#include <memory>
#include <sstream>
#include <streambuf>

using namespace Kites;

namespace
{

class Quiet
{
  public:
    Quiet() : previous_(std::cout.rdbuf(&sink_)) {}
    ~Quiet() { std::cout.rdbuf(previous_); }

  private:
    struct Null : std::streambuf
    {
        int overflow(int c) override { return c; }
    } sink_;
    std::streambuf *previous_;
};

template <typename Processor>
std::unique_ptr<Processor> run(const std::string &source)
{
    Quiet quiet;
    setupVmStateDirectory();
    auto vm = std::make_unique<Processor>();
    std::istringstream in(source);
    vm->LoadProgram(assemble(in));
    ProcessorBase &processor = *vm;
    for (int i = 0; i < 10000 && processor.StepAndNotify(); ++i)
    {
    }
    return vm;
}

template <typename Processor>
void expectUpperImmediates(const char *model)
{
    // Independent instructions only, so the program is valid without hazard handling.
    auto vm = run<Processor>(".text\nauipc a0, 1\nlui a1, 0x80000\nauipc a2, 0xfffff\nnop\n");
    EXPECT_EQ(vm->registers_.ReadGpr(10), 0x1000U) << model;
    EXPECT_EQ(vm->registers_.ReadGpr(11), 0xFFFFFFFF80000000ULL) << model;
    EXPECT_EQ(vm->registers_.ReadGpr(12), 8ULL - 0x1000ULL) << model;
}

} // namespace

TEST(Rv5sRegression, auipc_and_lui_on_all_models)
{
    expectUpperImmediates<RV5StageProcessorNHNF>("NH_NF");
    expectUpperImmediates<RV5StageProcessorHNF>("H_NF");
    expectUpperImmediates<RV5StageProcessorNHF>("NH_F");
    expectUpperImmediates<RV5StageProcessorHF>("H_F");
}

TEST(Rv5sRegression, la_on_hazard_and_forwarding_models)
{
    const char *source = ".data\nx: .word 7\ny: .word 9\n.text\nla a0, y\nlw a1, 0(a0)\n";
    for (int model = 0; model < 3; ++model)
    {
        std::unique_ptr<RV5StageVM_Base> vm;
        if (model == 0)
            vm = run<RV5StageProcessorHNF>(source);
        else if (model == 1)
            vm = run<RV5StageProcessorNHF>(source);
        else
            vm = run<RV5StageProcessorHF>(source);
        EXPECT_EQ(vm->registers_.ReadGpr(10), vm_config::config.getDataSectionStart() + 4) << model;
        EXPECT_EQ(vm->registers_.ReadGpr(11), 9U) << model;
    }
}

// An F/D instruction whose integer source was produced by the instruction just ahead must
// receive the forwarded value.
TEST(Rv5sRegression, forwarded_integer_source_of_fp_instructions)
{
    const char *source = R"(.data
d: .dword 0x4008000000000000
.text
li t0, 0x3ff0000000000000
fmv.d.x fa0, t0
li t1, 0x40400000
fmv.w.x fa1, t1
la t2, d
fld fa2, 0(t2)
addi t3, x0, 5
fcvt.d.l fa3, t3
)";
    for (int model = 0; model < 2; ++model)
    {
        std::unique_ptr<RV5StageVM_Base> vm;
        if (model == 0)
            vm = run<RV5StageProcessorHF>(source);
        else
            vm = run<RV5StageProcessorNHF>(source);
        EXPECT_EQ(vm->registers_.ReadFpr(10), 0x3FF0000000000000ULL) << model;
        EXPECT_EQ(vm->registers_.ReadFpr(11) & 0xFFFFFFFFULL, 0x40400000ULL) << model;
        EXPECT_EQ(vm->registers_.ReadFpr(12), 0x4008000000000000ULL) << model;
        EXPECT_EQ(vm->registers_.ReadFpr(13), 0x4014000000000000ULL) << model;
    }
}

// Run() must drain the pipeline to the end even though LoadProgram places a breakpoint at the
// end-of-program address (the batch CLI used to wait there forever).
TEST(Rv5sRegression, run_drains_past_end_of_program_breakpoint)
{
    Quiet quiet;
    setupVmStateDirectory();
    RV5StageProcessorHF vm;
    std::istringstream in(".text\naddi a0, x0, 5\naddi a1, a0, 1\n");
    vm.LoadProgram(assemble(in));
    vm.step_delay_ = 0;
    ProcessorBase &processor = vm;
    processor.Run();
    EXPECT_EQ(vm.registers_.ReadGpr(11), 6U);
    EXPECT_EQ(vm.instructions_retired_, 2U);
}
