#!/usr/bin/env python3
"""
Run the OP2 Fortran robustness suite: constructs that fparser2 cannot parse.

For each case under cases/*/case.json:
  1. Translate with --parser fparser2 (expected to fail at Stage 1).
  2. Translate with --parser flang (report pass/fail; note fparser2 fallback).

Usage:
  python3 eval_robustness.py
  python3 eval_robustness.py --cases assumed_rank unsigned
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional


FALLBACK_RE = re.compile(
    r"falling back to fparser2|fparser2 fallback|fparser2 invoked unexpectedly",
    re.IGNORECASE,
)


@dataclass
class ParserOutcome:
    parser: str
    ok: bool
    returncode: int
    detail: str
    used_fparser2_fallback: bool = False
    stderr: str = ""


@dataclass
class CaseResult:
    name: str
    description: str
    fparser2_reason: str
    outcomes: Dict[str, ParserOutcome] = field(default_factory=dict)

    @property
    def fparser2_failed(self) -> bool:
        out = self.outcomes.get("fparser2")
        return out is not None and not out.ok

    @property
    def flang_ok(self) -> bool:
        out = self.outcomes.get("flang")
        return out is not None and out.ok and not out.used_fparser2_fallback


def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def suite_root() -> Path:
    return Path(__file__).resolve().parent


def load_cases(only: Optional[List[str]] = None) -> List[Dict[str, Any]]:
    cases_dir = suite_root() / "cases"
    found: List[Dict[str, Any]] = []
    for path in sorted(cases_dir.glob("*/case.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        data["_config_dir"] = path.parent
        if only and data.get("name") not in only:
            continue
        found.append(data)
    return found


def translator_cmd(root: Path) -> List[str]:
    py = root / "translator-v2" / ".python" / "bin" / "python3"
    if not py.is_file():
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


def brief_error(text: str, limit: int = 240) -> str:
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if not lines:
        return "(no stderr)"
    # prefer the syntax-error line if present
    for ln in lines:
        if "syntax" in ln.lower() or ">>>" in ln or "error:" in ln.lower():
            return ln[:limit]
    return lines[-1][:limit]


def run_parser(
    root: Path,
    case: Dict[str, Any],
    parser: str,
    target: str,
    out_dir: Path,
) -> ParserOutcome:
    workdir = Path(case["_config_dir"])
    sources = [str(workdir / s) for s in case["sources"]]
    cmd = translator_cmd(root) + [
        "--parser",
        parser,
        "-t",
        target,
        "-o",
        str(out_dir),
        *sources,
    ]
    if parser == "flang":
        cmd.extend(["--flang-scan", str(scan_bin(root))])

    env = os.environ.copy()
    env["OP2_FLANG_SCAN"] = str(scan_bin(root))

    proc = subprocess.run(
        cmd,
        cwd=str(workdir),
        capture_output=True,
        text=True,
        env=env,
    )
    combined = (proc.stderr or "") + "\n" + (proc.stdout or "")
    fallback = bool(FALLBACK_RE.search(combined))

    if proc.returncode == 0 and not fallback:
        return ParserOutcome(
            parser=parser,
            ok=True,
            returncode=0,
            detail="translated",
            used_fparser2_fallback=False,
            stderr=proc.stderr or "",
        )

    if proc.returncode == 0 and fallback:
        return ParserOutcome(
            parser=parser,
            ok=False,
            returncode=0,
            detail="translated only via fparser2 fallback",
            used_fparser2_fallback=True,
            stderr=proc.stderr or "",
        )

    return ParserOutcome(
        parser=parser,
        ok=False,
        returncode=proc.returncode,
        detail=brief_error(combined),
        used_fparser2_fallback=fallback,
        stderr=proc.stderr or "",
    )


def evaluate_case(root: Path, case: Dict[str, Any], work: Path) -> CaseResult:
    name = case["name"]
    result = CaseResult(
        name=name,
        description=case.get("description", ""),
        fparser2_reason=case.get("fparser2_reason", ""),
    )
    targets = case.get("targets") or ["seq"]
    target = targets[0]

    for parser in ("fparser2", "flang"):
        out_dir = work / name / parser
        if out_dir.exists():
            shutil.rmtree(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        result.outcomes[parser] = run_parser(root, case, parser, target, out_dir)
    return result


def print_report(results: List[CaseResult]) -> int:
    print("=" * 72)
    print("OP2 ROBUSTNESS SUITE (fparser2-failing constructs)")
    print("=" * 72)

    fparser2_ok_count = 0
    flang_pass = 0
    flang_fail = 0

    for r in results:
        print()
        print(f"## {r.name}")
        print(f"   {r.description}")
        print(f"   why fparser2 fails: {r.fparser2_reason}")

        fp = r.outcomes["fparser2"]
        fl = r.outcomes["flang"]

        if fp.ok:
            fparser2_ok_count += 1
            print(f"   fparser2: UNEXPECTED PASS ({fp.detail})")
        else:
            print(f"   fparser2: FAIL as expected — {fp.detail}")

        if fl.ok and not fl.used_fparser2_fallback:
            flang_pass += 1
            print(f"   flang:    PASS ({fl.detail})")
        elif fl.used_fparser2_fallback:
            flang_fail += 1
            print(f"   flang:    FAIL (fell back to fparser2) — {fl.detail}")
        else:
            flang_fail += 1
            print(f"   flang:    FAIL — {fl.detail}")

    print()
    print("=" * 72)
    print(
        f"summary: {len(results)} cases | "
        f"fparser2 unexpected passes={fparser2_ok_count} | "
        f"flang pass={flang_pass} fail={flang_fail}"
    )
    print("=" * 72)

    # suite "success" means every case failed under fparser2 (the design goal)
    return 0 if fparser2_ok_count == 0 and results else 1


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--cases",
        nargs="+",
        help="subset of case names to run",
    )
    ap.add_argument(
        "--keep-work",
        type=Path,
        help="keep translator outputs under this directory",
    )
    args = ap.parse_args(argv)

    root = repo_root()
    cases = load_cases(args.cases)
    if not cases:
        print("no robustness cases found", file=sys.stderr)
        return 2

    scan_bin(root)  # fail fast if missing

    if args.keep_work:
        work = args.keep_work
        work.mkdir(parents=True, exist_ok=True)
        results = [evaluate_case(root, c, work) for c in cases]
        return print_report(results)

    with tempfile.TemporaryDirectory(prefix="op2_robustness_") as td:
        work = Path(td)
        results = [evaluate_case(root, c, work) for c in cases]
        return print_report(results)


if __name__ == "__main__":
    sys.exit(main())
