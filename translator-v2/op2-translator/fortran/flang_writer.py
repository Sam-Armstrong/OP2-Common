"""
Text-level kernel rewriting for the --parser flang code path.

When Stage 1 runs through LLVM Flang via op2-flang-scan, each `Function`
entity gets a `flang_source` attribute holding the cooked Fortran text of
that subprogram. The helpers in this module are the Flang-driven analogues
of the fparser2-based rewriters in `fortran.translator.kernels`:

    fparser2 helper                         flang_writer equivalent
    ------------------------------------    ------------------------------
    ftk.writeSource(entities, prologue)     write_source
    ftk.renameConsts(lang, entities, ...)   rename_consts
    ftk.renameEntities(entities, replace)   rename_entities
    ftk.fixHydraIO(entity)                  fix_hydra_io
    ftk.removeExternals(entity)             remove_externals

`extract_dependencies` is intentionally not re-implemented here: the
fparser2 helper only consults `entity.depends` and `app.findEntities`,
both of which work identically once `flang_parser.populate_program` has
attached Flang-derived depends to the entities.

These rewriters operate on the cooked Fortran text emitted by Flang's
prescanner (lowercase keywords, comments stripped, continuations
unified), which is what op2-flang-scan dumps in the `source` field of
each subprogram event. We avoid touching characters inside string
literals with a small helper.
"""

from __future__ import annotations

import re
from typing import Callable, Iterable, List, Optional, Sequence

from language import Lang
from store import Application, Entity, Function


# -----------------------------------------------------------------------------
# String-literal-aware replacement
# -----------------------------------------------------------------------------

def _substitute_outside_strings(source: str, replace_chunk: Callable[[str], str]) -> str:
    """
    Apply `replace_chunk` to every contiguous span of `source` that does not
    fall inside a Fortran character literal. Doubled-quote escapes inside
    literals (``'it''s'``, ``"a""b"``) are handled.
    """
    out: List[str] = []
    i = 0
    n = len(source)
    while i < n:
        c = source[i]
        if c == '"' or c == "'":
            quote = c
            j = i + 1
            while j < n:
                if source[j] == quote:
                    if j + 1 < n and source[j + 1] == quote:
                        j += 2
                        continue
                    j += 1
                    break
                j += 1
            out.append(source[i:j])
            i = j
            continue

        j = i
        while j < n and source[j] not in '"\'':
            j += 1
        out.append(replace_chunk(source[i:j]))
        i = j

    return "".join(out)


# -----------------------------------------------------------------------------
# Entity helpers
# -----------------------------------------------------------------------------

def _flang_functions(entities: Iterable[Entity]) -> List[Function]:
    """
    Return only those entities that are Functions with a Flang source text
    attached. Anything without flang_source is skipped silently - it's the
    caller's job to make sure they're operating in the Flang code path.
    """
    out: List[Function] = []
    for e in entities:
        if isinstance(e, Function) and getattr(e, "flang_source", None):
            out.append(e)
    return out


# -----------------------------------------------------------------------------
# Mutations
# -----------------------------------------------------------------------------

def rename_consts(
    lang: Lang,
    entities: Sequence[Entity],
    app: Application,
    replacement: Callable[[str], str],
) -> None:
    """
    Rename references to OP2 declared consts inside each entity's flang_source.

    Mirrors fparser2's `renameConsts`: an identifier is renamed if it matches
    a known const pointer *and* is not one of the kernel parameters of the
    enclosing subprogram. The replacement is performed with word boundaries
    and skips occurrences inside string literals.
    """
    const_ptrs = app.constPtrs()

    for entity in _flang_functions(entities):
        targets = {c.lower() for c in const_ptrs} - {p.lower() for p in entity.parameters}
        if not targets:
            continue

        pattern = re.compile(
            r"\b(" + "|".join(re.escape(t) for t in sorted(targets)) + r")\b",
            flags=re.IGNORECASE,
        )

        def sub_chunk(chunk: str, _pattern=pattern) -> str:
            return _pattern.sub(lambda m: replacement(m.group(1).lower()), chunk)

        entity.flang_source = _substitute_outside_strings(entity.flang_source, sub_chunk)


def rename_function_definition(entity: Function, replacement: str) -> None:
    """
    Rename the subroutine/function header (and its matching ``end`` statement
    if the name was repeated) within `entity.flang_source`.
    """
    if not getattr(entity, "flang_source", None):
        return

    old = re.escape(entity.name)

    def sub_chunk(chunk: str) -> str:
        chunk = re.sub(
            rf"(\b(?:subroutine|function)\s+){old}\b",
            lambda m: m.group(1) + replacement,
            chunk,
            flags=re.IGNORECASE,
        )
        chunk = re.sub(
            rf"(\bend\s+(?:subroutine|function)\s+){old}\b",
            lambda m: m.group(1) + replacement,
            chunk,
            flags=re.IGNORECASE,
        )
        return chunk

    entity.flang_source = _substitute_outside_strings(entity.flang_source, sub_chunk)
    entity.name = replacement


def rename_function_calls(entity: Function, name: str, replacement: str) -> None:
    """
    Rewrite ``call <name>(...)`` and function-style references ``<name>(...)``
    inside the entity body. The function-style match is conservative: we only
    rename occurrences that are followed by ``(``, to avoid mangling
    declaration types that happen to share a name.
    """
    if not getattr(entity, "flang_source", None):
        return

    old = re.escape(name)

    def sub_chunk(chunk: str) -> str:
        chunk = re.sub(
            rf"(\bcall\s+){old}\b",
            lambda m: m.group(1) + replacement,
            chunk,
            flags=re.IGNORECASE,
        )
        chunk = re.sub(
            rf"\b{old}(\s*\()",
            lambda m: replacement + m.group(1),
            chunk,
            flags=re.IGNORECASE,
        )
        return chunk

    entity.flang_source = _substitute_outside_strings(entity.flang_source, sub_chunk)


def rename_entities(entities: Sequence[Entity], replacement: Callable[[str], str]) -> None:
    """
    Rename a group of subprograms in lockstep, updating both the definitions
    and any call sites among the same group. Mirrors fparser2's
    `renameEntities`, which is only used by the (currently unregistered)
    FortranOpenMP SIMD code path.

    We do the rewrites in two passes so the old name is still available when
    we rewrite call sites between the entities in the group.
    """
    funcs = _flang_functions(entities)
    rename_plan = [(f, f.name, replacement(f.name)) for f in funcs]

    for func, old_name, new_name in rename_plan:
        for other, _, _ in rename_plan:
            if other is func:
                continue
            rename_function_calls(other, old_name, new_name)

    for func, _, new_name in rename_plan:
        rename_function_definition(func, new_name)


# -----------------------------------------------------------------------------
# Cleanup helpers
# -----------------------------------------------------------------------------

_EXTERNAL_LINE_RE = re.compile(r"^\s*external\s*(?:::)?\s*[^\n]*\n?", flags=re.IGNORECASE | re.MULTILINE)
_WRITE_STMT_RE = re.compile(r"^\s*write\s*\([^\n]*\)[^\n]*\n?", flags=re.IGNORECASE | re.MULTILINE)

_HYDRA_CLOBBER_CALLS = (
    "hyd_print", "hyd_dump", "hyd_kill", "hyd_error_print", "hyd_error_dump",
    "hyd_print_hydrales", "hyd_dump_hydrales", "hyd_kill_hydrales",
    "hyd_error_print_hydrales", "hyd_error_dump_hydrales",
)

_HYDRA_CALL_RE = re.compile(
    r"^(\s*)call\s+(" + "|".join(_HYDRA_CLOBBER_CALLS) + r")\b[^\n]*\n?",
    flags=re.IGNORECASE | re.MULTILINE,
)


def remove_externals(entity: Function) -> None:
    """
    Strip ``external`` declarations from `entity.flang_source`.
    """
    if not getattr(entity, "flang_source", None):
        return

    def sub_chunk(chunk: str) -> str:
        return _EXTERNAL_LINE_RE.sub("", chunk)

    entity.flang_source = _substitute_outside_strings(entity.flang_source, sub_chunk)


def fix_hydra_io(entity: Function) -> None:
    """
    Replace ``write(...)`` statements with ``continue`` and certain hydra
    helper calls with ``stop``, matching the fparser2 path.
    """
    if not getattr(entity, "flang_source", None):
        return

    def sub_chunk(chunk: str) -> str:
        chunk = _WRITE_STMT_RE.sub("continue\n", chunk)
        chunk = _HYDRA_CALL_RE.sub(lambda m: m.group(1) + "stop\n", chunk)
        return chunk

    entity.flang_source = _substitute_outside_strings(entity.flang_source, sub_chunk)


# -----------------------------------------------------------------------------
# Source emission
# -----------------------------------------------------------------------------

def write_source(entities: Sequence[Entity], prologue: Optional[str] = None) -> str:
    """
    Emit the Flang-derived source text for the given entities, concatenated in
    the same order that `fortran.translator.kernels.writeSource` uses
    (innermost dependencies last). The optional `prologue` is prepended to
    each entity, matching the fparser2 helper's API.

    Long lines (> 264 chars) are wrapped with Fortran continuations via the
    shared helper so downstream compilers don't reject them - the Flang
    prescanner unifies continuations on input which can produce very long
    cooked lines.
    """
    # Late import to avoid a circular dependency between flang_writer and
    # fortran.translator.kernels.
    from fortran.translator.kernels import addLineContinuations

    funcs = _flang_functions(entities)
    if not funcs:
        return ""

    pieces = []
    for entity in reversed(funcs):
        body = entity.flang_source or ""
        body = addLineContinuations(body)
        if prologue:
            body = prologue + body
        pieces.append(body.rstrip("\n"))

    return "\n\n".join(pieces) + "\n"
