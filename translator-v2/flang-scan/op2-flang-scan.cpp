// op2-flang-scan: parse Fortran with Flang and emit OP2 call sites / subprograms as JSON.
// Invoked by translator-v2/op2-translator/fortran/flang_parser.py.
//
// Usage: op2-flang-scan [--stdin] [--path <reported-path>] [path]

#include "flang/Parser/parsing.h"
#include "flang/Parser/parse-tree.h"
#include "flang/Parser/parse-tree-visitor.h"
#include "flang/Parser/provenance.h"
#include "flang/Parser/source.h"
#include "flang/Parser/message.h"

#include "llvm/Support/raw_ostream.h"

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
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fp = Fortran::parser;

class Json {
public:
    void beginObject() { comma(); out_ << "{"; first_.push_back(true); }
    void endObject() { out_ << "}"; first_.pop_back(); markWrote(); }

    void beginArray() { comma(); out_ << "["; first_.push_back(true); }
    void endArray() { out_ << "]"; first_.pop_back(); markWrote(); }

    void key(const std::string &k) {
        comma();
        writeString(k);
        out_ << ":";
        if (!first_.empty()) {
            first_.back() = true;
        }
    }

    void stringValue(const std::string &s) { comma(); writeString(s); markWrote(); }
    void intValue(int64_t v) { comma(); out_ << v; markWrote(); }
    void boolValue(bool b) { comma(); out_ << (b ? "true" : "false"); markWrote(); }
    void nullValue() { comma(); out_ << "null"; markWrote(); }

    void rawValue(const std::string &jsonText) { comma(); out_ << jsonText; markWrote(); }

    std::string str() const { return out_.str(); }

private:
    void comma() {
        if (!first_.empty()) {
            if (!first_.back()) {
                out_ << ",";
            }
            first_.back() = false;
        }
    }

    void markWrote() {
        if (!first_.empty()) first_.back() = false;
    }

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
    std::vector<bool> first_;
};

static std::string toLower(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string sourceText(fp::CharBlock src) {
    return src.ToString();
}

static void emitExpr(Json &json, const fp::Expr &e);
static void emitActualArgs(Json &json, const std::list<fp::ActualArgSpec> &args);

static std::optional<int64_t> foldIntExpr(const fp::Expr &e);

static std::optional<int64_t> parseIntText(const std::string &text) {
    try {
        size_t pos = 0;
        long long v = std::stoll(text, &pos);
        return static_cast<int64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<int64_t>
foldLiteralConstant(const fp::LiteralConstant &lit) {
    return std::visit([](const auto &alt) -> std::optional<int64_t> {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::IntLiteralConstant>) {
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

static std::optional<std::string>
designatorToName(const fp::Designator &d) {
    return std::visit([](const auto &alt) -> std::optional<std::string> {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, fp::DataRef>) {
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

struct CallView {
    std::string name;
    const std::list<fp::ActualArgSpec> *args = nullptr;
};

static std::optional<CallView> exprAsCall(const fp::Expr &e) {
    return std::visit([](const auto &alt) -> std::optional<CallView> {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::FunctionReference>>) {
            const fp::FunctionReference &fr = alt.value();
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
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }, e.u);
}

static void emitExpr(Json &json, const fp::Expr &e) {
    if (auto v = foldIntExpr(e)) {
        json.beginObject();
        json.key("kind"); json.stringValue("int");
        json.key("value"); json.intValue(*v);
        json.endObject();
        return;
    }

    auto extractCharLiteral = [](const fp::Expr &expr) -> std::optional<std::string> {
        return std::visit([](const auto &alt) -> std::optional<std::string> {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Fortran::common::Indirection<fp::CharLiteralConstant>>) {
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

    if (auto call = exprAsCall(e)) {
        json.beginObject();
        json.key("kind"); json.stringValue("call");
        json.key("name"); json.stringValue(call->name);
        json.key("args");
        emitActualArgs(json, *call->args);
        json.endObject();
        return;
    }

    json.beginObject();
    json.key("kind"); json.stringValue("raw");
    json.key("source"); json.stringValue(sourceText(e.source));
    json.endObject();
}

static void emitActualArgs(Json &json, const std::list<fp::ActualArgSpec> &args) {
    json.beginArray();
    for (const fp::ActualArgSpec &spec : args) {
        const fp::ActualArg &aa = std::get<fp::ActualArg>(spec.t);
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

struct DependsCollector {
    std::set<std::string> &out;

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

    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

static void emitBodyExpr(Json &json, const fp::Expr &e);

static std::string kindParamToString(const fp::KindParam &kp) {
    return std::visit([](const auto &alt) -> std::string {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, std::uint64_t>) {
            return std::to_string(alt);
        } else {
            return toLower(alt.thing.thing.thing.ToString());
        }
    }, kp.u);
}

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
            emitBodyExpr(json, alt.thing.value());
        }
    }, sub.u);
}

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

    json.beginObject();
    json.key("kind"); json.stringValue("unsupported");
    json.key("tag"); json.stringValue("expr");
    json.key("source"); json.stringValue(sourceText(e.source));
    json.endObject();
}

struct NameCollector {
    std::vector<std::string> &out;
    bool Pre(const fp::Name &n) { out.push_back(toLower(n.ToString())); return true; }
    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

struct LocalsCollector {
    Json &json;

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

struct BodyCollector {
    Json &jsonAssignments;
    Json &jsonCalls;
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

static const fp::Expr &unwrapScalarIntExpr(const fp::ScalarIntExpr &e) { return e.thing.thing.value(); }
static const fp::Expr &unwrapScalarLogicalExpr(const fp::ScalarLogicalExpr &e) { return e.thing.thing.value(); }
static const fp::Expr &unwrapScalarExpr(const fp::ScalarExpr &e) { return e.thing.value(); }
static const fp::Expr &unwrapConstantExpr(const fp::ConstantExpr &e) { return e.thing.value(); }
static const fp::Expr &unwrapScalarIntConstantExpr(const fp::ScalarIntConstantExpr &e) { return e.thing.thing.thing.value(); }
static const fp::Expr &unwrapSpecificationExpr(const fp::SpecificationExpr &e) { return unwrapScalarIntExpr(e.v); }

static std::optional<std::string> kindSelectorText(const std::optional<fp::KindSelector> &ks) {
    if (!ks) return std::nullopt;
    if (const auto *sice = std::get_if<fp::ScalarIntConstantExpr>(&ks->u)) {
        return sourceText(unwrapScalarIntConstantExpr(*sice).source);
    }
    return std::nullopt;
}

static void emitTypeParamValue(Json &json, const fp::TypeParamValue &tpv) {
    if (const auto *sie = std::get_if<fp::ScalarIntExpr>(&tpv.u)) {
        emitBodyExpr(json, unwrapScalarIntExpr(*sie));
        return;
    }
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
            const auto &lengthOpt = std::get<0>(alt.t);
            if (lengthOpt) emitTypeParamValue(json, *lengthOpt);
            else json.nullValue();
        }
    }, cs->u);
}

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
            json.key("kind"); json.stringValue("unsupported");
        }
        json.endObject();
    }, its.u);
}

static void emitDeclType(Json &json, const fp::DeclarationTypeSpec &dts) {
    if (const auto *its = std::get_if<fp::IntrinsicTypeSpec>(&dts.u)) {
        emitIntrinsicType(json, *its);
        return;
    }
    json.beginObject();
    json.key("kind"); json.stringValue("unsupported");
    json.endObject();
}

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

static void emitInitialization(Json &json, const std::optional<fp::Initialization> &init) {
    if (!init) { json.nullValue(); return; }
    if (const auto *ce = std::get_if<fp::ConstantExpr>(&init->u)) {
        emitBodyExpr(json, unwrapConstantExpr(*ce));
        return;
    }
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

static void emitBlock(Json &json, const fp::Block &block, const fp::AllCookedSources &cooked);

static std::pair<int, int> resolveLineColStmt(const fp::AllCookedSources &cooked, fp::CharBlock src) {
    if (src.empty()) return {0, 0};
    auto prov = cooked.GetProvenanceRange(src);
    if (!prov) return {0, 0};
    auto pos = cooked.allSources().GetSourcePosition(prov->start());
    if (pos) return {static_cast<int>(pos->line), static_cast<int>(pos->column)};
    return {0, 0};
}

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
            return false;
        }, loopControlOpt->u);
    }

    if (!handled) {
        json.key("mode"); json.stringValue("unsupported");
    }

    json.key("body"); emitBlock(json, block, cooked);
    json.endObject();
}

static void emitExecutionPartConstruct(Json &json, const fp::ExecutionPartConstruct &epc, const fp::AllCookedSources &cooked);

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

struct Scanner {
    Json &json;
    const fp::AllCookedSources &cooked;

    bool Pre(const fp::CallStmt &call) {
        const fp::Call &c = std::get<fp::Call>(call.t);
        const fp::ProcedureDesignator &pd = std::get<fp::ProcedureDesignator>(c.t);
        const auto &actualArgs = std::get<std::list<fp::ActualArgSpec>>(c.t);

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

    void emitLoop(const std::string &name, int line, int col,
                  const std::list<fp::ActualArgSpec> &args) {
        json.beginObject();
        json.key("kind"); json.stringValue(name);
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

    static fp::CharBlock spanningRange(fp::CharBlock a, fp::CharBlock b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        const char *start = a.begin();
        const char *end = b.begin() + b.size();
        if (end <= start) return a;
        return fp::CharBlock{start, static_cast<std::size_t>(end - start)};
    }

    bool Pre(const fp::SubroutineSubprogram &sub) {
        const auto &startStmt = std::get<fp::Statement<fp::SubroutineStmt>>(sub.t);
        const auto &endStmt = std::get<fp::Statement<fp::EndSubroutineStmt>>(sub.t);
        const fp::SubroutineStmt &subStmt = startStmt.statement;

        const fp::Name &nameNode = std::get<fp::Name>(subStmt.t);
        std::string name = toLower(nameNode.ToString());
        auto [line, col] = resolveLineCol(nameNode.source);

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

    bool Pre(const fp::FunctionSubprogram &fn) {
        const auto &startStmt = std::get<fp::Statement<fp::FunctionStmt>>(fn.t);
        const auto &endStmt = std::get<fp::Statement<fp::EndFunctionStmt>>(fn.t);
        const fp::FunctionStmt &fnStmt = startStmt.statement;

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

    static std::optional<std::string> resultName(const fp::FunctionStmt &fnStmt) {
        const auto &suffixOpt = std::get<std::optional<fp::Suffix>>(fnStmt.t);
        if (!suffixOpt) return std::nullopt;
        const auto &nameOpt = std::get<std::optional<fp::Name>>(suffixOpt->t);
        if (!nameOpt) return std::nullopt;
        return toLower(nameOpt->ToString());
    }

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
            json.stringValue(std::string(bodyRange.begin(), bodyRange.size()));
        } else {
            json.stringValue("");
        }

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

    template <typename T> bool Pre(const T &) { return true; }
    template <typename T> void Post(const T &) {}
};

static std::string slurpStdin() {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

static std::string writeTempFile(const std::string &contents) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    auto path = dir / ("op2-flang-scan-" + std::to_string(::getpid()) + ".F90");
    std::ofstream ofs(path);
    ofs << contents;
    ofs.close();
    return path.string();
}

int main(int argc, char **argv) {
    std::string path;
    bool readStdin = false;
    std::string originalPath;

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

    std::string tempFile;
    if (readStdin || path.empty()) {
        std::string body = slurpStdin();
        tempFile = writeTempFile(body);
        path = tempFile;
    }
    if (originalPath.empty()) originalPath = path;

    fp::Options options;
    options.isFixedForm = false;

    fp::AllSources allSources;
    fp::AllCookedSources cooked{allSources};
    fp::Parsing parsing{cooked};

    parsing.Prescan(path, options);
    parsing.Parse(llvm::errs());

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

    Json json;
    json.beginObject();
    json.key("path"); json.stringValue(originalPath);

    json.key("events");
    json.beginArray();
    Scanner scanner{json, cooked};
    fp::Walk(program, scanner);
    json.endArray();

    json.endObject();

    std::cout << json.str() << "\n";

    if (!tempFile.empty()) std::remove(tempFile.c_str());
    return 0;
}
