# OP2 Fortran robustness suite

Minimal OP2 apps that assess Flang Stage-1 robustness relative to fparser2
(installed pin: fparser 0.2.0, `std=f2008`).

## Categories

| Category | Intent |
|----------|--------|
| `syntax_gap` | Fortran constructs fparser2 cannot parse; Flang should translate |
| `negative_control` | Constructs neither parser handles on this toolchain |
| `pipeline` | OP2 Flang-path stress (validation, multi-file, macros, funcref, fallback) |
| `flang_gap` | Regression tests for former Flang-only gaps (Fortran `INCLUDE`) |

## Layout

```
robustness/
  eval_robustness.py
  run_robustness.sh
  cases/
    <name>/
      case.json
      *.F90 / *.inc
```

## case.json fields

- `name`, `description`, `sources`, `targets`
- `category` — see above (default `syntax_gap`)
- `expect_fparser2` / `expect_flang` — `pass` | `fail` | `fallback` | `pass_with_warning`
- `expected_warnings` — substrings required when expect is `pass_with_warning`
- `translator_flags` — extra CLI flags
- `fparser2_reason` / `notes` — free text for the report

## Usage

```bash
bash translator-v2/parser_eval/robustness/run_robustness.sh
bash translator-v2/parser_eval/robustness/run_robustness.sh --categories pipeline
bash translator-v2/parser_eval/robustness/run_robustness.sh --cases assumed_rank valid_const_write
bash translator-v2/parser_eval/robustness/run_robustness.sh --keep-work /tmp/op2_robust
```


## Notes

Results below are against **fparser 0.2.0** (`std=f2008`) and the built
`op2-flang-scan`. Re-run the suite after upgrading either side.

### `syntax_gap` — fparser2 fails, Flang passes

fparser2 only claims Fortran 2003 plus some 2008. These cases use F2018/F2023
(or incomplete F2008) syntax that its grammar rejects at Stage 1, while Flang
parses them and the OP2 Flang path translates without falling back:

- **DO CONCURRENT locality** — `SHARED` / `LOCAL` / `REDUCE` / `DEFAULT(NONE)`
  (stfc/fparser#409)
- **Assumed-rank / SELECT RANK** — `x(..)`, `SELECT RANK`
- **Assumed-type** — `TYPE(*)`
- **IMPORT forms** — `IMPORT, NONE` / `ONLY` / `ALL`
- **IMPLICIT NONE (TYPE, EXTERNAL)**
- **ERROR STOP …, QUIET=**, **FAIL IMAGE**, **SYNC MEMORY**, **FORM/CHANGE TEAM**
- **Submodule `MODULE PROCEDURE` bodies** (this fparser pin)
- **F2023** — conditional expressions `(cond ? x : y)`, `UNSIGNED`
- **`syntax_in_kernel`** — same idea, but the modern construct sits inside the
  OP2 kernel (not only host init)

### `negative_control` — both fail

Documents limits of *this* toolchain, not OP2 logic:

- **`procedure_pointer_init`** — `procedure(...), pointer :: p => target`
- **`enumeration_type`** — F2023 `ENUMERATION TYPE`

Both reject at parse (Flang Stage 1 fails, then fparser2 fallback also fails).

### `pipeline` — both expected to succeed (OP2-path stress)

These are valid OP2 mini-apps both parsers should handle. They check that the
Flang path stays native and still performs the same work as fparser2:

- **Multi-file / preprocess** — `multi_file_app`, `#define` → `op_par_loop`
  (`macro_op_loop`), `#include` of a loop site (`include_loop_site`)
- **funcref / `arr(i)`** — vector `OP_INC` via `u(i)=u(i)+…`
  (`funcref_vector_inc`); Flang-scan often emits `funcref`, resolved by name
  in `flang_validator` / codegen
- **Validation warnings** (`pass_with_warning` on both) — const write,
  `OP_READ` write, bad `OP_INC`, runtime-sized locals, write via child
  subroutine; translation still exits 0 with `loop.fallback` where applicable
- **`stage1_scan_fallback`** — intentional broken `--flang-scan` stub: Flang
  Stage 1 must fall back, then fparser2 finishes the ordinary source

Contrast: C preprocessor `#include` works for Flang (expanded before scan);
Fortran `INCLUDE` does not (see `flang_gap`).

### `flang_gap` — formerly Flang INCLUDE failures (now fixed)

Fortran `INCLUDE "foo.inc"` is **not** expanded by the translator’s C
preprocessor (fpp/pcpp). It is left in the free-form source and must be
resolved by the Stage-1 Fortran parser. fparser2 and Flang handle that
differently, which is what broke these cases.

**Why it failed under Flang**

1. The Python driver always preprocesses the real source file, then invokes
   `op2-flang-scan --stdin --path <original.F90>` with that text on stdin.
2. Flang’s parser API needs a real on-disk path, so the scan tool wrote the
   stdin body to something like `/tmp/op2-flang-scan-<pid>.F90`.
3. When Flang’s prescanner hits `INCLUDE "loop_site.inc"`, it looks for the
   file relative to the directory of the **file currently being scanned**
   (and then any configured search directories). That directory was `/tmp`,
   not the app directory next to the `.inc` files.
4. Flang reported `INCLUDE: Source file '….inc' was not found`, Stage 1
   raised a parse error, and the translator fell back to fparser2 for that
   file. Translation still completed, but not on the native Flang path.

**Why fparser2 still worked**

fparser2’s `FortranStringReader` is given the translator’s `include_dirs`
(and typically runs with cwd in the source directory), so it can open the
same sibling `.inc` even though the input itself is an in-memory string.

**How it was fixed**

In `op2-flang-scan`:

1. Prefer writing the stdin temp file **next to `--path`** (the original
   source directory), so Flang’s “directory of current file” lookup finds
   sibling `.inc` files. Fall back to the system temp dir only if that
   location is not writable.
2. Add that source directory to Flang’s `Options.searchDirectories`, and
   accept `-I <dir>` for the same include set the translator already passes
   to fparser2.

On the Python side, `run_scan` forwards `include_dirs` as `-I` and sets the
subprocess cwd to the source file’s parent as an extra safeguard.

After the fix, all three cases pass natively on both parsers (no Flang
Stage-1 fallback):

- `fortran_include_loop` — include of the `op_par_loop` site
- `fortran_include_host` — include of host-only init
- `fortran_include_nested` — nested includes

No cases were found where fparser2 translated and Flang failed *hard* without
a successful fparser2 fallback (for `seq` / `c_seq` mini-apps probed).
