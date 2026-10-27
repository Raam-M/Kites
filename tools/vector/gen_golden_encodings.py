#!/usr/bin/env python3
"""Generate golden RVV encodings with LLVM for the Kites assembler tests.

For every vector mnemonic (base forms from riscv-opcodes extensions/rv_v, rv_zvbb
and rv_zvbc, plus the segment load/store variants), this writes one or more assembly lines with
representative operands (masked and unmasked, boundary immediates), assembles them
with llvm-mc, and records the resulting 32-bit words:

    vadd.vv v8, v16, v24, v0.t|0x018c0457

Operands are chosen so that LLVM's static constraints are met: vector operands use
v8/v16/v24 (aligned to 8, never overlapping) and v0 only as the mask.

Usage:
    python3 -I tools/vector/gen_golden_encodings.py <llvm-mc> <output.txt> <rv_v> [<rv_zvbb> <rv_zvbc>]
"""

import re
import subprocess
import sys

MULTIPLY_ADD = {
    "vmacc", "vnmsac", "vmadd", "vnmsub", "vwmaccu", "vwmacc", "vwmaccsu", "vwmaccus",
    "vfmacc", "vfnmacc", "vfmsac", "vfnmsac", "vfmadd", "vfnmadd", "vfmsub", "vfnmsub",
    "vfwmacc", "vfwnmacc", "vfwmsac", "vfwnmsac",
}


def parse_rv_v(path):
    entries = []
    for raw in open(path):
        line = raw.split("#")[0].strip()
        if not line or line.startswith("$"):
            continue
        tokens = line.split()
        name = tokens[0]
        fields = [t for t in tokens[1:] if "=" not in t]
        fixed = dict(t.split("=") for t in tokens[1:] if "=" in t)
        entries.append((name, fields, fixed))
    return entries


def segment_names(name):
    """NFIELDS 2..8 variants of a load/store that has an nf field."""
    out = []
    for k in range(2, 9):
        if name.startswith("vlse"):
            out.append(f"vlsseg{k}e" + name[4:])
        elif name.startswith("vsse"):
            out.append(f"vssseg{k}e" + name[4:])
        elif name.startswith("vle"):
            out.append(f"vlseg{k}e" + name[3:])
        elif name.startswith("vse"):
            out.append(f"vsseg{k}e" + name[3:])
        else:
            m = re.match(r"(vlux|vlox|vsux|vsox)ei(\d+)\.v$", name)
            out.append(f"{m.group(1)}seg{k}ei{m.group(2)}.v")
    return out


def operand_lines(name, fields, fixed):
    """Assembly operand strings for one mnemonic (several variants)."""
    base = name.split(".")[0]
    opcode = int(fixed.get("6..0", "0"), 0)
    funct3 = int(fixed.get("14..12", "0"), 0)

    if name == "vsetvli":
        return ["a0, a1, e32, m2, ta, ma", "t0, zero, e8, mf8, tu, mu", "zero, zero, e64, m1, ta, mu"]
    if name == "vsetivli":
        return ["a0, 31, e16, m4, tu, ma", "a0, 0, e64, m8, ta, ma"]
    if name == "vsetvl":
        return ["a0, a1, a2"]

    if opcode in (0x07, 0x27):
        ops = ["v8", "(a0)"]
        if "rs2" in fields:
            ops.append("a1")
        if "vs2" in fields:
            ops.append("v16")
        lines = [", ".join(ops)]
        if "vm" in fields:
            lines.append(", ".join(ops + ["v0.t"]))
        return lines

    dest = []
    if "vd" in fields:
        dest = ["v8"]
    elif "rd" in fields:
        dest = ["fa0" if funct3 == 1 else "a0"]

    src1_variants = [None]
    if "vs1" in fields:
        src1_variants = ["v24"]
    elif "rs1" in fields:
        src1_variants = ["fa1" if funct3 == 5 else "a1"]
    elif "simm5" in fields:
        src1_variants = ["-16", "15"]
    elif "zimm5" in fields:
        src1_variants = ["0", "31"]
    elif "zimm6lo" in fields:
        src1_variants = ["0", "33", "63"]
    src2 = ["v16"] if "vs2" in fields else []

    vm_fixed_zero = "vm" not in fields and fixed.get("25") == "0"
    lines = []
    for src1 in src1_variants:
        s1 = [src1] if src1 is not None else []
        ops = dest + (s1 + src2 if base in MULTIPLY_ADD else src2 + s1)
        if vm_fixed_zero:
            ops.append("v0")
        lines.append(", ".join(ops))
        if "vm" in fields:
            lines.append(", ".join(ops + ["v0.t"]))
    return lines


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    llvm_mc, out_path, sources = sys.argv[1], sys.argv[2], sys.argv[3:]
    entries = [e for src in sources for e in parse_rv_v(src)]
    asm = []
    for name, fields, fixed in entries:
        names = [name]
        if "nf" in fields:
            names += segment_names(name)
            fields = [f for f in fields if f != "nf"]
        for mnemonic in names:
            for ops in operand_lines(name, fields, fixed):
                asm.append(f"{mnemonic} {ops}".strip())

    result = subprocess.run(
        [llvm_mc, "-triple=riscv64", "-mattr=+v,+zvbb,+zvbc", "-show-encoding"],
        input="\n".join(asm) + "\n", capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("llvm-mc failed:\n" + result.stderr)
    words = []
    for line in result.stdout.splitlines():
        m = re.search(r"encoding: \[0x([0-9a-f]{2}),0x([0-9a-f]{2}),0x([0-9a-f]{2}),0x([0-9a-f]{2})\]", line)
        if m:
            b = [int(x, 16) for x in m.groups()]
            words.append(b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24)
    if len(words) != len(asm):
        sys.exit(f"expected {len(asm)} encodings, got {len(words)}")

    with open(out_path, "w") as f:
        f.write("# Golden RVV encodings produced by llvm-mc (tools/vector/gen_golden_encodings.py).\n")
        f.write("# Format: <assembly>|<32-bit word>\n")
        for text, word in zip(asm, words):
            f.write(f"{text}|0x{word:08x}\n")
    print(f"wrote {len(asm)} golden encodings to {out_path}")


if __name__ == "__main__":
    main()
