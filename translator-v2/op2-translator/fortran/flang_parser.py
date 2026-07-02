"""
Stage-1 Fortran parser backend driven by LLVM Flang (via the op2-flang-scan
helper binary).

When the translator runs with ``--parser flang``, this module is the primary
Stage 1 frontend: it builds ``Program.loops``, ``Program.consts``, and
``Program.entities`` (with cooked kernel source text on each ``Function``)
from the JSON emitted by ``op2-flang-scan``. fparser2 is only used as a
lazy fallback when downstream stages need an AST (validation, CUDA kernel
rewrites, main-program translation).

The JSON schema is documented in ``translator-v2/flang-scan/README.md``.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

import op as OP
from store import Application, Function, Location, ParseError, Program


# -----------------------------------------------------------------------------
# Binary discovery
# -----------------------------------------------------------------------------

DEFAULT_BIN_NAME = "op2-flang-scan"


def _repo_build_candidate() -> Optional[Path]:
    """
    Return the default build location of op2-flang-scan relative to this file.
    """
    # fortran/flang_parser.py -> op2-translator/fortran/flang_parser.py
    # walk up to translator-v2/flang-scan/build/op2-flang-scan
    here = Path(__file__).resolve()
    translator_v2 = here.parents[2]
    candidate = translator_v2 / "flang-scan" / "build" / DEFAULT_BIN_NAME
    return candidate if candidate.is_file() else None


def resolve_scan_binary(override: Optional[str] = None) -> Path:
    """
    Locate the op2-flang-scan binary.

    Lookup order:
      1. explicit override (CLI flag)
      2. OP2_FLANG_SCAN environment variable
      3. translator-v2/flang-scan/build/op2-flang-scan
      4. op2-flang-scan on PATH
    """
    if override:
        p = Path(override)
        if not p.is_file():
            raise ParseError(f"op2-flang-scan not found at --flang-scan path: {p}")
        return p

    env = os.environ.get("OP2_FLANG_SCAN")
    if env:
        p = Path(env)
        if not p.is_file():
            raise ParseError(f"op2-flang-scan not found at OP2_FLANG_SCAN: {p}")
        return p

    repo = _repo_build_candidate()
    if repo is not None:
        return repo

    found = shutil.which(DEFAULT_BIN_NAME)
    if found:
        return Path(found)

    raise ParseError(
        "op2-flang-scan binary not found. Build it from translator-v2/flang-scan "
        "or set OP2_FLANG_SCAN / pass --flang-scan <path>."
    )


# -----------------------------------------------------------------------------
# Tool invocation
# -----------------------------------------------------------------------------

def run_scan(source: str, path: Path, scan_bin: Path) -> Dict[str, Any]:
    """
    Run op2-flang-scan on the given (already preprocessed) source text and
    return the parsed JSON document.
    """
    try:
        proc = subprocess.run(
            [str(scan_bin), "--stdin", "--path", str(path)],
            input=source.encode("utf-8"),
            capture_output=True,
            check=False,
        )
    except FileNotFoundError as e:
        raise ParseError(f"failed to launch op2-flang-scan: {e}")

    if proc.returncode != 0:
        stderr = proc.stderr.decode("utf-8", errors="replace")
        raise ParseError(
            f"op2-flang-scan failed for {path} (exit {proc.returncode}):\n{stderr}"
        )

    stdout = proc.stdout.decode("utf-8", errors="replace")
    try:
        return json.loads(stdout)
    except json.JSONDecodeError as e:
        raise ParseError(f"op2-flang-scan produced invalid JSON for {path}: {e}")


# -----------------------------------------------------------------------------
# JSON -> OP objects
# -----------------------------------------------------------------------------

def _make_loc(path: Path, node_loc: Optional[Dict[str, int]]) -> Location:
    line = 0
    col = 0
    if node_loc is not None:
        line = int(node_loc.get("line", 0) or 0)
        col = int(node_loc.get("column", 0) or 0)
    return Location(str(path), line, col)


def _arg_as_name(arg: Dict[str, Any], loc: Location) -> str:
    if arg.get("kind") != "name":
        raise ParseError(
            f"expected identifier, got {arg.get('kind')} ({arg!r})", loc
        )
    return str(arg["value"]).lower()


def _arg_as_int(arg: Dict[str, Any], loc: Location, optional: bool = False) -> Optional[int]:
    kind = arg.get("kind")
    if kind == "int":
        return int(arg["value"])
    if kind == "raw":
        # last-ditch: try parsing the raw source text
        text = str(arg.get("source", "")).strip()
        try:
            return int(text)
        except ValueError:
            pass
    if optional:
        return None
    raise ParseError(f"expected integer literal, got {kind} ({arg!r})", loc)


def _arg_as_string(arg: Dict[str, Any], loc: Location) -> str:
    kind = arg.get("kind")
    if kind == "string":
        return str(arg["value"])
    if kind == "raw":
        # Safety net: if the C++ visitor could not recognise a character
        # literal (e.g. a future Flang variant rearrangement), but the raw
        # source text still looks like a quoted string, accept it.
        text = str(arg.get("source", "")).strip()
        if len(text) >= 2 and text[0] == text[-1] and text[0] in ('"', "'"):
            return text[1:-1]
    raise ParseError(
        f"expected character literal, got {kind} ({arg!r})", loc
    )


def _arg_as_access_type(arg: Dict[str, Any], loc: Location) -> OP.AccessType:
    name = _arg_as_name(arg, loc).upper()
    mapping = {
        "OP_READ": 0, "OP_WRITE": 1, "OP_RW": 2,
        "OP_INC": 3, "OP_MIN": 4, "OP_MAX": 5, "OP_WORK": 6,
    }
    if name not in mapping:
        raise ParseError(
            f"invalid access type {name}, expected one of {', '.join(mapping)}", loc
        )
    return OP.AccessType(mapping[name])


# Re-use the type string parser from the fparser2 backend so the two paths
# produce identical OP.Type instances for the same input.
from fortran.parser import parseType  # noqa: E402


# -----------------------------------------------------------------------------
# Event dispatch
# -----------------------------------------------------------------------------

_PAR_LOOP_RE = re.compile(r"^op_par_loop_\d+$")
_SUBPROGRAM_KINDS = ("subroutine_subprogram", "function_subprogram")


def app_has_flang_stage1(app: Application) -> bool:
    """Return True if any program in the application was parsed with Flang."""
    return any(getattr(p, "stage1_backend", "fparser2") == "flang" for p in app.programs)


def build_program_from_flang(path: Path, source: str, data: Dict[str, Any]) -> Program:
    """
    Build a complete Stage 1 ``Program`` from op2-flang-scan JSON.

    Populates loops, consts, and Function entities (each with ``flang_source``,
    parameters, and raw dependency names). Cross-file dependency filtering is
    deferred to ``resolve_flang_dependencies`` once every translation unit has
    been loaded into the ``Application``.
    """
    program = Program(path, None, source)
    setattr(program, "stage1_backend", "flang")

    for event in data.get("events", []):
        kind = str(event.get("kind", ""))
        loc = _make_loc(path, event.get("location"))

        if kind in _SUBPROGRAM_KINDS:
            name = str(event.get("name", "")).lower()
            if name:
                program.entities.append(_function_from_subprogram_event(event, program))
            continue

        args = event.get("args", [])

        if kind == "op_decl_const":
            program.consts.append(_parse_const(args, loc))
        elif _PAR_LOOP_RE.match(kind):
            program.loops.append(_parse_loop(program, args, loc))
        else:
            raise ParseError(f"unexpected event kind from flang-scan: {kind}", loc)

    return program


def populate_program(program: Program, data: Dict[str, Any]) -> None:
    """
    Overlay Flang-derived Stage 1 data onto an existing fparser2-built program.

    Kept for compatibility with the hybrid overlay path when both parsers run;
    prefer ``build_program_from_flang`` for the primary ``--parser flang`` path.
    """
    built = build_program_from_flang(program.path, program.source, data)
    program.loops = built.loops
    program.consts = built.consts
    _merge_flang_entities(program, built.entities)


def _function_from_subprogram_event(event: Dict[str, Any], program: Program) -> Function:
    name = str(event.get("name", "")).lower()
    parameters = [str(p).lower() for p in event.get("parameters", [])]
    depends = {str(d).lower() for d in event.get("depends", [])}

    entity = Function(name, None, program)
    entity.parameters = parameters
    entity.depends = depends
    setattr(entity, "flang_source", str(event.get("source", "")))
    return entity


def _merge_flang_entities(program: Program, flang_entities: List[Function]) -> None:
    """Attach ``flang_source`` / depends from Flang entities onto fparser2 ones."""
    by_name = {entity.name.lower(): entity for entity in flang_entities}

    for entity in program.entities:
        if not isinstance(entity, Function):
            continue
        sp = by_name.get(entity.name.lower())
        if sp is None:
            continue

        setattr(entity, "flang_source", getattr(sp, "flang_source", ""))
        entity.depends = set(getattr(entity, "depends", set()) or set()) | set(sp.depends)

    existing = {entity.name.lower() for entity in program.entities}
    for entity in flang_entities:
        if entity.name.lower() not in existing:
            program.entities.append(entity)


def resolve_flang_dependencies(app: Application) -> None:
    """
    Filter each entity's ``depends`` set to names that refer to known subprograms
    in the application.

    This is the Flang equivalent of ``fortran.parser.parseFunctionDependencies``:
    the C++ scanner records every callee / function-reference name (including
    intrinsics and parameter names); we keep only edges that resolve to a
    ``Function`` entity somewhere in the app.
    """
    known_names: Set[str] = set()
    for program in app.programs:
        for entity in program.entities:
            if isinstance(entity, Function):
                known_names.add(entity.name.lower())

    for program in app.programs:
        for entity in program.entities:
            if not isinstance(entity, Function):
                continue
            entity.depends = {
                dep.lower()
                for dep in entity.depends
                if dep.lower() in known_names and dep.lower() != entity.name.lower()
            }


def _parse_const(args: List[Dict[str, Any]], loc: Location) -> OP.Const:
    if len(args) != 3:
        raise ParseError("incorrect number of arguments for op_decl_const", loc)

    ptr = _arg_as_name(args[0], loc)
    dim = _arg_as_int(args[1], loc)
    assert dim is not None

    typ_str = _arg_as_string(args[2], loc).strip().lower()
    typ = parseType(typ_str, loc)[0]

    return OP.Const(loc, ptr, dim, typ)


def _parse_loop(program: Program, args: List[Dict[str, Any]], loc: Location) -> OP.Loop:
    if len(args) < 3:
        raise ParseError("incorrect number of arguments for op_par_loop", loc)

    kernel = _arg_as_name(args[0], loc)
    name = f"{program.path.stem}_{len(program.loops) + 1}_{kernel}"
    loop = OP.Loop(name, loc, kernel)

    # args[0] = kernel name, args[1] = op_set, args[2:] = the op_arg_* entries.
    for arg_node in args[2:]:
        kind = arg_node.get("kind")

        if kind == "call":
            call_name = str(arg_node.get("name", "")).lower()
            sub_args = arg_node.get("args", [])

            if call_name == "op_arg_dat":
                _parse_arg_dat(loop, False, sub_args, loc)
            elif call_name == "op_opt_arg_dat":
                _parse_arg_dat(loop, True, sub_args, loc)
            elif call_name == "op_arg_gbl":
                _parse_arg_gbl(loop, False, sub_args, loc)
            elif call_name == "op_opt_arg_gbl":
                _parse_arg_gbl(loop, True, sub_args, loc)
            elif call_name == "op_arg_idx":
                _parse_arg_idx(loop, sub_args, loc)
            elif call_name == "op_arg_info":
                _parse_arg_info(loop, sub_args, loc)
            else:
                raise ParseError(f"invalid loop argument call: {call_name}", loc)

        else:
            raise ParseError(
                f"unable to parse op_par_loop argument (kind={kind})", loc
            )

    return loop


def _parse_arg_dat(loop: OP.Loop, opt: bool, args: List[Dict[str, Any]], loc: Location) -> None:
    expected = 7 if opt else 6
    if len(args) != expected:
        raise ParseError(
            f"incorrect number of arguments for op_{'opt_' if opt else ''}arg_dat", loc
        )

    args_list = args[1:] if opt else args

    dat_ptr = _arg_as_name(args_list[0], loc)
    map_idx = _arg_as_int(args_list[1], loc, optional=True)

    map_ptr: Optional[str] = _arg_as_name(args_list[2], loc)
    if map_ptr.upper() == "OP_ID":
        map_ptr = None

    dat_dim = _arg_as_int(args_list[3], loc, optional=True)

    dat_typ, dat_soa = parseType(_arg_as_string(args_list[4], loc), loc)
    access_type = _arg_as_access_type(args_list[5], loc)

    loop.addArgDat(loc, dat_ptr, dat_dim, dat_typ, dat_soa, map_ptr, map_idx, access_type, opt)


def _parse_arg_gbl(loop: OP.Loop, opt: bool, args: List[Dict[str, Any]], loc: Location) -> None:
    expected = 5 if opt else 4
    if len(args) != expected:
        raise ParseError(
            f"incorrect number of arguments for op_{'opt_' if opt else ''}arg_gbl", loc
        )

    args_list = args[1:] if opt else args

    ptr = _arg_as_name(args_list[0], loc)
    dim = _arg_as_int(args_list[1], loc, optional=True)
    typ = parseType(_arg_as_string(args_list[2], loc), loc)[0]
    access_type = _arg_as_access_type(args_list[3], loc)

    loop.addArgGbl(loc, ptr, dim, typ, access_type, opt)


def _parse_arg_idx(loop: OP.Loop, args: List[Dict[str, Any]], loc: Location) -> None:
    if len(args) != 2:
        raise ParseError("incorrect number of arguments for op_arg_idx", loc)

    map_idx = _arg_as_int(args[0], loc, optional=True)
    map_ptr: Optional[str] = _arg_as_name(args[1], loc)

    if map_ptr.upper() == "OP_ID":
        map_ptr = None

    loop.addArgIdx(loc, map_ptr, map_idx)


def _parse_arg_info(loop: OP.Loop, args: List[Dict[str, Any]], loc: Location) -> None:
    if len(args) != 4:
        raise ParseError("incorrect number of arguments for op_arg_info", loc)

    ptr = _arg_as_name(args[0], loc)
    dim = _arg_as_int(args[1], loc, optional=True)
    typ = parseType(_arg_as_string(args[2], loc), loc)[0]

    ref = _arg_as_int(args[3], loc)
    assert ref is not None

    loop.addArgInfo(loc, ptr, dim, typ, ref)
