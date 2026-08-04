# Parser evaluation examples — performance notes

Measurements for the unstructured-mesh examples (`airfoil`, `tri_diff`, `mesh_res`, `scale_mesh`, `scale_mesh_flat`): Flang Stage-1 subprocess/JSON overhead versus parse time, and runtime equivalence of generated `c_cuda` binaries (bandwidth, throughput, arithmetic intensity).

Reproduce with (WSL/Linux):

```bash
PYTHONUNBUFFERED=1 translator-v2/.python/bin/python3 translator-v2/parser_eval/examples/measure_performance.py
```

## Stage-1: subprocess / JSON vs parse time (Flang)

For each example the Fortran sources are preprocessed once, then scanned with a single `op2-flang-scan --batch --timing` process (warmup discarded; matches production `--parser flang`). Times below are **app totals**, mean of timed runs, in **milliseconds**. **Complete Flang pipeline** is the Python-observed wall (one spawn through `json.loads`); **LLVM Flang preprocess + parse** sums `Parsing::Prescan` + `Parse` over every translation unit inside that process.

| Example | Complete Flang pipeline (ms) | LLVM Flang preprocess + parse (ms) | JSON walk+emit (ms) | `json.loads` (ms) | spawn/IPC (ms) | materialise (ms) | LLVM ÷ complete | fparser2 parse (ms) |
|---------|-----------------------------:|-----------------------------------:|-------------------:|-----------------:|---------------:|----------------:|----------------:|--------------------:|
| airfoil | 210.71 | 127.87 | 11.07 | 1.30 | 41.47 | 6.07 | 60.7% | 187.15 |
| mesh_res | 182.13 | 109.49 | 9.23 | 0.35 | 38.11 | 5.02 | 60.1% | 101.37 |
| scale_mesh | 399.89 | 236.95 | 28.46 | 18.80 | 21.63 | 48.18 | 59.3% | 753.50 |
| scale_mesh_flat | 259.26 | 163.87 | 30.43 | 19.24 | 19.88 | 5.34 | 63.2% | 728.49 |
| tri_diff | 170.78 | 100.84 | 8.60 | 0.22 | 37.24 | 4.92 | 59.0% | 107.58 |

### Interpretation

- **airfoil**: LLVM Flang preprocess + parse is 127.87 ms (60.7% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 59.92 ms (0.47× that LLVM time).
- **mesh_res**: LLVM Flang preprocess + parse is 109.49 ms (60.1% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 52.71 ms (0.48× that LLVM time).
- **scale_mesh**: LLVM Flang preprocess + parse is 236.95 ms (59.3% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 117.08 ms (0.49× that LLVM time).
- **scale_mesh_flat**: LLVM Flang preprocess + parse is 163.87 ms (63.2% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 74.88 ms (0.46× that LLVM time).
- **tri_diff**: LLVM Flang preprocess + parse is 100.84 ms (59.0% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 50.98 ms (0.51× that LLVM time).

With `--batch`, spawn/IPC is paid **once per app** (~20–40 ms here), not once per file. Remaining non-parse cost is materialise + walk/JSON + `json.loads` (grows with fat kernel modules). fparser2 has no C++ subprocess; its column is pure in-process parse time for the same preprocessed inputs.

Overall, the Flang Stage-1 path is **mixed**: worse on airfoil, mesh_res, tri_diff, but **faster** on scale_mesh, scale_mesh_flat. LLVM Flang preprocess + parse vs fparser2: airfoil 0.68×, mesh_res 1.08×, scale_mesh 0.31×, scale_mesh_flat 0.22×, tri_diff 0.94×. Complete Flang pipeline vs fparser2: airfoil 1.13×, mesh_res 1.80×, scale_mesh 0.53×, scale_mesh_flat 0.36×, tri_diff 1.59×. Production Stage-1 now uses `op2-flang-scan --batch` so spawn/IPC is paid once per app.

True multi-file `Prescan`+`Parse` in one Flang call is **not** possible (one translation unit per call). Batch mode runs Prescan→Parse→walk sequentially in **one process**, amortising LLVM binary load. fparser2 remains fully in-process with a lighter F2008-oriented AST. How the paths scale after batching:

| Scaling driver | Flang path (batched) | fparser2 path |
|---|---|---|
| **# of source files** | One spawn; Prescan/Parse still per file (some fixed per-TU cost) | Linear in parse work only |
| **Total Fortran size / AST size** | Both grow (parse is ~O(source/AST)) | Same |
| **# / size of kernels** (in same files) | Parse grows; **walk+JSON** grows with extracted kernel text / OP2 events | Parse grows; no JSON, but still walks/builds ASTs in Python |
| **Very large apps** | Batch + compiled frontend often beats fparser2 | Competitive on small TUs |

### Scaling in practice (`scale_mesh`)

`scale_mesh` is a deliberately larger app: **10 source files / ~2.4k lines** of fat kernel modules versus `tri_diff`’s **1 file / ~200 lines**. Only one edge/cell kernel pair runs on the GPU (same mesh as `tri_diff`); the extra modules exist to inflate Stage-1 parse and per-file spawn cost.

| Metric | tri_diff | scale_mesh | scale ÷ tri |
|---|---:|---:|---:|
| Source files | 1 | 10 | 10.0× |
| Complete Flang pipeline (ms) | 170.8 | 399.9 | 2.34× |
| LLVM Flang preprocess + parse (ms) | 100.8 | 236.9 | 2.35× |
| spawn/IPC (ms) | 37.2 | 21.6 | 0.58× |
| fparser2 parse (ms) | 107.6 | 753.5 | 7.00× |
| Complete Flang ÷ fparser2 | 1.59× | 0.53× | — |

With batching, spawn stays ~one hit (22 ms vs 37 ms) even at 10× files; complete Flang grows mainly with parse/AST work (2.34×), while fparser2 tracks total AST size more steeply (7.00×). Net: multi-file `scale_mesh` is **faster** than fparser2 (0.53×).

### Multi-file vs single-file (`scale_mesh_flat`)

`scale_mesh_flat` keeps the same fat kernel bodies in **1 source file** (program + `CONTAINS`) instead of `10` files. GPU work is unchanged.

| Metric | scale_mesh (multi) | scale_mesh_flat | flat ÷ multi |
|---|---:|---:|---:|
| Source files | 10 | 1 | 0.10× |
| Complete Flang pipeline (ms) | 399.9 | 259.3 | 0.65× |
| LLVM Flang preprocess + parse (ms) | 236.9 | 163.9 | 0.69× |
| spawn/IPC (ms) | 21.6 | 19.9 | 0.92× |
| fparser2 parse (ms) | 753.5 | 728.5 | 0.97× |
| Complete Flang ÷ fparser2 | 0.53× | 0.36× | — |

After batching, spawn is already ~one hit on both (20 vs 22 ms). Flattening still helps LLVM Prescan+Parse (164 vs 237 ms) by removing per-TU frontend fixed cost. fparser2 is nearly unchanged (728 vs 753 ms). Complete Flang ÷ fparser2: 0.53× (multi) → 0.36× (flat).

## Runtime equivalence (`c_cuda`)

_Runtime section skipped in this run._

## Environment notes

- Stage-1 timings use `op2-flang-scan --timing` (materialise / Prescan+Parse / walk+JSON / stdout) plus Python wall/`json.loads`.
- Hardware counters via `ncu` need NVIDIA GPU Performance Counter access (often blocked in WSL: `ERR_NVGPUCTRPERM`). The harness then falls back to the algorithmic model in `measure_performance.py`.
- Raw numbers are written to `performance_results.json` beside this README.
