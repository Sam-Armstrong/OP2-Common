# Flang vs fparser2 equivalence evaluation

Recorded **26 August 2026** against fparser **0.2.4** and the built `op2-flang-scan`. Examples: `airfoil`, `mesh_res`, `tri_diff`. Overall: **3/3 examples passed** all hard checks.

This suite re-runs the dissertation equivalence tests (dependency trees, codegen time, `c_cuda` solution runtime) and adds loop-IR signatures, per-backend generated-artefact comparison, solution fingerprints, and derived GPU metrics (FLOPs, bandwidth utilisation, arithmetic intensity). Performance numbers are the **mean of three runs**.

Reproduce with (WSL/Linux):

```bash
bash translator-v2/parser_eval/run_equivalence.sh
```

## Backends

Translator `-t` names that exist for Fortran, plus makefile variants that **reuse** the same generated tree (MPI links the same kernels against `op2_mpi_*`; `genseq` is generated `seq` compiled through the sequential OP2 Fortran library; the makefile `seq` variant compiles the original sources and does not use translator output).

| Makefile variant | Translator target / output dir | Generated in this run? | Runnable here? |
|------------------|--------------------------------|------------------------:|----------------|
| `seq` | — | n/a (original sources) | yes (CPU; not timed — large meshes) |
| `genseq` | `seq` | yes | yes (CPU; not timed — large meshes) |
| `openmp` | `openmp` | yes | yes (CPU; built+run, both parsers) |
| `cuda` | `cuda` | yes | no (F_HAS_CUDA=false, needs nvfortran) |
| `hip` | `hip` | no (no Fortran HIP scheme) | no (no Fortran HIP scheme) |
| `c_cuda` | `c_cuda` | yes | yes (HAVE_C_CUDA; timed) |
| `c_hip` | `c_hip` | yes | no (no HIP/ROCm on this host) |
| `c_seq` | `c_seq` | yes | yes (CPU; not a f_app.mk variant — translator-only) |
| `mpi_seq` | — | n/a (original sources) | no (apps filter mpi_%; not required for artefact equivalence) |
| `mpi_genseq` | `seq` | yes | no (same generated tree as genseq) |
| `mpi_openmp` | `openmp` | yes | no (same generated tree as openmp) |
| `mpi_cuda` | `cuda` | yes | no (same generated tree as cuda) |
| `mpi_hip` | `hip` | no (no Fortran HIP scheme) | no (same generated tree as hip) |
| `mpi_c_cuda` | `c_cuda` | yes | no (same generated tree as c_cuda) |
| `mpi_c_hip` | `c_hip` | yes | no (same generated tree as c_hip) |

Hardware / toolchain notes: WSL Ubuntu, GNU Fortran, CUDA `sm_86` (RTX 3080-class). `HAVE_C_CUDA=true`, `F_HAS_CUDA=false`, no HIP. MPI makefile variants are filtered out of these apps (`VARIANT_FILTER_OUT := mpi_%`) and share generated trees.

## Code generation time

Wall time for a full translator invocation that requests every Fortran target (`seq`, `openmp`, `cuda`, `hip`, `c_cuda`, `c_hip`, `c_seq`), mean of 3 runs. Includes parse, validation, and kernel/host generation for each scheme that exists.

| Application | fparser2 (s) | Flang (s) | Ratio (Flang ÷ fparser2) |
|-------------|-------------:|----------:|-------------------------:|
| `airfoil` | 10.047±0.110 | 9.789±0.068 | 0.97× |
| `mesh_res` | 9.126±0.067 | 8.922±0.227 | 0.98× |
| `tri_diff` | 9.146±0.039 | 9.131±0.074 | 1.00× |

Samples:

- `airfoil` / `flang`: 9.712s, 9.843s, 9.813s
- `airfoil` / `fparser2`: 10.152s, 10.057s, 9.932s
- `mesh_res` / `flang`: 8.672s, 9.114s, 8.980s
- `mesh_res` / `fparser2`: 9.133s, 9.189s, 9.055s
- `tri_diff` / `flang`: 9.212s, 9.067s, 9.115s
- `tri_diff` / `fparser2`: 9.167s, 9.101s, 9.171s

## Dependency trees

Kernel → callee closure from `store.json` (`op2-translator -d`). Compared on `loop:` keys when present.

| Application | Loops | Match | Detail |
|-------------|------:|:-----:|--------|
| `airfoil` | 5 | yes | 5 entries match |
| `mesh_res` | 2 | yes | 2 entries match |
| `tri_diff` | 2 | yes | 2 entries match |

### `airfoil` closures

| Loop kernel | Transitive callees |
|-------------|--------------------|
| `adt_calc` | *(none)* |
| `bres_calc` | *(none)* |
| `res_calc` | *(none)* |
| `save_soln` | *(none)* |
| `update` | *(none)* |

### `mesh_res` closures

| Loop kernel | Transitive callees |
|-------------|--------------------|
| `res_calc` | `edge_contrib` |
| `update` | *(none)* |

### `tri_diff` closures

| Loop kernel | Transitive callees |
|-------------|--------------------|
| `cell_update` | *(none)* |
| `edge_flux` | `flux_pair` |

## Loop IR signatures

Per `op_par_loop`: kernel name, argument kinds, access modes, dat/map pointers, dimensions, SoA flags, and named constants. This is the translator's internal OP2 IR, independent of pretty-printed Fortran.

| Application | Loops | Match | Detail |
|-------------|------:|:-----:|--------|
| `airfoil` | 5 | yes | 5 loops match (kernel, args, maps, access modes, deps) |
| `mesh_res` | 2 | yes | 2 loops match (kernel, args, maps, access modes, deps) |
| `tri_diff` | 2 | yes | 2 loops match (kernel, args, maps, access modes, deps) |

### `airfoil` loops

| Kernel | nargs | Arguments (kind / access / map) |
|--------|------:|---------------------------------|
| `adt_calc` | 6 | ArgDat/READ/map0[1], ArgDat/READ/map0[2], ArgDat/READ/map0[3], ArgDat/READ/map0[4], ArgDat/READ, ArgDat/WRITE |
| `bres_calc` | 6 | ArgDat/READ/map0[1], ArgDat/READ/map0[2], ArgDat/READ/map1[1], ArgDat/READ/map1[1], ArgDat/INC/map1[1], ArgDat/READ |
| `res_calc` | 8 | ArgDat/READ/map0[1], ArgDat/READ/map0[2], ArgDat/READ/map1[1], ArgDat/READ/map1[2], ArgDat/READ/map1[1], ArgDat/READ/map1[2], ArgDat/INC/map1[1], ArgDat/INC/map1[2] |
| `save_soln` | 2 | ArgDat/READ, ArgDat/WRITE |
| `update` | 8 | ArgDat/READ, ArgDat/WRITE, ArgDat/RW, ArgDat/READ, ArgGbl/INC, ArgGbl/MAX, ArgIdx, ArgInfo |

### `mesh_res` loops

| Kernel | nargs | Arguments (kind / access / map) |
|--------|------:|---------------------------------|
| `res_calc` | 6 | ArgDat/READ/map0[1], ArgDat/READ/map0[2], ArgDat/READ/map1[1], ArgDat/READ/map1[2], ArgDat/INC/map1[1], ArgDat/INC/map1[2] |
| `update` | 7 | ArgDat/READ/map0[1], ArgDat/READ/map0[2], ArgDat/READ/map0[3], ArgDat/RW, ArgDat/RW, ArgGbl/INC, ArgGbl/MAX |

### `tri_diff` loops

| Kernel | nargs | Arguments (kind / access / map) |
|--------|------:|---------------------------------|
| `cell_update` | 7 | ArgDat/READ/map0[1], ArgDat/READ/map0[2], ArgDat/READ/map0[3], ArgDat/RW, ArgDat/RW, ArgGbl/INC, ArgGbl/MAX |
| `edge_flux` | 4 | ArgDat/READ/map0[1], ArgDat/READ/map0[2], ArgDat/INC/map0[1], ArgDat/INC/map0[2] |

## Kernel translation path (native vs sequential fallback)

Each Fortran scheme that has a sequential fallback (`c_cuda`, `c_hip`, `c_seq`, `cuda`, `openmp`) emits a native kernel host, a sequential `_fb` host, or both. A `(fallback)` line means Fortran→C/device translation failed and only sequential Fortran was generated for that loop. Equivalence requires the same native/fallback split.

| Application | Scheme | fparser2 native / fallback | Flang native / fallback | Match |
|-------------|--------|----------------------------|-------------------------|:-----:|
| `airfoil` | `Fortran/c_cuda` | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | yes |
| `airfoil` | `Fortran/c_hip` | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | yes |
| `airfoil` | `Fortran/c_seq` | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | yes |
| `airfoil` | `Fortran/cuda` | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | yes |
| `airfoil` | `Fortran/openmp` | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | yes |
| `airfoil` | `Fortran/seq` | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | airfoil_1_save_soln,airfoil_2_adt_calc,airfoil_3_res_calc,airfoil_4_bres_calc,airfoil_5_update / — | yes |
| `mesh_res` | `Fortran/c_cuda` | mesh_res_1_res_calc,mesh_res_2_update / — | mesh_res_1_res_calc,mesh_res_2_update / — | yes |
| `mesh_res` | `Fortran/c_hip` | mesh_res_1_res_calc,mesh_res_2_update / — | mesh_res_1_res_calc,mesh_res_2_update / — | yes |
| `mesh_res` | `Fortran/c_seq` | mesh_res_1_res_calc,mesh_res_2_update / — | mesh_res_1_res_calc,mesh_res_2_update / — | yes |
| `mesh_res` | `Fortran/cuda` | mesh_res_1_res_calc,mesh_res_2_update / — | mesh_res_1_res_calc,mesh_res_2_update / — | yes |
| `mesh_res` | `Fortran/openmp` | mesh_res_1_res_calc,mesh_res_2_update / — | mesh_res_1_res_calc,mesh_res_2_update / — | yes |
| `mesh_res` | `Fortran/seq` | mesh_res_1_res_calc,mesh_res_2_update / — | mesh_res_1_res_calc,mesh_res_2_update / — | yes |
| `tri_diff` | `Fortran/c_cuda` | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | yes |
| `tri_diff` | `Fortran/c_hip` | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | yes |
| `tri_diff` | `Fortran/c_seq` | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | yes |
| `tri_diff` | `Fortran/cuda` | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | yes |
| `tri_diff` | `Fortran/openmp` | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | yes |
| `tri_diff` | `Fortran/seq` | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | tri_diff_1_edge_flux,tri_diff_2_cell_update / — | yes |

## Generated artefacts per backend

For each translator target directory: same relative file tree; normalised C/C++/CUDA/HIP text equality. Fortran pretty-print is recorded (identifier tokens after folding `REAL(KIND=8)` → `real(8)`) but does not fail the check: Flang unparses cooked source while fparser2 pretty-prints the AST (`USE` order, split dummy declarations after OpenMP stride insertion). `hip` is expected to be absent: there is a `Hip` C++ target but no Fortran scheme.

| Application | Target | Files (flang / fparser2) | Tree | C/C++/CUDA | Fortran tokens |
|-------------|--------|-------------------------:|:----:|:----------:|:--------------:|
| `airfoil` | `seq` | 7 / 7 | yes | yes | yes |
| `airfoil` | `openmp` | 7 / 7 | yes | yes | no |
| `airfoil` | `cuda` | 7 / 7 | yes | yes | no |
| `airfoil` | `hip` | 0 / 0 | n/a | n/a | n/a |
| `airfoil` | `c_cuda` | 13 / 13 | yes | yes | yes |
| `airfoil` | `c_hip` | 13 / 13 | yes | yes | yes |
| `airfoil` | `c_seq` | 11 / 11 | yes | yes | yes |
| `mesh_res` | `seq` | 4 / 4 | yes | yes | yes |
| `mesh_res` | `openmp` | 4 / 4 | yes | yes | no |
| `mesh_res` | `cuda` | 4 / 4 | yes | yes | yes |
| `mesh_res` | `hip` | 0 / 0 | n/a | n/a | n/a |
| `mesh_res` | `c_cuda` | 7 / 7 | yes | yes | yes |
| `mesh_res` | `c_hip` | 7 / 7 | yes | yes | yes |
| `mesh_res` | `c_seq` | 5 / 5 | yes | yes | yes |
| `tri_diff` | `seq` | 4 / 4 | yes | yes | yes |
| `tri_diff` | `openmp` | 4 / 4 | yes | yes | no |
| `tri_diff` | `cuda` | 4 / 4 | yes | yes | yes |
| `tri_diff` | `hip` | 0 / 0 | n/a | n/a | n/a |
| `tri_diff` | `c_cuda` | 7 / 7 | yes | yes | yes |
| `tri_diff` | `c_hip` | 7 / 7 | yes | yes | yes |
| `tri_diff` | `c_seq` | 5 / 5 | yes | yes | yes |

Mismatches:

- `airfoil` / `openmp`: Fortran tokens differ: ['airfoil_2_adt_calc_kernel.F90', 'airfoil_3_res_calc_kernel.F90', 'airfoil_4_bres_calc_kernel.F90']
- `airfoil` / `cuda`: Fortran tokens differ: ['airfoil_2_adt_calc_kernel.F90', 'airfoil_3_res_calc_kernel.F90', 'airfoil_4_bres_calc_kernel.F90']
- `mesh_res` / `openmp`: Fortran tokens differ: ['mesh_res_1_res_calc_kernel.F90', 'mesh_res_2_update_kernel.F90']
- `tri_diff` / `openmp`: Fortran tokens differ: ['tri_diff_2_cell_update_kernel.F90']

## `c_cuda` runtime equivalence

Each example was translated with both parsers, built as `*_c_cuda`, and timed for wall-clock (mean of three runs). Nsight Compute GPU performance counters were unavailable (typical under WSL: `ERR_NVGPUCTRPERM`), so bandwidth utilisation, throughput, and arithmetic intensity are derived from an **algorithmic FLOP/byte model** (mesh sizes and kernel burn loops) divided by measured wall time. The same model is used for both parsers, so relative agreement tracks wall-time agreement; absolute util % uses an RTX 3080 peak DRAM bandwidth of 760 GB/s.

| Application | Parser | Wall (s) | Eff. BW (GB/s) | BW util % | GFLOP/s | AI (FLOP/B) | Source |
|-------------|--------|---------:|---------------:|----------:|-------:|------------:|--------|
| `airfoil` | flang | 5.679±0.024 | 13.2 | 1.73 | 25.4 | 1.923 | algorithmic_model |
| `airfoil` | fparser2 | 5.652±0.013 | 13.2 | 1.74 | 25.5 | 1.923 | algorithmic_model |
| `mesh_res` | flang | 24.641±0.063 | 32.8 | 4.32 | 126.1 | 3.843 | algorithmic_model |
| `mesh_res` | fparser2 | 24.681±0.072 | 32.8 | 4.31 | 125.9 | 3.843 | algorithmic_model |
| `tri_diff` | flang | 24.034±0.078 | 23.5 | 3.10 | 127.4 | 5.410 | algorithmic_model |
| `tri_diff` | fparser2 | 24.074±0.024 | 23.5 | 3.09 | 127.2 | 5.410 | algorithmic_model |

Wall-clock samples:

- `airfoil` / `flang`: 5.703s, 5.679s, 5.655s
- `airfoil` / `fparser2`: 5.657s, 5.661s, 5.637s
- `mesh_res` / `flang`: 24.568s, 24.669s, 24.684s
- `mesh_res` / `fparser2`: 24.701s, 24.742s, 24.602s
- `tri_diff` / `flang`: 23.944s, 24.085s, 24.072s
- `tri_diff` / `fparser2`: 24.047s, 24.080s, 24.093s

### Solution fingerprints

Numeric fields parsed from stdout (mass conservation sums, airfoil RMS-vs-reference, mesh sizes). These are independent of the FLOP model: they check that the generated kernels compute the same answer.

| Application | Field | fparser2 | Flang | Rel. diff |
|-------------|-------|---------:|------:|----------:|
| `airfoil` | `airfoil_rms_within_pct` | 3.681631e-06 | 3.681631e-06 | 0% |
| `mesh_res` | `ncell` | 7220000 | 7220000 | 0% |
| `mesh_res` | `nedge` | 10826200 | 10826200 | 0% |
| `mesh_res` | `q_max` | 0.786107 | 0.786107 | 0% |
| `mesh_res` | `sum_q` | 1.0 | 1.0 | 0% |
| `tri_diff` | `ncell` | 7220000 | 7220000 | 0% |
| `tri_diff` | `nedge` | 10826200 | 10826200 | 0% |
| `tri_diff` | `sum_u` | 1.0 | 1.0 | 0% |
| `tri_diff` | `u_max` | 0.00303459 | 0.00303459 | 0% |

### Interpretation

- **airfoil**: wall-clock relative difference 0.48% (flang=5.679s, fparser2=5.652s).
  - effective BW: flang=13.19, fparser2=13.25 (rel diff 0.48%)
  - BW util %: flang=1.735, fparser2=1.743 (rel diff 0.48%)
  - GFLOP/s: flang=25.36, fparser2=25.48 (rel diff 0.48%)
  - AI: flang=1.923, fparser2=1.923 (rel diff 0.00%)
- **mesh_res**: wall-clock relative difference 0.17% (flang=24.641s, fparser2=24.681s).
  - effective BW: flang=32.81, fparser2=32.76 (rel diff 0.17%)
  - BW util %: flang=4.317, fparser2=4.31 (rel diff 0.17%)
  - GFLOP/s: flang=126.1, fparser2=125.9 (rel diff 0.17%)
  - AI: flang=3.843, fparser2=3.843 (rel diff 0.00%)
- **tri_diff**: wall-clock relative difference 0.17% (flang=24.034s, fparser2=24.074s).
  - effective BW: flang=23.55, fparser2=23.51 (rel diff 0.17%)
  - BW util %: flang=3.099, fparser2=3.093 (rel diff 0.17%)
  - GFLOP/s: flang=127.4, fparser2=127.2 (rel diff 0.17%)
  - AI: flang=5.41, fparser2=5.41 (rel diff 0.00%)

Runtime (c_cuda, WSL, RTX 3080)

| App | fparser2 (s) | Flang (s) | Rel. diff | GFLOP/s | BW util % | AI (FLOP/B) |
|-----|-------------:|----------:|----------:|--------:|----------:|------------:|
| `airfoil` | 5.652±0.013 | 5.679±0.024 | **0.48%** | 25.5 / 25.4 | 1.74 / 1.73 | 1.923 |
| `mesh_res` | 24.681±0.072 | 24.641±0.063 | **0.17%** | 125.9 / 126.1 | 4.31 / 4.32 | 3.843 |
| `tri_diff` | 24.074±0.024 | 24.034±0.078 | **0.17%** | 127.2 / 127.4 | 3.09 / 3.10 | 5.410 |

GFLOP/s and BW util are fparser2 / Flang. AI is identical across parsers.

## Per-example checks

### `airfoil` — PASS

- **[OK]** codegen:flang: mean=9.789s std=0.068s samples=[9.712, 9.843, 9.813]
- **[OK]** codegen:fparser2: mean=10.047s std=0.110s samples=[10.152, 10.057, 9.932]
- **[OK]** loop dependency trees: 5 entries match
- **[OK]** loop IR signatures: 5 loops match (kernel, args, maps, access modes, deps)
- **[OK]** kernel path:Fortran/c_cuda: flang native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[] ; fparser2 native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[]
- **[OK]** kernel path:Fortran/c_hip: flang native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[] ; fparser2 native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[]
- **[OK]** kernel path:Fortran/c_seq: flang native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[] ; fparser2 native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[]
- **[OK]** kernel path:Fortran/cuda: flang native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[] ; fparser2 native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[]
- **[OK]** kernel path:Fortran/openmp: flang native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[] ; fparser2 native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[]
- **[OK]** kernel path:Fortran/seq: flang native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[] ; fparser2 native=['airfoil_1_save_soln', 'airfoil_2_adt_calc', 'airfoil_3_res_calc', 'airfoil_4_bres_calc', 'airfoil_5_update'] fallback=[]
- **[OK]** translated host programs: files=['airfoil.F90', 'airfoil_constants.F90', 'airfoil_kernels.F90'] fortran_token_diffs=['airfoil.F90'] (pretty-print ignored for pass/fail)
- **[OK]** target:seq: 7 files match
- **[OK]** target:openmp: Fortran tokens differ: ['airfoil_2_adt_calc_kernel.F90', 'airfoil_3_res_calc_kernel.F90', 'airfoil_4_bres_calc_kernel.F90'] (Fortran pretty-print token diffs ignored for pass/fail)
- **[OK]** target:cuda: Fortran tokens differ: ['airfoil_2_adt_calc_kernel.F90', 'airfoil_3_res_calc_kernel.F90', 'airfoil_4_bres_calc_kernel.F90'] (Fortran pretty-print token diffs ignored for pass/fail)
- **[OK]** target:hip: not generated (no Fortran scheme, or translator skipped)
- **[OK]** target:c_cuda: 13 files match
- **[OK]** target:c_hip: 13 files match
- **[OK]** target:c_seq: 11 files match
- **[OK]** codegen time comparable: flang=9.789s fparser2=10.047s ratio=1.03 (mean of 3)
- **[OK]** runtime:flang: wall=5.679±0.024s passed=True
- **[OK]** runtime:fparser2: wall=5.652±0.013s passed=True
- **[OK]** runtime roughly equal: flang=5.679s fparser2=5.652s rel_diff=0.0048 tol=0.02
- **[OK]** solution fingerprints: 1 numeric fields agree
- **[OK]** metric:GFLOP/s: flang=25.3574 fparser2=25.4796 rel_diff=0.0048 (tracks wall time under the algorithmic model)
- **[OK]** metric:effective BW: flang=13.1859 fparser2=13.2494 rel_diff=0.0048 (tracks wall time under the algorithmic model)
- **[OK]** metric:BW util %: flang=1.73498 fparser2=1.74334 rel_diff=0.0048 (tracks wall time under the algorithmic model)
- **[OK]** metric:AI: flang=1.92308 fparser2=1.92308 rel_diff=0.0000 (tracks wall time under the algorithmic model)

### `mesh_res` — PASS

- **[OK]** codegen:flang: mean=8.922s std=0.227s samples=[8.672, 9.114, 8.98]
- **[OK]** codegen:fparser2: mean=9.126s std=0.067s samples=[9.133, 9.189, 9.055]
- **[OK]** loop dependency trees: 2 entries match
- **[OK]** loop IR signatures: 2 loops match (kernel, args, maps, access modes, deps)
- **[OK]** kernel path:Fortran/c_cuda: flang native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[] ; fparser2 native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[]
- **[OK]** kernel path:Fortran/c_hip: flang native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[] ; fparser2 native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[]
- **[OK]** kernel path:Fortran/c_seq: flang native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[] ; fparser2 native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[]
- **[OK]** kernel path:Fortran/cuda: flang native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[] ; fparser2 native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[]
- **[OK]** kernel path:Fortran/openmp: flang native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[] ; fparser2 native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[]
- **[OK]** kernel path:Fortran/seq: flang native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[] ; fparser2 native=['mesh_res_1_res_calc', 'mesh_res_2_update'] fallback=[]
- **[OK]** translated host programs: files=['mesh_res.F90'] fortran_token_diffs=['mesh_res.F90'] (pretty-print ignored for pass/fail)
- **[OK]** target:seq: 4 files match
- **[OK]** target:openmp: Fortran tokens differ: ['mesh_res_1_res_calc_kernel.F90', 'mesh_res_2_update_kernel.F90'] (Fortran pretty-print token diffs ignored for pass/fail)
- **[OK]** target:cuda: 4 files match
- **[OK]** target:hip: not generated (no Fortran scheme, or translator skipped)
- **[OK]** target:c_cuda: 7 files match
- **[OK]** target:c_hip: 7 files match
- **[OK]** target:c_seq: 5 files match
- **[OK]** codegen time comparable: flang=8.922s fparser2=9.126s ratio=1.02 (mean of 3)
- **[OK]** runtime:flang: wall=24.641±0.063s passed=True
- **[OK]** runtime:fparser2: wall=24.681±0.072s passed=True
- **[OK]** runtime roughly equal: flang=24.641s fparser2=24.681s rel_diff=0.0017 tol=0.02
- **[OK]** solution fingerprints: 4 numeric fields agree
- **[OK]** metric:GFLOP/s: flang=126.099 fparser2=125.89 rel_diff=0.0017 (tracks wall time under the algorithmic model)
- **[OK]** metric:effective BW: flang=32.8106 fparser2=32.7561 rel_diff=0.0017 (tracks wall time under the algorithmic model)
- **[OK]** metric:BW util %: flang=4.31718 fparser2=4.31002 rel_diff=0.0017 (tracks wall time under the algorithmic model)
- **[OK]** metric:AI: flang=3.84324 fparser2=3.84324 rel_diff=0.0000 (tracks wall time under the algorithmic model)

### `tri_diff` — PASS

- **[OK]** codegen:flang: mean=9.131s std=0.074s samples=[9.212, 9.067, 9.115]
- **[OK]** codegen:fparser2: mean=9.146s std=0.039s samples=[9.167, 9.101, 9.171]
- **[OK]** loop dependency trees: 2 entries match
- **[OK]** loop IR signatures: 2 loops match (kernel, args, maps, access modes, deps)
- **[OK]** kernel path:Fortran/c_cuda: flang native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[] ; fparser2 native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[]
- **[OK]** kernel path:Fortran/c_hip: flang native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[] ; fparser2 native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[]
- **[OK]** kernel path:Fortran/c_seq: flang native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[] ; fparser2 native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[]
- **[OK]** kernel path:Fortran/cuda: flang native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[] ; fparser2 native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[]
- **[OK]** kernel path:Fortran/openmp: flang native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[] ; fparser2 native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[]
- **[OK]** kernel path:Fortran/seq: flang native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[] ; fparser2 native=['tri_diff_1_edge_flux', 'tri_diff_2_cell_update'] fallback=[]
- **[OK]** translated host programs: files=['tri_diff.F90'] fortran_token_diffs=['tri_diff.F90'] (pretty-print ignored for pass/fail)
- **[OK]** target:seq: 4 files match
- **[OK]** target:openmp: Fortran tokens differ: ['tri_diff_2_cell_update_kernel.F90'] (Fortran pretty-print token diffs ignored for pass/fail)
- **[OK]** target:cuda: 4 files match
- **[OK]** target:hip: not generated (no Fortran scheme, or translator skipped)
- **[OK]** target:c_cuda: 7 files match
- **[OK]** target:c_hip: 7 files match
- **[OK]** target:c_seq: 5 files match
- **[OK]** codegen time comparable: flang=9.131s fparser2=9.146s ratio=1.00 (mean of 3)
- **[OK]** runtime:flang: wall=24.034±0.078s passed=True
- **[OK]** runtime:fparser2: wall=24.074±0.024s passed=True
- **[OK]** runtime roughly equal: flang=24.034s fparser2=24.074s rel_diff=0.0017 tol=0.02
- **[OK]** solution fingerprints: 4 numeric fields agree
- **[OK]** metric:GFLOP/s: flang=127.391 fparser2=127.18 rel_diff=0.0017 (tracks wall time under the algorithmic model)
- **[OK]** metric:effective BW: flang=23.5487 fparser2=23.5098 rel_diff=0.0017 (tracks wall time under the algorithmic model)
- **[OK]** metric:BW util %: flang=3.09852 fparser2=3.09339 rel_diff=0.0017 (tracks wall time under the algorithmic model)
- **[OK]** metric:AI: flang=5.40968 fparser2=5.40968 rel_diff=0.0000 (tracks wall time under the algorithmic model)

## Overall findings

- **Verdict:** both parsers produce equivalent solutions on this suite.
- **Dependency trees:** match on every example.
- **Loop IR:** argument signatures, maps, and access modes match.
- **Generated backends:** file trees and kernel text match for every scheme that emitted files. Not generated: `hip`.
- **Codegen time** (Flang ÷ fparser2, mean of 3): airfoil 0.97×, mesh_res 0.98×, tri_diff 1.00×.
- **`c_cuda` wall-clock** relative difference (mean of 3): airfoil 0.48%, mesh_res 0.17%, tri_diff 0.17%.
- **FLOPs / bandwidth / AI:** under the algorithmic model these track wall time (identical total FLOPs and bytes per example). Nsight Compute counters, when available, would compare measured DRAM traffic and fp64 op counts directly.
- **MPI / HIP / CUDA Fortran:** MPI variants share the non-MPI generated trees (link-time difference only). Fortran `hip` has no scheme. Fortran `cuda` (CUDA Fortran `.CUF`) is generated but not runnable without `nvfortran` (`F_HAS_CUDA=false` here). `c_hip` is generated but not runnable without ROCm.

## Environment notes

- Codegen requests every Fortran translator target listed above; schemes without a Fortran implementation are skipped by the translator.
- Runtime is `c_cuda` only (the backend used in the dissertation table). One discarded GPU warmup precedes the three timed samples.
- fparser2 Fortran→C registers `f2008.Intrinsic_Function_Reference` (e.g. ABS) onto the existing f2003 handler. Without that, `c_cuda`/`c_hip`/`c_seq` fall back to sequential Fortran.
- Nsight Compute needs NVIDIA GPU performance-counter access (often `ERR_NVGPUCTRPERM` in WSL); the harness then uses the algorithmic model in `measure_performance.py`.
- Raw numbers: `translator-v2/parser_eval/equivalence/results.json`.
- Generated artefacts: `translator-v2/parser_eval/equivalence/out/`.

## Fortran OpenMP, CUDA Fortran, and hybrid C CUDA

The Fortran OpenMP (`openmp` / `f_openmp`), CUDA Fortran (`cuda` / `f_cuda`), and hybrid Fortran-host + C CUDA (`c_cuda` / `f_c_cuda`) backends also produced equivalent solutions with fparser2 and Flang. Both parsers emit native (non-fallback) kernels and matching generated file trees for all three schemes; C/CUDA kernel text is identical, and Fortran token differences on some OpenMP and CUDA Fortran files are pretty-print only (`USE` order, split dummy declarations after OpenMP stride insertion). OpenMP binaries built from each parser passed the application tests on every example with identical fingerprints: `airfoil` RMS 3.681633×10⁻⁶ % of the reference (main-loop 27.741 s Flang vs 27.755 s fparser2, 0.05 % relative difference); `mesh_res` `ncell=7220000`, `nedge=10826200`, `sum(q)=1`, `q_max=0.786107`; `tri_diff` the same mesh sizes with `sum(u)=1`, `u_max=0.00303459`. Hybrid `c_cuda` is the timed GPU path above (wall-clock relative differences 0.17–0.48 %, identical fingerprints). CUDA Fortran is generated by both parsers but could not be compiled here: `F_HAS_CUDA=false` and GNU Fortran cannot open `cudafor.mod` (NVIDIA `nvfortran` is required).
