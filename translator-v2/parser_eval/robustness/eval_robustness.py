#!/usr/bin/env python3
"""
Run the OP2 Fortran robustness suite.

Case kinds (case.json "category"):
  syntax_gap       — fparser2 cannot parse; Flang should translate
  negative_control — both parsers expected to fail (document limits)
  pipeline         — both parse; exercises OP2 Flang path (validation,
                     multi-file, macros, funcref, Stage-1 fallback, …)

Optional case.json fields:
  expect_fparser2 / expect_flang:
    pass | fail | fallback | pass_with_warning
  expected_warnings: substrings that must appear in combined output
  translator_flags: extra CLI flags
  fparser2_reason / notes: free-text for the report

Usage:
  python3 eval_robustness.py
  python3 eval_robustness.py --cases assumed_rank valid_const_write
  python3 eval_robustness.py --categories syntax_gap pipeline
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
from typing import Any, Dict, List, Optional, Sequence


FALLBACK_RE = re.compile(
    r"falling back to fparser2|fparser2 fallback|fparser2 invoked unexpectedly",
    re.IGNORECASE,
)

VALID_EXPECT = {"pass", "fail", "fallback", "pass_with_warning"}


@dataclass
class ParserOutcome:
    parser: str
    returncode: int
    detail: str
    used_fparser2_fallback: bool = False
    combined: str = ""

    @property
    def translated(self) -> bool:
        return self.returncode == 0


@dataclass
class CaseResult:
    name: str
    category: str
    description: str
    notes: str
    expect_fparser2: str
    expect_flang: str
    expected_warnings: List[str]
    outcomes: Dict[str, ParserOutcome] = field(default_factory=dict)
    mismatches: List[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.mismatches


def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def suite_root() -> Path:
    return Path(__file__).resolve().parent


def load_cases(
    only: Optional[List[str]] = None,
    categories: Optional[List[str]] = None,
) -> List[Dict[str, Any]]:
    cases_dir = suite_root() / "cases"
    found: List[Dict[str, Any]] = []
    for path in sorted(cases_dir.glob("*/case.json")):
        data = json.loads(path.read_text(encoding="utf-8"))
        data["_config_dir"] = path.parent
        # defaults for legacy syntax-gap cases
        data.setdefault("category", "syntax_gap")
        data.setdefault("expect_fparser2", "fail")
        data.setdefault("expect_flang", "pass")
        data.setdefault("expected_warnings", [])
        data.setdefault("translator_flags", [])
        data.setdefault("notes", data.get("fparser2_reason", ""))
        if only and data.get("name") not in only:
            continue
        if categories and data.get("category") not in categories:
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
    for ln in lines:
        lower = ln.lower()
        if "syntax" in lower or ">>>" in ln or "error:" in lower or "warning:" in lower:
            return ln[:limit]
    return lines[-1][:limit]


def classify_outcome(proc: subprocess.CompletedProcess[str]) -> ParserOutcome:
    combined = (proc.stderr or "") + "\n" + (proc.stdout or "")
    fallback = bool(FALLBACK_RE.search(combined))
    if proc.returncode == 0 and not fallback:
        detail = "translated"
    elif proc.returncode == 0 and fallback:
        detail = "translated via fparser2 fallback"
    else:
        detail = brief_error(combined)
    return ParserOutcome(
        parser="",
        returncode=proc.returncode,
        detail=detail,
        used_fparser2_fallback=fallback,
        combined=combined,
    )


def run_parser(
    root: Path,
    case: Dict[str, Any],
    parser: str,
    target: str,
    out_dir: Path,
) -> ParserOutcome:
    workdir = Path(case["_config_dir"])
    sources = [str(workdir / s) for s in case["sources"]]
    flags = list(case.get("translator_flags") or [])
    has_scan_flag = any(
        f == "--flang-scan" or f.startswith("--flang-scan=") for f in flags
    )
    cmd = translator_cmd(root) + [
        "--parser",
        parser,
        "-t",
        target,
        "-o",
        str(out_dir),
    ]
    # default scan binary first; case translator_flags may override --flang-scan
    if parser == "flang" and not has_scan_flag:
        cmd.extend(["--flang-scan", str(scan_bin(root))])
    cmd.extend(flags)
    cmd.extend(sources)

    env = os.environ.copy()
    if not has_scan_flag:
        env["OP2_FLANG_SCAN"] = str(scan_bin(root))
    else:
        # prevent env default from overriding an intentional failing stub
        env.pop("OP2_FLANG_SCAN", None)

    proc = subprocess.run(
        cmd,
        cwd=str(workdir),
        capture_output=True,
        text=True,
        env=env,
    )
    out = classify_outcome(proc)
    out.parser = parser
    return out


def expectation_met(expect: str, out: ParserOutcome, warnings: Sequence[str]) -> List[str]:
    """Return list of mismatch strings (empty if ok)."""
    if expect not in VALID_EXPECT:
        return [f"invalid expect value {expect!r}"]

    mismatches: List[str] = []
    has_warn = all(w.lower() in out.combined.lower() for w in warnings) if warnings else True

    if expect == "pass":
        if not out.translated:
            mismatches.append(f"wanted pass, got fail: {out.detail}")
        elif out.used_fparser2_fallback:
            mismatches.append("wanted pass without fparser2 fallback")
    elif expect == "fail":
        if out.translated and not out.used_fparser2_fallback:
            mismatches.append("wanted fail, but translated cleanly")
        elif out.translated and out.used_fparser2_fallback:
            # fallback that still succeeds is not a hard fail for negative controls
            # that both reject at parse — treat as fail-equivalent only if returncode!=0
            mismatches.append("wanted fail, but translated via fparser2 fallback")
    elif expect == "fallback":
        if not out.used_fparser2_fallback:
            if out.translated:
                mismatches.append("wanted fparser2 fallback, but Flang path stayed native")
            else:
                mismatches.append(f"wanted fparser2 fallback, got hard fail: {out.detail}")
        elif not out.translated:
            mismatches.append(f"fallback occurred but translation still failed: {out.detail}")
    elif expect == "pass_with_warning":
        if not out.translated:
            mismatches.append(f"wanted pass_with_warning, got fail: {out.detail}")
        elif out.used_fparser2_fallback:
            mismatches.append("wanted native pass_with_warning, got fparser2 fallback")
        elif warnings and not has_warn:
            mismatches.append(
                "missing expected warning(s): " + ", ".join(warnings)
            )

    return mismatches


def evaluate_case(root: Path, case: Dict[str, Any], work: Path) -> CaseResult:
    name = case["name"]
    result = CaseResult(
        name=name,
        category=case.get("category", "syntax_gap"),
        description=case.get("description", ""),
        notes=case.get("notes") or case.get("fparser2_reason", ""),
        expect_fparser2=case.get("expect_fparser2", "fail"),
        expect_flang=case.get("expect_flang", "pass"),
        expected_warnings=list(case.get("expected_warnings") or []),
    )
    target = (case.get("targets") or ["seq"])[0]

    for parser in ("fparser2", "flang"):
        out_dir = work / name / parser
        if out_dir.exists():
            shutil.rmtree(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        out = run_parser(root, case, parser, target, out_dir)
        result.outcomes[parser] = out
        expect = result.expect_fparser2 if parser == "fparser2" else result.expect_flang
        # warnings checked against the parser under test; for fparser2 pass_with_warning too
        warns = result.expected_warnings if expect == "pass_with_warning" else []
        for msg in expectation_met(expect, out, warns):
            result.mismatches.append(f"{parser}: {msg}")

    return result


def print_report(results: List[CaseResult]) -> int:
    print("=" * 72)
    print("OP2 ROBUSTNESS SUITE")
    print("=" * 72)

    by_cat: Dict[str, List[CaseResult]] = {}
    for r in results:
        by_cat.setdefault(r.category, []).append(r)

    ok_n = 0
    bad_n = 0

    for cat in sorted(by_cat):
        print()
        print(f"### category: {cat}")
        for r in by_cat[cat]:
            status = "OK" if r.ok else "MISMATCH"
            if r.ok:
                ok_n += 1
            else:
                bad_n += 1
            print()
            print(f"## [{status}] {r.name}")
            print(f"   {r.description}")
            if r.notes:
                print(f"   notes: {r.notes}")
            fp = r.outcomes["fparser2"]
            fl = r.outcomes["flang"]
            print(
                f"   fparser2: {fp.detail}"
                + (" [fallback]" if fp.used_fparser2_fallback else "")
                + f"  (expect {r.expect_fparser2})"
            )
            print(
                f"   flang:    {fl.detail}"
                + (" [fallback]" if fl.used_fparser2_fallback else "")
                + f"  (expect {r.expect_flang})"
            )
            for m in r.mismatches:
                print(f"   !! {m}")

    print()
    print("=" * 72)
    print(f"summary: {len(results)} cases | ok={ok_n} mismatch={bad_n}")
    for cat, items in sorted(by_cat.items()):
        c_ok = sum(1 for r in items if r.ok)
        print(f"  {cat}: {c_ok}/{len(items)} ok")
    print("=" * 72)
    return 0 if bad_n == 0 and results else 1


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cases", nargs="+", help="subset of case names")
    ap.add_argument(
        "--categories",
        nargs="+",
        help="subset of categories (syntax_gap, negative_control, pipeline, flang_gap)",
    )
    ap.add_argument("--keep-work", type=Path, help="keep translator outputs")
    args = ap.parse_args(argv)

    root = repo_root()
    cases = load_cases(args.cases, args.categories)
    if not cases:
        print("no robustness cases found", file=sys.stderr)
        return 2

    scan_bin(root)

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
