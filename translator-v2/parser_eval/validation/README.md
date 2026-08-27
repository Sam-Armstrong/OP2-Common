# Flang vs fparser2 validation-check equivalence

Recorded **27 August 2026** against fparser **0.2.4** and the built `op2-flang-scan`. Mini-apps exercise the Stage-2 checks from the dissertation (kernel resolution, unknown dependencies, parameter/const conflicts, const writes, slice/stride, `OP_READ` / `op_arg_idx` writes, `OP_INC` linearisation, runtime-dimension locals).

Overall: **18/19** cases matched the recorded expectation on both parsers; **18/19** had equivalent fparser2 and Flang outcomes.

Reproduce with (WSL/Linux):

```bash
bash translator-v2/parser_eval/validation/run_validation.sh
```

## What is compared

Each case is translated with `--parser fparser2` and `--parser flang` (`-t seq -d`). The driver classifies the result as:

- **error** — `OpError` (missing / ambiguous kernel, arity mismatch)
- **fallback** — translation exits 0 with `loop.fallback = True`
- **warning** — translation exits 0 with a validation warning, no fallback
- **pass** — clean translation, no validation warning
- **fail** — non-zero exit that is not an `OpError` (parse / toolchain)

Equivalence requires the same class, the same warning/error kinds, and (for `OP_INC`) the same linearisation labels (`no-ref`, `multi-ref`, `no-op`, `non increment`, `index mismatch`).

## Results

| Case | Check | Expect | fparser2 | Flang | Kinds (fparser2 / Flang) | Equivalent |
|------|-------|--------|----------|-------|--------------------------|:----------:|
| `ambiguous_kernel` | kernel resolution | `error` | `error` | `error` | ambiguous kernel | yes |
| `arg_idx_write` | read-only writes | `fallback` | `fallback` | `fallback` | arg_idx written | yes |
| `arity_mismatch` | kernel resolution | `error` | `error` | `error` | arity mismatch | yes |
| `child_read_write` | read-only writes | `fallback` | `fallback` | `fallback` | OP_READ written | yes |
| `clean_kernel` | control | `pass` | `pass` | `pass` | — | yes |
| `const_write` | const writes | `fallback` | `fallback` | `fallback` | const written | yes |
| `missing_kernel` | kernel resolution | `error` | `error` | `error` | missing kernel | yes |
| `op_inc_index` | OP_INC | `fallback` | `fallback` | `fallback` | OP_INC (index mismatch) | yes |
| `op_inc_multiref` | OP_INC | `fallback` | `fallback` | `fallback` | OP_INC (multi-ref) | yes |
| `op_inc_noninc` | OP_INC | `fallback` | `fallback` | `fallback` | OP_INC (invalid usage) | yes |
| `op_inc_noop` | OP_INC | `fallback` | `fallback` | `fallback` | OP_INC (no-op) | yes |
| `op_inc_noref` | OP_INC | `fallback` | `fallback` | `fallback` | OP_INC (no-ref) | yes |
| `op_inc_ok` | OP_INC | `pass` | `pass` | `pass` | — | yes |
| `op_read_write` | read-only writes | `fallback` | `fallback` | `fallback` | OP_READ written | yes |
| `param_const_conflict` | parameter/const conflict | `warning` | `warning` | `warning` | param/const conflict | yes |
| `runtime_local` | runtime-dimension locals | `warning` | `warning` | `warning` | runtime local arrays | yes |
| `slice_dat` | slice/stride | `fallback` | `fallback` | `fallback` | slice/stride | yes |
| `slice_gbl` | slice/stride | `fallback` | `fallback` | `fallback` | slice/stride | yes |
| `unknown_dep` | unknown dependencies | `fallback` | `fallback` | `pass` | unknown dependencies / — | **no** |

## Disagreements

### `unknown_dep`

Kernel calls a subroutine that is not defined in the application

- fparser2: `fallback` kinds=['unknown dependencies'] inc=— — unknown_dep.F90/17:0: Warning: scale unknown subroutine/function references:
- Flang: `pass` kinds=— inc=— — Translated program 1 of 1: /tmp/op2_validation_work/unknown_dep/flang/unknown_dep.F90
- outcome: fparser2=fallback flang=pass
- kinds: fparser2=['unknown dependencies'] flang=[]
- flang: wanted fallback, got pass (Translated program 1 of 1: /tmp/op2_validation_work/unknown_dep/flang/unknown_dep.F90)
- flang: missing kind(s) ['unknown dependencies']; got []
- notes: fparser2 keeps unknown CALL names in entity.depends. Flang resolve_flang_dependencies drops names that do not resolve to a Function, so this check may not fire on the Flang path.

## Per-check notes

- **kernel resolution** (equivalent): `ambiguous_kernel`, `arity_mismatch`, `missing_kernel`
- **read-only writes** (equivalent): `arg_idx_write`, `child_read_write`, `op_read_write`
- **control** (equivalent): `clean_kernel`
- **const writes** (equivalent): `const_write`
- **OP_INC** (equivalent): `op_inc_index`, `op_inc_multiref`, `op_inc_noninc`, `op_inc_noop`, `op_inc_noref`, `op_inc_ok`
- **parameter/const conflict** (equivalent): `param_const_conflict`
- **runtime-dimension locals** (equivalent): `runtime_local`
- **slice/stride** (equivalent): `slice_dat`, `slice_gbl`
- **unknown dependencies** (outcome mismatch): `unknown_dep`

## Conclusions

fparser2 and Flang agreed on **18/19** cases. They diverged on `unknown_dep`. Every check that inspects kernel bodies the two parsers both see — kernel resolution (`OpError` for missing, ambiguous, and arity mismatch), const writes, slice/stride incompatibility, `OP_READ` and `op_arg_idx` writes (including via a child subroutine), `OP_INC` linearisation (`no-ref`, `multi-ref`, `no-op`, `index mismatch`, `invalid usage`), parameter/const name clashes, and runtime-dimension locals — produced the same class and message kind. The one gap is unknown callees: fparser2 keeps unresolved `CALL` names on `entity.depends` and sets `loop.fallback`, while Flang's `resolve_flang_dependencies` drops names that do not resolve to a known `Function` before `validateLoop` runs, so the Flang path translates `unknown_dep` as a clean pass. That is a Stage-1 name-resolution difference, not a different check implementation.

## Environment notes

- Validation runs with `-t seq -d`; codegen is only needed to observe `Generated loop host … (fallback)` and `store.json` `fallback` flags.
- Warning-only checks (param/const conflict, runtime-dimension locals) must not set `loop.fallback`.
- Flang `resolve_flang_dependencies` drops callee names that do not resolve to a known Function before `validateLoop` runs.
- Raw numbers: `translator-v2/parser_eval/validation/results.json`.
- Cases: `translator-v2/parser_eval/validation/cases/`.
