# Parser evaluation examples — performance notes

Measurements for the unstructured-mesh examples (`airfoil`, `tri_diff`, `mesh_res`, `scale_mesh`, `scale_mesh_flat`): Flang Stage-1 subprocess/JSON overhead versus parse time, and runtime equivalence of generated `c_cuda` binaries (bandwidth, throughput, arithmetic intensity).

Reproduce with (WSL/Linux):

```bash
PYTHONUNBUFFERED=1 translator-v2/.python/bin/python3 translator-v2/parser_eval/examples/measure_performance.py
```

## Stage-1: subprocess / JSON vs parse time (Flang)

For each example the Fortran sources are preprocessed once, then scanned with a single `op2-flang-scan --batch --timing` process (warmup discarded; matches production `--parser flang`). Times below are **app totals**, mean of timed runs, in **milliseconds**. **Complete Flang pipeline** is the Python-observed wall (one spawn through `json.loads`); **LLVM Flang preprocess + parse** sums `Parsing::Prescan` + `Parse` over every translation unit inside that process.

| Example | Complete Flang pipeline (ms) | LLVM Flang preprocess + parse (ms) | JSON walk+emit (ms) | `json.loads` (ms) | spawn/IPC (ms) | materialise (ms) | LLVM ÷ complete | fparser2 full (ms) | fparser2 parse (ms) |
|---------|-----------------------------:|-----------------------------------:|-------------------:|-----------------:|---------------:|----------------:|----------------:|--------------------:|--------------------:|
| airfoil | 181.04 | 105.14 | 8.71 | 1.11 | 32.38 | 12.57 | 58.1% | 193.43 | 189.71 |
| mesh_res | 132.65 | 80.10 | 6.27 | 0.27 | 27.53 | 3.71 | 60.4% | 94.77 | 93.26 |
| scale_mesh | 381.60 | 234.47 | 29.07 | 19.90 | 13.82 | 42.11 | 61.4% | 807.96 | 778.01 |
| scale_mesh_flat | 236.62 | 150.91 | 29.10 | 21.70 | 12.07 | 4.52 | 63.8% | 746.71 | 719.50 |
| tri_diff | 143.72 | 85.00 | 7.07 | 0.26 | 31.59 | 4.10 | 59.1% | 77.46 | 76.21 |

### Interpretation

- **airfoil**: LLVM Flang preprocess + parse is 105.14 ms (58.1% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 54.76 ms (0.52× that LLVM time).
- **mesh_res**: LLVM Flang preprocess + parse is 80.10 ms (60.4% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 37.78 ms (0.47× that LLVM time).
- **scale_mesh**: LLVM Flang preprocess + parse is 234.47 ms (61.4% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 104.90 ms (0.45× that LLVM time).
- **scale_mesh_flat**: LLVM Flang preprocess + parse is 150.91 ms (63.8% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 67.39 ms (0.45× that LLVM time).
- **tri_diff**: LLVM Flang preprocess + parse is 85.00 ms (59.1% of the complete Flang pipeline). Non-parse overhead (materialise + walk/JSON emit + spawn/IPC + `json.loads`) is 43.02 ms (0.51× that LLVM time).

With `--batch`, spawn/IPC is paid **once per app** (~20–40 ms here), not once per file. Remaining non-parse cost is materialise + walk/JSON + `json.loads` (grows with fat kernel modules). fparser2 has no C++ subprocess: **parse** is `ParserFactory` only; **full** adds the in-process OP2 store walk (`fortran.parser.parseProgram`) over the same preprocessed inputs.

### Batching vs per-file spawn

Before `--batch`, Stage-1 launched a fresh `op2-flang-scan` for every source file (~35–40 ms spawn/IPC each). Production now uses one process and sequential Prescan→Parse→walk per TU. Per-file baselines below are frozen from the pre-batch harness; batch columns are the live measurements in the table above.

| Example | Files | Per-file wall (ms) | Batch wall (ms) | Wall batch÷per-file | Per-file spawn (ms) | Batch spawn (ms) | Per-file ÷ fparser2 | Batch ÷ fparser2 |
|---------|------:|-------------------:|----------------:|--------------------:|--------------------:|-----------------:|-------------------:|-------------------:|
| airfoil | 3 | 394.2 | 181.0 | 0.46× | 96.1 | 32.4 | 2.04× | 0.94× |
| mesh_res | 1 | 143.6 | 132.7 | 0.92× | 34.2 | 27.5 | 1.51× | 1.40× |
| scale_mesh | 10 | 1477.8 | 381.6 | 0.26× | 316.8 | 13.8 | 1.83× | 0.47× |
| scale_mesh_flat | 1 | 234.9 | 236.6 | 1.01× | 14.5 | 12.1 | 0.31× | 0.32× |
| tri_diff | 1 | 130.6 | 143.7 | 1.10× | 30.1 | 31.6 | 1.69× | 1.86× |

Multi-file apps gain the most: `airfoil` drops from ~2.7× fparser2 to ~1.1×; `scale_mesh` (10 files) from ~2.2× to **0.53×** (Flang faster). Single-file examples (`tri_diff`, `mesh_res`, `scale_mesh_flat`) change little on spawn — they already paid only one process hit — though `scale_mesh`’s LLVM parse also improved under batch because one process avoids repeating fixed per-invocation frontend setup across ten modules. True single Prescan+Parse over many TUs remains impossible; batching only amortises process/LLVM load.

Overall, the Flang Stage-1 path is **mixed**: worse on mesh_res, tri_diff, but **faster** on airfoil, scale_mesh, scale_mesh_flat. LLVM Flang preprocess + parse vs fparser2: airfoil 0.55×, mesh_res 0.86×, scale_mesh 0.30×, scale_mesh_flat 0.21×, tri_diff 1.12×. Complete Flang pipeline vs fparser2: airfoil 0.94×, mesh_res 1.40×, scale_mesh 0.47×, scale_mesh_flat 0.32×, tri_diff 1.86×. Production Stage-1 now uses `op2-flang-scan --batch` so spawn/IPC is paid once per app.

True multi-file `Prescan`+`Parse` in one Flang call is **not** possible (one translation unit per call). Batch mode runs Prescan→Parse→walk sequentially in **one process**, amortising LLVM binary load. fparser2 remains fully in-process with a lighter F2008-oriented AST. How the paths scale after batching:

| Scaling driver | Flang path (batched) | fparser2 path |
|---|---|---|
| **# of source files** | One spawn; Prescan/Parse still per file (some fixed per-TU cost) | Linear in parse work only |
| **Total Fortran size / AST size** | Both grow (parse is ~O(source/AST)) | Same |
| **# / size of kernels** (in same files) | Parse grows; **walk+JSON** grows with extracted kernel text / OP2 events | Parse grows; no JSON, but still walks/builds ASTs in Python |
| **Very large apps** | Batch + compiled frontend often beats fparser2 | Competitive on small TUs |

So many small files favour fparser2 (spawn dominates); few large files shrink Flang’s relative penalty; many kernels in one file mainly hurt both through source/AST size, with JSON emit still a small slice on these examples.

Everything is measured in milliseconds, and the Flang path will scale better to larger applications, so the translation performance cost is not a significant issue relative to the improvements in robustness.

### Scaling in practice (`scale_mesh`)

`scale_mesh` is a deliberately larger app: **10 source files / ~2.4k lines** of fat kernel modules versus `tri_diff`’s **1 file / ~200 lines**. Only one edge/cell kernel pair runs on the GPU (same mesh as `tri_diff`); the extra modules exist to inflate Stage-1 parse and per-file spawn cost.

| Metric | tri_diff | scale_mesh | scale ÷ tri |
|---|---:|---:|---:|
| Source files | 1 | 10 | 10.0× |
| Complete Flang pipeline (ms) | 143.7 | 381.6 | 2.66× |
| LLVM Flang preprocess + parse (ms) | 85.0 | 234.5 | 2.76× |
| spawn/IPC (ms) | 31.6 | 13.8 | 0.44× |
| fparser2 full (ms) | 77.5 | 808.0 | 10.43× |
| Complete Flang ÷ fparser2 | 1.86× | 0.47× | — |

With batching, spawn stays ~one hit (14 ms vs 32 ms) even at 10× files; complete Flang grows mainly with parse/AST work (2.66×), while fparser2 tracks total AST size more steeply (10.43×). Net: multi-file `scale_mesh` is **faster** than fparser2 (0.47×).

### Multi-file vs single-file (`scale_mesh_flat`)

`scale_mesh_flat` keeps the same fat kernel bodies in **1 source file** (program + `CONTAINS`) instead of `10` files. GPU work is unchanged.

| Metric | scale_mesh (multi) | scale_mesh_flat | flat ÷ multi |
|---|---:|---:|---:|
| Source files | 10 | 1 | 0.10× |
| Complete Flang pipeline (ms) | 381.6 | 236.6 | 0.62× |
| LLVM Flang preprocess + parse (ms) | 234.5 | 150.9 | 0.64× |
| spawn/IPC (ms) | 13.8 | 12.1 | 0.87× |
| fparser2 full (ms) | 808.0 | 746.7 | 0.92× |
| Complete Flang ÷ fparser2 | 0.47× | 0.32× | — |

After batching, spawn is already ~one hit on both (12 vs 14 ms). Flattening still helps LLVM Prescan+Parse (151 vs 234 ms) by removing per-TU frontend fixed cost. fparser2 is nearly unchanged (747 vs 808 ms). Complete Flang ÷ fparser2: 0.47× (multi) → 0.32× (flat).

## Runtime equivalence (`c_cuda`)

_Runtime section skipped in this run._

## Environment notes

- Stage-1 timings use `op2-flang-scan --batch --timing` (materialise / Prescan+Parse / walk+JSON / stdout per TU in one process) plus Python wall/`json.loads`. The batch-vs-per-file table compares against frozen pre-batch totals in `PER_FILE_BASELINE`.
- Hardware counters via `ncu` need NVIDIA GPU Performance Counter access (often blocked in WSL: `ERR_NVGPUCTRPERM`). The harness then falls back to the algorithmic model in `measure_performance.py`.
- Raw numbers are written to `performance_results.json` beside this README.
