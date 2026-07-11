// =============================================================================
// op2-flang-scan: Stage 1 parser for the OP2 Fortran translator.
// =============================================================================
//
// This is a small stand-alone executable that links against LLVM Flang's
// parser library and is invoked as a subprocess by the Python translator
// (see translator-v2/op2-translator/fortran/flang_parser.py).
//
// What it does
// ------------
// It reads a Fortran source file, asks Flang to parse it, walks the parse
// tree, and writes a single JSON document to stdout describing:
//
//   1. Every `op_par_loop_N(...)` call site (with its full argument tree).
//   2. Every `op_decl_const(...)` call site (with its full argument tree).
//   3. Every top-level subroutine and function definition (name,
//      parameter list, dependency call/reference names, and a textual
//      copy of the body so the Python side can rewrite kernels without
//      a fparser2 AST).
//
// Why this layer exists
// ---------------------
// The translator's original Stage 1 was written in pure Python on top of
// fparser2. That's portable and easy to hack on, but slow and lossy for
// some Fortran constructs. Routing Stage 1 through Flang gives us:
//
//   * A standards-tracking parser maintained by the Flang team.
//   * Faster parsing for large translation units (it's compiled C++).
//   * A single source of truth for parse-tree structure when we eventually
//     want to push more of the kernel rewriting work down here too.
//
// What it deliberately does NOT do
// --------------------------------
//   * No semantic analysis. Flang's later semantics passes can disambiguate
//     things like "is `q(i)` an array indexing or a function call?", but we
//     stop after Parse() and let the Python side filter / cross-reference.
//   * No code generation. We're a parser-only tool; we don't lower to MLIR
//     or LLVM IR.
//   * No persistent state. Each invocation parses one translation unit and
//     exits; the Python driver re-runs us per file.
//
// JSON output contract
// --------------------
// The shape consumed by the Python side is roughly:
//
//   {
//     "path": "<original input path>",
//     "events": [
//       {"kind": "op_par_loop_N", "location": {...}, "args": [...] },
//       {"kind": "op_decl_const", "location": {...}, "args": [...] },
//       {"kind": "subroutine_subprogram"|"function_subprogram",
//        "name": "...", "location": {...}, "parameters": [...],
//        "depends": [...], "source": "..."},
//       ...
//     ]
//   }
//
// Each `args` entry is one of: {"kind": "name"|"int"|"string"|"call"|"raw"}.
// See the "Expression serialization" section below for details.
//
// CLI
// ---
//   op2-flang-scan [--stdin] [--path <reported-path>] [path]
//
// When --stdin is given (or no path argument is supplied), the source is
// slurped from stdin into a temp file and that temp file is fed to Flang.
// The --path option overrides the path that we report in the JSON output
// (handy when the actual input came from stdin and the caller wants the
// JSON to mention the original file name).
// =============================================================================

// -----------------------------------------------------------------------------
// Flang public headers used by this tool.
//
// We only depend on the parser layer (parsing.h, parse-tree*.h, provenance.h,
// source.h, message.h). Everything from Sema, FIR, MLIR or the driver is
// intentionally excluded so the link surface stays small.
// -----------------------------------------------------------------------------
#include "flang/Parser/parsing.h"
#include "flang/Parser/parse-tree.h"
#include "flang/Parser/parse-tree-visitor.h"
#include "flang/Parser/provenance.h"
#include "flang/Parser/source.h"
#include "flang/Parser/message.h"

#include "llvm/Support/raw_ostream.h"

// Standard library: ctype/string utilities, the small JSON buffer, the parse
// tree variant tags (variant), and the OS shims for reading stdin / picking
// a temp path.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <process.h>     // _getpid on Windows (used to name the temp file)
#else
#include <unistd.h>      // ::getpid on POSIX
#endif

// Short alias for the namespace we live in for 95 % of this file.
namespace fp = Fortran::parser;

// =============================================================================
// JSON writer
// =============================================================================
//
// Tiny hand-written streaming JSON emitter, written so we can avoid pulling
// a real JSON library into the link. The interface is push-style: the caller
// drives a sequence of beginObject/key/value/endObject and beginArray/value/
// endArray calls and the writer takes care of placing the commas in the
// right spots.
//
// The state machine that decides whether to prefix the next token with a
// comma is a stack of bools (first_), one entry per currently-open
// object or array. The top entry says "is the next value I'm about to write
// the first one in this container?". Every value-producing call (a leaf,
// or a nested object/array) reads the top entry, emits the comma if needed,
// and then sets it to false. `key` is a special case: it acts as both the
// closing of one (key, value) pair and the opening of the next, so it
// resets the slot back to "first" so the value that follows doesn't get
// a stray leading comma.
//
// Strings are escaped per RFC 8259: the structural specials (", \, control
// characters, plus the usual whitespace shorthands), with anything else
// passed through as raw bytes. We do not transcode UTF-8.
// -----------------------------------------------------------------------------
class Json {
public:
    void beginObject() { comma(); out_ << "{"; first_.push_back(true); }
    void endObject() { out_ << "}"; first_.pop_back(); markWrote(); }

    void beginArray() { comma(); out_ << "["; first_.push_back(true); }
    void endArray() { out_ << "]"; first_.pop_back(); markWrote(); }

    // Emit "key":  Followed by exactly one value-emitting call.
    void key(const std::string &k) {
        comma();
        writeString(k);
        out_ << ":";
        // The value paired with this key is the *first* token after the colon
        // (so it must not have a leading comma) but it is NOT the first
        // entry in the surrounding object. Reuse the existing stack slot to
        // record that.
        if (!first_.empty()) {
            first_.back() = true;
        }
    }

    void stringValue(const std::string &s) { comma(); writeString(s); markWrote(); }
    void intValue(int64_t v) { comma(); out_ << v; markWrote(); }
    void boolValue(bool b) { comma(); out_ << (b ? "true" : "false"); markWrote(); }
    void nullValue() { comma(); out_ << "null"; markWrote(); }

    // Splice in a pre-rendered JSON fragment verbatim (no quoting/escaping).
    // Used to stitch together documents built with separate Json instances,
    // e.g. when two output arrays need to be populated by a single
    // interleaved tree walk (see BodyCollector).
    void rawValue(const std::string &jsonText) { comma(); out_ << jsonText; markWrote(); }

    // Snapshot of the buffer; safe to call once the top-level object/array
    // has been closed.
    std::string str() const { return out_.str(); }

private:
    // If this isn't the first value in the current container, emit a comma.
    // Either way, mark the slot as no-longer-first.
    void comma() {
        if (!first_.empty()) {
            if (!first_.back()) {
                out_ << ",";
            }
            first_.back() = false;
        }
    }

    // Some emitters (key) need to remember they wrote something without
    // going through `comma`. This sets the slot to "not first" without
    // emitting anything.
    void markWrote() {
        if (!first_.empty()) first_.back() = false;
    }

    // RFC 8259 string escaping. We bail out to \uXXXX for any control byte
    // we don't have a shorthand for; everything else (including non-ASCII
    // payload bytes) is passed through unchanged.
    void writeString(const std::string &s) {
        out_ << '"';
        for (char c : s) {
            switch (c) {
                case '"': out_ << "\\\""; break;
                case '\\': out_ << "\\\\"; break;
                case '\n': out_ << "\\n"; break;
                case '\r': out_ << "\\r"; break;
                case '\t': out_ << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out_ << buf;
                    } else {
                        out_ << c;
                    }
            }
        }
        out_ << '"';
    }

    std::ostringstream out_;
    std::vector<bool> first_;   // depth-stack of "is the next slot the first?"
};

// Lower-case an ASCII string. Used everywhere we hand a Fortran identifier
// to JSON, since Fortran is case-insensitive but the Python side does
// case-sensitive comparisons.
static std::string toLower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// =============================================================================
// Expression serialization
// =============================================================================
//
// `op_par_loop_N` and `op_decl_const` arguments arrive at parse time as
// generic `Fortran::parser::Expr` nodes. We translate each one into a small
// tagged JSON dictionary so the Python translator gets a stable, version-
// independent representation that doesn't require it to understand Flang's
// (large, churn-prone) parse-tree variant set.
//
// Output shapes
// -------------
//   {"kind": "name",   "value": "<identifier>"}
//       A bare identifier reference, e.g. `OP_READ`, `OP_ID`, `p_q`.
//
//   {"kind": "int",    "value": <folded-int>}
//       Anything that constant-folds to an integer (literals, signed
//       literals, parenthesised, simple +/-/*/divide/power chains).
//       Used for op_par_loop's iteration count, op_decl_const's dimension,
//       op_arg_dat indices, etc.
//
//   {"kind": "string", "value": "<literal contents>"}
//       A character literal, with surrounding quotes and any kind/encoding
//       prefix stripped. Used for the `typ` argument of op_decl_const,
//       e.g. `"real(8)"`.
//
//   {"kind": "call",   "name": "<identifier>", "args": [ ...expr... ]}
//       A nested function-style call, e.g. `op_arg_dat(...)` /
//       `op_arg_gbl(...)` / `op_arg_idx(...)`. We recurse into the args.
//
//   {"kind": "raw",    "source": "<original source text>"}
//       Fallback for anything the dispatch below doesn't recognise. The
//       Python side either re-parses the source slice or, if that also
//       fails, surfaces a clear error pointing at the location.
//
// The fparser2-driven Stage 1 parser in op2-translator/fortran/parser.py
// expects exactly these five tags; keep them in sync if you add more.
// =============================================================================

// Convenience wrapper: get a CharBlock back as a std::string. CharBlock is a
// (char*, size_t) view into Flang's cooked source buffer; ToString() copies.
static std::string sourceText(fp::CharBlock src) {
    return src.ToString();
}

// Forward declarations - emitExpr and emitActualArgs are mutually recursive.
static void emitExpr(Json &json, const fp::Expr &e);
static void emitActualArgs(Json &json, const std::list<fp::ActualArgSpec> &args);

// Attempt to fold an integer-valued Expr to a plain int. Returns nullopt for
// anything we do not understand, in which case the caller falls through to
// the next emitter ("name" / "string" / ... / "raw").
static std::optional<int64_t> foldIntExpr(const fp::Expr &e);

// Parse the textual representation of an integer literal as it appears in
// Flang's parse tree (e.g. "42", "-7", "1_8"). std::stoll handles the
// leading sign and digits; we deliberately ignore the trailing kind suffix.
static std::optional<int64_t> parseIntText(const std::string &text) {
    try {
        size_t pos = 0;
        long long v = std::stoll(text, &pos);
        // Accept any trailing kind suffix like 4_ik, 1_8, 3_kind
        return static_cast<int64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

// Pull an integer out of a LiteralConstant variant. We only handle integer
// literals here; reals/booleans/etc. fall through to nullopt and the caller
// continues its dispatch.
static std::optional<int64_t>
foldLiteralConstant(const fp::LiteralConstant &lit) {
    return std::visit([](const auto &alt) -> std::optional<int64_t> {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::IntLiteralConstant>) {
            // IntLiteralConstant is std::tuple<CharBlock, std::optional<KindParam>>
            const fp::CharBlock &cb = std::get<fp::CharBlock>(alt.t);
            return parseIntText(cb.ToString());
        } else if constexpr (std::is_same_v<T, fp::SignedIntLiteralConstant>) {
            const fp::CharBlock &cb = std::get<fp::CharBlock>(alt.t);
            return parseIntText(cb.ToString());
        } else {
            return std::nullopt;
        }
    }, lit.u);
}

// Tiny constant folder for integer-valued Exprs.
//
// We only support enough operators to recognise the kinds of expressions
// real OP2 source code uses for op_par_loop arities, op_decl_const sizes,
// op_arg_dat indices and similar parameters: literals, parenthesised
// subexpressions, unary +/-, and binary +, -, *, /, **. Anything more
// complicated yields nullopt and the caller emits the argument as "raw"
// text so the Python side can decide what to do.
static std::optional<int64_t> foldIntExpr(const fp::Expr &e) {
    return std::visit([](const auto &alt) -> std::optional<int64_t> {
        using T = std::decay_t<decltype(alt)>;

        if constexpr (std::is_same_v<T, fp::LiteralConstant>) {
            return foldLiteralConstant(alt);
        } else if constexpr (std::is_same_v<T, fp::Expr::Parentheses>) {
            return foldIntExpr(alt.v.value());
        } else if constexpr (std::is_same_v<T, fp::Expr::UnaryPlus>) {
            return foldIntExpr(alt.v.value());
        } else if constexpr (std::is_same_v<T, fp::Expr::Negate>) {
            auto inner = foldIntExpr(alt.v.value());
            return inner ? std::optional<int64_t>{-*inner} : std::nullopt;
        } else if constexpr (std::is_same_v<T, fp::Expr::Add>) {
            auto l = foldIntExpr(std::get<0>(alt.t).value());
            auto r = foldIntExpr(std::get<1>(alt.t).value());
            if (l && r) return *l + *r;
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, fp::Expr::Subtract>) {
            auto l = foldIntExpr(std::get<0>(alt.t).value());
            auto r = foldIntExpr(std::get<1>(alt.t).value());
            if (l && r) return *l - *r;
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, fp::Expr::Multiply>) {
            auto l = foldIntExpr(std::get<0>(alt.t).value());
            auto r = foldIntExpr(std::get<1>(alt.t).value());
            if (l && r) return (*l) * (*r);
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, fp::Expr::Divide>) {
            auto l = foldIntExpr(std::get<0>(alt.t).value());
            auto r = foldIntExpr(std::get<1>(alt.t).value());
            if (l && r && *r != 0) return (*l) / (*r);
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, fp::Expr::Power>) {
            auto l = foldIntExpr(std::get<0>(alt.t).value());
            auto r = foldIntExpr(std::get<1>(alt.t).value());
            if (l && r && *r >= 0) {
                int64_t result = 1;
                for (int64_t i = 0; i < *r; ++i) result *= *l;
                return result;
            }
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }, e.u);
}

// Pull a bare identifier out of a Designator (e.g. "p_q", "OP_ID", "OP_READ").
// Designator is a discriminated union (DataRef | Substring), and DataRef is
// itself a union of (Name | StructureComponent | ArrayElement | ...). We only
// succeed when the leaf is a single Name; anything more elaborate (e.g.
// `mod%name`) is reported as nullopt so the caller falls through to "raw".
static std::optional<std::string>
designatorToName(const fp::Designator &d) {
    return std::visit([](const auto &alt) -> std::optional<std::string> {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::DataRef>) {
            // DataRef can be a Name, StructureComponent, ArrayElement, etc.
            return std::visit([](const auto &inner) -> std::optional<std::string> {
                using U = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<U, fp::Name>) {
                    return toLower(inner.ToString());
                } else {
                    return std::nullopt;
                }
            }, alt.u);
        } else {
            return std::nullopt;
        }
    }, d.u);
}

// A view onto a "looks like a call" expression: just the callee identifier
// and a borrowed pointer into the parse tree's argument list. We hand both
// straight to emitActualArgs so we never need to copy the args.
struct CallView {
    std::string name;
    const std::list<fp::ActualArgSpec> *args = nullptr;
};

// Try to extract (callee-name, args) out of an Expr that looks like a call.
//
// At parse time `op_arg_dat(...)` shows up as a FunctionReference because
// Flang's parser doesn't yet know that `op_arg_dat` is a derived-type
// constructor (no semantics have run). The Python side knows what each
// helper means, so we just emit the call shape and let it interpret. We
// also leave a slot for the keyword-argument variant (StructureConstructor)
// even though OP2 source today doesn't use it.
static std::optional<CallView> exprAsCall(const fp::Expr &e) {
    return std::visit([](const auto &alt) -> std::optional<CallView> {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::FunctionReference>>) {
            const fp::FunctionReference &fr = alt.value();
            // FunctionReference wraps a Call = std::tuple<ProcedureDesignator, std::list<ActualArgSpec>>
            const fp::Call &call = fr.v;
            const fp::ProcedureDesignator &pd = std::get<fp::ProcedureDesignator>(call.t);
            const auto &args = std::get<std::list<fp::ActualArgSpec>>(call.t);
            return std::visit([&](const auto &p) -> std::optional<CallView> {
                using P = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<P, fp::Name>) {
                    return CallView{toLower(p.ToString()), &args};
                } else {
                    return std::nullopt;
                }
            }, pd.u);
        } else if constexpr (std::is_same_v<T, fp::StructureConstructor>) {
            // Keyword-arg form; Flang parses it directly as a StructureConstructor.
            // We do not see this for the plain-positional op_arg_dat calls, but
            // we leave a slot for it.
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }, e.u);
}

// Emit one expression as a JSON object.
//
// Dispatch order matters: we try the most specific shape first (integer
// folding, then character literal, then bare identifier, then nested call)
// and only fall back to the generic "raw" source-text emission when none
// of the structured paths match. Earlier successful matches short-circuit.
static void emitExpr(Json &json, const fp::Expr &e) {
    // 1. Integer literal (possibly signed / parenthesised / simple arithmetic).
    if (auto v = foldIntExpr(e)) {
        json.beginObject();
        json.key("kind"); json.stringValue("int");
        json.key("value"); json.intValue(*v);
        json.endObject();
        return;
    }

    // 2. Character literal, e.g. "real(8)".
    //
    // The parse-tree layout for character literals in an expression varies
    // between releases:
    //   - older Flang: Expr::u has `common::Indirection<CharLiteralConstant>`
    //     as its own arm.
    //   - newer Flang (LLVM 19+): Expr::u has `LiteralConstant`, whose own
    //     `u` variant holds `CharLiteralConstant` (and the other literal
    //     kinds).
    // We handle both shapes.
    auto extractCharLiteral = [](const fp::Expr &expr) -> std::optional<std::string> {
        return std::visit([](const auto &alt) -> std::optional<std::string> {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::CharLiteralConstant>>) {
                // CharLiteralConstant::t = std::tuple<std::optional<KindParam>, std::string>
                return std::get<std::string>(alt.value().t);
            } else if constexpr (std::is_same_v<T, fp::LiteralConstant>) {
                return std::visit([](const auto &inner) -> std::optional<std::string> {
                    using U = std::decay_t<decltype(inner)>;
                    if constexpr (std::is_same_v<U, fp::CharLiteralConstant>) {
                        return std::get<std::string>(inner.t);
                    } else {
                        return std::nullopt;
                    }
                }, alt.u);
            } else {
                return std::nullopt;
            }
        }, expr.u);
    };
    if (auto s = extractCharLiteral(e)) {
        json.beginObject();
        json.key("kind"); json.stringValue("string");
        json.key("value"); json.stringValue(*s);
        json.endObject();
        return;
    }

    // 3. Bare identifier (e.g. OP_READ, OP_ID, p_q).
    bool emitted = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Designator>>) {
            if (auto name = designatorToName(alt.value())) {
                json.beginObject();
                json.key("kind"); json.stringValue("name");
                json.key("value"); json.stringValue(*name);
                json.endObject();
                return true;
            }
        }
        return false;
    }, e.u);
    if (emitted) return;

    // 4. Nested call, e.g. op_arg_dat(...), op_arg_gbl(...), op_arg_idx(...).
    if (auto call = exprAsCall(e)) {
        json.beginObject();
        json.key("kind"); json.stringValue("call");
        json.key("name"); json.stringValue(call->name);
        json.key("args");
        emitActualArgs(json, *call->args);
        json.endObject();
        return;
    }

    // 5. Fallback: emit the raw source text so the Python side can either
    // parse it or flag it as unsupported.
    json.beginObject();
    json.key("kind"); json.stringValue("raw");
    json.key("source"); json.stringValue(sourceText(e.source));
    json.endObject();
}

// Emit a parenthesised argument list as a JSON array of expression objects.
//
// In the Fortran 2018 grammar an actual argument is either an expression,
// an alternate-return spec, a procedure name, or a procedure component
// reference. OP2 calls use plain expressions exclusively, so we recognise
// the Indirection<Expr> arm and emit a "raw" placeholder for everything
// else, which gives the Python side something to flag.
static void emitActualArgs(Json &json, const std::list<fp::ActualArgSpec> &args) {
    json.beginArray();
    for (const fp::ActualArgSpec &spec : args) {
        // ActualArgSpec = std::tuple<std::optional<Keyword>, ActualArg>
        // We currently ignore the optional keyword name; positional matching
        // is what every OP2 helper expects.
        const fp::ActualArg &aa = std::get<fp::ActualArg>(spec.t);
        // ActualArg variant: Indirection<Expr>, AltReturnSpec, ActualArgProcedureComponentRef, ProcedureName
        bool handled = std::visit([&](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Expr>>) {
                emitExpr(json, alt.value());
                return true;
            }
            return false;
        }, aa.u);
        if (!handled) {
            json.beginObject();
            json.key("kind"); json.stringValue("raw");
            json.key("source"); json.stringValue("<unsupported-actual-arg>");
            json.endObject();
        }
    }
    json.endArray();
}

// =============================================================================
// DependsCollector: per-subprogram dependency walker
// =============================================================================
//
// Walks one subprogram subtree and gathers the lower-cased names of
// everything that *looks* like a call to another subroutine or function.
//
// We intentionally collect a superset and let Python filter:
//
//   * `CallStmt` callees are unambiguous subroutine calls.
//   * `FunctionReference` callees may be real function calls *or* array
//     indexing - the parser cannot tell them apart without semantic
//     analysis (no symbol table at this stage). The Python side filters
//     these against the known entity list before storing them in
//     `Function.depends`, which mirrors fparser2's existing Part_Ref
//     post-processing in `parseFunctionDependencies`.
//
// The struct follows Flang's parse-tree-visitor convention: each Pre
// returns true to continue walking, the templated fallbacks make sure
// every other node type is silently traversed, and Post() is a no-op.
// =============================================================================
struct DependsCollector {
    std::set<std::string> &out;

    // Direct subroutine call: `call foo(...)`.
    bool Pre(const fp::CallStmt &cs) {
        const fp::Call &c = std::get<fp::Call>(cs.t);
        const fp::ProcedureDesignator &pd = std::get<fp::ProcedureDesignator>(c.t);
        std::visit([&](const auto &p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, fp::Name>) {
                out.insert(toLower(p.ToString()));
            }
        }, pd.u);
        return true;
    }

    // Function-style reference inside an expression: `x = foo(i, j)`. May
    // be a real function call or array indexing; Python disambiguates.
    bool Pre(const fp::FunctionReference &fr) {
        const fp::Call &c = fr.v;
        const fp::ProcedureDesignator &pd = std::get<fp::ProcedureDesignator>(c.t);
        std::visit([&](const auto &p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, fp::Name>) {
                out.insert(toLower(p.ToString()));
            }
        }, pd.u);
        return true;
    }

    // No-op fallbacks for every other parse-tree node type. Required by
    // Flang's Walk() so that the visitor matches every node it visits.
    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

// =============================================================================
// Kernel-body expression/statement serialization (for Stage 2 validation)
// =============================================================================
//
// Stage 2 (op2-translator/fortran/validator.py) needs to inspect *how* each
// kernel dummy parameter is used inside the kernel body: is it written when
// it should only be read, is it incremented correctly, is it sliced in a
// way that's incompatible with SIMD stride insertion, etc. The original
// implementation walks fparser2's AST directly, using parent-pointer checks
// (e.g. "is this Name the base of a Part_Ref?"). To run equivalent checks
// without fparser2 (--parser flang), we serialize a simplified expression
// tree for each subprogram's body and port the checks to walk that JSON
// tree in Python (see fortran/flang_validator.py), where the "parent"
// context is simply whatever the recursive walk was descending from.
//
// This is deliberately NOT a full unparse: we only capture the node shapes
// the validator actually inspects (assignments, calls, array/section
// subscripts, and the +/-/*//,** arithmetic skeleton needed by the
// increment check). Anything else collapses to a "literal"/"raw" leaf
// carrying the exact source text, which the Python side treats opaquely.
//
// Node shapes (all objects have a "kind" field):
//   {"kind": "name", "value": "<identifier>"}
//   {"kind": "literal"|"raw", "source": "<text>"}
//   {"kind": "part_ref", "name": "<base identifier>", "subscripts": [...]}
//       An unambiguous array-element/section reference (Designator ->
//       DataRef -> ArrayElement). Each subscript is either a nested expr
//       node (a scalar/vector index - Flang's grammar can't tell those
//       apart without semantics), or a "triplet" object (below).
//   {"kind": "triplet", "lower": expr|null, "upper": expr|null, "stride": expr|null}
//       A subscript-triplet (`a:b`, `a:b:c`, `:`, ...) - the syntactic
//       marker for a Fortran array *section*, which is what makes stride
//       insertion unsafe.
//   {"kind": "funcref", "name": "<callee>", "args": [...]}
//       A parenthesised reference that Flang cannot yet disambiguate
//       between "array element" and "function call" (no semantics have
//       run). The Python side resolves this the same way DependsCollector's
//       output is resolved: by checking whether "name" matches a known
//       Function entity.
//   {"kind": "binary", "op": "+"|"-"|"*"|"/"|"**"|"=="|"!="|"<"|"<="|">"|">="|
//                            "&&"|"||"|"//", "left": expr, "right": expr}
//       Arithmetic, relational (.EQ./.LT./...), logical (.AND./.OR./.EQV./
//       .NEQV.) and character-concatenation (//) binary operators all share
//       this shape; "op" is already the C++ spelling, not the Fortran one.
//   {"kind": "paren", "expr": expr}
//   {"kind": "unary", "op": "+"|"-"|"!", "expr": expr}
//       "!" is Fortran's `.NOT.`.
//   {"kind": "int_lit", "text": "<digits>", "kind_text": str|null}
//   {"kind": "real_lit", "text": "<digits-with-exponent>", "kind_text": str|null}
//       "text" is the literal's source spelling verbatim (so callers can
//       decide how to render exponent letters / kind suffixes); "kind_text"
//       is the raw source text of the kind selector, e.g. "8"/"RK"/"IK4",
//       or null if none was written.
//   {"kind": "logical_lit", "value": true|false}
//   {"kind": "char_lit", "value": "<contents>"} (quotes/kind prefix stripped)
//   {"kind": "unsupported", "tag": "<node-name>", "source": "<text>"}
//       A Fortran expression form Stage 3 does not (yet) understand
//       (defined operators, array/structure constructors, %LOC, complex
//       literals, ...). The Python side raises a clear "unsupported"
//       error pointing at the source text rather than guessing.
//
// Stage 2's checks only ever pattern-match on "name"/"part_ref"/"funcref"/
// "binary"/"paren"/"unary" and silently skip anything else, so adding the
// literal/unsupported leaf shapes above is backwards compatible with
// fortran/flang_validator.py.
// =============================================================================

static void emitBodyExpr(Json &json, const fp::Expr &e);

// Render a KindParam (R709: `_kind`, e.g. the `8`/`RK`/`IK4` in `1_RK`) as
// its raw source text. Kind selectors used in OP2 kernels are always a bare
// digit string or a bare uppercase name, so callers can decide which one
// they got with a simple `isdigit()`-style check; we deliberately don't try
// to fold the digit case to an int here.
static std::string kindParamToString(const fp::KindParam &kp) {
    return std::visit([](const auto &alt) -> std::string {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, std::uint64_t>) {
            return std::to_string(alt);
        } else {
            // Scalar<Integer<Constant<Name>>>
            return toLower(alt.thing.thing.thing.ToString());
        }
    }, kp.u);
}

// Emit one of the four literal-constant leaf shapes (int/real/logical/char),
// or an "unsupported" leaf for the handful of literal kinds OP2 kernels
// never use (Hollerith, BOZ, unsigned, complex).
static void emitLiteralConstant(Json &json, const fp::LiteralConstant &lit) {
    bool emitted = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::IntLiteralConstant>) {
            const auto &cb = std::get<fp::CharBlock>(alt.t);
            const auto &kindOpt = std::get<std::optional<fp::KindParam>>(alt.t);
            json.beginObject();
            json.key("kind"); json.stringValue("int_lit");
            json.key("text"); json.stringValue(sourceText(cb));
            json.key("kind_text");
            if (kindOpt) json.stringValue(kindParamToString(*kindOpt)); else json.nullValue();
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::RealLiteralConstant>) {
            const auto &real = std::get<fp::RealLiteralConstant::Real>(alt.t);
            const auto &kindOpt = std::get<std::optional<fp::KindParam>>(alt.t);
            json.beginObject();
            json.key("kind"); json.stringValue("real_lit");
            json.key("text"); json.stringValue(sourceText(real.source));
            json.key("kind_text");
            if (kindOpt) json.stringValue(kindParamToString(*kindOpt)); else json.nullValue();
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::LogicalLiteralConstant>) {
            json.beginObject();
            json.key("kind"); json.stringValue("logical_lit");
            json.key("value"); json.boolValue(std::get<bool>(alt.t));
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::CharLiteralConstant>) {
            json.beginObject();
            json.key("kind"); json.stringValue("char_lit");
            json.key("value"); json.stringValue(std::get<std::string>(alt.t));
            json.endObject();
            return true;
        }
        return false;
    }, lit.u);

    if (!emitted) {
        json.beginObject();
        json.key("kind"); json.stringValue("unsupported");
        json.key("tag"); json.stringValue("literal_constant");
        json.endObject();
    }
}

// A SectionSubscript is either a plain (scalar- or vector-valued) IntExpr,
// or a SubscriptTriplet. Only the latter is structurally distinguishable
// from a plain index at parse time (colon syntax isn't valid anywhere
// else), which is exactly the "is this a slice?" signal the validator needs.
static void emitBodySubscript(Json &json, const fp::SectionSubscript &sub) {
    std::visit([&](const auto &alt) {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::SubscriptTriplet>) {
            const auto &t = alt.t;
            auto emitBound = [&](const char *key, const std::optional<fp::Subscript> &bound) {
                json.key(key);
                if (bound) {
                    emitBodyExpr(json, bound->thing.thing.value());
                } else {
                    json.nullValue();
                }
            };
            json.beginObject();
            json.key("kind"); json.stringValue("triplet");
            emitBound("lower", std::get<0>(t));
            emitBound("upper", std::get<1>(t));
            emitBound("stride", std::get<2>(t));
            json.endObject();
        } else {
            // Subscript = ScalarIntExpr = Scalar<Integer<Indirection<Expr>>>.
            emitBodyExpr(json, alt.thing.value());
        }
    }, sub.u);
}

// Emit a Designator (R901: object-name | array-element | ... | substring).
// We only structurally decompose the two shapes the validator cares about
// (plain Name, and array-element via a plain-Name base); everything else
// (structure components, coindexed objects, substrings) becomes "raw".
static void emitDesignator(Json &json, const fp::Designator &d) {
    bool emitted = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::DataRef>) {
            return std::visit([&](const auto &inner) -> bool {
                using U = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<U, fp::Name>) {
                    json.beginObject();
                    json.key("kind"); json.stringValue("name");
                    json.key("value"); json.stringValue(toLower(inner.ToString()));
                    json.endObject();
                    return true;
                } else if constexpr (std::is_same_v<U, Fortran::common::Indirection<fp::ArrayElement>>) {
                    const fp::ArrayElement &ae = inner.value();
                    return std::visit([&](const auto &baseAlt) -> bool {
                        using V = std::decay_t<decltype(baseAlt)>;
                        if constexpr (std::is_same_v<V, fp::Name>) {
                            json.beginObject();
                            json.key("kind"); json.stringValue("part_ref");
                            json.key("name"); json.stringValue(toLower(baseAlt.ToString()));
                            json.key("subscripts");
                            json.beginArray();
                            for (const auto &s : ae.Subscripts()) emitBodySubscript(json, s);
                            json.endArray();
                            json.endObject();
                            return true;
                        }
                        return false;
                    }, ae.Base().u);
                } else {
                    return false;
                }
            }, alt.u);
        }
        return false;
    }, d.u);

    if (!emitted) {
        json.beginObject();
        json.key("kind"); json.stringValue("raw");
        json.key("source"); json.stringValue(sourceText(d.source));
        json.endObject();
    }
}

// Emit a FunctionReference (ambiguous array-element-or-call, RHS-only).
static void emitFuncRef(Json &json, const fp::FunctionReference &fr) {
    const fp::Call &call = fr.v;
    const fp::ProcedureDesignator &pd = std::get<fp::ProcedureDesignator>(call.t);
    const auto &args = std::get<std::list<fp::ActualArgSpec>>(call.t);

    std::optional<std::string> name = std::visit([](const auto &p) -> std::optional<std::string> {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, fp::Name>) {
            return toLower(p.ToString());
        } else {
            return std::nullopt;
        }
    }, pd.u);

    if (!name) {
        json.beginObject();
        json.key("kind"); json.stringValue("raw");
        json.key("source"); json.stringValue("<complex-procedure-designator>");
        json.endObject();
        return;
    }

    json.beginObject();
    json.key("kind"); json.stringValue("funcref");
    json.key("name"); json.stringValue(*name);
    json.key("args");
    json.beginArray();
    for (const fp::ActualArgSpec &spec : args) {
        const fp::ActualArg &aa = std::get<fp::ActualArg>(spec.t);
        bool handled = std::visit([&](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Expr>>) {
                emitBodyExpr(json, alt.value());
                return true;
            }
            return false;
        }, aa.u);
        if (!handled) {
            json.beginObject();
            json.key("kind"); json.stringValue("raw");
            json.key("source"); json.stringValue("<unsupported-actual-arg>");
            json.endObject();
        }
    }
    json.endArray();
    json.endObject();
}

// Emit a Variable (R902: designator | function-reference). Used for the LHS
// of an AssignmentStmt, which - like any parenthesised reference - can in
// principle parse as either shape until semantics run.
static void emitVariable(Json &json, const fp::Variable &v) {
    std::visit([&](const auto &alt) {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Designator>>) {
            emitDesignator(json, alt.value());
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::FunctionReference>>) {
            emitFuncRef(json, alt.value());
        }
    }, v.u);
}

static void emitBodyExpr(Json &json, const fp::Expr &e) {
    bool emitted = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;

        if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Designator>>) {
            emitDesignator(json, alt.value());
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::FunctionReference>>) {
            emitFuncRef(json, alt.value());
            return true;
        } else if constexpr (std::is_same_v<T, fp::Expr::Parentheses>) {
            json.beginObject();
            json.key("kind"); json.stringValue("paren");
            json.key("expr"); emitBodyExpr(json, alt.v.value());
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::Expr::UnaryPlus>) {
            json.beginObject();
            json.key("kind"); json.stringValue("unary");
            json.key("op"); json.stringValue("+");
            json.key("expr"); emitBodyExpr(json, alt.v.value());
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::Expr::Negate>) {
            json.beginObject();
            json.key("kind"); json.stringValue("unary");
            json.key("op"); json.stringValue("-");
            json.key("expr"); emitBodyExpr(json, alt.v.value());
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::Expr::Add> || std::is_same_v<T, fp::Expr::Subtract> ||
                              std::is_same_v<T, fp::Expr::Multiply> || std::is_same_v<T, fp::Expr::Divide> ||
                              std::is_same_v<T, fp::Expr::Power> || std::is_same_v<T, fp::Expr::Concat> ||
                              std::is_same_v<T, fp::Expr::LT> || std::is_same_v<T, fp::Expr::LE> ||
                              std::is_same_v<T, fp::Expr::EQ> || std::is_same_v<T, fp::Expr::NE> ||
                              std::is_same_v<T, fp::Expr::GE> || std::is_same_v<T, fp::Expr::GT> ||
                              std::is_same_v<T, fp::Expr::AND> || std::is_same_v<T, fp::Expr::OR> ||
                              std::is_same_v<T, fp::Expr::EQV> || std::is_same_v<T, fp::Expr::NEQV>) {
            // All of Fortran's binary intrinsic operators (arithmetic,
            // relational, logical, concatenation) share the same
            // IntrinsicBinary tuple<Indirection<Expr>, Indirection<Expr>>
            // shape; only the spelling of "op" differs. We emit the C++
            // spelling directly rather than the Fortran token, since the
            // only consumer is Stage 3 code generation.
            const char *op = "+";
            if constexpr (std::is_same_v<T, fp::Expr::Subtract>) op = "-";
            else if constexpr (std::is_same_v<T, fp::Expr::Multiply>) op = "*";
            else if constexpr (std::is_same_v<T, fp::Expr::Divide>) op = "/";
            else if constexpr (std::is_same_v<T, fp::Expr::Power>) op = "**";
            else if constexpr (std::is_same_v<T, fp::Expr::Concat>) op = "//";
            else if constexpr (std::is_same_v<T, fp::Expr::LT>) op = "<";
            else if constexpr (std::is_same_v<T, fp::Expr::LE>) op = "<=";
            else if constexpr (std::is_same_v<T, fp::Expr::EQ>) op = "==";
            else if constexpr (std::is_same_v<T, fp::Expr::NE>) op = "!=";
            else if constexpr (std::is_same_v<T, fp::Expr::GE>) op = ">=";
            else if constexpr (std::is_same_v<T, fp::Expr::GT>) op = ">";
            else if constexpr (std::is_same_v<T, fp::Expr::AND>) op = "&&";
            else if constexpr (std::is_same_v<T, fp::Expr::OR>) op = "||";
            else if constexpr (std::is_same_v<T, fp::Expr::EQV>) op = "==";
            else if constexpr (std::is_same_v<T, fp::Expr::NEQV>) op = "!=";

            json.beginObject();
            json.key("kind"); json.stringValue("binary");
            json.key("op"); json.stringValue(op);
            json.key("left"); emitBodyExpr(json, std::get<0>(alt.t).value());
            json.key("right"); emitBodyExpr(json, std::get<1>(alt.t).value());
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::Expr::NOT>) {
            json.beginObject();
            json.key("kind"); json.stringValue("unary");
            json.key("op"); json.stringValue("!");
            json.key("expr"); emitBodyExpr(json, alt.v.value());
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::LiteralConstant>) {
            emitLiteralConstant(json, alt);
            return true;
        }

        return false;
    }, e.u);

    if (emitted) return;

    // Anything else we don't decompose (array/structure constructors,
    // %LOC, defined operators, complex literals, substring inquiries, ...)
    // becomes an opaque "unsupported" leaf carrying the source text, so the
    // Python side can raise a clear error rather than silently misreading
    // it as a value.
    json.beginObject();
    json.key("kind"); json.stringValue("unsupported");
    json.key("tag"); json.stringValue("expr");
    json.key("source"); json.stringValue(sourceText(e.source));
    json.endObject();
}

// =============================================================================
// NameCollector / LocalsCollector: local array declaration walker
// =============================================================================
//
// Stage 2's "runtime dimension local arrays" check flags local arrays whose
// declared bounds reference a kernel parameter or an OP2 const (both
// runtime values - a red flag for stack-allocated arrays, especially on a
// GPU). For every locally-declared array we collect the lower-cased name of
// every identifier referenced anywhere in its shape-spec bound expressions;
// Python cross-references that against the const/parameter list.
// =============================================================================
struct NameCollector {
    std::vector<std::string> &out;
    bool Pre(const fp::Name &n) { out.push_back(toLower(n.ToString())); return true; }
    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

struct LocalsCollector {
    Json &json;   // emits directly into an open array of {"name", "dims"} objects

    static const fp::ArraySpec *findArraySpecAttr(const std::list<fp::AttrSpec> &attrs) {
        for (const auto &attr : attrs) {
            if (const auto *spec = std::get_if<fp::ArraySpec>(&attr.u)) return spec;
        }
        return nullptr;
    }

    static std::vector<std::string> collectShapeDimNames(const fp::ArraySpec *spec) {
        std::vector<std::string> names;
        if (!spec) return names;
        if (const auto *shapes = std::get_if<std::list<fp::ExplicitShapeSpec>>(&spec->u)) {
            NameCollector nc{names};
            for (const auto &shape : *shapes) fp::Walk(shape, nc);
        }
        return names;
    }

    // Pre(TypeDeclarationStmt): `TYPE, attrs :: entity-decl-list`. An
    // entity's array-ness/shape can come either from its own `name(spec)`
    // suffix or from a shared `dimension(spec)` attribute applying to the
    // whole entity-decl-list; we check both, matching fparser2's fallback.
    bool Pre(const fp::TypeDeclarationStmt &decl) {
        const auto &attrs = std::get<std::list<fp::AttrSpec>>(decl.t);
        const fp::ArraySpec *attrArraySpec = findArraySpecAttr(attrs);

        const auto &entityDecls = std::get<std::list<fp::EntityDecl>>(decl.t);
        for (const auto &entityDecl : entityDecls) {
            const fp::Name &nameNode = std::get<fp::ObjectName>(entityDecl.t);
            const auto &ownSpec = std::get<std::optional<fp::ArraySpec>>(entityDecl.t);

            const fp::ArraySpec *spec = ownSpec ? &*ownSpec : attrArraySpec;
            if (!spec) continue;

            std::vector<std::string> dims = collectShapeDimNames(spec);

            json.beginObject();
            json.key("name"); json.stringValue(toLower(nameNode.ToString()));
            json.key("dims");
            json.beginArray();
            for (const auto &d : dims) json.stringValue(d);
            json.endArray();
            json.endObject();
        }
        return true;
    }

    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

// =============================================================================
// BodyCollector: per-subprogram assignment/call walker (Stage 2 validation)
// =============================================================================
//
// Walks one subprogram's Execution_Part and records every assignment
// statement (lhs/rhs expr trees) and every direct subroutine call (`call
// foo(...)`, with its own arg expr trees). Like fparser2's flat `fpu.walk`,
// this deliberately ignores control-flow nesting (if/do/...) - none of the
// Stage 2 checks care which branch/loop a statement lives in, only that it
// exists somewhere in the body.
//
// Assignments and calls are written into two separate Json instances
// (rather than the shared per-file `json`) because a single tree walk
// interleaves the two statement kinds in source order, but the JSON
// contract wants them as two separate arrays; see Json::rawValue.
// =============================================================================
struct BodyCollector {
    Json &jsonAssignments;   // open array of {"line", "lhs", "rhs"}
    Json &jsonCalls;         // open array of {"line", "name", "args"}
    const fp::AllCookedSources &cooked;

    std::pair<int, int> resolveLineCol(fp::CharBlock src) const {
        if (src.empty()) return {0, 0};
        auto prov = cooked.GetProvenanceRange(src);
        if (!prov) return {0, 0};
        auto pos = cooked.allSources().GetSourcePosition(prov->start());
        if (pos) return {static_cast<int>(pos->line), static_cast<int>(pos->column)};
        return {0, 0};
    }

    bool Pre(const fp::AssignmentStmt &assign) {
        const auto &lhs = std::get<fp::Variable>(assign.t);
        const auto &rhs = std::get<fp::Expr>(assign.t);
        auto [line, col] = resolveLineCol(rhs.source);

        jsonAssignments.beginObject();
        jsonAssignments.key("line"); jsonAssignments.intValue(line);
        jsonAssignments.key("lhs"); emitVariable(jsonAssignments, lhs);
        jsonAssignments.key("rhs"); emitBodyExpr(jsonAssignments, rhs);
        jsonAssignments.endObject();
        return true;
    }

    bool Pre(const fp::CallStmt &call) {
        const fp::Call &c = std::get<fp::Call>(call.t);
        const fp::ProcedureDesignator &pd = std::get<fp::ProcedureDesignator>(c.t);
        const auto &args = std::get<std::list<fp::ActualArgSpec>>(c.t);

        std::string name;
        fp::CharBlock nameSrc;
        bool gotName = std::visit([&](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, fp::Name>) {
                name = toLower(alt.ToString());
                nameSrc = alt.source;
                return true;
            }
            return false;
        }, pd.u);

        if (!gotName) return true;

        auto [line, col] = resolveLineCol(nameSrc);

        jsonCalls.beginObject();
        jsonCalls.key("line"); jsonCalls.intValue(line);
        jsonCalls.key("name"); jsonCalls.stringValue(name);
        jsonCalls.key("args");
        jsonCalls.beginArray();
        for (const fp::ActualArgSpec &spec : args) {
            const fp::ActualArg &aa = std::get<fp::ActualArg>(spec.t);
            bool handled = std::visit([&](const auto &alt) -> bool {
                using T = std::decay_t<decltype(alt)>;
                if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Expr>>) {
                    emitBodyExpr(jsonCalls, alt.value());
                    return true;
                }
                return false;
            }, aa.u);
            if (!handled) {
                jsonCalls.beginObject();
                jsonCalls.key("kind"); jsonCalls.stringValue("raw");
                jsonCalls.key("source"); jsonCalls.stringValue("<unsupported-actual-arg>");
                jsonCalls.endObject();
            }
        }
        jsonCalls.endArray();
        jsonCalls.endObject();
        return true;
    }

    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

// =============================================================================
// Parse-tree "unwrap" helpers
// =============================================================================
//
// Flang wraps expressions in a chain of single-field "constraint" templates
// (Scalar<>, Integer<>, Logical<>, Constant<>, common::Indirection<>) that
// exist purely to document a grammar constraint (e.g. "this must be a
// scalar integer expression") and carry no data of their own beyond a
// `.thing` (or, for Indirection, a `.value()`) member wrapping the next
// layer. These helpers thread through one specific chain each so the
// declaration/statement emitters below can write `unwrapFoo(x)` instead of
// repeating `x.thing.thing.thing.value()` everywhere.
// =============================================================================
static const fp::Expr &unwrapScalarIntExpr(const fp::ScalarIntExpr &e) { return e.thing.thing.value(); }
static const fp::Expr &unwrapScalarLogicalExpr(const fp::ScalarLogicalExpr &e) { return e.thing.thing.value(); }
static const fp::Expr &unwrapScalarExpr(const fp::ScalarExpr &e) { return e.thing.value(); }
static const fp::Expr &unwrapConstantExpr(const fp::ConstantExpr &e) { return e.thing.value(); }
static const fp::Expr &unwrapScalarIntConstantExpr(const fp::ScalarIntConstantExpr &e) { return e.thing.thing.thing.value(); }
static const fp::Expr &unwrapSpecificationExpr(const fp::SpecificationExpr &e) { return unwrapScalarIntExpr(e.v); }

// =============================================================================
// Declaration serialization (for Stage 3 C++ code generation)
// =============================================================================
//
// fortran/flang_kernels_c.py needs the same information
// fortran/translator/kernels_c.py's `parseTypes` pulls out of an fparser2
// Specification_Part: for every declared name, its intrinsic type (with
// kind), whether it is an array (and if so its explicit-shape bounds), and
// whether it is a compile-time PARAMETER (and if so, its value).
//
// Node shapes (all objects have a "kind" field):
//   {"kind": "type_decl",
//    "type": <type>,
//    "is_parameter": bool,
//    "dim": <array-spec>|null,        (a shared `dimension(...)` attribute)
//    "entities": [{"name": str, "dim": <array-spec>|null, "init": expr|null}, ...]}
//   {"kind": "parameter_stmt", "defs": [{"name": str, "value": expr}, ...]}
//       The `PARAMETER(name = value, ...)` statement form (as opposed to
//       the `type, PARAMETER :: name = value` attribute form, which shows
//       up as a "type_decl" with is_parameter=true and a per-entity init).
//   {"kind": "data_stmt", "sets": [...]}  (see emitDataStmtNode below)
//
// <type> shapes:
//   {"kind": "intrinsic", "base": "integer"|"real"|"logical"|"character",
//    "kind_text": str|null, "charlen": expr|null}
//       "kind_text" is the raw kind-selector text (see kindParamToString);
//       "charlen" is only meaningful when base == "character".
//   {"kind": "unsupported"}
//       A derived type, CLASS(*), or other declaration-type-spec Stage 3
//       doesn't support.
//
// <array-spec> shapes:
//   {"kind": "explicit", "shape": [{"lb": expr|null, "ub": expr}, ...]}
//       Only explicit-shape (`(lb:ub)`) arrays are supported - the only
//       kind that makes sense for a value/kernel-parameter array with no
//       runtime shape information. lb is null when the spec omitted a
//       lower bound (implying a lower bound of 1).
//   {"kind": "unsupported"}
//       Assumed-shape/deferred-shape/assumed-size/assumed-rank - none of
//       which are legal for OP2 kernel parameters or locals anyway.
//
// Anything not recognised anywhere in this section (EXTERNAL statements,
// USE statements, IMPLICIT statements, ...) is simply not emitted at all,
// matching how the fparser2 path's `removeExternals` and
// `translateSpecificationPart`'s `Use_Stmt`/`Implicit_Part` handling both
// silently skip these constructs.
// =============================================================================

// R709 kind-param, as it appears on an intrinsic type spec (`REAL(8)`,
// `INTEGER(kind=IK)`, ...). Returns nullopt for the (rare) `KIND=*`
// assumed-size-character-style StarSize form, which Stage 3 doesn't need.
static std::optional<std::string> kindSelectorText(const std::optional<fp::KindSelector> &ks) {
    if (!ks) return std::nullopt;
    if (const auto *sice = std::get_if<fp::ScalarIntConstantExpr>(&ks->u)) {
        return sourceText(unwrapScalarIntConstantExpr(*sice).source);
    }
    return std::nullopt;
}

// R721 char-selector's length: either a plain expression (`(5)`, `(len=n)`)
// or the legacy `*5` numeric form; emits an expr-shaped node either way.
static void emitTypeParamValue(Json &json, const fp::TypeParamValue &tpv) {
    if (const auto *sie = std::get_if<fp::ScalarIntExpr>(&tpv.u)) {
        emitBodyExpr(json, unwrapScalarIntExpr(*sie));
        return;
    }
    // Star (assumed length, dummy args only) or Deferred (allocatable/
    // pointer character) - neither is legal for an OP2 kernel local/param.
    json.beginObject();
    json.key("kind"); json.stringValue("unsupported");
    json.key("tag"); json.stringValue("char_length");
    json.endObject();
}

static void emitCharLength(Json &json, const fp::CharLength &cl) {
    if (const auto *tpv = std::get_if<fp::TypeParamValue>(&cl.u)) {
        emitTypeParamValue(json, *tpv);
        return;
    }
    json.beginObject();
    json.key("kind"); json.stringValue("int_lit");
    json.key("text"); json.stringValue(std::to_string(std::get<std::uint64_t>(cl.u)));
    json.key("kind_text"); json.nullValue();
    json.endObject();
}

// R721 char-selector, in full: either a bare length-selector or the
// `(LEN=..., KIND=...)` form (whose kind we ignore - Stage 3 only supports
// default-kind CHARACTER, same as the fparser2 path).
static void emitCharLen(Json &json, const std::optional<fp::CharSelector> &cs) {
    if (!cs) { json.nullValue(); return; }

    std::visit([&](const auto &alt) {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::LengthSelector>) {
            std::visit([&](const auto &inner) {
                using U = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<U, fp::TypeParamValue>) {
                    emitTypeParamValue(json, inner);
                } else {
                    emitCharLength(json, inner);
                }
            }, alt.u);
        } else {
            // LengthAndKind: tuple<optional<TypeParamValue>, ScalarIntConstantExpr>.
            const auto &lengthOpt = std::get<0>(alt.t);
            if (lengthOpt) emitTypeParamValue(json, *lengthOpt);
            else json.nullValue();
        }
    }, cs->u);
}

// R704 intrinsic-type-spec -> INTEGER|REAL|DOUBLE PRECISION|COMPLEX|
//                             CHARACTER|LOGICAL [selector]
static void emitIntrinsicType(Json &json, const fp::IntrinsicTypeSpec &its) {
    std::visit([&](const auto &alt) {
        using T = std::decay_t<decltype(alt)>;
        json.beginObject();
        if constexpr (std::is_same_v<T, fp::IntegerTypeSpec> ||
                      std::is_same_v<T, fp::IntrinsicTypeSpec::Real> ||
                      std::is_same_v<T, fp::IntrinsicTypeSpec::Logical>) {
            json.key("kind"); json.stringValue("intrinsic");
            json.key("base"); json.stringValue(
                std::is_same_v<T, fp::IntegerTypeSpec> ? "integer" :
                std::is_same_v<T, fp::IntrinsicTypeSpec::Real> ? "real" : "logical");
            auto kt = kindSelectorText(alt.v);
            json.key("kind_text"); if (kt) json.stringValue(*kt); else json.nullValue();
            json.key("charlen"); json.nullValue();
        } else if constexpr (std::is_same_v<T, fp::IntrinsicTypeSpec::Character>) {
            json.key("kind"); json.stringValue("intrinsic");
            json.key("base"); json.stringValue("character");
            json.key("kind_text"); json.nullValue();
            json.key("charlen"); emitCharLen(json, alt.v);
        } else {
            // UnsignedTypeSpec, DoublePrecision, Complex, DoubleComplex -
            // none of these appear in real OP2 kernels.
            json.key("kind"); json.stringValue("unsupported");
        }
        json.endObject();
    }, its.u);
}

// R801 declaration-type-spec -> intrinsic-type-spec | TYPE(...) | CLASS(...) | ...
static void emitDeclType(Json &json, const fp::DeclarationTypeSpec &dts) {
    if (const auto *its = std::get_if<fp::IntrinsicTypeSpec>(&dts.u)) {
        emitIntrinsicType(json, *its);
        return;
    }
    json.beginObject();
    json.key("kind"); json.stringValue("unsupported");
    json.endObject();
}

// R816/R820 array-spec, restricted to the explicit-shape-spec-list case
// (the only one that makes sense for a kernel parameter/local).
static void emitArraySpec(Json &json, const fp::ArraySpec &spec) {
    const auto *shapes = std::get_if<std::list<fp::ExplicitShapeSpec>>(&spec.u);
    if (!shapes) {
        json.beginObject();
        json.key("kind"); json.stringValue("unsupported");
        json.endObject();
        return;
    }

    json.beginObject();
    json.key("kind"); json.stringValue("explicit");
    json.key("shape");
    json.beginArray();
    for (const fp::ExplicitShapeSpec &dim : *shapes) {
        const auto &lbOpt = std::get<0>(dim.t);
        const auto &ub = std::get<1>(dim.t);

        json.beginObject();
        json.key("lb");
        if (lbOpt) emitBodyExpr(json, unwrapSpecificationExpr(*lbOpt)); else json.nullValue();
        json.key("ub"); emitBodyExpr(json, unwrapSpecificationExpr(ub));
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

// An entity's own `= value` initializer (only meaningful when the
// enclosing type-decl is PARAMETER; translateSpecificationPart in
// fortran/flang_kernels_c.py errors out if a non-constant-expr
// initialization shows up on a PARAMETER entity).
static void emitInitialization(Json &json, const std::optional<fp::Initialization> &init) {
    if (!init) { json.nullValue(); return; }
    if (const auto *ce = std::get_if<fp::ConstantExpr>(&init->u)) {
        emitBodyExpr(json, unwrapConstantExpr(*ce));
        return;
    }
    // NullInit, InitialDataTarget, or the legacy `/values/` DATA-like form -
    // none of these are legal on a PARAMETER entity anyway.
    json.beginObject();
    json.key("kind"); json.stringValue("unsupported");
    json.key("tag"); json.stringValue("initialization");
    json.endObject();
}

static bool hasParameterAttr(const std::list<fp::AttrSpec> &attrs) {
    for (const fp::AttrSpec &attr : attrs) {
        if (std::holds_alternative<fp::Parameter>(attr.u)) return true;
    }
    return false;
}

// A DataStmtConstant (R841) is like a LiteralConstant but also allows the
// signed-literal forms (used only inside DATA statements and complex
// literal real/imaginary parts) and a bare named-constant reference.
static void emitDataStmtConstant(Json &json, const fp::DataStmtConstant &dc) {
    bool emitted = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::LiteralConstant>) {
            emitLiteralConstant(json, alt);
            return true;
        } else if constexpr (std::is_same_v<T, fp::SignedIntLiteralConstant>) {
            const auto &cb = std::get<fp::CharBlock>(alt.t);
            const auto &kindOpt = std::get<std::optional<fp::KindParam>>(alt.t);
            json.beginObject();
            json.key("kind"); json.stringValue("int_lit");
            json.key("text"); json.stringValue(sourceText(cb));
            json.key("kind_text");
            if (kindOpt) json.stringValue(kindParamToString(*kindOpt)); else json.nullValue();
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, fp::SignedRealLiteralConstant>) {
            const auto &signOpt = std::get<0>(alt.t);
            const auto &real = std::get<fp::RealLiteralConstant>(alt.t);
            const auto &realCore = std::get<fp::RealLiteralConstant::Real>(real.t);
            const auto &kindOpt = std::get<std::optional<fp::KindParam>>(real.t);
            std::string text = sourceText(realCore.source);
            if (signOpt && *signOpt == fp::Sign::Negative) text = "-" + text;
            json.beginObject();
            json.key("kind"); json.stringValue("real_lit");
            json.key("text"); json.stringValue(text);
            json.key("kind_text");
            if (kindOpt) json.stringValue(kindParamToString(*kindOpt)); else json.nullValue();
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Designator>>) {
            emitDesignator(json, alt.value());
            return true;
        }
        return false;
    }, dc.u);

    if (!emitted) {
        json.beginObject();
        json.key("kind"); json.stringValue("unsupported");
        json.key("tag"); json.stringValue("data_stmt_constant");
        json.endObject();
    }
}

// R837/R838 data-stmt -> DATA data-stmt-set [[,] data-stmt-set]...
//           data-stmt-set -> data-stmt-object-list / data-stmt-value-list /
static void emitDataStmtNode(Json &json, const fp::DataStmt &dstmt) {
    json.beginObject();
    json.key("kind"); json.stringValue("data_stmt");
    json.key("sets");
    json.beginArray();
    for (const fp::DataStmtSet &set : dstmt.v) {
        const auto &objects = std::get<std::list<fp::DataStmtObject>>(set.t);
        const auto &values = std::get<std::list<fp::DataStmtValue>>(set.t);

        json.beginObject();

        json.key("objects");
        json.beginArray();
        for (const fp::DataStmtObject &obj : objects) {
            std::visit([&](const auto &alt) {
                using T = std::decay_t<decltype(alt)>;
                if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Variable>>) {
                    emitVariable(json, alt.value());
                } else {
                    // DataImpliedDo - not used by OP2 kernels.
                    json.beginObject();
                    json.key("kind"); json.stringValue("unsupported");
                    json.key("tag"); json.stringValue("data_implied_do");
                    json.endObject();
                }
            }, obj.u);
        }
        json.endArray();

        json.key("values");
        json.beginArray();
        for (const fp::DataStmtValue &val : values) {
            const auto &repeatOpt = std::get<std::optional<fp::DataStmtRepeat>>(val.t);
            const auto &constant = std::get<fp::DataStmtConstant>(val.t);

            json.beginObject();
            json.key("repeated"); json.boolValue(repeatOpt.has_value());
            json.key("value"); emitDataStmtConstant(json, constant);
            json.endObject();
        }
        json.endArray();

        json.endObject();
    }
    json.endArray();
    json.endObject();
}

// DeclCollector: walks one subprogram's Specification_Part (via fp::Walk,
// so it doesn't matter whether a given statement landed in the
// grammar's Implicit_Part or its Declaration_Construct list - both are
// visited in source order) and appends one JSON node per declaration
// construct it understands into the open `decls` array. Anything it
// doesn't recognise (USE, IMPLICIT, EXTERNAL, ...) is simply never
// visited by any of the Pre() overloads below and so contributes nothing,
// which is exactly the "silently skip" behaviour the fparser2 path needs
// (see removeExternals/translateSpecificationPart).
struct DeclCollector {
    Json &json;

    static const fp::ArraySpec *findArraySpecAttr(const std::list<fp::AttrSpec> &attrs) {
        for (const fp::AttrSpec &attr : attrs) {
            if (const auto *spec = std::get_if<fp::ArraySpec>(&attr.u)) return spec;
        }
        return nullptr;
    }

    bool Pre(const fp::TypeDeclarationStmt &decl) {
        const auto &declTypeSpec = std::get<fp::DeclarationTypeSpec>(decl.t);
        const auto &attrs = std::get<std::list<fp::AttrSpec>>(decl.t);
        const auto &entityDecls = std::get<std::list<fp::EntityDecl>>(decl.t);

        const fp::ArraySpec *attrArraySpec = findArraySpecAttr(attrs);

        json.beginObject();
        json.key("kind"); json.stringValue("type_decl");
        json.key("type"); emitDeclType(json, declTypeSpec);
        json.key("is_parameter"); json.boolValue(hasParameterAttr(attrs));
        json.key("dim");
        if (attrArraySpec) emitArraySpec(json, *attrArraySpec); else json.nullValue();

        json.key("entities");
        json.beginArray();
        for (const fp::EntityDecl &ed : entityDecls) {
            const fp::Name &nameNode = std::get<fp::ObjectName>(ed.t);
            const auto &ownSpec = std::get<std::optional<fp::ArraySpec>>(ed.t);
            const auto &init = std::get<std::optional<fp::Initialization>>(ed.t);

            json.beginObject();
            json.key("name"); json.stringValue(toLower(nameNode.ToString()));
            json.key("dim");
            if (ownSpec) emitArraySpec(json, *ownSpec); else json.nullValue();
            json.key("init"); emitInitialization(json, init);
            json.endObject();
        }
        json.endArray();

        json.endObject();
        return true;
    }

    bool Pre(const fp::ParameterStmt &pstmt) {
        json.beginObject();
        json.key("kind"); json.stringValue("parameter_stmt");
        json.key("defs");
        json.beginArray();
        for (const fp::NamedConstantDef &def : pstmt.v) {
            const fp::NamedConstant &nc = std::get<fp::NamedConstant>(def.t);
            const fp::ConstantExpr &ce = std::get<fp::ConstantExpr>(def.t);

            json.beginObject();
            json.key("name"); json.stringValue(toLower(nc.v.ToString()));
            json.key("value"); emitBodyExpr(json, unwrapConstantExpr(ce));
            json.endObject();
        }
        json.endArray();
        json.endObject();
        return true;
    }

    bool Pre(const fp::DataStmt &dstmt) {
        emitDataStmtNode(json, dstmt);
        return true;
    }

    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

// =============================================================================
// Statement-tree serialization (for Stage 3 C++ code generation)
// =============================================================================
//
// Unlike BodyCollector (which flattens the body for Stage 2's "does this
// exist anywhere" checks), Stage 3 code generation needs control-flow
// structure preserved - an `if`/`do` body must nest inside its construct,
// not sit next to it in a flat list. This is a straightforward hand-written
// recursive descent over Block (= list<ExecutionPartConstruct>), mirroring
// fortran/translator/kernels_c.py's translateExecutionPart /
// translateIfConstruct / translateBlockNonlabelDoConstruct - just building
// a JSON tree instead of a C++ source string.
//
// Node shapes (all objects have a "kind" field):
//   {"kind": "assign", "line": int, "lhs": <variable-expr>, "rhs": expr}
//   {"kind": "call", "line": int, "name": str, "args": [expr, ...]}
//   {"kind": "continue"}
//   {"kind": "return"}
//   {"kind": "stop"}
//   {"kind": "write"}
//       Always translated as a no-op/comment, matching translateWriteStmt;
//       we don't bother capturing the write's format/arguments.
//   {"kind": "if_stmt", "cond": expr, "stmt": <single nested stmt>}
//       The single-line `IF (cond) stmt` form (R1139).
//   {"kind": "if_construct",
//    "branches": [{"cond": expr|null, "body": [stmt, ...]}, ...]}
//       One entry per THEN/ELSE IF/ELSE block, in source order; "cond" is
//       null for a trailing ELSE (there is at most one, always last).
//   {"kind": "do", "mode": "counted"|"while"|"unsupported",
//    "var": str, "lb": expr, "ub": expr, "step": expr|null,   (counted)
//    "cond": expr,                                            (while)
//    "body": [stmt, ...]}
//       "counted" is `DO i = lb, ub[, step]`; "while" is `DO WHILE (cond)`;
//       "unsupported" covers `DO CONCURRENT` and the bare infinite `DO`
//       (neither of which fortran/translator/kernels_c.py's
//       translateBlockNonlabelDoConstruct supports either).
//   {"kind": "data_stmt", "sets": [...]}  (see emitDataStmtNode)
//   {"kind": "unsupported", "tag": "<node-name>"}
//       Anything else (ALLOCATE, GOTO, SELECT CASE, WHERE, labelled DO,
//       ...) - matches the (mostly commented-out) gaps in
//       fortran/translator/kernels_c.py's TRANSLATE_TABLE.
// =============================================================================

static void emitBlock(Json &json, const fp::Block &block, const fp::AllCookedSources &cooked);

static std::pair<int, int> resolveLineColStmt(const fp::AllCookedSources &cooked, fp::CharBlock src) {
    if (src.empty()) return {0, 0};
    auto prov = cooked.GetProvenanceRange(src);
    if (!prov) return {0, 0};
    auto pos = cooked.allSources().GetSourcePosition(prov->start());
    if (pos) return {static_cast<int>(pos->line), static_cast<int>(pos->column)};
    return {0, 0};
}

// R1521 call-stmt, shared between statement-tree and (formerly) BodyCollector
// use; unlike BodyCollector's copy this one is the canonical statement-tree
// shape ("call" as a top-level statement kind, not nested under "kind":
// "call" inside an object with a separate "line").
static void emitCallStmtNode(Json &json, const fp::CallStmt &call, const fp::AllCookedSources &cooked) {
    const fp::Call &c = std::get<fp::Call>(call.t);
    const fp::ProcedureDesignator &pd = std::get<fp::ProcedureDesignator>(c.t);
    const auto &args = std::get<std::list<fp::ActualArgSpec>>(c.t);

    std::string name;
    fp::CharBlock nameSrc;
    bool gotName = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::Name>) {
            name = toLower(alt.ToString());
            nameSrc = alt.source;
            return true;
        }
        return false;
    }, pd.u);

    if (!gotName) {
        json.beginObject();
        json.key("kind"); json.stringValue("unsupported");
        json.key("tag"); json.stringValue("call_stmt");
        json.endObject();
        return;
    }

    auto [line, col] = resolveLineColStmt(cooked, nameSrc);

    json.beginObject();
    json.key("kind"); json.stringValue("call");
    json.key("line"); json.intValue(line);
    json.key("name"); json.stringValue(name);
    json.key("args");
    json.beginArray();
    for (const fp::ActualArgSpec &spec : args) {
        const fp::ActualArg &aa = std::get<fp::ActualArg>(spec.t);
        bool handled = std::visit([&](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::Expr>>) {
                emitBodyExpr(json, alt.value());
                return true;
            }
            return false;
        }, aa.u);
        if (!handled) {
            json.beginObject();
            json.key("kind"); json.stringValue("unsupported");
            json.key("tag"); json.stringValue("actual_arg");
            json.endObject();
        }
    }
    json.endArray();
    json.endObject();
}

// R515 action-stmt. Covers every statement kind that can appear either as
// its own line in a Block, or as the single trailing statement of a
// single-line IF.
static void emitActionStmt(Json &json, const fp::ActionStmt &a, const fp::AllCookedSources &cooked) {
    bool emitted = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;

        if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::AssignmentStmt>>) {
            const fp::AssignmentStmt &as = alt.value();
            const auto &lhs = std::get<fp::Variable>(as.t);
            const auto &rhs = std::get<fp::Expr>(as.t);
            auto [line, col] = resolveLineColStmt(cooked, rhs.source);

            json.beginObject();
            json.key("kind"); json.stringValue("assign");
            json.key("line"); json.intValue(line);
            json.key("lhs"); emitVariable(json, lhs);
            json.key("rhs"); emitBodyExpr(json, rhs);
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::CallStmt>>) {
            emitCallStmtNode(json, alt.value(), cooked);
            return true;
        } else if constexpr (std::is_same_v<T, fp::ContinueStmt>) {
            json.beginObject();
            json.key("kind"); json.stringValue("continue");
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::IfStmt>>) {
            const fp::IfStmt &ifs = alt.value();
            const auto &cond = std::get<fp::ScalarLogicalExpr>(ifs.t);
            const auto &inner = std::get<fp::UnlabeledStatement<fp::ActionStmt>>(ifs.t);

            json.beginObject();
            json.key("kind"); json.stringValue("if_stmt");
            json.key("cond"); emitBodyExpr(json, unwrapScalarLogicalExpr(cond));
            json.key("stmt"); emitActionStmt(json, inner.statement, cooked);
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::ReturnStmt>>) {
            json.beginObject();
            json.key("kind"); json.stringValue("return");
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::StopStmt>>) {
            json.beginObject();
            json.key("kind"); json.stringValue("stop");
            json.endObject();
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::WriteStmt>>) {
            json.beginObject();
            json.key("kind"); json.stringValue("write");
            json.endObject();
            return true;
        }

        return false;
    }, a.u);

    if (!emitted) {
        json.beginObject();
        json.key("kind"); json.stringValue("unsupported");
        json.key("tag"); json.stringValue("action_stmt");
        json.endObject();
    }
}

// R1134 if-construct -> if-then-stmt block [else-if-stmt block]...
//                       [else-stmt block] end-if-stmt
static void emitIfConstruct(Json &json, const fp::IfConstruct &ifc, const fp::AllCookedSources &cooked) {
    const auto &ifThen = std::get<fp::Statement<fp::IfThenStmt>>(ifc.t);
    const auto &thenBlock = std::get<fp::Block>(ifc.t);
    const auto &elseIfBlocks = std::get<std::list<fp::IfConstruct::ElseIfBlock>>(ifc.t);
    const auto &elseBlockOpt = std::get<std::optional<fp::IfConstruct::ElseBlock>>(ifc.t);

    json.beginObject();
    json.key("kind"); json.stringValue("if_construct");
    json.key("branches");
    json.beginArray();

    json.beginObject();
    json.key("cond");
    emitBodyExpr(json, unwrapScalarLogicalExpr(std::get<fp::ScalarLogicalExpr>(ifThen.statement.t)));
    json.key("body"); emitBlock(json, thenBlock, cooked);
    json.endObject();

    for (const fp::IfConstruct::ElseIfBlock &eib : elseIfBlocks) {
        const auto &stmt = std::get<fp::Statement<fp::ElseIfStmt>>(eib.t);
        const auto &blk = std::get<fp::Block>(eib.t);

        json.beginObject();
        json.key("cond");
        emitBodyExpr(json, unwrapScalarLogicalExpr(std::get<fp::ScalarLogicalExpr>(stmt.statement.t)));
        json.key("body"); emitBlock(json, blk, cooked);
        json.endObject();
    }

    if (elseBlockOpt) {
        const auto &blk = std::get<fp::Block>(elseBlockOpt->t);

        json.beginObject();
        json.key("cond"); json.nullValue();
        json.key("body"); emitBlock(json, blk, cooked);
        json.endObject();
    }

    json.endArray();
    json.endObject();
}

// R1119 do-construct -> nonlabel-do-stmt block end-do-stmt (labelled
// label-do-stmt loops are deliberately left unsupported, same as
// fortran/translator/kernels_c.py's `ctx.error("Unsupported labelled do
// construct")`).
static void emitDoConstruct(Json &json, const fp::DoConstruct &dc, const fp::AllCookedSources &cooked) {
    const auto &doStmt = std::get<fp::Statement<fp::NonLabelDoStmt>>(dc.t);
    const auto &block = std::get<fp::Block>(dc.t);
    const auto &loopControlOpt = std::get<std::optional<fp::LoopControl>>(doStmt.statement.t);

    json.beginObject();
    json.key("kind"); json.stringValue("do");

    bool handled = false;
    if (loopControlOpt) {
        handled = std::visit([&](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, fp::LoopControl::Bounds>) {
                json.key("mode"); json.stringValue("counted");
                json.key("var"); json.stringValue(toLower(alt.Name().thing.ToString()));
                json.key("lb"); emitBodyExpr(json, unwrapScalarExpr(alt.Lower()));
                json.key("ub"); emitBodyExpr(json, unwrapScalarExpr(alt.Upper()));
                json.key("step");
                if (alt.Step()) emitBodyExpr(json, unwrapScalarExpr(*alt.Step())); else json.nullValue();
                return true;
            } else if constexpr (std::is_same_v<T, fp::ScalarLogicalExpr>) {
                json.key("mode"); json.stringValue("while");
                json.key("cond"); emitBodyExpr(json, unwrapScalarLogicalExpr(alt));
                return true;
            }
            return false; // Concurrent (DO CONCURRENT)
        }, loopControlOpt->u);
    }

    if (!handled) {
        json.key("mode"); json.stringValue("unsupported");
    }

    json.key("body"); emitBlock(json, block, cooked);
    json.endObject();
}

// R510 execution-part-construct -> executable-construct | format-stmt |
//                                  entry-stmt | data-stmt | namelist-stmt
static void emitExecutionPartConstruct(Json &json, const fp::ExecutionPartConstruct &epc, const fp::AllCookedSources &cooked);

// R514 executable-construct -> action-stmt | ... | do-construct |
//                               if-construct | ...
static void emitExecutableConstruct(Json &json, const fp::ExecutableConstruct &ec, const fp::AllCookedSources &cooked) {
    bool emitted = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::Statement<fp::ActionStmt>>) {
            emitActionStmt(json, alt.statement, cooked);
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::IfConstruct>>) {
            emitIfConstruct(json, alt.value(), cooked);
            return true;
        } else if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::DoConstruct>>) {
            emitDoConstruct(json, alt.value(), cooked);
            return true;
        }
        return false;
    }, ec.u);

    if (!emitted) {
        json.beginObject();
        json.key("kind"); json.stringValue("unsupported");
        json.key("tag"); json.stringValue("executable_construct");
        json.endObject();
    }
}

static void emitExecutionPartConstruct(Json &json, const fp::ExecutionPartConstruct &epc, const fp::AllCookedSources &cooked) {
    bool emitted = std::visit([&](const auto &alt) -> bool {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::ExecutableConstruct>) {
            emitExecutableConstruct(json, alt, cooked);
            return true;
        } else if constexpr (std::is_same_v<T, fp::Statement<Fortran::common::Indirection<fp::DataStmt>>>) {
            emitDataStmtNode(json, alt.statement.value());
            return true;
        }
        return false;
    }, epc.u);

    if (!emitted) {
        json.beginObject();
        json.key("kind"); json.stringValue("unsupported");
        json.key("tag"); json.stringValue("execution_part_construct");
        json.endObject();
    }
}

static void emitBlock(Json &json, const fp::Block &block, const fp::AllCookedSources &cooked) {
    json.beginArray();
    for (const fp::ExecutionPartConstruct &epc : block) emitExecutionPartConstruct(json, epc, cooked);
    json.endArray();
}

// =============================================================================
// Scanner: top-level parse-tree visitor
// =============================================================================
//
// One Scanner instance is created per file and handed to Flang's Walk().
// Walk() invokes the appropriate Pre()/Post() overload for every parse-tree
// node it visits; the templated fallbacks at the bottom of the struct make
// sure unrecognised node types are silently traversed.
//
// Each successful Pre() emits zero or one JSON event into the open `events`
// array (see main()). The walk continues into the subtree (returning true)
// in every case so that, for example, `op_par_loop` calls inside a
// subroutine body are still discovered.
//
// The events emitted here are:
//
//   * Whenever a CallStmt callee matches `op_par_loop_<N>` -> "op_par_loop_N"
//     event with its full argument tree.
//   * Whenever a CallStmt callee matches `op_decl_const`   -> "op_decl_const"
//     event with its full argument tree.
//   * For every SubroutineSubprogram                       -> "subroutine_subprogram"
//     event with name, parameters, depends, and source body text.
//   * For every FunctionSubprogram                         -> "function_subprogram"
//     event (same shape as subroutine).
//
// All of these go into a single ordered events array; the Python side
// dispatches on the "kind" field.
// =============================================================================
struct Scanner {
    Json &json;                            // open events array we append into
    const fp::AllCookedSources &cooked;    // for mapping CharBlocks -> line/col

    // Pre(CallStmt): triggered for every `call ...(...)` statement. We only
    // care about two callee identifiers; everything else is ignored and the
    // walk continues (so we still find op_par_loop calls deeper in the tree).
    bool Pre(const fp::CallStmt &call) {
        // CallStmt's shape has drifted across LLVM releases:
        //   LLVM ~17: WRAPPER_CLASS_BOILERPLATE(CallStmt, Call) -> call.v
        //   LLVM ~18: struct with `Call call; optional<Chevrons> chevrons;`
        //   LLVM ~19+: TUPLE_CLASS_BOILERPLATE with
        //              std::tuple<Call, std::optional<Chevrons>> t
        // This code targets the LLVM 19+ layout (including current main).
        const fp::Call &c = std::get<fp::Call>(call.t);
        const fp::ProcedureDesignator &pd = std::get<fp::ProcedureDesignator>(c.t);
        const auto &actualArgs = std::get<std::list<fp::ActualArgSpec>>(c.t);

        // ProcedureDesignator can be Name | ProcComponentRef | ProcedureName.
        // We only emit events for plain-Name callees.
        std::string name;
        fp::CharBlock nameSrc;
        bool gotName = std::visit([&](const auto &alt) -> bool {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, fp::Name>) {
                name = toLower(alt.ToString());
                nameSrc = alt.source;
                return true;
            } else {
                return false;
            }
        }, pd.u);

        if (!gotName) return true;

        // Filter for the callees we actually care about.
        static const std::regex parLoopRe{"^op_par_loop_[0-9]+$"};
        const bool isParLoop = std::regex_match(name, parLoopRe);
        const bool isDeclConst = (name == "op_decl_const");
        if (!isParLoop && !isDeclConst) return true;

        auto [line, col] = resolveLineCol(nameSrc);

        if (isParLoop) {
            emitLoop(name, line, col, actualArgs);
        } else {
            emitConst(line, col, actualArgs);
        }
        return true;
    }

    // Map a CharBlock from the cooked source stream back to a (line, column)
    // in the *original* source file via Flang's provenance machinery. Returns
    // (0, 0) on failure - we never want to throw out of a visitor.
    std::pair<int, int> resolveLineCol(fp::CharBlock src) {
        if (src.empty()) return {0, 0};
        auto prov = cooked.GetProvenanceRange(src);
        if (!prov) return {0, 0};
        auto pos = cooked.allSources().GetSourcePosition(prov->start());
        if (pos) {
            return {static_cast<int>(pos->line), static_cast<int>(pos->column)};
        }
        return {0, 0};
    }

    // -- Per-event emitters ---------------------------------------------------
    //
    // Each helper below opens a new object inside the open `events` array and
    // closes it before returning. They are intentionally small and similar:
    // the JSON contract lives in the comments at the top of the file.

    void emitLoop(const std::string &name, int line, int col,
                  const std::list<fp::ActualArgSpec> &args) {
        json.beginObject();
        json.key("kind"); json.stringValue(name);   // op_par_loop_<N>
        json.key("location");
        {
            json.beginObject();
            json.key("line"); json.intValue(line);
            json.key("column"); json.intValue(col);
            json.endObject();
        }
        json.key("args");
        emitActualArgs(json, args);
        json.endObject();
    }

    void emitConst(int line, int col, const std::list<fp::ActualArgSpec> &args) {
        json.beginObject();
        json.key("kind"); json.stringValue("op_decl_const");
        json.key("location");
        {
            json.beginObject();
            json.key("line"); json.intValue(line);
            json.key("column"); json.intValue(col);
            json.endObject();
        }
        json.key("args");
        emitActualArgs(json, args);
        json.endObject();
    }

    // -------------------------------------------------------------------------
    // Subprogram events
    //
    // For every subroutine/function definition we emit the metadata the
    // existing fortran/parser.py exposes via its fparser2 walk: name,
    // parameter list, dependency call/ref names, and a textual representation
    // of the subprogram body.
    //
    // The body text is sliced from Flang's cooked-source stream rather than
    // re-pretty-printed via Unparse(). The cooked stream gives us a free-form,
    // lowercased, includes-expanded, comments-stripped rendering already, and
    // CharBlock-based slicing avoids a runtime dependency on Unparse's
    // template instantiation set (which has changed shape across LLVM
    // releases).
    //
    // The Python flang_writer module receives this text and applies its
    // text-level rewrites (rename_consts, fix_hydra_io, etc.) directly to it.
    // -------------------------------------------------------------------------

    // Build a CharBlock spanning two cooked-source ranges that belong to the
    // same parse-tree subprogram. The cooked source for one translation unit
    // lives in a single contiguous CookedSource buffer, so subtracting the
    // start pointer of `a` from the one-past-end pointer of `b` is well
    // defined when both belong to that buffer.
    static fp::CharBlock spanningRange(fp::CharBlock a, fp::CharBlock b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        const char *start = a.begin();
        const char *end = b.begin() + b.size();
        if (end <= start) return a;
        return fp::CharBlock{start, static_cast<std::size_t>(end - start)};
    }

    // Pre(SubroutineSubprogram): one event per `subroutine ... end subroutine`
    // (top-level or nested).
    bool Pre(const fp::SubroutineSubprogram &sub) {
        // SubroutineSubprogram::t =
        //   tuple< Statement<SubroutineStmt>, SpecificationPart,
        //          ExecutionPart, optional<InternalSubprogramPart>,
        //          Statement<EndSubroutineStmt> >
        const auto &startStmt = std::get<fp::Statement<fp::SubroutineStmt>>(sub.t);
        const auto &endStmt = std::get<fp::Statement<fp::EndSubroutineStmt>>(sub.t);
        const fp::SubroutineStmt &subStmt = startStmt.statement;

        // SubroutineStmt::t =
        //   tuple< list<PrefixSpec>, Name, list<DummyArg>,
        //          optional<LanguageBindingSpec>, ... >
        const fp::Name &nameNode = std::get<fp::Name>(subStmt.t);
        std::string name = toLower(nameNode.ToString());
        auto [line, col] = resolveLineCol(nameNode.source);

        // Dummy arguments may be plain Names or alternate-return specs (`*`).
        // Only the plain Names map to "parameters" in our entity model.
        std::vector<std::string> parameters;
        const auto &dummyArgs = std::get<std::list<fp::DummyArg>>(subStmt.t);
        for (const auto &arg : dummyArgs) {
            std::visit([&](const auto &inner) {
                using T = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<T, fp::Name>) {
                    parameters.push_back(toLower(inner.ToString()));
                }
            }, arg.u);
        }

        // Walk this subprogram's subtree to collect candidate dependency
        // names. We then drop the subprogram's own name to avoid spurious
        // self-recursion edges in the Python entity graph.
        std::set<std::string> depends;
        DependsCollector dc{depends};
        fp::Walk(sub, dc);
        depends.erase(name);

        emitSubprogram("subroutine_subprogram", name, line, col,
                       parameters, depends,
                       spanningRange(startStmt.source, endStmt.source),
                       std::get<fp::SpecificationPart>(sub.t),
                       std::get<fp::ExecutionPart>(sub.t),
                       /*fnStmt=*/nullptr);
        return true;
    }

    // Pre(FunctionSubprogram): one event per `function ... end function`.
    // Same shape as subroutines, with the slight grammar difference that
    // function parameters are a list of plain Names rather than DummyArgs.
    bool Pre(const fp::FunctionSubprogram &fn) {
        // FunctionSubprogram::t =
        //   tuple< Statement<FunctionStmt>, SpecificationPart,
        //          ExecutionPart, optional<InternalSubprogramPart>,
        //          Statement<EndFunctionStmt> >
        const auto &startStmt = std::get<fp::Statement<fp::FunctionStmt>>(fn.t);
        const auto &endStmt = std::get<fp::Statement<fp::EndFunctionStmt>>(fn.t);
        const fp::FunctionStmt &fnStmt = startStmt.statement;

        // FunctionStmt::t =
        //   tuple< list<PrefixSpec>, Name, list<Name>, optional<Suffix> >
        const fp::Name &nameNode = std::get<fp::Name>(fnStmt.t);
        std::string name = toLower(nameNode.ToString());
        auto [line, col] = resolveLineCol(nameNode.source);

        std::vector<std::string> parameters;
        const auto &paramList = std::get<std::list<fp::Name>>(fnStmt.t);
        for (const auto &n : paramList) {
            parameters.push_back(toLower(n.ToString()));
        }

        std::set<std::string> depends;
        DependsCollector dc{depends};
        fp::Walk(fn, dc);
        depends.erase(name);

        emitSubprogram("function_subprogram", name, line, col,
                       parameters, depends,
                       spanningRange(startStmt.source, endStmt.source),
                       std::get<fp::SpecificationPart>(fn.t),
                       std::get<fp::ExecutionPart>(fn.t),
                       &fnStmt);
        return true;
    }

    // Function_Stmt's optional RESULT(name) suffix - the variable that
    // holds the return value inside the body, which defaults to the
    // function's own name when no RESULT clause is present.
    static std::optional<std::string> resultName(const fp::FunctionStmt &fnStmt) {
        const auto &suffixOpt = std::get<std::optional<fp::Suffix>>(fnStmt.t);
        if (!suffixOpt) return std::nullopt;
        const auto &nameOpt = std::get<std::optional<fp::Name>>(suffixOpt->t);
        if (!nameOpt) return std::nullopt;
        return toLower(nameOpt->ToString());
    }

    // Function_Stmt's prefix (`REAL FUNCTION foo(...)`) return type, if any
    // was written there (as opposed to being declared on a local variable
    // matching the function/result name - the Python side falls back to
    // that when this is absent, mirroring
    // fortran/translator/kernels_c.py's parseFunctionTypeInfo).
    static void resultType(Json &json, const fp::FunctionStmt &fnStmt) {
        const auto &prefixes = std::get<std::list<fp::PrefixSpec>>(fnStmt.t);
        for (const fp::PrefixSpec &spec : prefixes) {
            if (const auto *dts = std::get_if<fp::DeclarationTypeSpec>(&spec.u)) {
                emitDeclType(json, *dts);
                return;
            }
        }
        json.nullValue();
    }

    // Shared writer for the two subprogram event shapes. `fnStmt` is
    // non-null only for function_subprogram events, and controls whether
    // the function-specific "result_name"/"result_type" keys are emitted.
    void emitSubprogram(const std::string &kind,
                        const std::string &name,
                        int line, int col,
                        const std::vector<std::string> &parameters,
                        const std::set<std::string> &depends,
                        fp::CharBlock bodyRange,
                        const fp::SpecificationPart &spec,
                        const fp::ExecutionPart &exec,
                        const fp::FunctionStmt *fnStmt) {
        json.beginObject();
        json.key("kind"); json.stringValue(kind);
        json.key("name"); json.stringValue(name);
        json.key("location");
        {
            json.beginObject();
            json.key("line"); json.intValue(line);
            json.key("column"); json.intValue(col);
            json.endObject();
        }
        json.key("parameters");
        {
            json.beginArray();
            for (const auto &p : parameters) json.stringValue(p);
            json.endArray();
        }
        json.key("depends");
        {
            json.beginArray();
            for (const auto &d : depends) json.stringValue(d);
            json.endArray();
        }
        json.key("source");
        if (bodyRange.size() > 0) {
            // CharBlock is a (char*, size_t) into Flang's owning buffer; we
            // copy it into a std::string for the JSON writer to escape.
            json.stringValue(std::string(bodyRange.begin(), bodyRange.size()));
        } else {
            json.stringValue("");
        }

        // Stage 2 validation data: local array declarations (for the
        // runtime-dimension check) and a flattened assignment/call walk of
        // the execution part (for read/inc/slice/const-write checks). See
        // the LocalsCollector / BodyCollector doc comments above.
        json.key("locals");
        {
            json.beginArray();
            LocalsCollector lc{json};
            fp::Walk(spec, lc);
            json.endArray();
        }

        Json assignmentsJson, callsJson;
        assignmentsJson.beginArray();
        callsJson.beginArray();
        BodyCollector bc{assignmentsJson, callsJson, cooked};
        fp::Walk(exec, bc);
        assignmentsJson.endArray();
        callsJson.endArray();

        json.key("assignments"); json.rawValue(assignmentsJson.str());
        json.key("calls"); json.rawValue(callsJson.str());

        // Stage 3 data: full typed declarations and a nested statement
        // tree (see the doc comments above DeclCollector / emitBlock).
        // These are additive to (and independent of) the Stage 2 fields
        // above - fortran/flang_kernels_c.py never reads "locals"/
        // "assignments"/"calls", and fortran/flang_validator.py never
        // reads "decls"/"stmts".
        json.key("decls");
        {
            json.beginArray();
            DeclCollector dc{json};
            fp::Walk(spec, dc);
            json.endArray();
        }

        json.key("stmts");
        emitBlock(json, exec.v, cooked);

        if (fnStmt != nullptr) {
            json.key("result_name");
            auto rn = resultName(*fnStmt);
            if (rn) json.stringValue(*rn); else json.nullValue();

            json.key("result_type");
            resultType(json, *fnStmt);
        }

        json.endObject();
    }

    // Default no-op fallbacks. Required by Flang's Walk() so that every
    // node in the parse tree has a matching Pre()/Post() pair regardless of
    // whether we actually care about it.
    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

// =============================================================================
// main: argument parsing, parse pipeline, JSON emission
// =============================================================================

// Read all of stdin into a string. Used when we're invoked with --stdin or
// without a path argument.
static std::string slurpStdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

// Write `contents` to a uniquely-named temp file and return its path. Flang's
// parser reads from a real file path rather than an in-memory buffer, so
// stdin-based invocations have to materialise the source on disk briefly.
// The temp file is deleted from main() before we return.
static std::string writeTempFile(const std::string &contents) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    auto path = dir / ("op2-flang-scan-" + std::to_string(::getpid()) + ".F90");
    std::ofstream ofs(path);
    ofs << contents;
    ofs.close();
    return path.string();
}

// Entry point. Steps performed:
//
//   1. Argument parsing.
//   2. Materialise the source on disk if it came in on stdin.
//   3. Configure Flang and run Prescan + Parse.
//   4. Bail out with a non-zero exit if Flang reported fatal parse errors.
//   5. Walk the parse tree with Scanner, accumulating JSON events.
//   6. Print the JSON document and clean up.
int main(int argc, char **argv) {
    // -- 1. Argument parsing --------------------------------------------------
    //
    // Recognised flags:
    //   --stdin            Force stdin mode even if a path is given.
    //   --path <reported>  Path string to put in the JSON "path" field
    //                      (handy when feeding stdin but reporting the
    //                      original source file name).
    //   <path>             Bare positional - source file to parse.
    //
    // Anything starting with a `-` we don't recognise is fatal (exit 2);
    // anything else is taken as the input path.
    std::string path;
    bool readStdin = false;
    std::string originalPath; // reported in JSON "path" field

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stdin") {
            readStdin = true;
        } else if (a == "--path" && i + 1 < argc) {
            originalPath = argv[++i];
        } else if (a.size() > 0 && a[0] != '-') {
            path = a;
        } else {
            std::cerr << "op2-flang-scan: unknown argument: " << a << "\n";
            return 2;
        }
    }

    // -- 2. Materialise stdin to a temp file if needed ------------------------
    std::string tempFile;
    if (readStdin || path.empty()) {
        std::string body = slurpStdin();
        tempFile = writeTempFile(body);
        path = tempFile;
    }
    if (originalPath.empty()) originalPath = path;

    // -- 3. Run Flang's parse pipeline ----------------------------------------
    //
    // The Python driver hands us source that has already been run through an
    // external preprocessor (pcpp/fpp) and a free-form converter, so there
    // are no live #-directives for Flang's prescanner to process. We still
    // need Flang's prescanner to do the Fortran-specific work (line
    // continuations, fixed/free form selection, comment stripping, etc.),
    // which is what Parsing::Prescan does.
    //
    // AllSources owns all the byte buffers we read; AllCookedSources owns
    // the post-prescan stream we hand to Parsing.
    fp::Options options;
    options.isFixedForm = false;

    fp::AllSources allSources;
    fp::AllCookedSources cooked{allSources};
    fp::Parsing parsing{cooked};

    parsing.Prescan(path, options);
    parsing.Parse(llvm::errs());

    // -- 4. Surface parse errors ----------------------------------------------
    if (!parsing.messages().empty() && parsing.messages().AnyFatalError()) {
        parsing.messages().Emit(llvm::errs(), cooked);
        if (!tempFile.empty()) std::remove(tempFile.c_str());
        return 1;
    }

    if (!parsing.parseTree().has_value()) {
        llvm::errs() << "op2-flang-scan: no parse tree produced for " << path << "\n";
        if (!tempFile.empty()) std::remove(tempFile.c_str());
        return 1;
    }

    const fp::Program &program = *parsing.parseTree();

    // -- 5. Walk the parse tree, building the JSON document -------------------
    Json json;
    json.beginObject();
    json.key("path"); json.stringValue(originalPath);

    json.key("events");
    json.beginArray();
    Scanner scanner{json, cooked};
    fp::Walk(program, scanner);
    json.endArray();

    json.endObject();

    // -- 6. Emit and clean up -------------------------------------------------
    std::cout << json.str() << "\n";

    if (!tempFile.empty()) std::remove(tempFile.c_str());
    return 0;
}
