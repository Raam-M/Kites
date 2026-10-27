/**
 * @file utils.cpp
 * @brief Contains utility functions for the VM.
 * @author Vishank Singh, https://github.com/VishankSingh
 */

#include "utils.h"
#include "common/globals.h"
#include "common/instructions.h"
#include "processor/registers.h"


#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>

namespace Kites
{
void setupVmStateDirectory()
{
    std::error_code ec;
    std::filesystem::create_directories(globals::vm_state_directory, ec);

    if (ec)
    {
        throw std::runtime_error(
            "Unable to create Kites VM state directory: " +
            globals::vm_state_directory.string() +
            " (" + ec.message() + ")");
    }

    // std::filesystem::path registers_file = vm_state_dir / "registers_dump.json";
    // std::filesystem::path errors_file = vm_state_dir / "errors_dump.json";
    // std::filesystem::path vm_state_dump_file_path = vm_state_dir / "vm_state_dump.json";

    if (!std::filesystem::exists(globals::registers_dump_file_path))
    {
        std::ofstream(globals::registers_dump_file_path).close();
    }
    if (!std::filesystem::exists(globals::errors_dump_file_path))
    {
        std::ofstream(globals::errors_dump_file_path).close();
    }
    if (!std::filesystem::exists(globals::memory_dump_file_path))
    {
        std::ofstream(globals::memory_dump_file_path).close();
    }
    if (!std::filesystem::exists(globals::cache_dump_file_path))
    {
        std::ofstream(globals::cache_dump_file_path).close();
    }
    if (!std::filesystem::exists(globals::vm_state_dump_file_path))
    {
        std::ofstream(globals::vm_state_dump_file_path).close();
    }
    if (!std::filesystem::exists(globals::disassembly_file_path))
    {
        std::ofstream(globals::disassembly_file_path).close();
    }
    if (!std::filesystem::exists(globals::custom_pseudo_instructions_file_path))
    {
        std::ofstream(globals::custom_pseudo_instructions_file_path).close();
    }
    if (!std::filesystem::exists(globals::config_file_path))
    {
        SetupConfigFile();
    }
}

int64_t CountLines(const std::string &filename)
{
    std::ifstream file(filename);
    if (!file.is_open())
    {
        throw std::ios_base::failure("Could not open the file.");
    }

    int64_t lines = 0;
    std::string line;
    while (std::getline(file, line))
    {
        lines++;
    }

    file.close();
    return lines;
}

std::string GetLineFromFile(const std::string &fileName, unsigned int lineNumber)
{
    std::ifstream file(fileName);
    if (!file.is_open())
    {
        throw std::ios_base::failure("Could not open the file.");
    }

    std::string line;
    unsigned int currentLine = 0;

    while (std::getline(file, line))
    {
        if (++currentLine == lineNumber)
        {
            return line;
        }
    }

    throw std::out_of_range("Line number out of range.");
}

std::string ParseEscapedString(const std::string &input)
{
    std::ostringstream oss;
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] == '\\' && i + 1 < input.size())
        {
            switch (input[i + 1])
            {
            case 'n':
                oss.put('\n');
                ++i;
                break; // Newline
            case 't':
                oss.put('\t');
                ++i;
                break; // Tab
            case '\\':
                oss.put('\\');
                ++i;
                break; // Backslash
            case '"':
                oss.put('"');
                ++i;
                break; // Double quote
            default:
                oss.put('\\');
                oss.put(input[i + 1]);
                ++i;
                break; // Unhandled escape
            }
        }
        else
        {
            oss.put(input[i]);
        }
    }
    return oss.str();
}

void DumpErrors(const std::filesystem::path &filename, const std::vector<ParseError> &errors)
{
    std::ofstream file(filename);
    if (!file.is_open())
    {
        throw std::runtime_error("Unable to open file: " + filename.string());
    }

    file << "{\n";
    file << "    \"errorCode\": 1,\n";
    file << "    \"errors\": [\n";

    for (size_t i = 0; i < errors.size(); ++i)
    {
        const auto &error = errors[i];
        file << "        {\n";
        file << "            \"line\": " << error.line << ",\n";
        file << "            \"message\": \"" << error.message << "\"\n";
        file << "        }";
        if (i != errors.size() - 1)
        {
            file << ",";
        }
        file << "\n";
    }

    file << "    ]\n";
    file << "}\n";

    file.close();
}

void DumpNoErrors(const std::filesystem::path &filename)
{
    std::ofstream file(filename);
    if (!file.is_open())
    {
        throw std::runtime_error("Unable to open file: " + filename.string());
    }

    file << "{\n";
    file << "    \"errorCode\": 0,\n";
    file << "    \"errors\": []\n";
    file << "}\n";

    file.close();
}

namespace
{
/// Appends "0x" and 16 zero-padded hex digits (the dump's register format).
void appendHex16(std::string &out, uint64_t value)
{
    static const char kDigits[] = "0123456789abcdef";
    char text[18] = {'0', 'x'};
    for (int i = 17; i >= 2; --i)
    {
        text[i] = kDigits[value & 0xF];
        value >>= 4;
    }
    out.append(text, sizeof(text));
}

/// Appends one register section: "    \"name\": {\n" entries "    },\n".
template <typename Values>
void appendRegisterSection(std::string &out, const char *name, char prefix, const Values &values)
{
    out += "    \"";
    out += name;
    out += "\": {\n";
    for (size_t i = 0; i < values.size(); ++i)
    {
        out += "        \"";
        out += prefix;
        out += std::to_string(i);
        out += i >= 10 ? "\": \"" : "\" : \"";
        appendHex16(out, values[i]);
        out += i + 1 != values.size() ? "\",\n" : "\"\n";
    }
    out += "    },\n";
}
} // namespace

void DumpRegisters(const std::filesystem::path &filename, RegisterFile &register_file)
{
    // This dump is written after every step, so it is formatted directly into one string
    // (stream manipulators per value were a measurable part of the time per step).
    std::vector<uint64_t> gp_registers = register_file.GetGprValues();
    std::vector<uint64_t> fp_registers = register_file.GetFprValues();

    std::ofstream out(filename);
    if (!out.is_open())
    {
        throw std::runtime_error("Unable to open file: " + filename.string());
    }
    std::string file;
    file.reserve(4096);

    file += "{\n";
    file += "    \"control and status registers\": {\n";
    for (auto it = csr_to_address.begin(); it != csr_to_address.end();)
    {
        file += "        \"";
        file += it->first;
        file += "\": \"";
        appendHex16(file, register_file.ReadCsr(it->second));
        ++it;
        file += it != csr_to_address.end() ? "\",\n" : "\"\n";
    }
    file += "    },\n";

    appendRegisterSection(file, "gp_registers", 'x', gp_registers);
    appendRegisterSection(file, "fp_registers", 'f', fp_registers);

    // Vector registers as hex strings, most significant byte first, so element 0 is in the
    // rightmost (least significant) digits.
    // This dump is written after every step. Leading zeros are omitted so that unused
    // registers cost a few bytes and scalar programs keep a small file (writing a file larger
    // than 4 KiB after every step measurably slowed down scalar runs).
    const std::vector<uint8_t> &vector_bytes = register_file.GetVrBytes();
    const size_t vlenb = register_file.GetVlenb();
    static const char kHexDigits[] = "0123456789abcdef";
    std::string section = "    \"vector_registers\": {\n";
    section.reserve(section.size() + 32 * (2 * vlenb + 24));
    for (size_t reg = 0; reg < 32 && vlenb > 0; ++reg)
    {
        section += "        \"v" + std::to_string(reg) + "\"" + (reg >= 10 ? "" : " ") + ": \"0x";
        bool leading = true;
        for (size_t b = vlenb; b-- > 0;)
        {
            const uint8_t byte = vector_bytes[reg * vlenb + b];
            for (unsigned digit : {static_cast<unsigned>(byte >> 4), static_cast<unsigned>(byte & 0xF)})
            {
                if (leading && digit == 0)
                    continue;
                leading = false;
                section += kHexDigits[digit];
            }
        }
        if (leading)
            section += '0';
        section += reg != 31 ? "\",\n" : "\"\n";
    }
    section += "    }\n";
    file += section;
    file += "}\n";
    out << file;
}

// void DumpDisasssembly(const std::filesystem::path &filename, const AssembledProgram &program) {
//   const std::map<std::string, SymbolData>& symbol_table = program.symbol_table;
//   // auto& insntrucion_number_disassembly_mapping =
//   program.insntrucion_number_disassembly_mapping;
//   // auto& line_number_instruction_number_mapping =
//   program.line_number_instruction_number_mapping;
//   // const auto& instruction_number_line_number_mapping =
//   program.instruction_number_line_number_mapping; std::vector<std::pair<ICUnit, bool>>
//   intermediate_code = program.intermediate_code; const auto& text_buffer = program.text_buffer;
//   std::map<unsigned int, unsigned int> disassembly_number_line_number_mapping;
//   std::unordered_map<uint64_t, std::string> label_for_address;
//   for (const auto& [name, data] : symbol_table) {
//     if (!data.isData) {
//       label_for_address[data.address] = name; // overwrites previous if same address
//     }
//     // std::cout << "Symbol: " << name
//     //           << ", Address: " << std::hex << data.address
//     //           << ", Line: " << data.line_number
//     //           << (data.isData ? " (Data)" : " (Code)") << std::dec << std::endl;
//   }
//   if (label_for_address.find(0) == label_for_address.end()) {
//     label_for_address[0] = "start"; // Add a default label for address 0
//   }
//   unsigned int instruction_index = 0;
//   // unsigned int symbol_index = 0;
//   unsigned int line_number = 1;
//   size_t max_address = intermediate_code.size() * 4;
//   int hex_digits = 1;
//   size_t temp = max_address;
//   while (temp >>= 4) ++hex_digits;
//   while (instruction_index < intermediate_code.size()) {
//     const auto& [ICBlock, isData] = intermediate_code[instruction_index];
//     uint64_t current_address = instruction_index * 4;
//     auto it = label_for_address.find(current_address);
//     if (it != label_for_address.end()) {
//       if (line_number > 1) {
//         std::cout << std::endl;
//       }
//       ++line_number;
//       std::cout << std::setw(16) << std::setfill('0') << std::hex
//                 << current_address
//                 << std::dec << std::setfill(' ')
//                 << " <" << it->second << ">:" << std::endl;
//       ++line_number;
//     }
//     std::cout << "  "
//               << std::setw(hex_digits) << std::setfill(' ') << std::right << std::hex
//               << current_address
//               << std::dec << std::left << std::setw(0)
//               << ": ";
//     if (instruction_index < text_buffer.size()) {
//       uint32_t raw = text_buffer[instruction_index];
//       std::cout << std::setfill('0') << std::setw(8) << std::right << std::hex
//                 << raw
//                 << std::dec << std::setfill(' ') << "          	";
//     } else {
//       std::cout << " ????????\t";
//     }
//     std::cout << ICBlock << std::endl;
//     disassembly_number_line_number_mapping[instruction_index] = line_number;
//     ++line_number;
//     ++instruction_index;
//   }
//   std::cout << "\n[Disassembly Number → Line Number Mapping]\n";
//   for (const auto& [inst_idx, line] : disassembly_number_line_number_mapping) {
//     std::cout << "Instruction " << inst_idx << " => Line " << line << '\n';
//   }
// }

void DumpDisasssembly(const std::filesystem::path &filename, AssembledProgram &program)
{
    std::ofstream out(filename);
    if (!out)
    {
        std::cerr << "Failed to open disassembly output file: " << filename << std::endl;
        return;
    }

    const std::map<std::string, SymbolData> &symbol_table = program.symbol_table;
    const std::vector<std::pair<ICUnit, bool>> &intermediate_code = program.intermediate_code;
    const std::vector<uint32_t> &text_buffer = program.text_buffer;
    std::map<unsigned int, unsigned int> instruction_number_disassembly_mapping;

    std::unordered_map<uint64_t, std::string> label_for_address;
    for (const auto &[name, data] : symbol_table)
    {
        if (!data.isData)
        {
            label_for_address[data.address] = name;
        }
    }

    if (label_for_address.find(0) == label_for_address.end())
    {
        label_for_address[0] = "start";
    }

    unsigned int instruction_index = 0;
    unsigned int line_number = 1;

    size_t max_address = intermediate_code.size() * 4;
    int hex_digits = 1;
    size_t temp = max_address;
    while (temp >>= 4)
        ++hex_digits;

    while (instruction_index < intermediate_code.size())
    {
        const auto &[ICBlock, isData] = intermediate_code[instruction_index];
        uint64_t current_address = instruction_index * 4;

        auto it = label_for_address.find(current_address);
        if (it != label_for_address.end())
        {
            if (line_number > 1)
            {
                out << std::endl;
                ++line_number;
            }
            out << std::setw(16) << std::setfill('0') << std::hex << current_address << std::dec
                << std::setfill(' ') << " <" << it->second << ">:" << std::endl;
            ++line_number;
        }

        out << "  " << std::setw(hex_digits) << std::setfill(' ') << std::right << std::hex
            << current_address << std::dec << std::left << std::setw(0) << ": ";

        if (instruction_index < text_buffer.size())
        {
            uint32_t raw = text_buffer[instruction_index];
            out << std::setfill('0') << std::setw(8) << std::right << std::hex << raw << std::dec
                << std::setfill(' ') << "             ";
        }
        else
        {
            out << " ????????             ";
        }

        if (ICBlock.hasMachineCode())
        {
            // Encoded during parsing (vector instructions): print the decoded operands.
            out << instruction_set::disassemble(ICBlock.getMachineCode()) << std::endl;
        }
        else
        {
            out << ICBlock << std::endl;
        }
        instruction_number_disassembly_mapping[instruction_index] = line_number;

        ++line_number;
        ++instruction_index;
    }

    program.instruction_number_disassembly_mapping = instruction_number_disassembly_mapping;
}

void SetupConfigFile()
{
    std::ofstream config_file(globals::config_file_path);
    if (!config_file.is_open())
    {
        throw std::runtime_error("Unable to open config file: " +
                                 globals::config_file_path.string());
    }

    config_file << "[General]\n";
    config_file << "name=vm\n\n";

    config_file << "[Execution]\n";
    config_file << "run_step_delay=0   ; in ms\n";
    config_file << "processor_type=single_stage\n";
    config_file << "hazard_detection=false\n";
    config_file << "forwarding=false\n";
    config_file << "branch_prediction=none\n\n";

    config_file << "[Memory]\n";
    config_file << "memory_size=0xffffffffffffffff\n";
    config_file << "block_size=1024\n\n";

    config_file << "[Cache]\n";
    config_file << "cache_enabled=false\n";
    config_file << "cache_size=0\n";
    config_file << "cache_block_size=0\n";
    config_file << "cache_associativity=0\n";
    config_file << "cache_read_miss_policy=read_allocate\n";
    config_file << "cache_replacement_policy=LRU\n";
    config_file << "cache_write_hit_policy=write_back\n";
    config_file << "cache_write_miss_policy=write_allocate\n\n";

    config_file << "[BranchPrediction]\n";
    config_file << "branch_prediction_type=always_not_taken\n";
    config_file << "branch_prediction_table_size=0\n";
    config_file << "branch_prediction_table_associativity=0\n";
    config_file.close();
}
}//namespace Kites
