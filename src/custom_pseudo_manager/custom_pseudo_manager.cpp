#include "custom_pseudo_manager.h"
#include "common/globals.h"
#include "processor/vector/vector_isa.h"
#include "processor/vector/vtype.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <set>
#include <unordered_map>
namespace Kites
{
namespace customPseudoManager
{
namespace
{
/**
 * Splits operands separated by commas and/or whitespace ("a, b", "a,b", "a b"). Returns false
 * for an empty operand (consecutive, leading or trailing commas).
 */
bool splitOperands(const QString &text, QStringList &out)
{
    out.clear();
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return true;
    static const QRegularExpression separator(R"(\s*,\s*|\s+)");
    for (const QString &piece : trimmed.split(separator, Qt::KeepEmptyParts))
    {
        if (piece.isEmpty())
            return false;
        out << piece;
    }
    return true;
}

} // namespace

bool addCustomPseudoInstruction(const QString &pseudoInst,
                                const QString &expansion,
                                QString &errorMessage, bool isUpdate)
{
    // we will add the new pseudo instruction and its expansion to the custom pseudo instruction
    // list for now we will just return true to indicate that the pseudo instruction was added
    // successfully in future we will implement the actual logic to store and manage custom pseudo
    // instructions
    ParsedPseudoInstruction newInstruction;
    if (!parse(newInstruction, pseudoInst, expansion, errorMessage))
    {
        return false;
    }
    if (!validatePseudoInstruction(newInstruction, errorMessage))
    {
        return false;
    }

    QJsonObject root = loadInstructionsFromDisk(errorMessage);

    if (!isUpdate && root.contains(newInstruction.name))
    {
        errorMessage = QString("Instruction \"%1\" already exists. Use update instead.")
                           .arg(newInstruction.name);
        return false;
    }
    else if (isUpdate)
    {
        // remove the old instruction and add the updated one
        root.remove(newInstruction.name);
    }

    QJsonObject instructionObject;
    instructionObject["args"] = QJsonArray::fromStringList(newInstruction.args);
    instructionObject["expansion"] = QJsonArray::fromStringList(newInstruction.expansion);
    root[newInstruction.name] = instructionObject;
    return saveToDisk(root, errorMessage);
}

bool parse(ParsedPseudoInstruction &parsedInst, const QString &pseudoInst,
                                const QString &expansion, QString &errorMessage)
{

    const QStringList tokens = pseudoInst.split(QRegularExpression("[\\s,]"), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
    {
        errorMessage = "Pseudo instruction cannot be empty.";
        return false;
    }
    parsedInst.name = tokens.first();

    // Accept arguments separated by spaces and/or commas (commas may
    // optionally be followed by a space). Example accepted forms:
    //   "r1 r2"  "r1, r2"  "r1,r2"  "r1, r2,r3"
    QStringList args;
    const QStringList rawArgs = tokens.mid(1);
    for (const QString &raw : rawArgs)
    {
        const QStringList pieces = raw.split(',', Qt::KeepEmptyParts);
        for (QString p : pieces)
        {
            p = p.trimmed();
            if (p.isEmpty())
            {
                errorMessage = "Invalid pseudo instruction: empty argument (consecutive commas?)";
                return false;
            }
            args << p;
        }
    }
    parsedInst.args = args;

    for (const QString &line : expansion.split('\n', Qt::SkipEmptyParts))
    {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;

        // Normalize expansion line: accept commas between operands with or
        // without spaces and store a whitespace-separated normalized form.
        QStringList parts = trimmed.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        QStringList normalizedParts;
        for (const QString &part : parts)
        {
            const QStringList sub = part.split(',', Qt::KeepEmptyParts);
            for (QString s : sub)
            {
                s = s.trimmed();
                if (s.isEmpty())
                {
                    errorMessage = "Invalid expansion: empty operand (consecutive commas?)";
                    return false;
                }
                normalizedParts << s;
            }
        }
        parsedInst.expansion << normalizedParts.join(' ');
    }

    return true;
}

bool validatePseudoInstruction(const ParsedPseudoInstruction &parsedInst,
                                                    QString &errorMessage)
{
    static const QRegularExpression validNameRegex("^[a-zA-Z_][a-zA-Z0-9_.]*$");

    if (!validNameRegex.match(parsedInst.name).hasMatch())
    {
        errorMessage = QString("Invalid instruction name \"%1\". "
                               "Must start with a letter and contain only "
                               "letters, digits, underscores, or dots.")
                           .arg(parsedInst.name);
        return false;
    }

    // only register arg of the form r1,r1....r32 allowed
    static const QRegularExpression validArg(R"(^r([1-9]|[1-2][0-9]|3[0-2])$)");
    for (const QString &arg : parsedInst.args)
    {
        if (!validArg.match(arg).hasMatch())
        {
            errorMessage = QString("Invalid argument \"%1\". "
                                   "Must be a valid register name (e.g., r0, r1, ..., r32).")
                               .arg(arg);
            return false;
        }
    }

    for (const QString &line : parsedInst.expansion)
    {
        if (line.isEmpty())
            continue;
        if (!isValidExpainsionLine(line, parsedInst.args, errorMessage))
        {
            return false;
        }
    }

    return true;
}

std::string expandPseudoInstruction(const std::string &source)
{
    std::vector<int> unusedMapping;
    return expandPseudoInstruction(source, unusedMapping);
}

std::string expandPseudoInstruction(const std::string &source,
                                     std::vector<int> &expandedLineToOriginalLine)
{
    expandedLineToOriginalLine.clear();

    QString errorMessage; // just a dummy variable
    QJsonObject pseudoInstrcutions = loadInstructionsFromDisk(errorMessage);

    if (pseudoInstrcutions.isEmpty())
    {
        const int lineCount = QString::fromStdString(source).count('\n') + 1;
        expandedLineToOriginalLine.reserve(lineCount);
        for (int i = 1; i <= lineCount; ++i)
        {
            expandedLineToOriginalLine.push_back(i);
        }
        return source;
    }

    const QString qSource = QString::fromStdString(source);
    const QStringList sourceLines = qSource.split('\n');
    QStringList expandedLines;

    for (int sourceLineIndex = 0; sourceLineIndex < sourceLines.size(); ++sourceLineIndex)
    {
        const QString &line = sourceLines[sourceLineIndex];
        const int originalLineNumber = sourceLineIndex + 1; // 1-based

        const QString trimmedLine = line.trimmed();

        if (trimmedLine.isEmpty())
        {
            expandedLines << line;
            expandedLineToOriginalLine.push_back(originalLineNumber);
            continue;
        }

        const QStringList tokens =
            trimmedLine.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        const QString instName = tokens.first();

        if (!pseudoInstrcutions.contains(instName))
        {
            expandedLines << line;
            expandedLineToOriginalLine.push_back(originalLineNumber);
            continue;
        }

        const QJsonObject instructionObject = pseudoInstrcutions[instName].toObject();
        const QJsonArray argsArray = instructionObject["args"].toArray();
        const QJsonArray expansionArray = instructionObject["expansion"].toArray();

        QStringList declaredArgs;

        // Arguments are separated by commas and/or spaces; an empty argument (consecutive
        // commas) is treated as a mismatch.
        const bool badArgs = !splitOperands(trimmedLine.mid(instName.size()), declaredArgs);

        if (badArgs || declaredArgs.size() != argsArray.size())
        {
            // argument count mismatch, treat as normal instruction
            expandedLines << line;
            expandedLineToOriginalLine.push_back(originalLineNumber);
            continue;
        }

        QHash<QString, QString> argMapping;
        for (int i = 0; i < argsArray.size(); i++)
        {
            argMapping[argsArray[i].toString()] = declaredArgs[i];
        }

        for (const QJsonValue &expansionLineval : expansionArray)
        {
            // Split expansion line on whitespace, then accept commas between
            // operands; detect empty pieces (consecutive commas) and treat
            // as invalid expansion (skip expansion).
            const QString expansionText = expansionLineval.toString().trimmed();
            const qsizetype firstSpace = expansionText.indexOf(QRegularExpression("\\s"));
            QStringList expansionLineTokens;
            bool badExpansion = false;
            if (firstSpace < 0)
            {
                expansionLineTokens << expansionText;
            }
            else
            {
                QStringList operands;
                badExpansion = !splitOperands(expansionText.mid(firstSpace), operands);
                expansionLineTokens << expansionText.left(firstSpace) << operands;
            }
            if (badExpansion)
            {
                // malformed expansion template; fall back to leaving source line as-is
                expandedLines << line;
                expandedLineToOriginalLine.push_back(originalLineNumber);
                break;
            }

            static const QRegularExpression memoryOperand(R"(^(.*)\((\w+)\)$)");
            for (QString &token : expansionLineTokens)
            {
                if (argMapping.contains(token))
                {
                    token = argMapping[token];
                }
                else if (const auto m = memoryOperand.match(token);
                         m.hasMatch() && argMapping.contains(m.captured(2)))
                {
                    // memory operand such as (r1) in a vector load or store
                    token = m.captured(1) + "(" + argMapping[m.captured(2)] + ")";
                }
            }

            if (expansionLineTokens.size() <= 1)
            {
                expandedLines << expansionLineTokens.join(' ');
            }
            else
            {
                const QString mnemonic = expansionLineTokens.first();
                const QStringList operands = expansionLineTokens.mid(1);
                expandedLines << QString("%1 %2").arg(mnemonic, operands.join(", "));
            }
            expandedLineToOriginalLine.push_back(originalLineNumber);
        }
    }

    return expandedLines.join('\n').toStdString();
}

namespace
{
/**
 * Vector instruction in an expansion: every operand is checked against the instruction's
 * operand list from the vector ISA table. Register operands may be a declared argument
 * (r1..r32) or a register of the right kind (vN, xN, fN); a memory operand is "(arg)" or
 * "(xN)"; immediates must be in range; vsetvli/vsetivli take vtype fields; "v0.t" is optional
 * where the instruction can be masked. Standard vector pseudo-instructions (vneg.v, ...) are
 * checked only for operand kinds.
 */
bool isValidVectorExpansionLine(const QStringList &parts, const QStringList &declaredArgs,
                                QString &errorMessage)
{
    static const QRegularExpression vectorRegister(R"(^v([0-9]|[1-2][0-9]|3[0-1])$)");
    static const QRegularExpression intRegister(R"(^x([0-9]|[1-2][0-9]|3[0-1])$)");
    static const QRegularExpression floatRegister(R"(^f([0-9]|[1-2][0-9]|3[0-1])$)");
    static const QRegularExpression memory(R"(^(0)?\((\w+)\)$)");

    const QString opcode = parts.first().toLower();
    const QStringList ops = parts.mid(1);
    auto isArg = [&](const QString &s) { return declaredArgs.contains(s); };
    auto isInteger = [](const QString &s)
    {
        bool ok = false;
        s.toLongLong(&ok, 0);
        return ok;
    };
    auto fail = [&](const QString &why)
    {
        errorMessage = QString("Invalid operands for \"%1\": %2").arg(opcode, why);
        return false;
    };

    const rvv::InsnInfo *info = rvv::findByMnemonic(opcode.toStdString());
    if (info == nullptr) // standard vector pseudo-instruction
    {
        for (const QString &op : ops)
        {
            if (!(isArg(op) || vectorRegister.match(op).hasMatch() ||
                  intRegister.match(op).hasMatch() || floatRegister.match(op).hasMatch() ||
                  op == "v0.t" || isInteger(op)))
                return fail(QString("\"%1\" is not a declared arg, register or integer").arg(op));
        }
        return true;
    }

    qsizetype k = 0;
    for (const rvv::Operand &operand : info->operands)
    {
        if (operand.kind == rvv::OperandKind::OptionalMask)
        {
            if (k < ops.size() && ops[k] == "v0.t")
                ++k;
            continue;
        }
        if (k >= ops.size())
            return fail(QString("expected %1")
                            .arg(QString::fromStdString(rvv::expectedSyntax(*info))));
        const QString &op = ops[k];
        switch (operand.kind)
        {
        case rvv::OperandKind::VectorRegister:
            if (!isArg(op) && !vectorRegister.match(op).hasMatch())
                return fail(QString("\"%1\" must be a declared arg or v0..v31").arg(op));
            break;
        case rvv::OperandKind::IntRegister:
            if (!isArg(op) && !intRegister.match(op).hasMatch())
                return fail(QString("\"%1\" must be a declared arg or x0..x31").arg(op));
            break;
        case rvv::OperandKind::FloatRegister:
            if (!isArg(op) && !floatRegister.match(op).hasMatch())
                return fail(QString("\"%1\" must be a declared arg or f0..f31").arg(op));
            break;
        case rvv::OperandKind::Memory:
        {
            const auto m = memory.match(op);
            if (!m.hasMatch() || !(isArg(m.captured(2)) || intRegister.match(m.captured(2)).hasMatch()))
                return fail(QString("\"%1\" must be (arg) or (xN)").arg(op));
            break;
        }
        case rvv::OperandKind::Immediate:
        {
            bool ok = false;
            const qlonglong value = op.toLongLong(&ok, 0);
            int64_t min = 0;
            int64_t max = 0;
            rvv::immediateRange(*info, operand.field, min, max);
            if (!ok || value < min || value > max)
                return fail(QString("\"%1\" must be an integer in [%2, %3]").arg(op).arg(min).arg(max));
            break;
        }
        case rvv::OperandKind::VtypeImmediate:
        {
            const qsizetype first = k;
            while (k < ops.size() && rvv::isVtypeKeyword(ops[k].toStdString()))
                ++k;
            if (k == first)
                return fail(QString("\"%1\" is not a vtype field (e32, m1, ta, ma, ...)").arg(op));
            continue; // k already advanced
        }
        case rvv::OperandKind::MaskV0:
            if (op != "v0")
                return fail("the selector/carry operand must be v0");
            break;
        default:
            break;
        }
        ++k;
    }
    if (k != ops.size())
        return fail(QString("too many operands; expected %1")
                        .arg(QString::fromStdString(rvv::expectedSyntax(*info))));
    return true;
}
} // namespace

bool isValidExpainsionLine(const QString &line,
                                                const QStringList &declaredArgs,
                                                QString &errorMessage)
{
    static std::unordered_map<QString, std::pair<int, std::set<int>>> instruction_to_operand_count =
        {{"add", {3, {}}},
         {"sub", {3, {}}},
         {"and", {3, {}}},
         {"or", {3, {}}},
         {"xor", {3, {}}},
         {"sll", {3, {}}},
         {"srl", {3, {}}},
         {"sra", {3, {}}},
         {"slt", {3, {}}},
         {"sltu", {3, {}}},
         {"addw", {3, {}}},
         {"subw", {3, {}}},
         {"sllw", {3, {}}},
         {"srlw", {3, {}}},
         {"sraw", {3, {}}},

         // I-type: rd, rs1, imm
         {"addi", {3, {2}}},
         {"xori", {3, {2}}},
         {"ori", {3, {2}}},
         {"andi", {3, {2}}},
         {"slli", {3, {2}}},
         {"srli", {3, {2}}},
         {"srai", {3, {2}}},
         {"slti", {3, {2}}},
         {"sltiu", {3, {2}}},
         {"addiw", {3, {2}}},
         {"slliw", {3, {2}}},
         {"srliw", {3, {2}}},
         {"sraiw", {3, {2}}},

         // Load: rd, imm(rs1) — treated as rd, rs1, imm
         {"lb", {3, {2}}},
         {"lh", {3, {2}}},
         {"lw", {3, {2}}},
         {"ld", {3, {2}}},
         {"lbu", {3, {2}}},
         {"lhu", {3, {2}}},
         {"lwu", {3, {2}}},

         // Store: rs2, imm(rs1) — treated as rs1, rs2, imm
         {"sb", {3, {2}}},
         {"sh", {3, {2}}},
         {"sw", {3, {2}}},
         {"sd", {3, {2}}},

         // Branch: rs1, rs2, imm
         {"beq", {3, {2}}},
         {"bne", {3, {2}}},
         {"blt", {3, {2}}},
         {"bge", {3, {2}}},
         {"bltu", {3, {2}}},
         {"bgeu", {3, {2}}},

         // U-type: rd, imm
         {"lui", {2, {1}}},
         {"auipc", {2, {1}}},

         // Jump
         {"jal", {2, {1}}},
         {"jalr", {3, {2}}},

         // No operands
         {"ecall", {0, {}}},
         {"ebreak", {0, {}}},
         {"nop", {0, {}}},
         {"ret", {0, {}}},
         {"fence", {0, {}}},
         {"fence_i", {0, {}}},

         // CSR: rd, csr, rs1
         {"csrrw", {3, {1}}},
         {"csrrs", {3, {1}}},
         {"csrrc", {3, {1}}},
         {"csrrwi", {3, {1, 2}}},
         {"csrrsi", {3, {1, 2}}},
         {"csrrci", {3, {1, 2}}},

         // Pseudos with operands
         {"la", {2, {1}}},
         {"li", {2, {1}}},
         {"mv", {2, {}}},
         {"not", {2, {}}},
         {"neg", {2, {}}},
         {"negw", {2, {}}},
         {"sext.w", {2, {}}},
         {"seqz", {2, {}}},
         {"snez", {2, {}}},
         {"sltz", {2, {}}},
         {"sgtz", {2, {}}},

         // Branch pseudos: rs, imm
         {"beqz", {2, {1}}},
         {"bnez", {2, {1}}},
         {"blez", {2, {1}}},
         {"bgez", {2, {1}}},
         {"bltz", {2, {1}}},
         {"bgtz", {2, {1}}},

         // Branch pseudos: rs1, rs2, imm
         {"bgt", {3, {2}}},
         {"ble", {3, {2}}},
         {"bgtu", {3, {2}}},
         {"bleu", {3, {2}}},

         // Jump pseudos
         {"j", {1, {0}}},
         {"jr", {1, {}}},
         {"call", {1, {0}}},
         {"tail", {1, {0}}},

         // M extension
         {"mul", {3, {}}},
         {"mulh", {3, {}}},
         {"mulhsu", {3, {}}},
         {"mulhu", {3, {}}},
         {"div", {3, {}}},
         {"divu", {3, {}}},
         {"rem", {3, {}}},
         {"remu", {3, {}}},
         {"mulw", {3, {}}},
         {"divw", {3, {}}},
         {"divuw", {3, {}}},
         {"remw", {3, {}}},
         {"remuw", {3, {}}},

         // F extension: rd, rs1, rs2
         {"flw", {3, {2}}},
         {"fsw", {3, {2}}},
         {"fadd.s", {3, {}}},
         {"fsub.s", {3, {}}},
         {"fmul.s", {3, {}}},
         {"fdiv.s", {3, {}}},
         {"fsqrt.s", {2, {}}},
         {"fsgnj.s", {3, {}}},
         {"fsgnjn.s", {3, {}}},
         {"fsgnjx.s", {3, {}}},
         {"fmin.s", {3, {}}},
         {"fmax.s", {3, {}}},
         {"fmadd.s", {4, {}}},
         {"fmsub.s", {4, {}}},
         {"fnmsub.s", {4, {}}},
         {"fnmadd.s", {4, {}}},
         {"fcvt.w.s", {2, {}}},
         {"fcvt.wu.s", {2, {}}},
         {"fmv.x.w", {2, {}}},
         {"feq.s", {3, {}}},
         {"flt.s", {3, {}}},
         {"fle.s", {3, {}}},
         {"fclass.s", {2, {}}},
         {"fcvt.s.w", {2, {}}},
         {"fcvt.s.wu", {2, {}}},
         {"fmv.w.x", {2, {}}},
         {"fcvt.l.s", {2, {}}},
         {"fcvt.lu.s", {2, {}}},
         {"fcvt.s.l", {2, {}}},
         {"fcvt.s.lu", {2, {}}},

         // D extension
         {"fld", {3, {2}}},
         {"fsd", {3, {2}}},
         {"fadd.d", {3, {}}},
         {"fsub.d", {3, {}}},
         {"fmul.d", {3, {}}},
         {"fdiv.d", {3, {}}},
         {"fsqrt.d", {2, {}}},
         {"fsgnj.d", {3, {}}},
         {"fsgnjn.d", {3, {}}},
         {"fsgnjx.d", {3, {}}},
         {"fmin.d", {3, {}}},
         {"fmax.d", {3, {}}},
         {"fmadd.d", {4, {}}},
         {"fmsub.d", {4, {}}},
         {"fnmsub.d", {4, {}}},
         {"fnmadd.d", {4, {}}},
         {"fcvt.s.d", {2, {}}},
         {"fcvt.d.s", {2, {}}},
         {"feq.d", {3, {}}},
         {"flt.d", {3, {}}},
         {"fle.d", {3, {}}},
         {"fclass.d", {2, {}}},
         {"fcvt.w.d", {2, {}}},
         {"fcvt.wu.d", {2, {}}},
         {"fcvt.d.w", {2, {}}},
         {"fcvt.d.wu", {2, {}}},
         {"fcvt.l.d", {2, {}}},
         {"fcvt.lu.d", {2, {}}},
         {"fmv.x.d", {2, {}}},
         {"fcvt.d.l", {2, {}}},
         {"fcvt.d.lu", {2, {}}},
         {"fmv.d.x", {2, {}}}};

    // const auto opcodeTable = getOpcodeTable();

    // Operands may be separated by commas and/or spaces (e.g. "sw r1, x2, 0"); consecutive
    // commas, which would produce an empty operand, are rejected.
    const QString text = line.trimmed();
    const qsizetype firstSpace = text.indexOf(QRegularExpression("\\s"));
    QStringList normalizedParts{firstSpace < 0 ? text : text.left(firstSpace)};
    QStringList operands;
    if (firstSpace >= 0 && !splitOperands(text.mid(firstSpace), operands))
    {
        errorMessage = "Invalid expansion: empty operand (consecutive commas?)";
        return false;
    }
    normalizedParts << operands;
    const QString opcode = normalizedParts.first().toLower();

    if (rvv::isVectorMnemonic(opcode.toStdString()) || rvv::isVectorPseudo(opcode.toStdString()))
    {
        return isValidVectorExpansionLine(normalizedParts, declaredArgs, errorMessage);
    }

    if (instruction_to_operand_count.count(opcode) == 0)
    {
        errorMessage = QString("Unknown opcode \"%1\" in expansion.").arg(opcode);
        return false;
    }

    const auto [expectedCount, immediatePositions] = instruction_to_operand_count.at(opcode);
    const int operandCount = normalizedParts.size() - 1;

    if (operandCount != expectedCount)
    {
        errorMessage = QString("Opcode \"%1\" expects %2 operands but got %3.")
                           .arg(opcode)
                           .arg(expectedCount)
                           .arg(operandCount);
        return false;
    }

    static const QRegularExpression validRegister(R"(^x([0-9]|[1-2][0-9]|3[0-1])$)");

    for (int i = 0; i < operandCount; i++)
    {
        const QString operand = normalizedParts[i + 1];

        if (immediatePositions.count(i) != 0)
        {
            bool ok;
            operand.toInt(&ok);
            if (!ok)
            {
                errorMessage = QString("Operand %1 of \"%2\" must be an integer, got \"%3\".")
                                   .arg(i + 1)
                                   .arg(opcode)
                                   .arg(operand);
                return false;
            }
        }
        else
        {
            const bool isDeclaredArg = declaredArgs.contains(operand);
            const bool isHardRegister = validRegister.match(operand).hasMatch();

            if (!isDeclaredArg && !isHardRegister)
            {
                errorMessage = QString("Operand \"%1\" in expansion of \"%2\" is not a "
                                       "declared arg or valid register.")
                                   .arg(operand)
                                   .arg(opcode);
                return false;
            }
        }
    }

    return true;
}

QJsonObject loadInstructionsFromDisk(QString &errorMessage)
{
    QFile file(globals::custom_pseudo_instructions_file_path);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorMessage = "Could not open file for reading.";
        return {};
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError)
    {
        errorMessage = "Failed to parse JSON: " + parseError.errorString();
        return {};
    }

    return doc.object();
}

bool saveToDisk(const QJsonObject &root, QString &errorMessage)
{
    QFile file(globals::custom_pseudo_instructions_file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        errorMessage = "Could not open file for writing.";
        return false;
    }

    QJsonDocument doc(root);
    QByteArray data = doc.toJson(QJsonDocument::Indented);
    qint64 bytesWritten = file.write(data);
    file.close();
    if (bytesWritten != data.size())
    {
        errorMessage = "Failed to write all data to file.";
        return false;
    }

    return true;
}
}// namespace customPseudoManager
}// namespace Kites
