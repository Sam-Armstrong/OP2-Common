# OP2 Fortran robustness suite

Minimal OP2 apps that exercise Fortran constructs **fparser2 cannot parse**
(installed translator pin: fparser 0.2.0, `std=f2008`). Each case is translated
with `--parser fparser2` (expected Stage-1 failure) and `--parser flang`
(report pass/fail, including any fparser2 fallback).

## Layout

```
robustness/
  eval_robustness.py
  run_robustness.sh
  cases/
    <name>/
      case.json
      <name>.F90
```

## Usage

```bash
bash translator-v2/parser_eval/robustness/run_robustness.sh
bash translator-v2/parser_eval/robustness/run_robustness.sh --cases assumed_rank unsigned
bash translator-v2/parser_eval/robustness/run_robustness.sh --keep-work /tmp/op2_robust
```

## Cases

| Case | Construct |
|------|-----------|
| `do_concurrent_shared` | F2018 `DO CONCURRENT ... SHARED(...)` |
| `do_concurrent_reduce` | F2018 `DO CONCURRENT ... REDUCE(+:...)` |
| `assumed_rank` | F2018 assumed-rank `x(..)` |
| `select_rank` | F2018 `SELECT RANK` |
| `assumed_type` | F2018 `TYPE(*)` |
| `implicit_none_external` | F2018 `IMPLICIT NONE (TYPE, EXTERNAL)` |
| `import_none` | F2018 `IMPORT, NONE` |
| `error_stop_quiet` | F2018 `ERROR STOP ..., QUIET=` |
| `conditional_expr` | F2023 `(cond ? x : y)` |
| `unsigned` | F2023 `UNSIGNED` |
