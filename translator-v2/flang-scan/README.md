# op2-flang-scan

A small C++ tool that uses LLVM Flang's parser as the Stage-1 frontend for the
OP2 Fortran translator. It reads a preprocessed free-form Fortran source file,
walks Flang's parse tree, and emits a JSON document describing every
`op_par_loop_N(...)` and `op_decl_const(...)` call site along with their
argument trees.

The Python side (`translator-v2/op2-translator/fortran/flang_parser.py`) invokes
this binary and consumes the JSON when the translator is run with
`--parser flang`.

## Installing LLVM Flang

You need a build of LLVM/Flang that exposes the Flang parser libraries and
their CMake config files. A few options, in rough order of convenience:

### 1. Distro packages (easiest on Linux)

On Debian / Ubuntu 24.04+:

```bash
sudo apt-get install flang-new libflang-dev llvm-dev mlir-tools libmlir-dev
```

Package names change frequently across LLVM versions; `apt search flang` will
show what is available. You want the `-dev` packages so that headers and CMake
files are present in `/usr/lib/llvm-<ver>/lib/cmake/`.

On Fedora:

```bash
sudo dnf install flang-devel llvm-devel mlir-devel
```

### 2. Homebrew (macOS)

```bash
brew install llvm
```

Flang headers land in `$(brew --prefix llvm)/include/flang/...` and the CMake
config files are under `$(brew --prefix llvm)/lib/cmake/`.

### 3. Build from source (any OS, required if distro packages are too old)

```bash
git clone --depth 1 https://github.com/llvm/llvm-project.git
cd llvm-project
cmake -S llvm -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="clang;mlir;flang" \
    -DLLVM_TARGETS_TO_BUILD=host \
    -DCMAKE_INSTALL_PREFIX=$HOME/.local/llvm
cmake --build build --target install
```

You only need the parser libraries (`FortranParser`, `FortranCommon`,
`FortranSupport`) and their headers at runtime; a full Flang build still takes
a while, so grab a coffee.

### 4. Windows

On Windows the path of least resistance is WSL + one of the options above.
Native Windows builds of Flang exist but are not routinely tested against this
tool.

## Building op2-flang-scan

Once LLVM/Flang is installed, point CMake at the install prefix and build:

```bash
cd translator-v2/flang-scan
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=$HOME/.local/llvm
cmake --build build
```

The binary lands at `translator-v2/flang-scan/build/op2-flang-scan`.

If your distro installed Flang under a versioned prefix (e.g.
`/usr/lib/llvm-20`), pass that as `-DCMAKE_PREFIX_PATH=/usr/lib/llvm-20`.

### Troubleshooting

- **"missing imported targets: clangBasic clangOptions clangDriver"** — this
  comes from `FlangConfig.cmake`, which unconditionally pulls in clang
  targets even though the parser does not need them. The `CMakeLists.txt`
  here deliberately bypasses `find_package(Flang)` and links the parser
  libraries directly, so just re-run `cmake -B build` after pulling the
  latest tree.
- **"Flang parser headers not found at .../flang/Parser"** — LLVM was built
  without Flang. Rebuild with
  `-DLLVM_ENABLE_PROJECTS="mlir;flang"` and reinstall.
- **`LLVMSupport` / `FortranParser` undefined references** — your LLVM was
  not installed (only built). Re-run `cmake --build build --target install`
  in the `llvm-project` tree.

## Running the translator against Flang

The Python driver looks for the scan binary in this order:

1. `--flang-scan <path>` command-line flag
2. `OP2_FLANG_SCAN` environment variable
3. `translator-v2/flang-scan/build/op2-flang-scan`
4. `op2-flang-scan` on `PATH`

To switch from `fparser2` (default) to Flang for Stage 1:

```bash
python3 translator-v2/op2-translator --parser flang airfoil.F90 -o generated/
```

Or via the shell wrapper:

```bash
translator-v2/op2-translator.sh --parser flang airfoil.F90 -o generated/
```

## JSON format

```jsonc
{
  "path": "airfoil_op.F90",
  "events": [
    {
      "kind": "op_par_loop_5",
      "location": {"line": 120, "column": 3},
      "args": [
        {"kind": "name",   "value": "save_soln"},
        {"kind": "name",   "value": "edges"},
        {"kind": "call",   "name": "op_arg_dat",
         "args": [
           {"kind": "name",   "value": "p_q"},
           {"kind": "int",    "value": -1},
           {"kind": "name",   "value": "op_id"},
           {"kind": "int",    "value": 4},
           {"kind": "string", "value": "real(8)"},
           {"kind": "name",   "value": "op_read"}
         ]}
      ]
    },
    {
      "kind": "op_decl_const",
      "location": {"line": 42, "column": 3},
      "args": [
        {"kind": "name",   "value": "gam"},
        {"kind": "int",    "value": 1},
        {"kind": "string", "value": "real(8)"}
      ]
    },
    {
      "kind": "subroutine_subprogram",
      "name": "res_calc",
      "location": {"line": 60, "column": 15},
      "parameters": ["x1", "x2", "q1", "q2", "adt1", "adt2", "res1", "res2"],
      "depends": [],
      "source": "subroutine res_calc(...)\n  ...\nend subroutine\n",
      "locals": [
        {"name": "dx", "dims": []}
      ],
      "assignments": [
        {
          "line": 82,
          "lhs": {"kind": "funcref", "name": "res1", "args": [{"kind": "literal", "source": "1"}]},
          "rhs": {
            "kind": "binary", "op": "+",
            "left":  {"kind": "funcref", "name": "res1", "args": [{"kind": "literal", "source": "1"}]},
            "right": {"kind": "name", "value": "f"}
          }
        }
      ],
      "calls": []
    }
  ]
}
```

`locals`/`assignments`/`calls` are only used by Stage 2 validation (see
below); Stage 1 only needs `name`/`parameters`/`depends`/`source`. Their
expression-tree node shapes (`name`, `part_ref`, `funcref`, `triplet`,
`binary`, `paren`, `unary`, `literal`/`raw`) are documented in the comment
above `emitBodyExpr` in `op2-flang-scan.cpp`.

## Notes / limitations

- **Stage 1 (complete with `--parser flang`)**: loop metadata, `op_decl_const`
  extraction, subprogram discovery, dependency names, and kernel source text
  (cooked-source slices from Flang). Recursive dependency closure runs in
  Python (`extractDependencies`). `Fortran/seq` kernel emission uses
  `fortran/flang_writer.py` without an fparser2 AST.
- **Stage 2 (complete with `--parser flang`)**: `fortran/flang_validator.py`
  ports every check in `fortran/validator.py` (parameter/const conflicts,
  const writes, `OP_READ` writes, `OP_INC` increment shape, slice/stride
  compatibility, runtime-dimension local arrays) to walk the `locals` /
  `assignments` / `calls` expression trees above instead of an fparser2 AST,
  including propagating checks through child subroutine/function calls
  (the Flang equivalent of `fortran.util.mapParam`/`getCall`). A loop is
  validated this way whenever its kernel and every transitive dependency
  were parsed by Flang (`Function.flang_body` is set on all of them); this is
  checked once per loop by `fortran.flang_validator.can_validate_with_flang`.
- **fparser2 fallback**: if Flang scan fails for a file, Stage 1 falls back to
  fparser2 for that file. If a loop's dependency closure includes an entity
  without `flang_body` (e.g. it came from an fparser2-fallback file),
  validation falls back to fparser2 for that loop too, lazily attaching an
  AST to every Flang-parsed program in the app. Main-program translation
  (`op_par_loop`/`op_decl_const` call rewriting) is a text-only regex
  transform (`translateProgram2`) and never needs an AST at all. Kernel
  translation for schemes other than `Fortran/seq` (`Fortran/cuda`,
  `c_seq`, `c_cuda`, `c_hip`) still walks an fparser2 AST and prints a
  one-time warning when run under `--parser flang` - that part of Stage 3
  hasn't been ported yet.
- Only the free-form preprocessed output produced by the Python driver is
  supported. Fixed-form sources should be fpp/cpp-preprocessed to free-form
  first (which is what the driver already does).
- The tool does not run Flang's semantic analysis; name resolution / type
  checking of OP2 arguments stays on the Python side. This is also why most
  parenthesised references (`arr(i)`) show up as `funcref` rather than
  `part_ref` - Flang can't yet tell "array element" from "function call"
  without a symbol table, so `flang_validator.py` resolves that ambiguity
  itself (`funcref` whose name matches a known `Function` entity is treated
  as a call; otherwise it's treated as an array/parameter reference).
