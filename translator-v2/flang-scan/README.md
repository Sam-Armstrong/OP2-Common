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

`op2-flang-scan` tracks Flang's parse-tree API (tuple-class nodes such
as `CallStmt::t` and `ArrayElement::Subscripts()`). That layout first
shipped in **LLVM 23**, so the scanner needs **LLVM Flang >= 23**
(parser headers, `libFortranParser`, and LLVM CMake config). LLVM 18--22
will not compile it.

See `docs/getting_started.rst` for the full install recipe. In short:

### 1. Distro packages (LLVM >= 23)

Ubuntu 24.04 archive packages (`libflang-18-dev` ... `libflang-20-dev`)
are too old. Use [apt.llvm.org](https://apt.llvm.org/) instead:

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 23
sudo apt-get install -y libflang-23-dev llvm-23-dev
export LLVM_INSTALL_PATH=/usr/lib/llvm-23
```

### 2. Homebrew (macOS)

Fine if `llvm-config --version` is **>= 23**:

```bash
brew install llvm
export LLVM_INSTALL_PATH="$(brew --prefix llvm)"
```

### 3. Build from source

Use this if a >= 23 package is not available, or to track LLVM `main`.
See `docs/getting_started.rst` for the CMake flags.

```bash
git clone --depth 1 https://github.com/llvm/llvm-project.git
cd llvm-project
cmake -S llvm -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="mlir;flang" \
    -DLLVM_TARGETS_TO_BUILD=host \
    -DCMAKE_INSTALL_PREFIX=$HOME/.local/llvm \
    -DLLVM_ENABLE_RTTI=ON
cmake --build build --target install
```

You only need the parser libraries (`FortranParser`, `FortranCommon`,
`FortranSupport`) and their headers at runtime; a full Flang build still takes
a while.

### 4. Windows

On Windows the path of least resistance is WSL + apt.llvm.org 23 (or
the from-source recipe). Native Windows builds of Flang exist but are
not routinely tested against this tool.

## Building op2-flang-scan

The OP2 library build will compile this tool when LLVM Flang is detected
(`make -C op2 config` prints `LLVM Flang FOUND`, then `make -C op2` or
`make -C op2 flang-scan`). See `docs/getting_started.rst` for installing LLVM
Flang, `LLVM_INSTALL_PATH`, and `OP2_FORTRAN_PARSER`.

To configure the scanner by hand, point CMake at the LLVM install prefix:

```bash
cd translator-v2/flang-scan
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=$HOME/.local/llvm
cmake --build build
```

The binary lands at `translator-v2/flang-scan/build/op2-flang-scan`.

If your distro installed Flang under a versioned prefix (e.g.
`/usr/lib/llvm-23`), pass that as `-DCMAKE_PREFIX_PATH=/usr/lib/llvm-23`.

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
- **`#error host endianness is not known`** — Flang's `uint128.h` needs
  `FLANG_LITTLE_ENDIAN` or `FLANG_BIG_ENDIAN`. The CMakeLists.txt now defines
  this from `CMAKE_CXX_BYTE_ORDER`; re-run `cmake -B build`.
- **`LLVMSupport` / `FortranParser` undefined references** — your LLVM was
  not installed (only built). Re-run `cmake --build build --target install`
  in the `llvm-project` tree.
- **`zstd::libzstd_shared` was not found** — distro LLVM packages often
  require zstd at CMake generate time. Install `libzstd-dev` (Debian/Ubuntu)
  or `libzstd-devel` (Fedora), or rely on the CMakeLists.txt workaround that
  locates `libzstd.so.1` on the system library path.

## Running the translator against Flang

The Python driver looks for the scan binary in this order:

1. `--flang-scan <path>` command-line flag
2. `OP2_FLANG_SCAN` environment variable
3. `op2/bin/op2-flang-scan` (installed by `make -C op2`)
4. `translator-v2/flang-scan/build/op2-flang-scan`
5. `op2-flang-scan` on `PATH`

To switch from `fparser2` (default) to Flang for Stage 1, either set
`OP2_FORTRAN_PARSER=flang` when using the OP2 app Makefiles, or pass the flag
directly:

```bash
python3 translator-v2/op2-translator --parser flang airfoil.F90 -o generated/
```

Or via the shell wrapper:

```bash
translator-v2/op2-translator.sh --parser flang airfoil.F90 -o generated/
```

When multiple Fortran sources are passed, the Python driver invokes
`op2-flang-scan --batch` once and feeds every preprocessed translation unit
over stdin. Flang still runs `Prescan`+`Parse` **per file** (the API is
one TU per call), but LLVM binary load / process spawn happens only once.

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
          "lhs": {"kind": "funcref", "name": "res1", "args": [{"kind": "int_lit", "text": "1", "kind_text": null}]},
          "rhs": {
            "kind": "binary", "op": "+",
            "left":  {"kind": "funcref", "name": "res1", "args": [{"kind": "int_lit", "text": "1", "kind_text": null}]},
            "right": {"kind": "name", "value": "f"}
          }
        }
      ],
      "calls": [],
      "decls": [
        {
          "kind": "type_decl",
          "type": {"kind": "intrinsic", "base": "real", "kind_text": "8"},
          "dim": null,
          "is_parameter": false,
          "entities": [{"name": "dx", "dim": null, "init": null}]
        }
      ],
      "stmts": [
        {
          "kind": "assign",
          "line": 82,
          "lhs": {"kind": "funcref", "name": "res1", "args": [{"kind": "int_lit", "text": "1", "kind_text": null}]},
          "rhs": { "...": "same expr shape as assignments[].rhs above" }
        }
      ]
    }
  ]
}
```

`locals`/`assignments`/`calls` are used by Stage 2 validation (see below) and
by a couple of Stage 3 whole-body analyses (parameter const-ness, atomicAdd
detection) that don't care about control-flow position; `decls`/`stmts` are
used by Stage 3 kernel-to-C++ translation, which does. Stage 1 only needs
`name`/`parameters`/`depends`/`source`. Their expression-tree node shapes
(`name`, `part_ref`, `funcref`, `triplet`, `binary`, `unary`, `paren`,
`int_lit`, `real_lit`, `logical_lit`, `char_lit`, `unsupported`) are
documented in the comment above `emitBodyExpr` in `op2-flang-scan.cpp`.
`decls` (one entry per `type_decl`/`parameter_stmt`/`data_stmt` in the
subprogram's specification part) and `stmts` (a fully nested tree of
`assign`/`call`/`if_stmt`/`if_construct`/`do`/`return`/`stop`/`write`/
`continue` statements) are documented above `DeclCollector` and
`BodyStatementEmitter` respectively in `op2-flang-scan.cpp`.

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
- **Stage 3 (complete with `--parser flang`)**:
  `fortran/flang_kernels_c.py` ports `fortran/translator/kernels_c.py`
  (kernel-to-C++ translation: type resolution, parameter const-ness
  inference, statement/expression codegen) to walk `decls`/`stmts` instead of
  an fparser2 AST, and `fortran/flang_kernels.py` ports the AST mutations
  `fortran/schemes.py` applies beforehand (`renameConsts`, `fixHydraIO`,
  `insertAtomicIncs`; `removeExternals` has no Flang equivalent because
  `EXTERNAL` statements are never turned into a `decls` node in the first
  place). Unlike the fparser2 path, `info.consts` is built directly from
  `Application.consts()` (the already-parsed `op_decl_const` calls) rather
  than by re-parsing the consts module's declarations - this works
  identically for both parser backends. This is used by every C++-emitting
  scheme (`Fortran/c_seq`, `c_cuda`, `c_hip`) whenever every kernel entity
  involved has `flang_body` set; see
  `fortran.schemes._use_flang_kernels_c`/`fortran.flang_kernels_c.canTranslateWithFlang`.
  The Fortran-output `Fortran/cuda` and `Fortran/openmp` schemes are also
  Flang-native via `fortran/flang_writer.py` (`rename_consts`, `rename_entities`,
  `fix_hydra_io`, `remove_externals`, `insert_strides`, `insert_atomic_incs`,
  `write_source`).
- **fparser2 fallback**: if Flang scan fails for a file, Stage 1 falls back to
  fparser2 for that file. If a loop's dependency closure includes an entity
  without `flang_body` (e.g. it came from an fparser2-fallback file),
  validation and kernel translation fall back to fparser2 for that loop too,
  lazily attaching an AST to every Flang-parsed program in the app.
  Main-program translation (`op_par_loop`/`op_decl_const` call rewriting) is
  a text-only regex transform (`translateProgram2`) and never needs an AST
  at all.
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
