/**
 * @file vector_unit.cpp
 * @brief Vector instruction dispatch, legality checks, configuration instructions and the
 * atomic commit of results.
 */
#include "processor/vector/vector_unit.h"

#include "processor/memory_controller.h"
#include "processor/processor_base.h"
#include "processor/registers.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_exec.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace Kites
{
namespace rvv
{
namespace detail
{

ExecState::ExecState(const InsnInfo &insn, uint32_t instruction, ExecutionContext &ctx)
    : info(insn), word(instruction), context(ctx)
{
    RegisterFile &regs = ctx.registers;
    vlenb = static_cast<unsigned>(regs.GetVlenb());
    vlen = vlenb * 8;
    vtype = VType{regs.ReadCsr(csr_address::kVtype)};
    sew = vtype.sew();
    sewLog2 = vtype.sewLog2();
    lmulLog2 = vtype.lmulLog2();
    vl = regs.ReadCsr(csr_address::kVl);
    vstart = regs.ReadCsr(csr_address::kVstart);
    vlmax = vtype.vill() ? 0 : rvv::vlmax(vlen, sewLog2, lmulLog2);
    masked = info.hasField(kVm) && field(kVm) == 0;
    source = regs.GetVrBytes();
    destination = source;
}

uint64_t ExecState::scalarOperand() const
{
    if (info.hasField(kRs1))
    {
        return info.funct3 == kOpFVF ? fpr(field(kRs1)) : gpr(field(kRs1));
    }
    if (info.hasField(kSimm5))
    {
        return static_cast<uint64_t>(signExtend(field(kSimm5), 5));
    }
    if (info.hasField(kZimm5))
    {
        return field(kZimm5);
    }
    return 0;
}

uint64_t ExecState::src1(uint64_t index, unsigned eew) const
{
    if (hasVectorSrc1())
    {
        return read(vs1(), index, eew);
    }
    return scalarOperand() & maskBits(eew);
}

uint64_t ExecState::read(unsigned reg, uint64_t index, unsigned eew) const
{
    const unsigned bytes = eew / 8;
    const uint64_t offset = static_cast<uint64_t>(reg) * vlenb + index * bytes;
    if (offset + bytes > source.size())
    {
        illegal("register group extends past v31");
    }
    uint64_t value = 0;
    for (unsigned b = 0; b < bytes; ++b)
    {
        value |= static_cast<uint64_t>(source[offset + b]) << (8 * b);
    }
    return value;
}

void ExecState::write(unsigned reg, uint64_t index, unsigned eew, uint64_t value)
{
    const unsigned bytes = eew / 8;
    const uint64_t offset = static_cast<uint64_t>(reg) * vlenb + index * bytes;
    if (offset + bytes > destination.size())
    {
        illegal("register group extends past v31");
    }
    for (unsigned b = 0; b < bytes; ++b)
    {
        destination[offset + b] = static_cast<uint8_t>(value >> (8 * b));
    }
}

bool ExecState::maskBit(unsigned reg, uint64_t index) const
{
    const uint64_t offset = static_cast<uint64_t>(reg) * vlenb + index / 8;
    return ((source.at(offset) >> (index % 8)) & 1U) != 0;
}

void ExecState::writeMaskBit(unsigned reg, uint64_t index, bool value)
{
    const uint64_t offset = static_cast<uint64_t>(reg) * vlenb + index / 8;
    const auto bit = static_cast<uint8_t>(1U << (index % 8));
    uint8_t &byte = destination.at(offset);
    byte = value ? static_cast<uint8_t>(byte | bit) : static_cast<uint8_t>(byte & ~bit);
}

uint64_t ExecState::groupElements(const Group &g) const
{
    return static_cast<uint64_t>(groupRegisters(g)) * vlen / g.eew;
}

void ExecState::inactive(const Group &dest, uint64_t index)
{
    if (config().agnosticFillOnes && vtype.vma())
    {
        write(dest.reg, index, dest.eew, ~0ULL);
    }
}

void ExecState::inactiveMaskBit(unsigned reg, uint64_t index)
{
    if (config().agnosticFillOnes && vtype.vma())
    {
        writeMaskBit(reg, index, true);
    }
}

void ExecState::tailFrom(const Group &dest, uint64_t first)
{
    // With no body elements, no tail element is updated either.
    if (vstart >= vl || !config().agnosticFillOnes || !vtype.vta())
    {
        return;
    }
    const uint64_t count = groupElements(dest);
    for (uint64_t i = first; i < count; ++i)
    {
        write(dest.reg, i, dest.eew, ~0ULL);
    }
}

void ExecState::maskTail(unsigned reg, uint64_t first)
{
    // Mask destination tails are always agnostic.
    if (vstart >= vl || !config().agnosticFillOnes)
    {
        return;
    }
    for (uint64_t i = first; i < vlen; ++i)
    {
        writeMaskBit(reg, i, true);
    }
}

uint64_t ExecState::gpr(unsigned reg) const
{
    return context.registers.ReadGpr(reg);
}

uint64_t ExecState::fpr(unsigned reg) const
{
    return context.registers.ReadFpr(reg);
}

uint64_t ExecState::csr(size_t address) const
{
    return context.registers.ReadCsr(address);
}

void ExecState::illegal(const std::string &reason) const
{
    throw IllegalInstruction("Illegal vector instruction '" + disassemble(word) + "': " + reason);
}

void ExecState::require(bool condition, const std::string &reason) const
{
    if (!condition)
    {
        illegal(reason);
    }
}

void ExecState::requireVtype() const
{
    require(!vtype.vill(), "vtype is illegal (vill = 1); execute vsetvli/vsetivli/vsetvl first");
    require(vstart < vlmax || vlmax == 0,
            "vstart = " + std::to_string(vstart) + " exceeds the largest element index");
}

void ExecState::requireVstartZero() const
{
    require(vstart == 0, "this instruction requires vstart = 0");
}

Group ExecState::group(unsigned reg, unsigned eew) const
{
    int eewLog2 = 0;
    while ((1U << eewLog2) < eew)
    {
        ++eewLog2;
    }
    return {reg, eew, eewLog2 - sewLog2 + lmulLog2};
}

void ExecState::checkGroup(const Group &g, const char *name) const
{
    require(g.emulLog2 >= -3 && g.emulLog2 <= 3,
            std::string("EMUL of ") + name + " is outside [1/8, 8]");
    const unsigned n = groupRegisters(g);
    require(g.reg % n == 0, std::string(name) + " = v" + std::to_string(g.reg) +
                                " is not a multiple of its register-group size " +
                                std::to_string(n));
    require(g.reg + n <= kNumVectorRegisters,
            std::string(name) + " register group extends past v31");
}

void ExecState::checkOverlap(const Group &dest, const Group &src, const char *srcName) const
{
    if (!overlaps(dest, src) || dest.eew == src.eew)
    {
        return;
    }
    const std::string what = std::string("destination overlaps ") + srcName + " illegally";
    if (dest.eew < src.eew)
    {
        // Allowed only in the lowest-numbered part of the source group.
        require(dest.reg == src.reg, what);
        return;
    }
    // Allowed only in the highest-numbered part of the destination group, with source EMUL >= 1.
    require(src.emulLog2 >= 0 &&
                src.reg + groupRegisters(src) == dest.reg + groupRegisters(dest),
            what);
}

void ExecState::forbidOverlap(const Group &dest, const Group &src, const char *srcName) const
{
    require(!overlaps(dest, src), std::string("destination must not overlap ") + srcName);
}

void ExecState::checkMaskOverlap(const Group &dest) const
{
    if (masked && dest.eew > 1)
    {
        require(!overlaps(dest, maskGroup(0)),
                "destination of a masked instruction overlaps the mask register v0");
    }
}

// ---------------------------------------------------------------- vset{i}vl{i}

void executeConfig(ExecState &s)
{
    const unsigned rd = s.field(kRd);
    uint64_t requested = 0;
    if (s.info.mnemonic == "vsetvli")
        requested = s.field(kZimm11);
    else if (s.info.mnemonic == "vsetivli")
        requested = s.field(kZimm10);
    else
        requested = s.gpr(s.field(kRs2));

    uint64_t newVtype = kVillBit;
    uint64_t newVl = 0;
    if (isSupportedVtype(requested))
    {
        const VType vt{requested};
        const uint64_t max = vlmax(s.vlen, vt.sewLog2(), vt.lmulLog2());
        bool keepVl = false;
        uint64_t avl = 0;
        if (s.info.mnemonic == "vsetivli")
        {
            avl = s.field(kZimm5);
        }
        else if (s.field(kRs1) != 0)
        {
            avl = s.gpr(s.field(kRs1));
        }
        else if (rd != 0)
        {
            avl = ~0ULL; // request VLMAX
        }
        else
        {
            keepVl = true;
        }

        if (keepVl)
        {
            // rs1 = rd = x0 keeps vl; reserved if vill was set or VLMAX would change (D4).
            if (!s.vtype.vill() && s.vlmax == max)
            {
                newVtype = requested;
                newVl = s.vl;
            }
        }
        else
        {
            newVtype = requested;
            newVl = std::min(avl, max);
        }
    }

    s.newVtype = newVtype;
    s.newVl = newVl;
    s.gprWrite = std::make_pair(rd, newVl);
}

// ---------------------------------------------------------------- commit

namespace
{

void commitMemory(ExecState &s)
{
    if (s.stores.empty())
    {
        return;
    }
    MemoryController &mem = s.context.memory;
    // Track each byte's value before the first store and after the last one, so overlapping
    // element stores (e.g. repeated indices) are undone correctly.
    std::map<uint64_t, std::pair<uint8_t, uint8_t>> changed;
    for (const PendingStore &st : s.stores)
    {
        for (unsigned b = 0; b < st.bytes; ++b)
        {
            changed.emplace(st.address + b, std::make_pair(mem.peekByte(st.address + b), 0));
        }
        switch (st.bytes)
        {
        case 1:
            mem.writeByte(st.address, static_cast<uint8_t>(st.value));
            break;
        case 2:
            mem.writeHalfWord(st.address, static_cast<uint16_t>(st.value));
            break;
        case 4:
            mem.writeWord(st.address, static_cast<uint32_t>(st.value));
            break;
        default:
            mem.writeDoubleWord(st.address, st.value);
            break;
        }
    }
    MemoryChange run{0, {}, {}};
    bool open = false;
    for (auto &[address, values] : changed)
    {
        values.second = mem.peekByte(address);
        if (open && address != run.address + run.old_bytes_vec.size())
        {
            if (run.old_bytes_vec != run.new_bytes_vec)
                s.context.delta.memory_changes.push_back(run);
            open = false;
        }
        if (!open)
        {
            run = MemoryChange{address, {}, {}};
            open = true;
        }
        run.old_bytes_vec.push_back(values.first);
        run.new_bytes_vec.push_back(values.second);
    }
    if (open && run.old_bytes_vec != run.new_bytes_vec)
    {
        s.context.delta.memory_changes.push_back(run);
    }
}

void commit(ExecState &s)
{
    RegisterFile &regs = s.context.registers;
    StepDelta &delta = s.context.delta;

    commitMemory(s);

    for (unsigned r = 0; r < kNumVectorRegisters; ++r)
    {
        const auto first = static_cast<std::ptrdiff_t>(r * s.vlenb);
        const auto last = first + static_cast<std::ptrdiff_t>(s.vlenb);
        if (!std::equal(s.source.begin() + first, s.source.begin() + last,
                        s.destination.begin() + first))
        {
            std::vector<uint8_t> oldBytes(s.source.begin() + first, s.source.begin() + last);
            std::vector<uint8_t> newBytes(s.destination.begin() + first,
                                          s.destination.begin() + last);
            regs.WriteVr(r, newBytes);
            delta.vector_register_changes.push_back({r, std::move(oldBytes), std::move(newBytes)});
        }
    }

    if (s.gprWrite && s.gprWrite->first != 0)
    {
        const unsigned rd = s.gprWrite->first;
        const uint64_t oldValue = regs.ReadGpr(rd);
        regs.WriteGpr(rd, s.gprWrite->second);
        if (oldValue != s.gprWrite->second)
            delta.register_changes.push_back({rd, 0, oldValue, s.gprWrite->second});
    }
    if (s.fprWrite)
    {
        const unsigned rd = s.fprWrite->first;
        const uint64_t oldValue = regs.ReadFpr(rd);
        regs.WriteFpr(rd, s.fprWrite->second);
        if (oldValue != s.fprWrite->second)
            delta.register_changes.push_back({rd, 2, oldValue, s.fprWrite->second});
    }

    auto writeCsr = [&](size_t address, uint64_t value)
    {
        const uint64_t oldValue = regs.ReadCsr(address);
        if (oldValue != value)
        {
            regs.WriteCsr(address, value);
            delta.register_changes.push_back(
                {static_cast<unsigned int>(address), 1, oldValue, value});
        }
    };
    if (s.newVtype)
        writeCsr(csr_address::kVtype, *s.newVtype);
    if (s.newVl)
        writeCsr(csr_address::kVl, *s.newVl);
    if (s.setVxsat)
        writeCsr(csr_address::kVxsat, 1);
    if (s.fflags != 0)
    {
        writeCsr(csr_address::kFflags, regs.ReadCsr(csr_address::kFflags) | s.fflags);
    }
    writeCsr(csr_address::kVstart, 0);
}

} // namespace

} // namespace detail

void execute(uint32_t word, ExecutionContext &context)
{
    const InsnInfo *info = decode(word);
    if (info == nullptr)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "reserved vector encoding 0x%08x", word);
        throw IllegalInstruction(std::string("Illegal vector instruction: ") + buf);
    }
    detail::ExecState state(*info, word, context);
    if (info->isConfig())
    {
        detail::executeConfig(state);
    }
    else if (info->isLoad() || info->isStore())
    {
        detail::executeMemory(state);
    }
    else if (info->funct3 == kOpFVV || info->funct3 == kOpFVF)
    {
        detail::executeFloat(state);
    }
    else
    {
        detail::executeInteger(state);
    }
    detail::commit(state);
}

bool isImplemented(const InsnInfo &info)
{
    if (info.isConfig() || info.isLoad() || info.isStore())
    {
        return true;
    }
    if (info.funct3 == kOpFVV || info.funct3 == kOpFVF)
    {
        return detail::isImplementedFloat(info);
    }
    return detail::isImplementedInteger(info);
}

} // namespace rvv
} // namespace Kites
