# Running Tests

Kites uses [GoogleTest](https://github.com/google/googletest) for unit testing, integrated with CMake/CTest. Two test targets are available:

- **`kites_test`** — plain test build, no instrumentation. Use this for everyday local testing.
- **`kites_test_cov`** — same test suite, built with coverage instrumentation (`--coverage`), plus a `coverage` target that generates an HTML report via [`gcovr`](https://gcovr.com/).

Both targets are produced from a single configure step; you choose which one to build/run afterward.

## Prerequisites

- CMake 3.24+
- Ninja
- A GCC- or Clang-style compiler (coverage instrumentation requires this — see [Notes](#notes))
- [`gcovr`](https://gcovr.com/) on your `PATH` (only required if configuring with `ENABLE_COVERAGE=ON`)

## Configure

Both test targets live under the same configure preset:

```sh
cmake --preset test
```

## Running tests (no coverage)

```sh
cmake --build --preset test-only
ctest --preset test-only
```

This builds and runs `kites_test` only — the fast path for everyday local test runs.

## Running tests (with coverage)

```sh
cmake --build --preset test-coverage
ctest --preset test-coverage
```

This builds and runs `kites_test_cov`, whose CTest test names are prefixed with `cov_` so they don't collide with the plain suite's test names.

### Generating the HTML coverage report

The `coverage` target runs the instrumented test suite via CTest and then invokes `gcovr` to produce an HTML report:

```sh
cmake --build build/coverage --target coverage
```

The report is written to `build/coverage/coverage/index.html`.

## Build options

Set via `-D<OPTION>=ON|OFF` at configure time, or in `CMakePresets.json`:

| Option              | Default | Description                                      |
|---------------------|---------|---------------------------------------------------|
| `DISABLE_GUI`        | `ON`    | Disable GUI components during tests               |
| `VM_DEBUG_PRINTS`     | `OFF`   | Enable debug prints in the VM during tests        |
| `ENABLE_COVERAGE`     | `OFF`   | Build the `kites_test_cov` target and `coverage` target |

## Vector extension tests

The vector tests are in `test/vector/` and run as part of `kites_test`. To run
only them:

```sh
cd test
../build/<preset>/test/kites_test --gtest_filter='Vec*'
```

| File | Checks |
|------|--------|
| `vector_assembler_test.cpp` | Every vector mnemonic and pseudo-instruction encodes exactly as LLVM `llvm-mc` encodes it; disassembly round trip; syntax and range errors |
| `vector_config_test.cpp` | `vset{i}vl{i}`, `vill`, reset state, vector CSRs |
| `vector_integer_test.cpp` | Integer and fixed-point arithmetic for every legal SEW/LMUL, masked and unmasked, against an independent reference model (`vector_reference.h`) |
| `vector_permute_test.cpp` | Reductions, mask instructions, slides, gathers, compress, moves |
| `vector_memory_test.cpp` | Unit-stride, strided, indexed, segment, mask, whole-register and fault-only-first accesses |
| `vector_float_test.cpp` | Floating-point arithmetic, compares, conversions, estimates, flags |
| `vector_legality_test.cpp` | Illegal usage is rejected with state unchanged; undo/redo; pipeline guard |
| `vector_examples_test.cpp` | The programs in `examples/vector/` |

Test names start with the requirement ID they verify (for example
`VM01_indexed` checks requirement VM-01 of the scope document).

### Regenerating generated files

The encoding table and the golden encodings are generated from
[riscv-opcodes](https://github.com/riscv/riscv-opcodes) (`extensions/rv_v`) and
LLVM:

```sh
python3 -I tools/vector/gen_vector_isa.py <riscv-opcodes>/extensions/rv_v \
    src/processor/vector/vector_isa_table.inc <riscv-opcodes commit>
python3 -I tools/vector/gen_golden_encodings.py <riscv-opcodes>/extensions/rv_v \
    "$(brew --prefix llvm)/bin/llvm-mc" test/vector/golden/encodings.txt
```

## Notes

- Coverage instrumentation (`--coverage`) requires GCC or Clang; configuring with `ENABLE_COVERAGE=ON` on another compiler will fail with a clear error.
- If you're on Windows with a Qt-bundled MinGW toolchain, confirm it actually ships `libgcov.a` before relying on coverage builds — some trimmed-down distributions omit it. You can check with:
  ```sh
  g++ -print-file-name=libgcov.a
  ```
  A real path back means it's present; the bare filename echoed back means it isn't found.