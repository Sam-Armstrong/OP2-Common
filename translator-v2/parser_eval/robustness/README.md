# OP2 Fortran robustness suite

Minimal OP2 apps that assess Flang Stage-1 robustness relative to fparser2
(installed pin: fparser 0.2.0, `std=f2008`).

## Categories

| Category | Intent |
|----------|--------|
| `syntax_gap` | Fortran constructs fparser2 cannot parse; Flang should translate |
| `negative_control` | Constructs neither parser handles on this toolchain |
| `pipeline` | OP2 Flang-path stress (validation, multi-file, macros, funcref, fallback) |

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
