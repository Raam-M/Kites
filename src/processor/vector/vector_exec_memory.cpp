/**
 * @file vector_exec_memory.cpp
 * @brief Vector loads and stores: unit-stride, strided, indexed, segment, fault-only-first,
 * mask and whole-register forms.
 *
 * Elements are accessed one at a time, in increasing element order (fields of a segment in
 * increasing field order), through the memory controller so the caches observe the traffic.
 * Stores are staged and applied when the instruction commits.
 */
#include "config/config.h"
#include "processor/memory_controller.h"
#include "processor/vector/vector_config.h"
#include "processor/vector/vector_exec.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace Kites
{
namespace rvv
{
namespace detail
{
namespace
{

using u64 = uint64_t;

constexpr unsigned kLumopUnit = 0x00;
constexpr unsigned kLumopWholeRegister = 0x08;
constexpr unsigned kLumopMask = 0x0B;
constexpr unsigned kLumopFaultOnlyFirst = 0x10;

std::string hexAddress(u64 address)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(address));
    return buf;
}

/// Reads one element; returns false if the address is outside memory.
bool loadElement(ExecState &s, u64 address, unsigned bytes, u64 &value)
{
    MemoryController &mem = s.context.memory;
    s.bytesLoaded += bytes;
    if (address + bytes < address)
    {
        return false;
    }
    try
    {
        switch (bytes)
        {
        case 1:
            value = mem.readByte(address);
            break;
        case 2:
            value = mem.readHalfWord(address);
            break;
        case 4:
            value = mem.readWord(address);
            break;
        default:
            value = mem.readDoubleWord(address);
            break;
        }
    }
    catch (const std::out_of_range &)
    {
        return false;
    }
    return true;
}

[[noreturn]] void accessFault(const ExecState &s, u64 address)
{
    throw AccessFault("Vector memory access fault in '" + disassemble(s.word) + "' at address " +
                      hexAddress(address));
}

void stageStore(ExecState &s, u64 address, unsigned bytes, u64 value)
{
    // Validate now so the commit cannot fail half way through.
    if (address + bytes < address || address + bytes > vm_config::config.getMemorySize())
    {
        accessFault(s, address);
    }
    s.stores.push_back({address, bytes, value});
}

unsigned widthToEew(unsigned width)
{
    return width == 0 ? 8 : (8U << (width - 4)); // 000 -> 8, 101 -> 16, 110 -> 32, 111 -> 64
}

void wholeRegister(ExecState &s, unsigned eew)
{
    // vl<nr>re<eew>.v / vs<nr>r.v ignore vtype and vl.
    const unsigned count = ((s.word >> 29) & 7U) + 1;
    s.require(count == 1 || count == 2 || count == 4 || count == 8,
              "whole-register count must be 1, 2, 4 or 8");
    const unsigned reg = s.vd();
    s.require(reg % count == 0, "register must be a multiple of " + std::to_string(count));
    const unsigned bytes = eew / 8;
    const u64 elements = static_cast<u64>(count) * s.vlenb / bytes;
    const u64 base = s.gpr(s.field(kRs1));
    for (u64 i = s.vstart; i < elements; ++i)
    {
        const u64 address = base + i * bytes;
        if (s.info.isLoad())
        {
            u64 value = 0;
            if (!loadElement(s, address, bytes, value))
                accessFault(s, address);
            s.write(reg, i, eew, value);
        }
        else
        {
            stageStore(s, address, bytes, s.read(reg, i, eew));
        }
    }
}

void maskLoadStore(ExecState &s)
{
    // vlm.v / vsm.v: EEW = 8, evl = ceil(vl / 8), always unmasked.
    s.requireVtype();
    const u64 evl = (s.vl + 7) / 8;
    const u64 base = s.gpr(s.field(kRs1));
    const unsigned reg = s.vd();
    for (u64 i = s.vstart; i < evl; ++i)
    {
        const u64 address = base + i;
        if (s.info.isLoad())
        {
            u64 value = 0;
            if (!loadElement(s, address, 1, value))
                accessFault(s, address);
            s.write(reg, i, 8, value);
        }
        else
        {
            stageStore(s, address, 1, s.read(reg, i, 8));
        }
    }
    if (s.info.isLoad() && config().agnosticFillOnes && s.vstart < evl)
    {
        for (u64 i = evl; i < s.vlenb; ++i)
            s.write(reg, i, 8, 0xFF);
    }
}

} // namespace

void executeMemory(ExecState &s)
{
    const unsigned mop = (s.word >> 26) & 3U;
    const unsigned lumop = (s.word >> 20) & 31U;
    const unsigned eew = widthToEew(s.info.funct3);
    const bool load = s.info.isLoad();
    s.require(eew <= config().elen(),
              "EEW = " + std::to_string(eew) + " exceeds ELEN = " + std::to_string(config().elen()));

    if (mop == 0 && lumop == kLumopWholeRegister)
    {
        wholeRegister(s, eew);
        return;
    }
    if (mop == 0 && lumop == kLumopMask)
    {
        maskLoadStore(s);
        return;
    }

    s.requireVtype();
    const bool indexed = mop == 1 || mop == 3;
    const bool strided = mop == 2;
    const bool faultOnlyFirst = mop == 0 && lumop == kLumopFaultOnlyFirst;
    (void)kLumopUnit;

    // Unit-stride/strided data use EEW from the instruction; indexed data use SEW and the
    // instruction's EEW applies to the index vector.
    const unsigned dataEew = indexed ? s.sew : eew;
    const Group data = s.group(s.vd(), dataEew);
    s.checkGroup(data, load ? "vd" : "vs3");
    const unsigned nfields = s.info.nfields;
    const unsigned fieldRegs = groupRegisters(data);
    s.require(nfields * fieldRegs <= 8, "NFIELDS * EMUL must not exceed 8");
    s.require(data.reg + nfields * fieldRegs <= kNumVectorRegisters,
              "segment register groups extend past v31");

    auto fieldGroup = [&](unsigned f)
    { return Group{data.reg + f * fieldRegs, dataEew, data.emulLog2}; };

    Group index{};
    if (indexed)
    {
        index = s.group(s.vs2(), eew);
        s.checkGroup(index, "vs2");
    }
    if (load)
    {
        for (unsigned f = 0; f < nfields; ++f)
        {
            const Group g = fieldGroup(f);
            s.checkMaskOverlap(g);
            if (indexed)
            {
                if (nfields > 1)
                    s.forbidOverlap(g, index, "the index register group vs2");
                else
                    s.checkOverlap(g, index, "the index register group vs2");
            }
        }
    }

    const u64 base = s.gpr(s.field(kRs1));
    const int64_t stride = strided ? static_cast<int64_t>(s.gpr(s.field(kRs2))) : 0;
    const unsigned bytes = dataEew / 8;
    u64 vlAfter = s.vl;

    for (u64 i = s.vstart; i < s.vl && i < vlAfter; ++i)
    {
        if (!s.active(i))
        {
            if (load)
                for (unsigned f = 0; f < nfields; ++f)
                    s.inactive(fieldGroup(f), i);
            continue;
        }
        for (unsigned f = 0; f < nfields; ++f)
        {
            u64 address = 0;
            if (indexed)
                address = base + s.read(index.reg, i, eew) + f * bytes;
            else if (strided)
                address = base + static_cast<u64>(stride) * i + f * bytes;
            else
                address = base + (i * nfields + f) * bytes;

            if (load)
            {
                u64 value = 0;
                if (!loadElement(s, address, bytes, value))
                {
                    if (faultOnlyFirst && i > 0)
                    {
                        vlAfter = i; // trim vl instead of trapping
                        break;
                    }
                    accessFault(s, address);
                }
                s.write(fieldGroup(f).reg, i, dataEew, value);
            }
            else
            {
                stageStore(s, address, bytes, s.read(fieldGroup(f).reg, i, dataEew));
            }
        }
    }

    if (vlAfter != s.vl)
    {
        s.newVl = vlAfter;
    }
    if (load)
    {
        for (unsigned f = 0; f < nfields; ++f)
            s.tailFrom(fieldGroup(f), vlAfter);
    }
}

} // namespace detail
} // namespace rvv
} // namespace Kites
