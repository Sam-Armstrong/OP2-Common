import cProfile
import dataclasses
import json
import os
import pdb
import pstats
from argparse import ArgumentParser, ArgumentTypeError, Namespace
from multiprocessing import Pool
from datetime import datetime
from fnmatch import fnmatch
from pathlib import Path

import cpp
import fortran
from jinja import env
from language import Lang
from op import OpError, Type
from scheme import Scheme
from store import Application, ParseError
from target import Target
from util import getVersion, safeFind


def main(argv=None) -> None:
    """
    Entry point for the OP2 source-to-source translator.

    Orchestrates the full translation pipeline: argument parsing, source file
    parsing, validation, code generation for each target, and program
    translation output. Supports C++ and Fortran OP2 applications and can
    generate code for multiple backend targets (e.g. sequential, CUDA, OpenMP).

    Args:
        argv: Optional list of command-line arguments. If None, defaults to
            sys.argv as per ArgumentParser behaviour.
    """
    # Build arg parser
    parser = ArgumentParser(prog="op2-translator")

    # Flags
    parser.add_argument("-V", "--version", help="Version", action="version", version=getVersion())
    parser.add_argument("-v", "--verbose", help="Verbose", action="store_true")
    parser.add_argument("-d", "--dump", help="JSON store dump", action="store_true")
    parser.add_argument("-o", "--out", help="Output directory", type=isDirPath)
    parser.add_argument("-c", "--config", help="Target configuration", action="append", type=json.loads, default=[])
    parser.add_argument("-soa", "--force_soa", help="Force Structs of Arrays", action="store_true")
    parser.add_argument("-mp", "--multiprocess_parse", help="Force Multiprocess Parsing", action="store_true")

    parser.add_argument("--suffix", help="Add a suffix to generated program translations", default="")

    parser.add_argument("-I", help="Add to include directories", type=isDirPath, action="append", nargs=1, default=[])
    parser.add_argument("-D", help="Add to preprocessor defines", action="append", nargs=1, default=[])

    target_names = [target.name for target in Target.all()]
    parser.add_argument(
        "-t",
        "--target",
        help="Code-generation target",
        type=str,
        action="append",
        nargs=1,
        choices=target_names,
        default=[],
    )

    parser.add_argument("file_paths", help="Input OP2 sources", type=isFilePath, nargs="+")

    for lang in Lang.all():
        lang.addArgs(parser)

    # Invoke arg parser
    args = parser.parse_args(argv)

    if os.environ.get("OP_AUTO_SOA") is not None:
        args.force_soa = True

    file_parents = [Path(file_path).parent for file_path in args.file_paths]

    if args.out is None:
        args.out = file_parents[0]

    script_parents = list(Path(__file__).resolve().parents)
    if len(script_parents) >= 3 and script_parents[2].stem == "OP2-Common":
        args.I = [[str(script_parents[2].joinpath("op2/include"))]] + args.I

    args.I = [[str(file_parent)] for file_parent in dict.fromkeys(file_parents).keys()] + args.I

    # Collect the set of file extensions
    extensions = {str(Path(file_path).suffix)[1:] for file_path in args.file_paths}

    # Validate the file extensions
    if not extensions:
        exit("Missing file extensions, unable to determine target language.")
    elif len(extensions) > 1:
        exit("Varying file extensions, unable to determine target language.")
    else:
        [extension] = extensions

    lang = Lang.find(extension)

    if lang is None:
        exit(f"Unknown file extension: {extension}")

    lang.parseArgs(args)

    Type.set_formatter(lang.formatType)

    if len(args.target) == 0:
        args.target = [[target_name] for target_name in target_names]

    include_dirs = set([Path(dir) for [dir] in args.I])
    defines = [define for [define] in args.D]

    try:
        app = parse(args, lang)
    except ParseError as e:
        print(e)
        exit(1)

    if args.consts_module is not None:
        app.consts_module = lang.parseProgram(Path(args.consts_module), include_dirs, defines)

    if args.extra_consts_list is not None:
        with open(args.extra_consts_list, "r") as f:
            for line in f:
                const_ptr = line.strip()

                if const_ptr != "":
                    app.external_consts.add(const_ptr.lower())

    if args.force_soa:
        for program in app.programs:
            for loop in program.loops:
                loop.dats = [dataclasses.replace(dat, soa=True) for dat in loop.dats]

    if args.verbose:
        print()
        print(app)

    # Validation phase
    try:
        print()
        print("Validating...")
        validate(args, lang, app)
    except OpError as e:
        print(e)
        exit(1)

    for [target] in args.target:
        target = Target.find(target)
        scheme = Scheme.find((lang, target))

        if not scheme:
            print(f"No scheme registered for {lang}/{target}\n")
            continue

        print(f"Translation scheme: {scheme}")
        codegen(args, scheme, app, args.force_soa)
        print()

    # Generate program translations
    for i, program in enumerate(app.programs, 1):
        source = lang.translateProgram(program, include_dirs, defines, args.force_soa)

        new_file = os.path.splitext(os.path.basename(program.path))[0]
        ext = os.path.splitext(os.path.basename(program.path))[1]
        new_path = Path(args.out, f"{new_file}{args.suffix}{ext}")

        write_file(new_path, source, args)

        print(f"Translated program {i} of {len(args.file_paths)}: {new_path}")


def write_file(path: Path, text: str, args: Namespace) -> None:
    """
    Write generated source text to a file, with safety checks.

    Prevents accidental overwriting of input files by comparing the output
    path against all input file paths. Also skips writing if the file already
    exists and its content is identical to the new text, avoiding unnecessary
    filesystem writes and preserving timestamps.

    Args:
        path: Destination file path to write to.
        text: The generated source code content.
        args: Parsed command-line arguments, used to check input file paths.

    Raises:
        SystemExit: If writing would overwrite an input file.
    """
    if path.exists():
        for input_path in args.file_paths:
            if not path.samefile(input_path):
                continue

            print(f"Error: generating file '{path}' would overwrite input file")
            print(f"Pass an output directory with -o <path>")
            exit(1)

    if path.is_file():
        prev_text = path.read_text()

        if text == prev_text:
            return

    with path.open("w") as f:
        # f.write(f"{scheme.lang.com_delim} Auto-generated at {datetime.now()} by op2-translator\n\n")
        f.write(text)


def parse(args: Namespace, lang: Lang) -> Application:
    """
    Parse all input source files into an Application representation.

    Parses each input file using the appropriate language parser. When the
    language's AST is serializable (e.g. Fortran via fparser), parsing is
    parallelised across a multiprocessing pool. Otherwise, files are parsed
    sequentially.

    Args:
        args: Parsed command-line arguments containing file paths, include
            directories, and preprocessor defines.
        lang: The detected source language handler.

    Returns:
        An Application instance populated with parsed Program objects.

    Raises:
        SystemExit: If a Fortran syntax error is encountered during parallel
            parsing.
        ParseError: Propagated to the caller if an OP2 API parse error occurs.
    """
    f_args = [(i, raw_path, lang, args) for i, raw_path in enumerate(args.file_paths, 1)]

    print(f"Parsing files:")
    for raw_path in args.file_paths:
        print(f"    {raw_path}")

    app = Application()

    if lang.ast_is_serializable:
        try:
            if args.multiprocess_parse:
                app.programs = Pool().starmap(parse_file, f_args)
            else:
                app.programs = [parse_file(*args) for args in f_args]
        except fortran.FortranSyntaxError as err:
            print()
            print(f"Syntax error in file {err.filename}:")
            print(err.message)

            exit(1)
    else:
        app.programs = []
        for a in f_args:
            app.programs.append(parse_file(*a))

    return app


def parse_file(i, raw_path, lang, args):
    """
    Parse a single source file into a Program representation.

    Worker function used by both sequential and multiprocessing-based parsing.
    Extracts include directories and preprocessor defines from the arguments
    and delegates to the language-specific parser.

    Args:
        i: 1-based index of the file being parsed (used for progress display
            in the caller).
        raw_path: File path string of the source file to parse.
        lang: The language handler providing the parseProgram method.
        args: Parsed command-line arguments containing include directories (-I)
            and preprocessor defines (-D).

    Returns:
        A Program object representing the parsed source file.
    """
    include_dirs = set([Path(dir) for [dir] in args.I])
    defines = [define for [define] in args.D]

    return lang.parseProgram(Path(raw_path), include_dirs, defines)


def validate(args: Namespace, lang: Lang, app: Application) -> None:
    """
    Run semantic validation on the parsed application and optionally dump the store.

    Performs language-specific semantic checks on the parsed OP2 application
    (e.g. verifying dat dimensions, map arities, and argument consistency).
    If the --dump flag is set, serialises the application state to a JSON file
    for debugging and inspection.

    Args:
        args: Parsed command-line arguments, including the dump flag and output
            directory.
        lang: The language handler used for validation rules.
        app: The parsed Application to validate.

    Raises:
        OpError: Propagated to the caller if a semantic validation error is
            detected.
    """
    # Lazily attach fparser2 ASTs for Flang Stage 1 before validation.
    if hasattr(lang, "prepare_flang_fallback"):
        include_dirs = set(Path(d[0]) for d in args.I)
        defines = [d[0] for d in args.D]
        lang.prepare_flang_fallback(app, include_dirs, defines)

    app.validate(lang)

    # Create a JSON dump
    if args.dump:
        store_path = Path(args.out, "store.json")
        serializer = lambda o: getattr(o, "__dict__", "unserializable")

        # Write application dump
        with open(store_path, "w") as file:
            file.write(json.dumps(app, default=serializer, indent=4))

        print("Dumped store:", store_path, end="\n\n")


def codegen(args: Namespace, scheme: Scheme, app: Application, force_soa: bool) -> None:
    """
    Generate backend-specific loop host kernels, constants, and master kernel files.

    Iterates over all OP2 parallel loops in the application and generates
    target-specific kernel source files using the provided translation scheme.
    Also generates a constants module and a master kernel file if the scheme
    requires them. Loops that cannot be fully translated for the target fall
    back to a sequential implementation.

    Args:
        args: Parsed command-line arguments, including output directory, include
            directories, preprocessor defines, and target configuration.
        scheme: The translation scheme pairing a language with a backend target,
            providing Jinja templates and generation methods.
        app: The parsed and validated Application containing all loops and data.
        force_soa: If True, force Struct-of-Arrays data layout for all dats.
    """
    # Collect the paths of the generated files
    include_dirs = set([Path(dir) for [dir] in args.I])
    defines = [define for [define] in args.D]

    fallback_loops = {}

    # Generate loop hosts
    for i, (loop, program) in enumerate(app.loops(), 1):
        force_generate = scheme.target == Target.find("seq")

        # Generate loop host source
        res = scheme.genLoopHost(env, loop, program, app, i, args.config, force_generate)

        if res is None:
            print(f"Error: unable to generate loop host {i}")
            continue

        files, fallback = res

        Path(args.out, scheme.target.name).mkdir(parents=True, exist_ok=True)
        for index, (source, extension) in enumerate(files):
            name = f"{loop.name}_kernel"
            if index > 0:
                name += f"_aux{index}"

            path = Path(
                args.out,
                scheme.target.name,
                f"{name}{extension}",
            )

            write_file(path, source, args)

        if not fallback:
            fallback_loops[loop.name] = False
            print(f"Generated loop host {i} of {len(app.loops())}: {loop.name}")

        if fallback:
            fallback_loops[loop.name] = True
            print(f"Generated loop host {i} of {len(app.loops())} (fallback): {loop.name}")

    # Generate consts file
    if scheme.consts_template is not None and getattr(scheme.lang, "user_consts_module", None) is None:
        source, name = scheme.genConsts(env, app)

        Path(args.out, scheme.target.name).mkdir(parents=True, exist_ok=True)
        path = Path(args.out, scheme.target.name, name)

        write_file(path, source, args)
        print(f"Generated consts file: {path}")

    # Generate master kernel file
    if len(scheme.master_kernel_templates) > 0:
        user_types_name = f"user_types.{scheme.lang.include_ext}"
        user_types_candidates = [Path(dir, user_types_name) for dir in include_dirs]
        user_types_file = safeFind(user_types_candidates, lambda p: p.is_file())

        files = scheme.genMasterKernel(env, app, user_types_file, fallback_loops)

        for index, (source, extension) in enumerate(files):
            Path(args.out, scheme.target.name).mkdir(parents=True, exist_ok=True)

            name = f"op2_kernels"
            if index > 0:
                name += f"_aux{index}"

            path = Path(args.out, scheme.target.name, f"{name}{extension}")

            write_file(path, source, args)
            print(f"Generated master kernel file: {path}")


def isDirPath(path):
    """
    Validate that a path refers to an existing directory.

    Used as an argparse type validator for directory arguments such as -o and -I.

    Args:
        path: The path string to validate.

    Returns:
        The path string unchanged if it is a valid directory.

    Raises:
        ArgumentTypeError: If the path does not point to an existing directory.
    """
    if os.path.isdir(path):
        return path
    else:
        raise ArgumentTypeError(f"invalid dir path: {path}")


def isFilePath(path):
    """
    Validate that a path refers to an existing file.

    Used as an argparse type validator for the positional file_paths argument.

    Args:
        path: The path string to validate.

    Returns:
        The path string unchanged if it is a valid file.

    Raises:
        ArgumentTypeError: If the path does not point to an existing file.
    """
    if os.path.isfile(path):
        return path
    else:
        raise ArgumentTypeError(f"invalid file path: {path}")


if __name__ == "__main__":
    if os.environ.get("OP2_TRANSLATOR_PROFILE"):
        profiler = cProfile.Profile()

        profiler.enable()
        main()
        profiler.disable()

        stats = pstats.Stats(profiler)
        stats.sort_stats(pstats.SortKey.CUMULATIVE).print_stats(10)
    else:
        main()
