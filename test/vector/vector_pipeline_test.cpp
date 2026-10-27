/**
 * @file vector_pipeline_test.cpp
 * @brief Vector and CSR instructions on the four 5-stage pipeline models (B1, SI-04).
 *
 * Vector and CSR instructions pass through IF..MEM without control signals and execute in WB.
 * Every test compares a pipeline model with the single-cycle model or with known values.
 */
#include "vector_test_support.h"

#include "config/config.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"

#include <array>

using namespace Kites;
using namespace vtest;
namespace csr = Kites::csr_address;

namespace
{

constexpr uint64_t kMemoryWindow = 4096; // bytes of the data section compared

/// Architectural state compared between processor models.
struct ArchState
{
    std::array<uint64_t, 32> gpr{};
    std::array<uint64_t, 32> fpr{};
    std::vector<uint8_t> vregs;
    std::array<uint64_t, 8> csrs{};
    std::vector<uint8_t> memory;

    bool operator==(const ArchState &o) const
    {
        return gpr == o.gpr && fpr == o.fpr && vregs == o.vregs && csrs == o.csrs &&
               memory == o.memory;
    }
};

ArchState capture(ProcessorBase &vm)
{
    ArchState s;
    for (unsigned r = 0; r < 32; ++r)
    {
        s.gpr[r] = vm.registers_.ReadGpr(r);
        s.fpr[r] = vm.registers_.ReadFpr(r);
        const auto bytes = vm.registers_.ReadVr(r);
        s.vregs.insert(s.vregs.end(), bytes.begin(), bytes.end());
    }
    const size_t addresses[] = {csr::kVl,   csr::kVtype, csr::kVstart, csr::kVxrm,
                                csr::kVxsat, 0x001,       0x002,        csr::kVlenb};
    for (size_t i = 0; i < 8; ++i)
        s.csrs[i] = vm.registers_.ReadCsr(addresses[i]);
    const uint64_t base = vm_config::config.getDataSectionStart();
    for (uint64_t a = 0; a < kMemoryWindow; ++a)
        s.memory.push_back(vm.memory_controller_.peekByte(base + a));
    return s;
}

/// Describes the first difference, for failure messages.
std::string difference(const ArchState &a, const ArchState &b)
{
    std::ostringstream out;
    for (unsigned r = 0; r < 32; ++r)
        if (a.gpr[r] != b.gpr[r])
            out << "x" << r << ": " << a.gpr[r] << " vs " << b.gpr[r] << "; ";
    for (unsigned r = 0; r < 32; ++r)
        if (a.fpr[r] != b.fpr[r])
            out << "f" << r << " differs; ";
    for (size_t i = 0; i < a.vregs.size(); ++i)
        if (a.vregs[i] != b.vregs[i])
        {
            out << "v" << i / kVlenb << " byte " << i % kVlenb << " differs; ";
            break;
        }
    for (size_t i = 0; i < a.csrs.size(); ++i)
        if (a.csrs[i] != b.csrs[i])
            out << "csr #" << i << ": " << a.csrs[i] << " vs " << b.csrs[i] << "; ";
    for (size_t i = 0; i < a.memory.size(); ++i)
        if (a.memory[i] != b.memory[i])
        {
            out << "memory +" << i << " differs; ";
            break;
        }
    return out.str();
}

AssembledProgram assembleSource(const std::string &source)
{
    std::istringstream in(source);
    return assemble(in);
}

template <typename Processor>
std::unique_ptr<Processor> load(const AssembledProgram &program)
{
    QuietOutput quiet;
    setupVmStateDirectory();
    auto vm = std::make_unique<Processor>();
    vm->LoadProgram(program);
    return vm;
}

constexpr unsigned kStepLimit = 100000;

/// Runs a program until the pipeline has drained, failing the test (instead of hanging) if it
/// does not finish within kStepLimit cycles.
template <typename Processor>
std::unique_ptr<Processor> runProgram(const AssembledProgram &program)
{
    auto vm = load<Processor>(program);
    QuietOutput quiet;
    ProcessorBase &processor = *vm;
    unsigned steps = 0;
    while (processor.StepAndNotify())
    {
        if (++steps > kStepLimit)
        {
            ADD_FAILURE() << "program did not finish within " << kStepLimit << " cycles";
            break;
        }
    }
    return vm;
}

ArchState singleCycleState(const AssembledProgram &program)
{
    auto vm = load<RVSSProcessor>(program);
    QuietOutput quiet;
    for (unsigned steps = 0; vm->program_counter_ < vm->program_size_; ++steps)
    {
        if (steps > kStepLimit)
        {
            ADD_FAILURE() << "single-cycle run did not finish";
            break;
        }
        vm->Step();
    }
    return capture(*vm);
}

template <typename Processor>
void expectSameAsSingleCycle(const AssembledProgram &program, const std::string &what)
{
    const ArchState expected = singleCycleState(program);
    auto vm = runProgram<Processor>(program);
    const ArchState actual = capture(*vm);
    EXPECT_TRUE(actual == expected) << what << ": " << difference(actual, expected);
}

const char *const kExamples[] = {"vvadd_i32.s",   "memcpy.s",     "dot_product.s",
                                 "masked_abs.s",  "gather_scatter.s", "prefix_sum.s",
                                 "matmul_i32.s",  "saxpy_f32.s",  "strlen.s",
                                 "rgb_to_grey.s", "fir_q15.s"};

/// Mixed scalar/vector code with back-to-back dependencies in both directions.
const char *const kDependentChain = R"(.data
buf: .word 1, 2, 3, 4, 5, 6, 7, 8
out: .word 0, 0, 0, 0, 0, 0, 0, 0
.text
la a0, buf
la a1, out
addi a2, x0, 8
vsetvli t0, a2, e32, m2, ta, ma
vle32.v v8, (a0)
vadd.vi v8, v8, 3
vmv.x.s t1, v8
addi t2, t1, 100
vadd.vx v8, v8, t2
vse32.v v8, (a1)
lw t3, 4(a1)
vredsum.vs v12, v8, v8
vmv.x.s t4, v12
add t5, t4, t3
csrr t6, vl
add t6, t6, t5
csrwi vxrm, 2
csrr s1, vxrm
sw t6, 28(a1)
vle32.v v16, (a1)
vfirst.m s2, v0
vcpop.m s3, v16
add s4, s2, s3
)";

} // namespace

// SI-04: every example program gives the same final state on the hazard-detecting pipelines
// as on the single-cycle processor. The examples contain no NOPs, so the pipelines must stall
// for every dependency on a vector or CSR result.
TEST(VecPipeline, SI04_examples_match_single_cycle_on_hazard_models)
{
    for (const char *name : kExamples)
    {
        const AssembledProgram program = [&]
        {
            QuietOutput quiet;
            return assemble(std::string("../examples/vector/") + name);
        }();
        expectSameAsSingleCycle<RV5StageProcessorHNF>(program, std::string("H_NF ") + name);
        expectSameAsSingleCycle<RV5StageProcessorHF>(program, std::string("H_F ") + name);
    }
}

TEST(VecPipeline, PL01_dependent_chain_on_hazard_models)
{
    const auto program = assembleSource(kDependentChain);
    expectSameAsSingleCycle<RV5StageProcessorHNF>(program, "H_NF");
    expectSameAsSingleCycle<RV5StageProcessorHF>(program, "H_F");
    auto vm = runProgram<RV5StageProcessorHF>(program);
    EXPECT_EQ(vm->registers_.ReadGpr(18 /* s2 */), static_cast<uint64_t>(-1));
    EXPECT_EQ(vm->registers_.ReadCsr(csr::kVxrm), 2U);
}

// Without hazard detection the program must separate a vector or CSR producer from a scalar
// consumer by two instructions, as for a load without forwarding. Scalar-to-vector and
// vector-to-vector dependencies need no separation.
TEST(VecPipeline, PL02_no_hazard_models_with_two_nops)
{
    const char *source = R"(.text
addi a0, x0, 5
vsetvli t0, a0, e8, m1, ta, ma
nop
nop
addi t1, t0, 1
vmv.v.x v1, t1
vadd.vv v2, v1, v1
vmv.x.s a1, v2
nop
nop
addi a2, a1, 1
csrr a3, vl
nop
nop
add a4, a3, a2
)";
    const auto program = assembleSource(source);
    expectSameAsSingleCycle<RV5StageProcessorNHNF>(program, "NH_NF");
    expectSameAsSingleCycle<RV5StageProcessorNHF>(program, "NH_F");
    auto vm = runProgram<RV5StageProcessorNHNF>(program);
    EXPECT_EQ(vm->registers_.ReadGpr(5 /* t0 */), 5U);
    EXPECT_EQ(vm->registers_.ReadGpr(12 /* a2 */), 13U);
    EXPECT_EQ(vm->registers_.ReadGpr(14 /* a4 */), 18U);
}

// A scalar instruction that reads the result of the vector instruction just ahead of it waits
// two cycles on both hazard models: the value only exists after WB, so it cannot be forwarded.
TEST(VecPipeline, PL03_stall_cycles_for_vector_results)
{
    const char *dependent = ".text\nvsetivli t0, 4, e32, m1, ta, ma\nvmv.x.s a0, v1\n"
                            "addi a1, a0, 1\n";
    const char *independent = ".text\nvsetivli t0, 4, e32, m1, ta, ma\nvmv.x.s a0, v1\n"
                              "addi a1, a2, 1\n";
    const auto withDependency = assembleSource(dependent);
    const auto withoutDependency = assembleSource(independent);

    auto hnfDep = runProgram<RV5StageProcessorHNF>(withDependency);
    auto hnfFree = runProgram<RV5StageProcessorHNF>(withoutDependency);
    EXPECT_EQ(hnfDep->cycle_s_ - hnfFree->cycle_s_, 2U);

    auto hfDep = runProgram<RV5StageProcessorHF>(withDependency);
    auto hfFree = runProgram<RV5StageProcessorHF>(withoutDependency);
    EXPECT_EQ(hfDep->cycle_s_ - hfFree->cycle_s_, 2U);

    EXPECT_EQ(hfDep->instructions_retired_, 3U);
    EXPECT_EQ(hfFree->cycle_s_, 3U + 4U); // three instructions plus four cycles to drain
}

// A vector instruction never waits for an older scalar instruction: it reads its scalar
// operands in WB, after the producer has written back.
TEST(VecPipeline, PL04_vector_after_scalar_needs_no_stall)
{
    const char *source = ".text\naddi a0, x0, 5\nvsetvli t0, a0, e8, m1, ta, ma\n"
                         "addi a1, x0, 7\nvmv.v.x v1, a1\n";
    const auto program = assembleSource(source);
    for (auto *vmName : {"NH_NF", "H_NF", "NH_F", "H_F"})
    {
        std::unique_ptr<RV5StageVM_Base> vm;
        const std::string name = vmName;
        if (name == "NH_NF")
            vm = runProgram<RV5StageProcessorNHNF>(program);
        else if (name == "H_NF")
            vm = runProgram<RV5StageProcessorHNF>(program);
        else if (name == "NH_F")
            vm = runProgram<RV5StageProcessorNHF>(program);
        else
            vm = runProgram<RV5StageProcessorHF>(program);
        EXPECT_EQ(vm->registers_.ReadGpr(5), 5U) << name;
        EXPECT_EQ(vm->registers_.ReadVr(1)[4], 7U) << name;
        EXPECT_EQ(vm->cycle_s_, 4U + 4U) << name; // no stalls
    }
}

// Stores and loads stay in program order between the vector unit (WB) and scalar memory
// accesses (MEM), with no separating instructions. Scalar-to-scalar dependencies are two
// instructions apart, so the program is also valid on NH_NF.
TEST(VecPipeline, PL05_memory_order_between_vector_and_scalar_accesses)
{
    ASSERT_EQ(vm_config::config.getDataSectionStart(), 0x10000000U);
    const char *source = R"(.data
a: .word 1, 2, 3, 4
b: .word 0, 0, 0, 0
.text
lui a0, 0x10000
lui a1, 0x10000
addi t2, x0, 40
nop
addi a1, a1, 16
vsetivli t0, 4, e32, m1, ta, ma
vle32.v v1, (a0)
vse32.v v1, (a1)
lw t1, 8(a1)
sw t2, 12(a1)
vle32.v v2, (a1)
vmv.v.i v3, 9
vse32.v v3, (a0)
lw t3, 0(a0)
)";
    const auto program = assembleSource(source);
    expectSameAsSingleCycle<RV5StageProcessorNHNF>(program, "NH_NF");
    expectSameAsSingleCycle<RV5StageProcessorHNF>(program, "H_NF");
    expectSameAsSingleCycle<RV5StageProcessorNHF>(program, "NH_F");
    expectSameAsSingleCycle<RV5StageProcessorHF>(program, "H_F");
    auto vm = runProgram<RV5StageProcessorHF>(program);
    EXPECT_EQ(vm->registers_.ReadGpr(6), 3U);
    EXPECT_EQ(vm->registers_.ReadVr(2)[12], 40U);
    EXPECT_EQ(vm->registers_.ReadGpr(28), 9U);
}

// Vector instructions fetched after a taken branch or a jump are flushed before WB.
TEST(VecPipeline, PL06_wrong_path_vector_instructions_do_not_execute)
{
    const char *source = R"(.text
vsetivli t0, 4, e32, m1, ta, ma
beq x0, x0, skip
vadd.vi v1, v1, 1
vmv.v.i v2, 5
csrwi vxrm, 3
skip:
jal x0, end
vadd.vi v3, v3, 1
csrwi vxrm, 1
end:
addi a0, x0, 1
)";
    const auto program = assembleSource(source);
    for (int model = 0; model < 4; ++model)
    {
        std::unique_ptr<RV5StageVM_Base> vm;
        if (model == 0)
            vm = runProgram<RV5StageProcessorNHNF>(program);
        else if (model == 1)
            vm = runProgram<RV5StageProcessorHNF>(program);
        else if (model == 2)
            vm = runProgram<RV5StageProcessorNHF>(program);
        else
            vm = runProgram<RV5StageProcessorHF>(program);
        EXPECT_EQ(vm->registers_.ReadVr(1), std::vector<uint8_t>(kVlenb, 0)) << model;
        EXPECT_EQ(vm->registers_.ReadVr(2), std::vector<uint8_t>(kVlenb, 0)) << model;
        EXPECT_EQ(vm->registers_.ReadVr(3), std::vector<uint8_t>(kVlenb, 0)) << model;
        EXPECT_EQ(vm->registers_.ReadCsr(csr::kVxrm), 0U) << model;
        EXPECT_EQ(vm->registers_.ReadGpr(10), 1U) << model;
    }
}

// An illegal vector instruction stops the pipeline in WB with an error, points the PC at the
// instruction and changes nothing; stepping again reports the same error.
TEST(VecPipeline, PL07_illegal_instruction_in_wb)
{
    const char *source = ".text\naddi a0, x0, 3\nvadd.vv v1, v2, v3\naddi a1, x0, 4\n";
    auto vm = load<RV5StageProcessorHF>(assembleSource(source));
    vm->registers_.WriteVr(2, std::vector<uint8_t>(kVlenb, 1));
    QuietOutput quiet;
    bool thrown = false;
    ArchState before;
    for (int i = 0; i < 10 && !thrown; ++i)
    {
        before = capture(*vm);
        try
        {
            vm->Step();
        }
        catch (const std::runtime_error &)
        {
            thrown = true;
        }
    }
    ASSERT_TRUE(thrown) << "vadd.vv with vtype.vill = 1 must be rejected";
    EXPECT_EQ(vm->program_counter_, 4U);
    EXPECT_TRUE(capture(*vm) == before) << difference(capture(*vm), before);
    EXPECT_EQ(vm->registers_.ReadGpr(10), 3U); // the older instruction completed
    EXPECT_EQ(vm->registers_.ReadGpr(11), 0U); // the younger one did not
    EXPECT_THROW(vm->Step(), std::runtime_error);

    // A CSR write to a read-only CSR is rejected the same way.
    auto csrVm = load<RV5StageProcessorHNF>(assembleSource(".text\ncsrwi vl, 3\n"));
    EXPECT_THROW(
        {
            ProcessorBase &processor = *csrVm;
            for (unsigned i = 0; i < 10; ++i)
                processor.StepAndNotify();
        },
        std::runtime_error);
    EXPECT_EQ(csrVm->registers_.ReadCsr(csr::kVl), 0U);
}

// Undo walks back through vector register, CSR and memory changes made in WB; redo replays them.
TEST(VecPipeline, PL08_undo_redo)
{
    auto vm = load<RV5StageProcessorHF>(assembleSource(kDependentChain));
    QuietOutput quiet;
    const ArchState initial = capture(*vm);
    std::vector<ArchState> history;
    for (int i = 0; i < 200 && (vm->program_counter_ < vm->program_size_ || history.size() < 40);
         ++i)
    {
        vm->Step();
        history.push_back(capture(*vm));
    }
    for (size_t i = 0; i < history.size(); ++i)
        vm->Undo();
    EXPECT_TRUE(capture(*vm) == initial) << difference(capture(*vm), initial);
    for (size_t i = 0; i < history.size(); ++i)
    {
        vm->Redo();
        ASSERT_TRUE(capture(*vm) == history[i]) << "redo step " << i << ": "
                                                << difference(capture(*vm), history[i]);
    }
}

// A scalar floating-point operation waits for an older vector instruction, so fflags are
// updated in program order.
TEST(VecPipeline, PL09_scalar_fp_after_vector_fp)
{
    const char *source = R"(.data
x: .word 0x3f800000, 0x40000000, 0x40400000, 0x40800000
.text
la a0, x
vsetivli t0, 4, e32, m1, ta, ma
vle32.v v1, (a0)
addi t1, x0, 3
fcvt.s.w f1, t1
vfdiv.vf v2, v1, f1
fadd.s f2, f1, f1
csrr a1, fflags
vfredosum.vs v3, v2, v2
fmul.s f3, f1, f1
)";
    const auto program = assembleSource(source);
    expectSameAsSingleCycle<RV5StageProcessorHNF>(program, "H_NF");
    expectSameAsSingleCycle<RV5StageProcessorHF>(program, "H_F");
}
