/**
 * @file vector_isa.cpp
 * @brief Table-driven description of the RVV 1.0 instruction set.
 */
#include "processor/vector/vector_isa.h"

#include "processor/vector/vtype.h"

#include <array>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace Kites
{
namespace rvv
{

namespace
{

struct RawEntry
{
    const char *name;
    uint32_t match;
    uint32_t mask;
    uint32_t fields;
};

// Encodings generated from riscv-opcodes; see tools/vector/gen_vector_isa.py.
constexpr RawEntry kRawEntries[] = {
#define KITES_RVV_INSN(name, match, mask, fields) {name, match, mask, fields},
#include "processor/vector/vector_isa_table.inc"
#undef KITES_RVV_INSN
};

struct FieldPosition
{
    uint32_t field;
    unsigned hi;
    unsigned lo;
};

constexpr std::array<FieldPosition, 13> kFieldPositions = {{
    {kVd, 11, 7},
    {kVs3, 11, 7},
    {kRd, 11, 7},
    {kVs1, 19, 15},
    {kVs2, 24, 20},
    {kRs1, 19, 15},
    {kRs2, 24, 20},
    {kVm, 25, 25},
    {kNf, 31, 29},
    {kSimm5, 19, 15},
    {kZimm5, 19, 15},
    {kZimm10, 29, 20},
    {kZimm11, 30, 20},
}};

constexpr uint32_t kNfMask = 0xE0000000U;

const FieldPosition &position(uint32_t field)
{
    for (const FieldPosition &p : kFieldPositions)
    {
        if (p.field == field)
        {
            return p;
        }
    }
    return kFieldPositions[0];
}

bool startsWith(const std::string &s, std::string_view prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

// Multiply-add instructions list the multiplier (vs1/rs1/fs1) before vs2 in assembly.
bool isMultiplyAdd(const std::string &base)
{
    static const std::unordered_set<std::string> names = {
        "vmacc",   "vnmsac",  "vmadd",   "vnmsub",   "vwmaccu", "vwmacc",  "vwmaccsu",
        "vwmaccus", "vfmacc", "vfnmacc", "vfmsac",   "vfnmsac", "vfmadd",  "vfnmadd",
        "vfmsub",  "vfnmsub", "vfwmacc", "vfwnmacc", "vfwmsac", "vfwnmsac"};
    return names.count(base) != 0;
}

// Name of the NFIELDS = k segment variant of a unit-stride, strided or indexed load/store.
std::string segmentMnemonic(const std::string &m, unsigned k)
{
    const std::string ks = std::to_string(k);
    if (startsWith(m, "vlse"))
        return "vlsseg" + ks + "e" + m.substr(4);
    if (startsWith(m, "vsse"))
        return "vssseg" + ks + "e" + m.substr(4);
    if (startsWith(m, "vle"))
        return "vlseg" + ks + "e" + m.substr(3);
    if (startsWith(m, "vse"))
        return "vsseg" + ks + "e" + m.substr(3);
    for (const char *prefix : {"vlux", "vlox", "vsux", "vsox"})
    {
        if (startsWith(m, std::string(prefix) + "ei"))
            return std::string(prefix) + "seg" + ks + "ei" + m.substr(6);
    }
    return m + ".seg" + ks; // not reached for rv_v
}

std::vector<Operand> buildOperands(const InsnInfo &in)
{
    std::vector<Operand> ops;
    if (in.isConfig())
    {
        if (in.mnemonic == "vsetvli")
            return {{OperandKind::IntRegister, kRd},
                    {OperandKind::IntRegister, kRs1},
                    {OperandKind::VtypeImmediate, kZimm11}};
        if (in.mnemonic == "vsetivli")
            return {{OperandKind::IntRegister, kRd},
                    {OperandKind::Immediate, kZimm5},
                    {OperandKind::VtypeImmediate, kZimm10}};
        return {{OperandKind::IntRegister, kRd},
                {OperandKind::IntRegister, kRs1},
                {OperandKind::IntRegister, kRs2}};
    }

    if (in.isLoad() || in.isStore())
    {
        ops.push_back({OperandKind::VectorRegister, in.isLoad() ? kVd : kVs3});
        ops.push_back({OperandKind::Memory, kRs1});
        if (in.hasField(kRs2))
            ops.push_back({OperandKind::IntRegister, kRs2});
        if (in.hasField(kVs2))
            ops.push_back({OperandKind::VectorRegister, kVs2});
    }
    else
    {
        if (in.hasField(kVd))
            ops.push_back({OperandKind::VectorRegister, kVd});
        if (in.hasField(kRd))
            ops.push_back({in.funct3 == kOpFVV ? OperandKind::FloatRegister
                                               : OperandKind::IntRegister,
                           kRd});

        bool hasSrc1 = true;
        Operand src1{OperandKind::VectorRegister, kVs1};
        if (in.hasField(kRs1))
            src1 = {in.funct3 == kOpFVF ? OperandKind::FloatRegister : OperandKind::IntRegister,
                    kRs1};
        else if (in.hasField(kSimm5))
            src1 = {OperandKind::Immediate, kSimm5};
        else if (in.hasField(kZimm5))
            src1 = {OperandKind::Immediate, kZimm5};
        else if (!in.hasField(kVs1))
            hasSrc1 = false;

        const Operand src2{OperandKind::VectorRegister, kVs2};
        const bool hasSrc2 = in.hasField(kVs2);

        if (isMultiplyAdd(in.base))
        {
            if (hasSrc1)
                ops.push_back(src1);
            if (hasSrc2)
                ops.push_back(src2);
        }
        else
        {
            if (hasSrc2)
                ops.push_back(src2);
            if (hasSrc1)
                ops.push_back(src1);
        }
    }

    const bool vmFixed = ((in.mask >> 25) & 1U) != 0;
    if (in.hasField(kVm))
        ops.push_back({OperandKind::OptionalMask, kVm});
    else if (vmFixed && ((in.match >> 25) & 1U) == 0)
        ops.push_back({OperandKind::MaskV0, kVm});
    return ops;
}

InsnInfo makeInfo(const std::string &mnemonic, uint32_t match, uint32_t mask, uint32_t fields)
{
    InsnInfo in;
    in.mnemonic = mnemonic;
    const size_t dot = mnemonic.find('.');
    in.base = mnemonic.substr(0, dot);
    in.suffix = dot == std::string::npos ? "" : mnemonic.substr(dot + 1);
    in.match = match;
    in.mask = mask;
    in.fields = fields;
    in.opcode = match & 0x7FU;
    in.funct3 = (match >> 12) & 7U;
    in.nfields = 1;
    return in;
}

struct Tables
{
    std::vector<InsnInfo> entries;
    std::unordered_map<std::string, size_t> byName;
};

const Tables &tables()
{
    static const Tables t = []
    {
        Tables built;
        for (const RawEntry &raw : kRawEntries)
        {
            const bool segmentable = (raw.fields & kNf) != 0;
            if (!segmentable)
            {
                built.entries.push_back(makeInfo(raw.name, raw.match, raw.mask, raw.fields));
                continue;
            }
            // nf = 0 is the plain form; nf = k-1 is the NFIELDS = k segment form.
            for (unsigned k = 1; k <= 8; ++k)
            {
                const std::string name = k == 1 ? raw.name : segmentMnemonic(raw.name, k);
                InsnInfo in = makeInfo(name, raw.match | ((k - 1) << 29), raw.mask | kNfMask,
                                       raw.fields & ~static_cast<uint32_t>(kNf));
                in.nfields = k;
                built.entries.push_back(in);
            }
        }
        for (size_t i = 0; i < built.entries.size(); ++i)
        {
            built.entries[i].operands = buildOperands(built.entries[i]);
            built.byName.emplace(built.entries[i].mnemonic, i);
        }
        return built;
    }();
    return t;
}

std::string regName(char prefix, uint32_t index)
{
    return std::string(1, prefix) + std::to_string(index);
}

int64_t signExtend5(uint32_t value)
{
    return (value & 0x10U) != 0 ? static_cast<int64_t>(value) - 32 : static_cast<int64_t>(value);
}

} // namespace

const std::vector<InsnInfo> &instructionTable()
{
    return tables().entries;
}

const InsnInfo *findByMnemonic(std::string_view mnemonic)
{
    const Tables &t = tables();
    auto it = t.byName.find(std::string(mnemonic));
    return it == t.byName.end() ? nullptr : &t.entries[it->second];
}

bool isVectorMnemonic(std::string_view mnemonic)
{
    return findByMnemonic(mnemonic) != nullptr;
}

bool isVectorInstruction(uint32_t word)
{
    const uint32_t opcode = word & 0x7FU;
    if (opcode == kOpcodeOpV)
    {
        return true;
    }
    if (opcode == kOpcodeLoadFp || opcode == kOpcodeStoreFp)
    {
        // width 000/101/110/111 are vector element widths; 001..100 are scalar FP accesses.
        const uint32_t width = (word >> 12) & 7U;
        return width == 0 || width >= 5;
    }
    return false;
}

const InsnInfo *decode(uint32_t word)
{
    for (const InsnInfo &in : tables().entries)
    {
        if ((word & in.mask) == in.match)
        {
            return &in;
        }
    }
    return nullptr;
}

uint32_t getField(uint32_t word, uint32_t field)
{
    const FieldPosition &p = position(field);
    const uint32_t width = p.hi - p.lo + 1;
    return (word >> p.lo) & ((1U << width) - 1U);
}

uint32_t setField(uint32_t word, uint32_t field, uint32_t value)
{
    const FieldPosition &p = position(field);
    const uint32_t width = p.hi - p.lo + 1;
    const uint32_t bits = ((1U << width) - 1U) << p.lo;
    return (word & ~bits) | ((value << p.lo) & bits);
}

std::string disassemble(uint32_t word)
{
    const InsnInfo *in = decode(word);
    if (in == nullptr)
    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "unknown(0x%08x)", word);
        return buf;
    }
    std::string text = in->mnemonic;
    bool first = true;
    auto append = [&](const std::string &operand)
    {
        text += first ? " " : ", ";
        text += operand;
        first = false;
    };
    for (const Operand &op : in->operands)
    {
        const uint32_t value = getField(word, op.field);
        switch (op.kind)
        {
        case OperandKind::VectorRegister:
            append(regName('v', value));
            break;
        case OperandKind::IntRegister:
            append(regName('x', value));
            break;
        case OperandKind::FloatRegister:
            append(regName('f', value));
            break;
        case OperandKind::Memory:
            append("(" + regName('x', value) + ")");
            break;
        case OperandKind::Immediate:
            append(std::to_string(op.field == kSimm5 ? signExtend5(value)
                                                     : static_cast<int64_t>(value)));
            break;
        case OperandKind::VtypeImmediate:
            append(vtypeToString(value));
            break;
        case OperandKind::MaskV0:
            append("v0");
            break;
        case OperandKind::OptionalMask:
            if (value == 0)
                append("v0.t");
            break;
        }
    }
    return text;
}

std::string expectedSyntax(const InsnInfo &info)
{
    std::string text = info.mnemonic;
    bool first = true;
    auto append = [&](const std::string &operand)
    {
        text += first ? " " : ", ";
        text += operand;
        first = false;
    };
    for (const Operand &op : info.operands)
    {
        switch (op.kind)
        {
        case OperandKind::VectorRegister:
            append(op.field == kVd ? "vd" : op.field == kVs3 ? "vs3" : op.field == kVs1 ? "vs1" : "vs2");
            break;
        case OperandKind::IntRegister:
            append(op.field == kRd ? "rd" : op.field == kRs1 ? "rs1" : "rs2");
            break;
        case OperandKind::FloatRegister:
            append(op.field == kRd ? "fd" : "fs1");
            break;
        case OperandKind::Memory:
            append("(rs1)");
            break;
        case OperandKind::Immediate:
            append(op.field == kSimm5 ? "simm5" : "uimm5");
            break;
        case OperandKind::VtypeImmediate:
            append("e<sew>, m<lmul>[, ta|tu][, ma|mu]");
            break;
        case OperandKind::MaskV0:
            append("v0");
            break;
        case OperandKind::OptionalMask:
            text += "[, v0.t]";
            break;
        }
    }
    return text;
}

namespace
{
const std::vector<std::pair<std::string, std::string>> &pseudoTable()
{
    static const std::vector<std::pair<std::string, std::string>> table = {
        {"vneg.v", "vd, vs[, v0.t]"},       {"vnot.v", "vd, vs[, v0.t]"},
        {"vwcvt.x.x.v", "vd, vs[, v0.t]"},  {"vwcvtu.x.x.v", "vd, vs[, v0.t]"},
        {"vncvt.x.x.w", "vd, vs[, v0.t]"},  {"vfneg.v", "vd, vs[, v0.t]"},
        {"vfabs.v", "vd, vs[, v0.t]"},      {"vmmv.m", "vd, vs"},
        {"vmnot.m", "vd, vs"},              {"vmclr.m", "vd"},
        {"vmset.m", "vd"},                  {"vmsgt.vv", "vd, va, vb[, v0.t]"},
        {"vmsgtu.vv", "vd, va, vb[, v0.t]"}, {"vmsge.vv", "vd, va, vb[, v0.t]"},
        {"vmsgeu.vv", "vd, va, vb[, v0.t]"}, {"vmfgt.vv", "vd, va, vb[, v0.t]"},
        {"vmfge.vv", "vd, va, vb[, v0.t]"}, {"vmslt.vi", "vd, va, imm[, v0.t]"},
        {"vmsltu.vi", "vd, va, imm[, v0.t]"}, {"vmsge.vi", "vd, va, imm[, v0.t]"},
        {"vmsgeu.vi", "vd, va, imm[, v0.t]"}, {"vmsge.vx", "vd, va, rs1[, v0.t[, vt]]"},
        {"vmsgeu.vx", "vd, va, rs1[, v0.t[, vt]]"}, {"vl1r.v", "vd, (rs1)"},
        {"vl2r.v", "vd, (rs1)"},            {"vl4r.v", "vd, (rs1)"},
        {"vl8r.v", "vd, (rs1)"},
    };
    return table;
}
} // namespace

const std::vector<std::string> &pseudoMnemonics()
{
    static const std::vector<std::string> names = []
    {
        std::vector<std::string> out;
        for (const auto &[name, syntax] : pseudoTable())
            out.push_back(name);
        return out;
    }();
    return names;
}

bool isVectorPseudo(std::string_view mnemonic)
{
    return !pseudoSyntax(mnemonic).empty();
}

std::string pseudoSyntax(std::string_view mnemonic)
{
    for (const auto &[name, syntax] : pseudoTable())
        if (name == mnemonic)
            return name + " " + syntax;
    return "";
}

void immediateRange(const InsnInfo &info, uint32_t field, int64_t &min, int64_t &max)
{
    (void)info;
    if (field == kSimm5)
    {
        min = -16;
        max = 15;
    }
    else
    {
        min = 0;
        max = 31;
    }
}

} // namespace rvv
} // namespace Kites
