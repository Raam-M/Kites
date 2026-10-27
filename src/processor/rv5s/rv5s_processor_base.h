#pragma once
#include "processor/pipeline_registers.h"
#include "processor/processor_base.h"
#include "processor/processor_manager.h"
#include "rv5s_control_unit.h"

namespace Kites
{
struct PipelineRegisterChange
{
    IF_ID_Register old_if_id_reg;
    IF_ID_Register new_if_id_reg;

    ID_EX_Register old_id_ex_reg;
    ID_EX_Register new_id_ex_reg;

    EX_MEM_Register old_ex_mem_reg;
    EX_MEM_Register new_ex_mem_reg;

    MEM_WB_Register old_mem_wb_reg;
    MEM_WB_Register new_mem_wb_reg;
};

struct RV5StageStepDelta : public StepDelta
{
    PipelineRegisterChange pipeline_register_change;
};

class RV5StageVM_Base : public ProcessorBase
{
  public:
    RV5StageVM_Base() = default;
    ~RV5StageVM_Base() = default;

    // --- VM Control Functions ---

    void PrintType()
    {
        std::cout << "rv5s_vm" << std::endl;
    }
    void DebugRun() override;
    void Reset() override;
    void Undo() override;
    void Redo() override;

  protected:
    // Pipeline Registers
    IF_ID_Register if_id_reg_;
    ID_EX_Register id_ex_reg_;
    EX_MEM_Register ex_mem_reg_;
    MEM_WB_Register mem_wb_reg_;

    // The control unit for the pipeline
    RV5SControlUnit control_unit_;

    std::stack<RV5StageStepDelta> undo_stack_{};
    std::stack<RV5StageStepDelta> redo_stack_{};
    RV5StageStepDelta current_delta_;

    // Set when EX/MEM redirected the PC this cycle; consumed by fetch_and_advance_pc().
    bool pc_redirected_ = false;

    void memory_writeback();
    void memory_writeback_float();
    void memory_writeback_double();

    void register_write_back(const uint64_t &write_data);
    void memory_read();
    // void memory_read_float();
    // void memory_read_double();

    void begin_step_delta();
    void finalize_step_delta();

    void print_pipeline_registers_debug();
    bool is_pipeline_drained() const;
    void setProcessorState() override;
    void Run() override; // run debug run adn reset are same across all rv5s vms
    bool StepAndNotify() override;

    // --- Private methods for each pipeline stage ---
    virtual void pipeline_fetch() = 0;

    void redirect_pc(uint64_t target);
    void fetch_and_advance_pc(bool stalled);

    void pipeline_decode();

    virtual void pipeline_execute() = 0;
    virtual uint64_t pipeline_execute_float() = 0;
    virtual uint64_t pipeline_execute_double() = 0;

    uint64_t execute_float();
    uint64_t execute_double();

    void execute_csr() {};

    // LUI/AUIPC immediate (the 20-bit U field from ImmGenerator) shifted into place and
    // sign-extended from bit 31, as RV64 requires.
    static uint64_t upper_immediate(int64_t imm)
    {
        return static_cast<uint64_t>(
            static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(imm) << 12)));
    }

    // Integer source (rs1) of an F/D instruction that reads a GPR in EX: load/store base,
    // fmv.w.x/fmv.d.x and integer-to-float conversions. Forwarding models take it from EX/MEM
    // when the instruction just ahead produced it; otherwise the register file is current.
    uint64_t forwarded_fp_gpr_source(unsigned reg) const;

    void pipeline_memory();
    void pipeline_writeback();
    void pipeline_writeback_float();
    void pipeline_writeback_double();

    // --- Specialized handler functions (called from pipeline stages) ---
    virtual void handle_syscall() = 0;
};
}//namespace Kites