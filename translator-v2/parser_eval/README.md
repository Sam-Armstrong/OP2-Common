# Flang vs fparser2 parser evaluation suite

Compares OP2 Fortran translator output and runtime when using `--parser flang`
versus `--parser fparser2` on a small, extensible set of example applications.

## Layout

```
parser_eval/
  eval_parsers.py          # main evaluation driver
  run_eval.sh              # WSL/Linux entry point
  examples/
    <name>/
      example.json         # required: describes sources, targets, runtime
      ...                  # optional app sources / Makefile
```

Current examples (all unstructured-mesh style):

- `airfoil` — existing CFD mini-app
- `tri_diff` — triangular cell mesh, edge→cell diffusion
- `mesh_res` — edge residual with edge→node and edge→cell maps

## Adding an example

1. Create `examples/<name>/example.json` (see existing examples).
2. Either point `workdir` / `sources` at an existing app under `apps/fortran/`,
   or place a self-contained app next to `example.json` with a Makefile that
   includes `makefiles/f_app.mk`.
3. Re-run `./run_eval.sh` (optionally `--examples <name>`).

## Checks performed

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

Constructs that fparser2 cannot parse (Flang comparison) live under
[`robustness/`](robustness/):

```bash
bash translator-v2/parser_eval/robustness/run_robustness.sh
```
