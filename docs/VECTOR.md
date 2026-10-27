# RISC-V Vector Extension (RVV 1.0)

Kites implements the RISC-V **V** extension, version 1.0, on the single-cycle
processor (`RVSS`). All 627 vector instructions assemble, disassemble and
execute, together with the standard vector pseudo-instructions.

| Parameter | Value |
|-----------|-------|
| VLEN (register width) | 128 bits |
| ELEN (largest element) | 64 bits |
| SEW (element width) | 8, 16, 32, 64 |
| LMUL (register grouping) | 1/8, 1/4, 1/2, 1, 2, 4, 8 |
| Floating point | SEW = 32 (F) and SEW = 64 (D) |
| Processor models | Single-cycle only. The 5-stage pipelines stop with an error on vector instructions. |

The design and its decisions are described in
[the scope document](vector_extension/scope_document.pdf).

## Programming model

- 32 vector registers `v0`–`v31`, each VLEN bits wide. Element 0 occupies the
  least significant bits.
- `vsetvli`/`vsetivli`/`vsetvl` choose SEW and LMUL and set
  `vl = min(AVL, VLMAX)`, where `VLMAX = LMUL × VLEN / SEW`.
- With LMUL > 1, an operand is a group of registers whose first register must be
  a multiple of LMUL.
- `v0.t` masks an instruction: element *i* is processed only if bit *i* of `v0`
  is set.
- Masked-off and tail elements are left **undisturbed**, including when `ta`/`ma`
  is requested. Both behaviours are allowed by the spec.

### Vector CSRs

| CSR | Address | Access | Contents |
|-----|---------|--------|----------|
| `vstart` | 0x008 | read/write | first element to process (reset to 0 after every vector instruction) |
| `vxsat` | 0x009 | read/write | fixed-point saturation flag |
| `vxrm` | 0x00A | read/write | fixed-point rounding mode: 0 rnu, 1 rne, 2 rdn, 3 rod |
| `vcsr` | 0x00F | read/write | `vxrm` in bits 2:1, `vxsat` in bit 0 |
| `vl` | 0xC20 | read-only | current vector length |
| `vtype` | 0xC21 | read-only | current vector type |
| `vlenb` | 0xC22 | read-only | VLEN / 8 = 16 |

After a reset, `vtype.vill = 1` and `vl = 0`, so a `vset{i}vl{i}` must run before
other vector instructions. Read CSRs with `csrr`, for example `csrr t0, vlenb`.
Writing `vl`, `vtype` or `vlenb` is an illegal instruction.

## Assembly syntax

The syntax matches GNU `as` and LLVM:

```asm
vsetvli  t0, a0, e32, m2, ta, ma    # LMUL defaults to m1; policies default to tu, mu
vsetivli t0, 8, e8, mf2, tu, mu
vle32.v  v4, (a1)                   # unit-stride load
vle32.v  v4, (a1), v0.t             # ... masked
vlse64.v v8, (a1), t1               # strided load, stride in bytes
vluxei32.v v8, (a1), v4             # indexed load, byte offsets in v4
vlseg3e8.v v0, (a1)                 # segment load: fields to v0, v1, v2
vadd.vv  v1, v2, v3                 # vd, vs2, vs1
vadd.vx  v1, v2, a0
vadd.vi  v1, v2, -5                 # simm5: -16..15 (shift/slide/gather immediates: 0..31)
vmacc.vv v1, v2, v3                 # multiply-add: vd, vs1, vs2
vfmacc.vf v1, fa0, v2
vmerge.vvm v1, v2, v3, v0           # v0 selects, it is not a mask here
vmv.x.s  a0, v1
```

Pseudo-instructions: `vneg.v`, `vnot.v`, `vwcvt(u).x.x.v`, `vncvt.x.x.w`,
`vfneg.v`, `vfabs.v`, `vmmv.m`, `vmnot.m`, `vmclr.m`, `vmset.m`,
`vmsgt(u).vv`, `vmsge(u).vv`, `vmfgt.vv`, `vmfge.vv`, `vmslt(u).vi`,
`vmsge(u).vi`, `vmsge(u).vx` (including the masked and temporary-register forms),
and `vl1r.v`/`vl2r.v`/`vl4r.v`/`vl8r.v`. Kites also accepts the CSR pseudo-instructions
`csrr`, `csrw`, `csrs`, `csrc`, `csrwi`, `csrsi` and `csrci`.

If an instruction has the wrong operands, the error message shows its expected
syntax, for example `vadd.vv vd, vs2, vs1[, v0.t]`.

## Errors at run time

Illegal vector usage stops the program with a message that names the rule
broken. The PC stays on the offending instruction, and no register, CSR or memory
location is changed. Examples:

- a vector instruction executed while `vtype.vill = 1`
- a register group not aligned to LMUL (for example `v3` with LMUL = 2)
- an EMUL outside 1/8..8, or more than 8 registers in a segment access
- forbidden overlaps: a masked destination overlapping `v0`, widening and
  narrowing overlap rules, `vrgather`/`vslideup`/`vcompress` source and destination
- a non-zero `vstart` for reductions, `vcpop`, `vfirst`, `viota`, `vmsbf`/`vmsif`/`vmsof` and `vcompress`
- SEW = 8 or 16 for floating-point operations; a reserved rounding mode in `frm`

## Where to see the state

- **GUI register view:** the *Vector* section shows `v0`–`v31` in hex, with
  element 0 in the rightmost digits. The *Vector CSRs* section shows `vl`,
  `vtype` (decoded, e.g. `e32, m1, ta, ma`), `vstart`, `vxrm`, `vxsat`, `vcsr`
  and `vlenb`.
- **`registers_dump.json`:** written by the GUI and the CLI. It contains the vector
  CSRs and a `vector_registers` section in the same hex format.
- **Disassembly:** vector instructions show all their operands.
- **Profiler:** vector instructions are counted as `V-Type`.
- **Undo/redo:** works for vector instructions, including vector registers,
  vector CSRs and memory written by vector stores.

## Behaviour chosen where the spec allows a choice

- `vl = min(AVL, VLMAX)` for every AVL.
- Agnostic elements are left undisturbed.
- Elements are accessed in increasing order, one memory access per element.
  Indexed-unordered accesses therefore behave like ordered ones. Misaligned
  element addresses are allowed.
- `vfredusum`/`vfwredusum` add in element order, like the ordered forms.
- `vsetvli x0, x0, ...` that would change VLMAX sets `vill`.
- A fault-only-first load treats an address beyond the end of memory as a fault.

## Known differences from the specification

- `frm = RMM` rounds ties to even in floating-point arithmetic. Conversions to
  integer do honour RMM.
- Single-precision scalars are stored in `f` registers zero-extended, as in
  Kites' F extension, rather than NaN-boxed. `vfmv.f.s` follows the same
  convention, and `.vf` operands use the low 32 bits.
- Underflow detection follows the host processor's tininess rule.

## Examples

The programs in [`examples/vector/`](../examples/vector) are also run as tests:

| Program | Shows |
|---------|-------|
| `vvadd_i32.s` | strip-mined vector add |
| `memcpy.s` | byte copy with e8/m8 |
| `dot_product.s` | `vmul` + `vredsum` |
| `masked_abs.s` | masking with `vmslt` and `v0.t` |
| `gather_scatter.s` | indexed loads and stores |
| `prefix_sum.s` | log-step scan with `vslideup` |
| `matmul_i32.s` | matrix multiply, vector and scalar versions |
| `saxpy_f32.s` | `vfmacc.vf` |
| `strlen.s` | fault-only-first load, `vmseq` + `vfirst` |
| `rgb_to_grey.s` | segment loads, widening multiply-add, narrowing shift |
| `fir_q15.s` | fixed point: `vsmul`, `vsadd`, `vxrm`, `vxsat` |

## For developers

- Encodings are generated from riscv-opcodes by
  [`tools/vector/gen_vector_isa.py`](../tools/vector/gen_vector_isa.py) into
  `src/processor/vector/vector_isa_table.inc`.
- `src/processor/vector/` holds the ISA table (`vector_isa.*`), `vtype` helpers,
  the dispatcher, the legality checks and the atomic commit (`vector_unit.*`), and
  the instruction groups (`vector_exec_integer.cpp`, `vector_exec_memory.cpp`,
  `vector_exec_float.cpp`).
- `src/assembler/parse_formats/v_formats.cpp` parses every vector instruction from
  the operand pattern in the table.
- Tests are in `test/vector/`; see [TESTING.md](TESTING.md#vector-extension-tests).
