/**
 * @file v_formats.cpp
 * @brief Parsing of RVV 1.0 vector instructions and of the CSR pseudo-instructions.
 *
 * Vector instructions are not parsed with one function per syntax form. Instead, each mnemonic's
 * operand list comes from the vector ISA table, and a single matcher checks the tokens of the
 * line against it and encodes the instruction word directly.
 */
#include "assembler/parser.h"
#include "common/instructions.h"
#include "processor/registers.h"
#include "processor/vector/vector_isa.h"
#include "processor/vector/vector_unit.h"
#include "processor/vector/vtype.h"
#include "utils/utils.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Kites
{
namespace
{

uint32_t registerNumber(const std::string &name)
{
    // Canonical names are x<n>, f<n> or v<n>.
    return static_cast<uint32_t>(std::stoul(name.substr(1)));
}

bool isSingle(const std::vector<Token> &group, TokenType type)
{
    return group.size() == 1 && group[0].type == type;
}

/// Parses "e32[, m2][, ta|tu][, ma|mu]" from operand groups; returns false if malformed.
bool parseVtypeKeywords(const std::vector<std::vector<Token>> &groups, size_t first,
                        uint64_t &vtype)
{
    size_t g = first;
    auto next = [&](std::string &value)
    {
        if (g >= groups.size() || !isSingle(groups[g], TokenType::VTYPE_FIELD))
            return false;
        value = groups[g][0].value;
        return true;
    };

    std::string value;
    if (!next(value) || value[0] != 'e')
        return false;
    const unsigned sew = static_cast<unsigned>(std::stoul(value.substr(1)));
    ++g;

    int lmulLog2 = 0;
    if (g < groups.size() && next(value) && value[0] == 'm' && value != "ma" && value != "mu")
    {
        if (value[1] == 'f')
            lmulLog2 = -static_cast<int>(value[2] == '2' ? 1 : value[2] == '4' ? 2 : 3);
        else
            lmulLog2 = value[1] == '1' ? 0 : value[1] == '2' ? 1 : value[1] == '4' ? 2 : 3;
        ++g;
    }
    bool tailAgnostic = false;
    if (g < groups.size() && next(value) && (value == "ta" || value == "tu"))
    {
        tailAgnostic = value == "ta";
        ++g;
    }
    bool maskAgnostic = false;
    if (g < groups.size() && next(value) && (value == "ma" || value == "mu"))
    {
        maskAgnostic = value == "ma";
        ++g;
    }
    if (g != groups.size())
        return false;
    vtype = rvv::encodeVtype(sew, lmulLog2, tailAgnostic, maskAgnostic);
    return true;
}

} // namespace

std::vector<std::vector<Token>> Parser::operandGroups()
{
    std::vector<std::vector<Token>> groups;
    std::vector<Token> current;
    bool any = false;
    const unsigned line = currentToken().line_number;
    for (int k = 1;; ++k)
    {
        const Token t = peekToken(k);
        if (t.type == TokenType::EOF_ || t.line_number != line)
            break;
        any = true;
        if (t.type == TokenType::COMMA)
        {
            groups.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(t);
        }
    }
    if (any)
        groups.push_back(current);
    return groups;
}

void Parser::emitEncoded(const std::string &mnemonic, uint32_t word)
{
    ICUnit block;
    block.setOpcode(mnemonic);
    block.setLineNumber(currentToken().line_number);
    block.setInstructionIndex(instruction_index_);
    block.setMachineCode(word);
    intermediate_code_.emplace_back(block, true);
    instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
    instruction_index_++;
}

Parser::VectorEncodeResult Parser::encodeVector(const rvv::InsnInfo &info,
                                                const std::vector<std::vector<Token>> &groups,
                                                uint32_t &word)
{
    word = info.match;
    size_t g = 0;

    for (const rvv::Operand &op : info.operands)
    {
        if (op.kind == rvv::OperandKind::OptionalMask)
        {
            if (g < groups.size())
            {
                if (!isSingle(groups[g], TokenType::VEC_MASK))
                    return VectorEncodeResult::SyntaxError;
                word = rvv::setField(word, rvv::kVm, 0);
                ++g;
            }
            else
            {
                word = rvv::setField(word, rvv::kVm, 1);
            }
            continue;
        }

        if (op.kind == rvv::OperandKind::VtypeImmediate)
        {
            uint64_t vtype = 0;
            const uint64_t limit = op.field == rvv::kZimm11 ? 2047 : 1023;
            if (g + 1 == groups.size() && isSingle(groups[g], TokenType::NUM))
            {
                const int64_t raw = std::stoll(groups[g][0].value);
                if (raw < 0 || static_cast<uint64_t>(raw) > limit)
                    return VectorEncodeResult::SyntaxError;
                vtype = static_cast<uint64_t>(raw);
            }
            else if (!parseVtypeKeywords(groups, g, vtype))
            {
                return VectorEncodeResult::SyntaxError;
            }
            word = rvv::setField(word, op.field, static_cast<uint32_t>(vtype));
            g = groups.size();
            continue;
        }

        if (g >= groups.size())
            return VectorEncodeResult::SyntaxError;
        const std::vector<Token> &group = groups[g++];

        switch (op.kind)
        {
        case rvv::OperandKind::VectorRegister:
            if (!isSingle(group, TokenType::VEC_REGISTER))
                return VectorEncodeResult::SyntaxError;
            word = rvv::setField(word, op.field, registerNumber(group[0].value));
            break;
        case rvv::OperandKind::IntRegister:
            if (!isSingle(group, TokenType::GP_REGISTER))
                return VectorEncodeResult::SyntaxError;
            word = rvv::setField(word, op.field,
                                 registerNumber(reg_alias_to_name.at(group[0].value)));
            break;
        case rvv::OperandKind::FloatRegister:
            if (!isSingle(group, TokenType::FP_REGISTER))
                return VectorEncodeResult::SyntaxError;
            word = rvv::setField(word, op.field,
                                 registerNumber(reg_alias_to_name.at(group[0].value)));
            break;
        case rvv::OperandKind::Memory:
        {
            // "(rs1)" or "0(rs1)"
            size_t t = 0;
            if (group.size() == 4 && group[0].type == TokenType::NUM && group[0].value == "0")
                t = 1;
            if (group.size() != t + 3 || group[t].type != TokenType::LPAREN ||
                group[t + 1].type != TokenType::GP_REGISTER ||
                group[t + 2].type != TokenType::RPAREN)
                return VectorEncodeResult::SyntaxError;
            word = rvv::setField(word, op.field,
                                 registerNumber(reg_alias_to_name.at(group[t + 1].value)));
            break;
        }
        case rvv::OperandKind::Immediate:
        {
            if (!isSingle(group, TokenType::NUM))
                return VectorEncodeResult::SyntaxError;
            const int64_t value = std::stoll(group[0].value);
            int64_t min = 0;
            int64_t max = 0;
            rvv::immediateRange(info, op.field, min, max);
            if (value < min || value > max)
            {
                reportImmediateRange(group[0], min, max);
                return VectorEncodeResult::Reported;
            }
            word = rvv::setField(word, op.field, static_cast<uint32_t>(value));
            break;
        }
        case rvv::OperandKind::MaskV0:
            if (!isSingle(group, TokenType::VEC_REGISTER) || group[0].value != "v0")
                return VectorEncodeResult::SyntaxError;
            break;
        default:
            return VectorEncodeResult::SyntaxError;
        }
    }
    if (g != groups.size())
        return VectorEncodeResult::SyntaxError;

    if (!rvv::isImplemented(info))
    {
        const Token opcode = currentToken();
        recordError(ParseError(opcode.line_number,
                               "Vector instruction not supported yet: " + info.mnemonic));
        errors_.all_errors.emplace_back(errors::SyntaxError(
            "Unsupported instruction", info.mnemonic + " is not supported by this version of Kites",
            filename_, opcode.line_number, opcode.column_number,
            GetLineFromFile(filename_, opcode.line_number)));
        return VectorEncodeResult::Reported;
    }
    return VectorEncodeResult::Ok;
}

void Parser::reportImmediateRange(const Token &token, int64_t min, int64_t max)
{
    recordError(ParseError(token.line_number, "Immediate value out of range"));
    errors_.all_errors.emplace_back(errors::ImmediateOutOfRangeError(
        "Immediate value out of range",
        "Expected: " + std::to_string(min) + " <= imm <= " + std::to_string(max), filename_,
        token.line_number, token.column_number, GetLineFromFile(filename_, token.line_number)));
}

bool Parser::parse_vector()
{
    const rvv::InsnInfo *info = rvv::findByMnemonic(currentToken().value);
    if (info == nullptr)
        return false;
    uint32_t word = 0;
    switch (encodeVector(*info, operandGroups(), word))
    {
    case VectorEncodeResult::SyntaxError:
        return false;
    case VectorEncodeResult::Reported:
        skipCurrentLine();
        return true;
    case VectorEncodeResult::Ok:
        break;
    }
    emitEncoded(info->mnemonic, word);
    skipCurrentLine();
    return true;
}

bool Parser::parse_vector_pseudo()
{
    const Token opcode = currentToken();
    const std::string &name = opcode.value;
    const std::vector<std::vector<Token>> groups = operandGroups();

    using Groups = std::vector<std::vector<Token>>;
    auto token = [&](TokenType type, const std::string &value)
    { return std::vector<Token>{Token(type, value, opcode.line_number, opcode.column_number)}; };
    const std::vector<Token> x0 = token(TokenType::GP_REGISTER, "x0");
    const std::vector<Token> v0 = token(TokenType::VEC_REGISTER, "v0");
    const bool masked = !groups.empty() && isSingle(groups.back(), TokenType::VEC_MASK);
    const size_t plain = groups.size() - (masked ? 1 : 0); // operands before the mask
    auto withMask = [&](Groups operands)
    {
        if (masked)
            operands.push_back(groups.back());
        return operands;
    };
    auto isVector = [&](size_t i)
    { return i < plain && isSingle(groups[i], TokenType::VEC_REGISTER); };

    std::vector<std::pair<std::string, Groups>> expansion;

    // Single-instruction aliases: vd, vs[, v0.t]
    static const std::unordered_map<std::string, std::string> unaryAliases = {
        {"vneg.v", "vrsub.vx"},     {"vnot.v", "vxor.vi"},     {"vwcvt.x.x.v", "vwadd.vx"},
        {"vwcvtu.x.x.v", "vwaddu.vx"}, {"vncvt.x.x.w", "vnsrl.wx"}, {"vfneg.v", "vfsgnjn.vv"},
        {"vfabs.v", "vfsgnjx.vv"},  {"vmmv.m", "vmand.mm"},    {"vmnot.m", "vmnand.mm"}};
    // Operand-swapping compares: vd, va, vb[, v0.t] -> vd, vb, va
    static const std::unordered_map<std::string, std::string> swappedCompares = {
        {"vmsgt.vv", "vmslt.vv"}, {"vmsgtu.vv", "vmsltu.vv"}, {"vmsge.vv", "vmsle.vv"},
        {"vmsgeu.vv", "vmsleu.vv"}, {"vmfgt.vv", "vmflt.vv"}, {"vmfge.vv", "vmfle.vv"}};

    if (auto it = unaryAliases.find(name); it != unaryAliases.end())
    {
        if (plain != 2 || !isVector(0) || !isVector(1))
            return false;
        const std::string &real = it->second;
        Groups operands = {groups[0], groups[1]};
        if (real.back() == 'x') // .vx / .wx forms take x0
            operands.push_back(x0);
        else if (real == "vxor.vi")
            operands.push_back(token(TokenType::NUM, "-1"));
        else // vs, vs
            operands.push_back(groups[1]);
        if (masked && (name == "vmmv.m" || name == "vmnot.m"))
            return false;
        expansion.push_back({real, withMask(operands)});
    }
    else if (name == "vmclr.m" || name == "vmset.m")
    {
        if (groups.size() != 1 || !isVector(0))
            return false;
        expansion.push_back({name == "vmclr.m" ? "vmxor.mm" : "vmxnor.mm",
                             {groups[0], groups[0], groups[0]}});
    }
    else if (auto it = swappedCompares.find(name); it != swappedCompares.end())
    {
        if (plain != 3 || !isVector(0) || !isVector(1) || !isVector(2))
            return false;
        expansion.push_back({it->second, withMask({groups[0], groups[2], groups[1]})});
    }
    else if (name == "vmslt.vi" || name == "vmsltu.vi" || name == "vmsge.vi" || name == "vmsgeu.vi")
    {
        if (plain != 3 || !isVector(0) || !isVector(1) || !isSingle(groups[2], TokenType::NUM))
            return false;
        const bool isUnsigned = name[5] == 'u';
        const bool lessThan = name.find("lt") != std::string::npos;
        const int64_t imm = std::stoll(groups[2][0].value);
        const int64_t min = isUnsigned ? 0 : -15;
        if (imm < min || imm > 16)
        {
            reportImmediateRange(groups[2][0], min, 16);
            skipCurrentLine();
            return true;
        }
        if (isUnsigned && imm == 0)
        {
            // x <u 0 is always false; x >=u 0 is always true.
            expansion.push_back(
                {lessThan ? "vmsne.vv" : "vmseq.vv", withMask({groups[0], groups[1], groups[1]})});
        }
        else
        {
            const std::string real = std::string(lessThan ? "vmsle" : "vmsgt") +
                                     (isUnsigned ? "u" : "") + ".vi";
            expansion.push_back({real, withMask({groups[0], groups[1],
                                                 token(TokenType::NUM, std::to_string(imm - 1))})});
        }
    }
    else if (name == "vmsge.vx" || name == "vmsgeu.vx")
    {
        const std::string slt = name == "vmsge.vx" ? "vmslt.vx" : "vmsltu.vx";
        const size_t count = groups.size();
        if (count < 3 || !isVector(0) || !isVector(1) ||
            !isSingle(groups[2], TokenType::GP_REGISTER))
            return false;
        const std::vector<Token> &vd = groups[0];
        if (count == 3)
        {
            // vmslt vd, va, x; vmnand.mm vd, vd, vd
            expansion.push_back({slt, {vd, groups[1], groups[2]}});
            expansion.push_back({"vmnand.mm", {vd, vd, vd}});
        }
        else if (count == 4 && masked)
        {
            if (vd[0].value == "v0")
                return false; // needs the temporary-register form
            expansion.push_back({slt, {vd, groups[1], groups[2], groups[3]}});
            expansion.push_back({"vmxor.mm", {vd, vd, v0}});
        }
        else if (count == 5 && isSingle(groups[3], TokenType::VEC_MASK) &&
                 isSingle(groups[4], TokenType::VEC_REGISTER))
        {
            const std::vector<Token> &vt = groups[4];
            expansion.push_back({slt, {vt, groups[1], groups[2]}});
            if (vd[0].value == "v0")
            {
                expansion.push_back({"vmandn.mm", {vd, vd, vt}});
            }
            else
            {
                expansion.push_back({"vmandn.mm", {vt, v0, vt}});
                expansion.push_back({"vmandn.mm", {vd, vd, v0}});
                expansion.push_back({"vmor.mm", {vd, vt, vd}});
            }
        }
        else
        {
            return false;
        }
    }
    else if (name == "vl1r.v" || name == "vl2r.v" || name == "vl4r.v" || name == "vl8r.v")
    {
        expansion.push_back({"vl" + name.substr(2, 1) + "re8.v", groups});
    }
    else
    {
        return false;
    }

    // Encode every instruction first, so a bad line produces no partial output.
    std::vector<std::pair<std::string, uint32_t>> words;
    for (const auto &[mnemonic, operands] : expansion)
    {
        const rvv::InsnInfo *info = rvv::findByMnemonic(mnemonic);
        uint32_t word = 0;
        switch (encodeVector(*info, operands, word))
        {
        case VectorEncodeResult::SyntaxError:
            return false;
        case VectorEncodeResult::Reported:
            skipCurrentLine();
            return true;
        case VectorEncodeResult::Ok:
            words.emplace_back(mnemonic, word);
            break;
        }
    }
    for (const auto &[mnemonic, word] : words)
        emitEncoded(mnemonic, word);
    skipCurrentLine();
    return true;
}

bool Parser::parse_csr_pseudo()
{
    const Token opcode = currentToken();
    const std::string &name = opcode.value;
    const std::vector<std::vector<Token>> groups = operandGroups();
    if (groups.size() != 2)
        return false;

    ICUnit block;
    block.setLineNumber(opcode.line_number);
    block.setInstructionIndex(instruction_index_);

    if (name == "csrr")
    {
        // csrr rd, csr  ->  csrrs rd, csr, x0
        if (!isSingle(groups[0], TokenType::GP_REGISTER) ||
            !isSingle(groups[1], TokenType::CSR_REGISTER))
            return false;
        block.setOpcode("csrrs");
        block.setRd(reg_alias_to_name.at(groups[0][0].value));
        block.setCsr(csr_to_address.at(groups[1][0].value));
        block.setRs1("x0");
    }
    else
    {
        if (!isSingle(groups[0], TokenType::CSR_REGISTER))
            return false;
        block.setRd("x0");
        block.setCsr(csr_to_address.at(groups[0][0].value));
        if (name.back() == 'i')
        {
            // csrwi/csrsi/csrci csr, uimm5  ->  csrrwi/csrrsi/csrrci x0, csr, uimm5
            if (!isSingle(groups[1], TokenType::NUM))
                return false;
            const int64_t imm = std::stoll(groups[1][0].value);
            if (imm < 0 || imm > 31)
            {
                recordError(ParseError(opcode.line_number, "Immediate value out of range"));
                errors_.all_errors.emplace_back(errors::ImmediateOutOfRangeError(
                    "Immediate value out of range", "Expected: 0 <= imm <= 31", filename_,
                    groups[1][0].line_number, groups[1][0].column_number,
                    GetLineFromFile(filename_, opcode.line_number)));
                skipCurrentLine();
                return true;
            }
            block.setOpcode("csrr" + name.substr(3)); // csrwi -> csrrwi, ...
            block.setImm(std::to_string(imm));
        }
        else
        {
            // csrw/csrs/csrc csr, rs  ->  csrrw/csrrs/csrrc x0, csr, rs
            if (!isSingle(groups[1], TokenType::GP_REGISTER))
                return false;
            block.setOpcode("csrr" + name.substr(3)); // csrw -> csrrw, ...
            block.setRs1(reg_alias_to_name.at(groups[1][0].value));
        }
    }

    intermediate_code_.emplace_back(block, true);
    instruction_number_line_number_mapping_[instruction_index_] = block.getLineNumber();
    instruction_index_++;
    skipCurrentLine();
    return true;
}

} // namespace Kites
