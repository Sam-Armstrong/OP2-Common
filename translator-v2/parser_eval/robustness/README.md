# OP2 Fortran robustness suite

Minimal OP2 apps that assess Flang Stage-1 robustness relative to fparser2
(installed pin: fparser 0.2.4, `std=f2008`).

## Categories

| Category | Intent |
|----------|--------|
| `syntax_gap` | Fortran constructs fparser2 cannot parse; Flang should translate |
| `fparser2_gap` | Valid F2003/F2008 that fparser2 rejects (parser bug, not a newer standard) |
| `negative_control` | Constructs neither parser handles on this toolchain |
| `pipeline` | OP2 Flang-path stress (validation, multi-file, macros, funcref, fallback) |
| `flang_gap` | Regression tests for former Flang-only gaps (Fortran `INCLUDE`) |

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


## Notes

Results below are against **fparser 0.2.4** (`std=f2008`) and the built
`op2-flang-scan`. Re-run the suite after upgrading either side.

### `syntax_gap` — fparser2 fails, Flang passes

fparser2 is run with `std=f2008` and only claims Fortran 2003 plus *some*
Fortran 2008. Each case below uses a construct from Fortran 2018 or 2023 (or
an F2008 feature fparser2 never finished). fparser2 rejects the source at
Stage 1 with a syntax error; LLVM Flang parses it and the OP2 Flang path
translates without falling back to fparser2.

#### `assumed_rank`

The helper subroutine declares a dummy argument as `real(8), intent(inout) ::
x(..)`, Fortran 2018 *assumed-rank* syntax where `..` means “any rank”.
fparser2’s F2008 grammar has no production for `(..)` in a type declaration,
so parsing stops before any OP2 logic runs. Flang implements assumed-rank
arrays and the `rank()` intrinsic, so Stage 1 succeeds and the rest of the
pipeline treats the host helper like any other subroutine.

#### `assumed_type`

The helper uses `type(*), intent(in) :: x`, the Fortran 2018 *assumed-type*
form that accepts a dummy of any derived or intrinsic type. fparser2 does not
recognise `type(*)` as a valid type-spec and reports a syntax error. Flang
accepts assumed-type dummies as part of its F2018 support, so the file parses
and translates normally.

#### `conditional_expr`

Host initialisation uses a Fortran 2023 *conditional expression*,
`(flag > 0 ? 2.0d0 : 1.0d0)`, analogous to a ternary operator. That token
sequence is not part of any grammar rule in fparser2’s F2008 parser. Flang
targets a much newer standard and parses conditional expressions, so the
assignment to `alpha` is accepted and the OP2 loop proceeds unchanged.

#### `do_concurrent_default_none`

The host loop is `do concurrent (i = 1:nnode) default(none) shared(u)`. The
`default(none)` and explicit `shared(u)` clauses are Fortran 2018
*concurrent-locality* specifiers added alongside `DO CONCURRENT`. fparser2
supports plain `DO CONCURRENT` (since 0.1.3) but not the locality list that
follows the loop header ([stfc/fparser#409](https://github.com/stfc/fparser/issues/409)).
Flang’s frontend implements the full F2018 concurrent-locality syntax, so the
loop parses and translation completes.

#### `do_concurrent_local`

Same locality extension as above, with `local(t)` declaring `t` as a
per-iteration local variable inside `do concurrent (i = 1:nnode) local(t)`.
fparser2’s parser stops at `local` because that keyword is not in its
`Do_Concurrent` rule. Flang accepts `LOCAL` specifiers and the loop body
parses without error.

#### `do_concurrent_reduce`

The host uses `do concurrent (i = 1:nnode) reduce(+:s)` to accumulate into
`s` with a reduction operator, another F2018 locality form. fparser2 has no
grammar for `reduce(+:…)` after the concurrent header. Flang implements
`REDUCE` locality and the reduction update in the loop body, so Stage 1 passes.

#### `do_concurrent_shared`

The host uses `do concurrent (i = 1:nnode) shared(u)`, the simplest F2018
locality specifier (explicitly marking `u` as shared). Even this form is
outside fparser2’s F2008 `DO CONCURRENT` production. Flang parses `SHARED(u)`
and the subsequent OP2 declarations and kernel call proceed on the native
Flang path.

#### `error_stop_quiet`

A dead branch contains `error stop 1, quiet=.true.`. The `QUIET=` keyword
on `ERROR STOP` is Fortran 2018 (it suppresses the error message on stop).
fparser2’s F2008 grammar only allows the older `ERROR STOP` forms without
that named argument. Flang recognises the F2018 `ERROR STOP` statement and
parses the guarded branch even though it never executes.

#### `fail_image`

A dead branch contains `fail image`, the Fortran 2018 statement that marks
the current coarray image as failed. fparser2 has no parse rule for `FAIL
IMAGE` as a statement keyword sequence. Flang’s coarray-related F2018 support
includes this statement, so the construct is accepted inside the `if
(unreachable)` block.

#### `form_team`

A dead branch uses `form team (1, t)`, `change team (t)`, and `end team`
with `team_type` from `ISO_FORTRAN_ENV` — the Fortran 2018 *team* construct
for partitioned parallelism. fparser2 does not implement these team
statements or the associated types. Flang parses the full `FORM TEAM` /
`CHANGE TEAM` / `END TEAM` sequence as valid F2018 syntax.

#### `implicit_none_external`

The main program begins with `implicit none (type, external)` instead of
plain `implicit none`. The parenthesised `(TYPE, EXTERNAL)` form is Fortran
2018: implicit typing is disabled for entities of type and for external
names separately. fparser2 only accepts the traditional single-token
`IMPLICIT NONE` and rejects the parenthesised variant. Flang implements the
F2018 implicit-typing rules, so the program unit parses.

#### `import_all`

An internal subroutine in a module contains `import, all`, importing every
host association from the enclosing scoping unit. The `IMPORT, ALL` (and
related `ONLY` / `NONE`) forms are Fortran 2018 extensions to the `IMPORT`
statement. fparser2’s grammar does not list `ALL` as a valid `IMPORT`
selector. Flang accepts the F2018 import forms, so the module and main
program both parse.

#### `import_none`

An internal subroutine uses `import, none`, declaring that no host
associations are imported — the strictest F2018 `IMPORT` form. As with
`import_all`, fparser2 fails because `NONE` is not a recognised token in its
`Import_Stmt` rule. Flang parses `IMPORT, NONE` and the empty host-association
list implied by it.

#### `import_only`

An internal subroutine uses `import, only: a`, importing a single host
name. fparser2 rejects `IMPORT, ONLY:` with a name list. Flang’s F2018 import
support includes selective `ONLY` imports, so the module helper and OP2
driver translate successfully.

#### `select_rank`

The helper combines assumed-rank `x(..)` with a `select rank (x)` /
`rank (1)` / `rank default` construct to branch on the rank of the dummy
argument. Both assumed-rank declarations and the `SELECT RANK` construct are
Fortran 2018 features absent from fparser2’s F2008 grammar. Flang implements
the full rank-selection construct, so parsing succeeds even though the OP2
kernel itself remains an ordinary rank-1 subroutine.

#### `submodule_module_procedure`

The source defines a parent module interface, a `submodule` with a
`module procedure greet` body (F2008 submodule syntax), and a main program
that calls `greet`. fparser2 0.2.4 still rejects this particular
`MODULE PROCEDURE` body form inside a submodule — the grammar for separating
module interface from submodule implementation is incomplete on this pin.
Flang’s submodule and separate-module-procedure support is more complete, so
all three program units parse and link logically for Stage 1.

#### `sync_memory`

The host calls `sync memory`, the Fortran 2008/2018 statement that acts as
a memory fence for atomic and coarray semantics. fparser2 has no
`Sync_Memory_Stmt` rule. Flang includes `SYNC MEMORY` in its statement
grammar, so the standalone statement parses before the OP2 initialisation
calls.

#### `unsigned`

The host declares `unsigned :: steps` and assigns `steps = 2u`, using the
Fortran 2023 *unsigned* integer type and literal suffix `u`. Unsigned types
are outside fparser2’s F2008 type system entirely. Flang (with appropriate
standard flags) accepts `UNSIGNED` declarations and unsigned literals, so
the conversion to `alpha` parses and the OP2 loop runs.

#### `syntax_in_kernel` (pipeline category)

This case is categorised under `pipeline` because it also stress-tests OP2
kernel extraction, but the syntax gap is the same: the kernel subroutine
`scale_vec` contains `do concurrent (i = 1:4) shared(u, alpha)` inside the
loop body that fparser2 must parse when building the dependency closure.
fparser2 fails when it reaches the kernel’s concurrent-locality header (not
just host code). Flang parses the kernel body natively, so the Flang path
extracts and translates the vector kernel without Stage-1 fallback — the
scenario that matters when modern syntax appears inside user kernels rather
than only in host setup code.

### `negative_control` — both fail

Documents limits of *this* toolchain, not OP2 logic:

- **`procedure_pointer_init`** — `procedure(...), pointer :: p => target`
- **`enumeration_type`** — F2023 `ENUMERATION TYPE`

Both reject at parse (Flang Stage 1 fails, then fparser2 fallback also fails).

### `pipeline` — both expected to succeed (OP2-path stress)

These are valid OP2 mini-apps both parsers should handle. They check that the
Flang path stays native and still performs the same work as fparser2:

- **Multi-file / preprocess** — `multi_file_app`, `#define` → `op_par_loop`
  (`macro_op_loop`), `#include` of a loop site (`include_loop_site`)
- **funcref / `arr(i)`** — vector `OP_INC` via `u(i)=u(i)+…`
  (`funcref_vector_inc`); Flang-scan often emits `funcref`, resolved by name
  in `flang_validator` / codegen
- **Validation warnings** (`pass_with_warning` on both) — const write,
  `OP_READ` write, bad `OP_INC`, runtime-sized locals, write via child
  subroutine; translation still exits 0 with `loop.fallback` where applicable
- **`stage1_scan_fallback`** — intentional broken `--flang-scan` stub: Flang
  Stage 1 must fall back, then fparser2 finishes the ordinary source

Contrast: C preprocessor `#include` works for Flang (expanded before scan);
Fortran `INCLUDE` does not (see `flang_gap`).

### `flang_gap` — formerly Flang INCLUDE failures (now fixed)

Fortran `INCLUDE "foo.inc"` is **not** expanded by the translator’s C
preprocessor (fpp/pcpp). It is left in the free-form source and must be
resolved by the Stage-1 Fortran parser. fparser2 and Flang handle that
differently, which is what broke these cases.

**Why it failed under Flang**

1. The Python driver always preprocesses the real source file, then invokes
   `op2-flang-scan --stdin --path <original.F90>` with that text on stdin.
2. Flang’s parser API needs a real on-disk path, so the scan tool wrote the
   stdin body to something like `/tmp/op2-flang-scan-<pid>.F90`.
3. When Flang’s prescanner hits `INCLUDE "loop_site.inc"`, it looks for the
   file relative to the directory of the **file currently being scanned**
   (and then any configured search directories). That directory was `/tmp`,
   not the app directory next to the `.inc` files.
4. Flang reported `INCLUDE: Source file '….inc' was not found`, Stage 1
   raised a parse error, and the translator fell back to fparser2 for that
   file. Translation still completed, but not on the native Flang path.

**Why fparser2 still worked**

fparser2’s `FortranStringReader` is given the translator’s `include_dirs`
(and typically runs with cwd in the source directory), so it can open the
same sibling `.inc` even though the input itself is an in-memory string.

**How it was fixed**

In `op2-flang-scan`:

1. Prefer writing the stdin temp file **next to `--path`** (the original
   source directory), so Flang’s “directory of current file” lookup finds
   sibling `.inc` files. Fall back to the system temp dir only if that
   location is not writable.
2. Add that source directory to Flang’s `Options.searchDirectories`, and
   accept `-I <dir>` for the same include set the translator already passes
   to fparser2.

On the Python side, `run_scan` forwards `include_dirs` as `-I` and sets the
subprocess cwd to the source file’s parent as an extra safeguard.

After the fix, all three cases pass natively on both parsers (no Flang
Stage-1 fallback):

- `fortran_include_loop` — include of the `op_par_loop` site
- `fortran_include_host` — include of host-only init
- `fortran_include_nested` — nested includes

No cases were found where fparser2 translated and Flang failed *hard* without
a successful fparser2 fallback (for `seq` / `c_seq` mini-apps probed).
