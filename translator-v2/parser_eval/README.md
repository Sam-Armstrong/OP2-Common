# Flang vs fparser2 parser evaluation suite

Compares OP2 Fortran translator output and runtime when using `--parser flang`
versus `--parser fparser2` on a small, extensible set of example applications,
plus a robustness suite of mini-apps that stress Stage 1–3 of the Fortran path.

## Layout

```
parser_eval/
  eval_parsers.py          # example-app evaluation driver
  run_eval.sh              # WSL/Linux entry point
  examples/
    <name>/
      example.json         # required: describes sources, targets, runtime
      ...                  # optional app sources / Makefile
  robustness/
    eval_robustness.py     # robustness driver
    run_robustness.sh
    cases/<name>/case.json
```

Current examples (all unstructured-mesh style):

- `airfoil` — existing CFD mini-app
- `tri_diff` — triangular cell mesh, edge→cell diffusion
- `mesh_res` — edge residual with edge→node and edge→cell maps
- `scale_mesh` — larger multi-file variant (~2.4k lines, 10 sources) for Stage-1 scaling
- `scale_mesh_flat` — same fat kernels in one file (spawn-amortisation contrast)

## Adding an example

1. Create `examples/<name>/example.json` (see existing examples).
2. Either point `workdir` / `sources` at an existing app under `apps/fortran/`,
   or place a self-contained app next to `example.json` with a Makefile that
   includes `makefiles/f_app.mk`.
3. Re-run `./run_eval.sh` (optionally `--examples <name>`).

## Checks performed (examples)

For each example:

- **Codegen time** — wall time to run the translator with each parser
- **Dependency trees** — kernel → callee closure from `store.json` (`-d`)
- **Generated file tree** — same relative paths under the output directory
- **Generated content** — normalized C++/CUDA text equality
- **Build + run** (if configured) — pass string + runtime within tolerance

## Usage

```bash
bash translator-v2/parser_eval/run_eval.sh
bash translator-v2/parser_eval/run_eval.sh --examples tri_diff mesh_res
bash translator-v2/parser_eval/run_eval.sh --skip-runtime
```

## Robustness suite

Mini OP2 apps that compare Flang Stage 1 (`op2-flang-scan`) with fparser2
(installed pin: **fparser 0.2.4**, `std=f2008`). Each case records an expected
outcome per parser: `pass`, `fail`, `fallback`, or `pass_with_warning`.

```bash
bash translator-v2/parser_eval/robustness/run_robustness.sh
bash translator-v2/parser_eval/robustness/run_robustness.sh --categories pipeline
bash translator-v2/parser_eval/robustness/run_robustness.sh --cases assumed_rank valid_const_write
```

### Categories

| Category | Intent | Typical outcome |
|----------|--------|-----------------|
| `syntax_gap` | **Standards coverage** — grammar fparser2 cannot parse | fparser2 **fail**, Flang **pass** |
| `negative_control` | Constructs **neither** parser handles on this toolchain | both **fail** |
| `pipeline` | OP2 robustness edge cases (API, kernels, preprocess, validation) | both **pass** (or Flang `fallback` / `pass_with_warning`) |
| `flang_gap` | Former Flang-only INCLUDE path bugs (now fixed) | both **pass** natively |

`syntax_in_kernel` lives under `pipeline` because it also stresses kernel
extraction, but it is a Fortran 2018 syntax gap (fparser2 fail / Flang pass).

---

## Robustness results

Recorded **24 August 2026** against fparser **0.2.4** (`std=f2008`) and the
built `op2-flang-scan`. **67/67 cases matched their expected outcomes.**

| Category | Cases | Result |
|----------|------:|--------|
| `syntax_gap` (standards coverage) | 23 | 23/23 OK |
| `pipeline` (OP2 robustness) | 39 | 39/39 OK |
| `flang_gap` (Fortran `INCLUDE`) | 3 | 3/3 OK |
| `negative_control` | 2 | 2/2 OK |
| **Total** | **67** | **67/67 OK** |

Headline split for the write-up:

- **24 cases**: fparser2 **fails**, Flang **translates** (23 `syntax_gap` + `syntax_in_kernel`).
- **38 cases**: **both parsers translate** (native Flang path, no fparser2 fallback), including 8 `pass_with_warning` validation cases.
- **1 case**: Flang Stage-1 **fallback** to fparser2 then succeeds (`stage1_scan_fallback`).
- **2 cases**: **both fail** (`enumeration_type`, `procedure_pointer_init`).

### Standards coverage (`syntax_gap`) — fparser2 fail, Flang pass

fparser2 is run with `std=f2008` and only claims Fortran 2003 plus *some*
Fortran 2008. These cases use a construct whose **tokens/grammar** fparser2
rejects at Stage 1. LLVM Flang parses it and the OP2 Flang path translates
without falling back.

`DO CONCURRENT REDUCE` locality is **Fortran 2023** (Fortran 2018 added
`LOCAL` / `LOCAL_INIT` / `SHARED` / `DEFAULT(NONE)` only).

| Case | Fortran standard | Construct | fparser2 | Flang |
|------|------------------|-----------|----------|-------|
| `coarray_decl` | **Fortran 2008** | Coarray `real(8) :: buf[*]` plus `SYNC ALL` | fail | pass |
| `submodule_module_procedure` | **Fortran 2008** | `SUBMODULE` with `MODULE PROCEDURE` body | fail | pass |
| `sync_all` | **Fortran 2008** | `SYNC ALL` | fail | pass |
| `sync_memory` | **Fortran 2008** | `SYNC MEMORY` | fail | pass |
| `assumed_rank` | **Fortran 2018** | Assumed-rank dummy `x(..)` | fail | pass |
| `assumed_type` | **Fortran 2018** | Assumed-type dummy `type(*)` | fail | pass |
| `do_concurrent_default_none` | **Fortran 2018** | `DO CONCURRENT … DEFAULT(NONE) SHARED(…)` | fail | pass |
| `do_concurrent_local` | **Fortran 2018** | `DO CONCURRENT … LOCAL(t)` | fail | pass |
| `do_concurrent_local_init` | **Fortran 2018** | `DO CONCURRENT … LOCAL_INIT(t)` | fail | pass |
| `do_concurrent_shared` | **Fortran 2018** | `DO CONCURRENT … SHARED(u)` | fail | pass |
| `error_stop_quiet` | **Fortran 2018** | `ERROR STOP 1, QUIET=.TRUE.` | fail | pass |
| `event_post_wait` | **Fortran 2018** | `EVENT POST` / `EVENT WAIT` | fail | pass |
| `fail_image` | **Fortran 2018** | `FAIL IMAGE` | fail | pass |
| `form_team` | **Fortran 2018** | `FORM TEAM` / `CHANGE TEAM` / `END TEAM` | fail | pass |
| `implicit_none_external` | **Fortran 2018** | `IMPLICIT NONE (TYPE, EXTERNAL)` | fail | pass |
| `import_all` | **Fortran 2018** | `IMPORT, ALL` | fail | pass |
| `import_none` | **Fortran 2018** | `IMPORT, NONE` | fail | pass |
| `import_only` | **Fortran 2018** | `IMPORT, ONLY:` | fail | pass |
| `select_rank` | **Fortran 2018** | `SELECT RANK` on assumed-rank | fail | pass |
| `conditional_expr` | **Fortran 2023** | Conditional expr `(cond ? x : y)` | fail | pass |
| `do_concurrent_reduce` | **Fortran 2023** | `DO CONCURRENT … REDUCE(+:s)` | fail | pass |
| `notify_wait` | **Fortran 2023** | `NOTIFY WAIT` | fail | pass |
| `unsigned` | **Fortran 2023** | `UNSIGNED` type and `2u` literal | fail | pass |

Related (filed under `pipeline`, same fail/pass pattern):

| Case | Fortran standard | Construct | fparser2 | Flang |
|------|------------------|-----------|----------|-------|
| `syntax_in_kernel` | **Fortran 2018** | `DO CONCURRENT SHARED` **inside** the OP2 kernel | fail | pass |

Counts among the 23 `syntax_gap` cases: **4× Fortran 2008**, **15× Fortran 2018**,
**4× Fortran 2023**.

### Negative controls — both fail

Toolchain limits, not OP2 logic. Flang Stage 1 errors, then fparser2 fallback
also fails, so translation does not complete.

| Case | Fortran standard | Construct | fparser2 | Flang |
|------|------------------|-----------|----------|-------|
| `procedure_pointer_init` | **Fortran 2008** | `PROCEDURE(…), POINTER :: p => target` | fail | fail |
| `enumeration_type` | **Fortran 2023** | `ENUMERATION TYPE` | fail | fail |

### Parse-only “standard” identifiers — both pass

Stage 1 does **not** run semantic analysis. New *intrinsics* that look like
ordinary function/subroutine references therefore parse on **both** backends.
These are useful robustness checks (unknown names must not crash Stage 1) but
they are **not** grammar gaps.

| Case | Fortran standard | What is in the source | fparser2 | Flang |
|------|------------------|------------------------|----------|-------|
| `complex_re_im` | **Fortran 2008** | Complex `%RE` designator | pass | pass |
| `contiguous_dummy` | **Fortran 2008** | `CONTIGUOUS` dummy attribute | pass | pass |
| `critical_construct` | **Fortran 2008** | `CRITICAL` / `END CRITICAL` | pass | pass |
| `co_sum` | **Fortran 2018** | `CALL CO_SUM(alpha)` | pass | pass |
| `failed_images` | **Fortran 2018** | `FAILED_IMAGES()` | pass | pass |
| `image_status` | **Fortran 2018** | `IMAGE_STATUS(1)` | pass | pass |
| `out_of_range` | **Fortran 2018** | `OUT_OF_RANGE(n, 0_1)` | pass | pass |
| `team_number` | **Fortran 2018** | `TEAM_NUMBER()` | pass | pass |

fparser2 0.2.4 already implements several Fortran 2008 features (`CONTIGUOUS`,
`%RE`/`%IM`, `CRITICAL`). Coarrays, `SYNC ALL`/`SYNC MEMORY`, and this pin’s
`MODULE PROCEDURE` submodule form remain syntax gaps (table above).

### OP2 pipeline — both pass (robustness edge cases)

Valid OP2 mini-apps. Unless noted, Flang stays on the native path (no
fparser2 fallback).

**API / Stage-1 extraction**

| Case | What it stresses | fparser2 | Flang |
|------|------------------|----------|-------|
| `indirect_map` | Mapped `op_arg_dat` + mapped `OP_INC` | pass | pass |
| `gbl_reduction` | `op_arg_gbl` `OP_INC` and `OP_MAX` | pass | pass |
| `arg_idx_info` | `op_arg_idx` + `op_arg_info` | pass | pass |
| `opt_arg_dat` | `op_opt_arg_dat` | pass | pass |
| `type_soa` | `"real(8):soa"` type strings | pass | pass |
| `type_alias_r8` | `"r8"` alias instead of `"real(8)"` | pass | pass |
| `string_single_quotes` | Single-quoted type/name strings | pass | pass |
| `mixed_case_api` | Mixed-case `Op_Par_Loop_2` / kernel names | pass | pass |
| `decl_const_in_module` | `op_decl_const` from a used module | pass | pass |
| `two_loops` | Two kernels / two `op_par_loop` sites | pass | pass |

**Preprocessor, includes, multi-file**

| Case | What it stresses | fparser2 | Flang |
|------|------------------|----------|-------|
| `multi_file_app` | Kernel in a separate module file | pass | pass |
| `macro_op_loop` | `#define` expanding to `op_par_loop_2` | pass | pass |
| `include_loop_site` | C `#include` of the loop site | pass | pass |
| `ifdef_around_loop` | Inactive `#ifdef` must not extract a missing kernel | pass | pass |
| `loop_inside_if` | `op_par_loop` nested in a host `IF` (text rewrite) | pass | pass |

**Kernel language / Stage-3**

| Case | What it stresses | fparser2 | Flang |
|------|------------------|----------|-------|
| `kernel_if_else` | `IF` / `ELSE IF` / `ELSE` | pass | pass |
| `kernel_do_counted` | Counted `DO` with stride on a vector dat | pass | pass |
| `kernel_array_triplet` | `u(:) = …` (Flang `part_ref`, not `funcref`) | pass | pass |
| `kernel_intrinsics` | `ABS` / `SQRT` / `MAX` | pass | pass |
| `kernel_logical` | Logical dat and `.NOT.` | pass | pass |
| `helper_function_call` | Contained `FUNCTION` called from the kernel | pass | pass |
| `funcref_vector_inc` | `u(i) = u(i)+…` (Flang often emits `funcref`) | pass | pass |

**Semantic validation (Stage 2)** — translation still exits 0; both parsers
emit the expected warning and set `loop.fallback` where applicable.

| Case | Warning | fparser2 | Flang |
|------|---------|----------|-------|
| `valid_const_write` | const written | pass_with_warning | pass_with_warning |
| `valid_op_read_write` | marked OP_READ but was written | pass_with_warning | pass_with_warning |
| `valid_op_inc_bad` | marked OP_INC but not incremented | pass_with_warning | pass_with_warning |
| `valid_runtime_local` | runtime dimension local arrays | pass_with_warning | pass_with_warning |
| `valid_child_read_write` | OP_READ write via child subroutine | pass_with_warning | pass_with_warning |
| `valid_arg_idx_write` | is an op_arg_idx but was written | pass_with_warning | pass_with_warning |
| `valid_slice_gbl` | element-wise access incompatible with stride insertion | pass_with_warning | pass_with_warning |

**Fallback**

| Case | What it stresses | fparser2 | Flang |
|------|------------------|----------|-------|
| `stage1_scan_fallback` | Broken `--flang-scan` stub; ordinary source | pass | **fallback** (then fparser2 succeeds) |

### Former Flang `INCLUDE` gap (`flang_gap`) — both pass

Fortran `INCLUDE "foo.inc"` is not expanded by the C preprocessor. After the
scan-tool path/`-I` fix, Flang resolves sibling `.inc` files the same way
fparser2 does.

| Case | What it stresses | fparser2 | Flang |
|------|------------------|----------|-------|
| `fortran_include_loop` | `INCLUDE` of the `op_par_loop` site | pass | pass |
| `fortran_include_host` | `INCLUDE` of host-only init | pass | pass |
| `fortran_include_nested` | Nested `INCLUDE`s | pass | pass |

### Notes for the write-up

- **Grammar vs names.** True Flang wins are *syntax* (new statement forms,
  assumed-rank `(..)`, `IMPORT, ALL`, unsigned types, conditional
  expressions). A new *intrinsic name* such as `IMAGE_STATUS` is not a parse
  failure for fparser2, because Stage 1 never consults an intrinsic table.
- **Kernel bodies matter.** `syntax_in_kernel` is the case that matches
  production: modern Fortran inside the extracted kernel, not only host setup.
- **Parity.** On ordinary OP2 Fortran (maps, reductions, `op_arg_idx`,
  optional args, macros, `#include`, mixed case, validation warnings) Flang
  and fparser2 now agree: both translate, and Flang does not fall back.
- Re-run the suite after upgrading fparser or LLVM Flang; expected fail/pass
  pairs can move if fparser2 grows F2018/F2023 grammar.
