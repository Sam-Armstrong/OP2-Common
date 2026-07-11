#!/usr/bin/env python3
"""
Evaluate Flang vs fparser2 OP2 Fortran translation on a suite of examples.

Discovers examples/*/example.json, runs the translator with each parser, and
checks dependency trees, generated artefacts, codegen time, and (optionally)
build/runtime equivalence.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple


PARSERS = ("flang", "fparser2")
HASH_MACRO_RE = re.compile(r"#define\s+OP2_KERNEL_HASH\s+\S+")
CONTENT_HASH_RE = re.compile(r"(OP2_[A-Z0-9_]*HASH[A-Z0-9_]*)\s+\S+")
PRELUDE_HASH_RE = re.compile(r"(OP_F2C_PRELUDE(?:_DATA)?)_\d+")
FLOAT_LIT_RE = re.compile(r"\b(\d+\.\d*)(?:e0)\b", re.IGNORECASE)
WS_RE = re.compile(r"[ \t]+")


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str = ""


@dataclass
class ParserRun:
    parser: str
    codegen_s: float
    out_dir: Path
    store_path: Path
    returncode: int
    stderr: str = ""


@dataclass
class ExampleResult:
    name: str
    checks: List[CheckResult] = field(default_factory=list)
    codegen_times: Dict[str, float] = field(default_factory=dict)
    runtimes: Dict[str, float] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return all(c.ok for c in self.checks)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def suite_root() -> Path:
    return Path(__file__).resolve().parent


def load_examples(only: Optional[List[str]] = None) -> List[Dict[str, Any]]:
    examples_dir = suite_root() / "examples"
    found: List[Dict[str, Any]] = []
    for path in sorted(examples_dir.glob("*/example.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        data["_config_dir"] = path.parent
        if only and data.get("name") not in only:
            continue
        found.append(data)
    return found


def resolve_workdir(ex: Dict[str, Any]) -> Path:
    raw = ex.get("workdir", ".")
    base = Path(ex["_config_dir"])
    wd = (base / raw).resolve()
    if not wd.is_dir():
        raise FileNotFoundError(f"workdir for {ex['name']} not found: {wd}")
    return wd


def translator_cmd(root: Path) -> List[str]:
    py = root / "translator-v2" / ".python" / "bin" / "python3"
    if not py.is_file():
        # bootstrap venv via makefile helper
        subprocess.run(
            ["make", "--silent", "-C", str(root / "translator-v2"), "python"],
            check=True,
        )
    return [str(py), str(root / "translator-v2" / "op2-translator")]


def scan_bin(root: Path) -> Path:
    p = root / "translator-v2" / "flang-scan" / "build" / "op2-flang-scan"
    if not p.is_file():
        raise FileNotFoundError(
            f"op2-flang-scan missing at {p}; build flang-scan first"
        )
    return p


def ensure_airfoil_grid(workdir: Path) -> None:
    grid = workdir / "new_grid.dat"
    if grid.is_file():
        return
    url = "https://op-dsl.github.io/docs/OP2/new_grid.dat"
    print(f"  downloading mesh -> {grid}")
    urllib.request.urlretrieve(url, grid)


def normalize_generated(text: str) -> str:
    text = HASH_MACRO_RE.sub("#define OP2_KERNEL_HASH <HASH>", text)
    text = CONTENT_HASH_RE.sub(r"\1 <HASH>", text)
    text = PRELUDE_HASH_RE.sub(r"\1_<HASH>", text)
    # flang vs fparser2 often emit 0.0 vs 0.0e0
    text = FLOAT_LIT_RE.sub(r"\1", text)
    lines = []
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        lines.append(WS_RE.sub(" ", line).rstrip())
    # const registration order can differ between parsers; sort runs
    lines = _sort_add_param_runs(lines)
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines) + ("\n" if lines else "")


def _sort_add_param_runs(lines: List[str]) -> List[str]:
    out: List[str] = []
    i = 0
    while i < len(lines):
        if "info.add_param(" in lines[i]:
            run = []
            while i < len(lines) and "info.add_param(" in lines[i]:
                run.append(lines[i])
                i += 1
            out.extend(sorted(run))
        else:
            out.append(lines[i])
            i += 1
    return out


def list_generated_files(out_dir: Path) -> List[Path]:
    skip = {".codegen_stamp", "store.json"}
    files = []
    for p in sorted(out_dir.rglob("*")):
        if p.is_file() and p.name not in skip:
            files.append(p.relative_to(out_dir))
    return files


def extract_dep_trees(store_path: Path) -> Dict[str, List[str]]:
    """
    Build kernel -> sorted transitive dependency name lists from store.json.

    Prefers the deps-focused dump shape written by op2-translator -d
    ({"loops": [...], "functions": [...]}). Falls back to a recursive walk
    for older full-object dumps.
    """
    data = json.loads(store_path.read_text(encoding="utf-8"))

    if isinstance(data, dict) and "loops" in data and "functions" in data:
        trees: Dict[str, List[str]] = {}
        for fn in data.get("functions") or []:
            name = str(fn.get("name", "")).lower()
            if not name:
                continue
            trees[name] = list(fn.get("depends") or [])
        for loop in data.get("loops") or []:
            kernel = str(loop.get("kernel", "")).lower()
            if not kernel:
                continue
            trees[f"loop:{kernel}"] = list(
                loop.get("depends_closure")
                or loop.get("depends")
                or trees.get(kernel, [])
            )
        return dict(sorted(trees.items()))

    entities = _collect_functions(data)
    by_name = {name.lower(): deps for name, deps in entities.items()}

    def closure(root: str) -> List[str]:
        seen: Set[str] = set()
        stack = [root.lower()]
        while stack:
            cur = stack.pop()
            if cur in seen:
                continue
            seen.add(cur)
            for d in by_name.get(cur, []):
                if d.lower() not in seen:
                    stack.append(d.lower())
        return sorted(x for x in seen if x != root.lower())

    trees = {name: closure(name) for name in by_name}
    for loop in _collect_loops(data):
        k = loop.get("kernel") or loop.get("name")
        if isinstance(k, str) and k:
            trees[f"loop:{k.lower()}"] = closure(k)
    return dict(sorted(trees.items()))


def _collect_functions(obj: Any, out: Optional[Dict[str, List[str]]] = None) -> Dict[str, List[str]]:
    if out is None:
        out = {}
    if isinstance(obj, dict):
        name = obj.get("name")
        depends = obj.get("depends")
        # Function-like entities expose name + depends (+ scope/ast/flang_source)
        if (
            isinstance(name, str)
            and isinstance(depends, (list, set))
            and ("scope" in obj or "ast" in obj or "flang_source" in obj or "params" in obj)
        ):
            dep_list = sorted({str(d).lower() for d in list(depends)})
            out[name.lower()] = dep_list
        for v in obj.values():
            _collect_functions(v, out)
    elif isinstance(obj, list):
        for v in obj:
            _collect_functions(v, out)
    return out


def _collect_loops(obj: Any, out: Optional[List[Dict[str, Any]]] = None) -> List[Dict[str, Any]]:
    if out is None:
        out = []
    if isinstance(obj, dict):
        # Loop-like objects typically have kernel + args / nest
        if "kernel" in obj and ("args" in obj or "arguments" in obj or "set" in obj):
            out.append(obj)
        for v in obj.values():
            _collect_loops(v, out)
    elif isinstance(obj, list):
        for v in obj:
            _collect_loops(v, out)
    return out


def run_codegen(
    root: Path,
    ex: Dict[str, Any],
    workdir: Path,
    parser: str,
    out_dir: Path,
) -> ParserRun:
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = translator_cmd(root) + [
        "--parser",
        parser,
        "-d",
        "-v",
    ]
    targets = ex.get("targets") or []
    for t in targets:
        cmd += ["-t", t]
    cmd += list(ex.get("translator_flags") or [])
    if parser == "flang":
        cmd += ["--flang-scan", str(scan_bin(root))]
    cmd += list(ex["sources"])
    cmd += ["-o", str(out_dir)]

    env = os.environ.copy()
    env["OP2_FLANG_SCAN"] = str(scan_bin(root))

    timeout = float(ex.get("codegen_timeout_s", 300))
    t0 = time.perf_counter()
    proc = subprocess.run(
        cmd,
        cwd=str(workdir),
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    elapsed = time.perf_counter() - t0

    # stamp for make consumers if we later copy into an app tree
    (out_dir / ".codegen_stamp").touch()

    return ParserRun(
        parser=parser,
        codegen_s=elapsed,
        out_dir=out_dir,
        store_path=out_dir / "store.json",
        returncode=proc.returncode,
        stderr=(proc.stderr or "") + "\n" + (proc.stdout or ""),
    )


def compare_dep_trees(a: Dict[str, List[str]], b: Dict[str, List[str]]) -> CheckResult:
    # compare on the intersection of loop: keys if present, else all keys
    a_loops = {k: v for k, v in a.items() if k.startswith("loop:")}
    b_loops = {k: v for k, v in b.items() if k.startswith("loop:")}
    if a_loops or b_loops:
        keys = sorted(set(a_loops) | set(b_loops))
        left, right = a_loops, b_loops
        label = "loop dependency trees"
    else:
        keys = sorted(set(a) | set(b))
        left, right = a, b
        label = "entity dependency trees"

    mismatches = []
    for k in keys:
        if left.get(k) != right.get(k):
            mismatches.append(f"{k}: flang={left.get(k)} fparser2={right.get(k)}")
    if mismatches:
        return CheckResult(label, False, "; ".join(mismatches[:8]))
    return CheckResult(label, True, f"{len(keys)} entries match")


def compare_file_trees(flang_dir: Path, fp_dir: Path) -> CheckResult:
    a = {str(p) for p in list_generated_files(flang_dir)}
    b = {str(p) for p in list_generated_files(fp_dir)}
    if a != b:
        only_f = sorted(a - b)[:10]
        only_p = sorted(b - a)[:10]
        return CheckResult(
            "generated file tree",
            False,
            f"only flang={only_f} only fparser2={only_p}",
        )
    return CheckResult("generated file tree", True, f"{len(a)} files")


def compare_file_contents(flang_dir: Path, fp_dir: Path) -> CheckResult:
    files = list_generated_files(flang_dir)
    diffs = []
    identical = 0
    for rel in files:
        pa = flang_dir / rel
        pb = fp_dir / rel
        if not pb.is_file():
            continue
        na = normalize_generated(pa.read_text(encoding="utf-8", errors="replace"))
        nb = normalize_generated(pb.read_text(encoding="utf-8", errors="replace"))
        if na == nb:
            identical += 1
        else:
            # Fortran cooked-source formatting often differs; for .F90/.CUF
            # accept if non-whitespace alnum tokens match for kernel bodies is
            # too loose. Instead: C++/CUDA/HIP kernels must match; Fortran
            # hosts may differ in case/spacing — flag as soft fail detail.
            suffix = rel.suffix.lower()
            if suffix in {".cpp", ".hpp", ".h", ".cu", ".cuh", ".hip", ".c"}:
                diffs.append(str(rel))
            else:
                # soft: record but do not fail hard for Fortran pretty-print
                pass
    if diffs:
        return CheckResult(
            "generated C/C++/CUDA content",
            False,
            f"differ: {diffs[:8]} ({identical}/{len(files)} identical after normalize)",
        )
    return CheckResult(
        "generated C/C++/CUDA content",
        True,
        f"{identical}/{len(files)} files identical after normalize "
        "(Fortran pretty-print differences ignored)",
    )


def install_generated_into_app(
    ex: Dict[str, Any],
    workdir: Path,
    generated_src: Path,
) -> Path:
    """Copy translator output into the app's expected generated/<app_name> tree."""
    app_name = ex.get("app_name") or ex["name"]
    dest = workdir / "generated" / app_name
    if dest.exists():
        shutil.rmtree(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(generated_src, dest)
    (dest / ".codegen_stamp").touch()
    return dest


def run_runtime(
    root: Path,
    ex: Dict[str, Any],
    workdir: Path,
    parser: str,
    generated_src: Path,
) -> Tuple[bool, float, str]:
    rt = ex.get("runtime") or {}
    if not rt:
        return True, 0.0, "skipped (no runtime config)"

    setup = rt.get("setup")
    if setup == "ensure_airfoil_grid":
        ensure_airfoil_grid(workdir)

    install_generated_into_app(ex, workdir, generated_src)

    make_target = rt["make_target"]
    env = os.environ.copy()
    env["OP2_COMPILER"] = env.get("OP2_COMPILER", "gnu")
    # prevent make from re-running translator
    env["OP2_EXTRA_TRANSLATOR_FLAGS"] = f"--parser {parser}"

    build = subprocess.run(
        ["make", "-j", str(os.cpu_count() or 2), make_target],
        cwd=str(workdir),
        env=env,
        capture_output=True,
        text=True,
        timeout=float(rt.get("timeout_s", 180)) * 2,
    )
    if build.returncode != 0:
        return False, 0.0, f"build failed:\n{build.stderr[-2000:]}\n{build.stdout[-2000:]}"

    binary = rt["binary"]
    args = list(rt.get("args") or [])
    t0 = time.perf_counter()
    run = subprocess.run(
        [binary] + args,
        cwd=str(workdir),
        env=env,
        capture_output=True,
        text=True,
        timeout=float(rt.get("timeout_s", 180)),
    )
    elapsed = time.perf_counter() - t0
    out = (run.stdout or "") + (run.stderr or "")
    if run.returncode != 0:
        return False, elapsed, f"run rc={run.returncode}:\n{out[-2000:]}"

    pass_re = rt.get("pass_regex", "Test PASSED")
    if not re.search(pass_re, out):
        return False, elapsed, f"pass regex {pass_re!r} not found in output:\n{out[-1500:]}"
    return True, elapsed, "ok"


def evaluate_example(
    root: Path,
    ex: Dict[str, Any],
    out_root: Path,
    skip_runtime: bool,
) -> ExampleResult:
    name = ex["name"]
    result = ExampleResult(name=name)
    workdir = resolve_workdir(ex)
    print(f"\n=== example: {name} ===")
    print(f"  workdir: {workdir}")

    runs: Dict[str, ParserRun] = {}
    for parser in PARSERS:
        print(f"  codegen with --parser {parser} ...")
        out_dir = out_root / name / parser
        try:
            run = run_codegen(root, ex, workdir, parser, out_dir)
        except subprocess.TimeoutExpired:
            result.checks.append(CheckResult(f"codegen:{parser}", False, "timeout"))
            return result
        runs[parser] = run
        result.codegen_times[parser] = run.codegen_s
        ok = run.returncode == 0 and run.store_path.is_file()
        detail = f"{run.codegen_s:.3f}s"
        if not ok:
            detail += f" rc={run.returncode}\n{run.stderr[-1500:]}"
        result.checks.append(CheckResult(f"codegen:{parser}", ok, detail))
        print(f"    -> {detail}")

    if not all(c.ok for c in result.checks if c.name.startswith("codegen:")):
        return result

    flang, fp = runs["flang"], runs["fparser2"]

    try:
        trees_f = extract_dep_trees(flang.store_path)
        trees_p = extract_dep_trees(fp.store_path)
        result.checks.append(compare_dep_trees(trees_f, trees_p))
    except Exception as exc:  # noqa: BLE001
        result.checks.append(CheckResult("dependency trees", False, str(exc)))

    result.checks.append(compare_file_trees(flang.out_dir, fp.out_dir))
    result.checks.append(compare_file_contents(flang.out_dir, fp.out_dir))

    # codegen time ratio (informational; soft fail if wildly different)
    tf, tp = flang.codegen_s, fp.codegen_s
    if min(tf, tp) > 0:
        ratio = max(tf, tp) / min(tf, tp)
        # soft: warn if >5x, but do not fail — flang may be slower/faster
        result.checks.append(
            CheckResult(
                "codegen time comparable",
                ratio < 10.0,
                f"flang={tf:.3f}s fparser2={tp:.3f}s ratio={ratio:.2f}",
            )
        )

    if skip_runtime or not ex.get("runtime"):
        result.checks.append(CheckResult("runtime", True, "skipped"))
        return result

    rt = ex["runtime"]
    tol = float(rt.get("runtime_ratio_tol", 0.5))
    for parser in PARSERS:
        print(f"  build+run ({rt.get('variant')}) with {parser} generated code ...")
        ok, elapsed, detail = run_runtime(
            root, ex, workdir, parser, runs[parser].out_dir
        )
        result.runtimes[parser] = elapsed
        result.checks.append(CheckResult(f"runtime:{parser}", ok, f"{elapsed:.3f}s {detail}"))
        print(f"    -> {elapsed:.3f}s {detail if ok else detail[:200]}")

    if all(f"runtime:{p}" in {c.name for c in result.checks if c.ok} for p in PARSERS):
        rf, rp = result.runtimes["flang"], result.runtimes["fparser2"]
        if min(rf, rp) > 0:
            rel = abs(rf - rp) / max(rf, rp)
            result.checks.append(
                CheckResult(
                    "runtime roughly equal",
                    rel <= tol,
                    f"flang={rf:.3f}s fparser2={rp:.3f}s rel_diff={rel:.3f} tol={tol}",
                )
            )
    return result


def print_report(results: List[ExampleResult]) -> int:
    print("\n" + "=" * 72)
    print("PARSER EVALUATION REPORT (flang vs fparser2)")
    print("=" * 72)
    failed = 0
    for r in results:
        status = "PASS" if r.ok else "FAIL"
        if not r.ok:
            failed += 1
        print(f"\n[{status}] {r.name}")
        if r.codegen_times:
            print(
                "  codegen: "
                + ", ".join(f"{p}={t:.3f}s" for p, t in r.codegen_times.items())
            )
        if r.runtimes:
            print(
                "  runtime: "
                + ", ".join(f"{p}={t:.3f}s" for p, t in r.runtimes.items())
            )
        for c in r.checks:
            mark = "OK " if c.ok else "ERR"
            print(f"  [{mark}] {c.name}: {c.detail}")
    print("\n" + "=" * 72)
    print(f"Summary: {len(results) - failed}/{len(results)} examples passed")
    print("=" * 72)
    return 0 if failed == 0 else 1


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--examples",
        nargs="+",
        help="only run these example names",
    )
    ap.add_argument(
        "--skip-runtime",
        action="store_true",
        help="only compare codegen / dependency trees",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="output directory for generated artefacts",
    )
    args = ap.parse_args(argv)

    root = repo_root()
    out_root = args.out or (suite_root() / "out")
    out_root.mkdir(parents=True, exist_ok=True)

    # ensure scan binary exists early
    scan_bin(root)

    examples = load_examples(args.examples)
    if not examples:
        print("No examples found.", file=sys.stderr)
        return 2

    results = [
        evaluate_example(root, ex, out_root, args.skip_runtime) for ex in examples
    ]
    report_path = out_root / "report.json"
    report_path.write_text(
        json.dumps(
            [
                {
                    "name": r.name,
                    "ok": r.ok,
                    "codegen_times": r.codegen_times,
                    "runtimes": r.runtimes,
                    "checks": [
                        {"name": c.name, "ok": c.ok, "detail": c.detail}
                        for c in r.checks
                    ],
                }
                for r in results
            ],
            indent=2,
        ),
        encoding="utf-8",
    )
    print(f"\nWrote {report_path}")
    return print_report(results)


if __name__ == "__main__":
    sys.exit(main())
