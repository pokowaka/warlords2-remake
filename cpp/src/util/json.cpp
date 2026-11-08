#include "util/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace w2 {

const Json& Json::None() {
    static const Json kN;
    return kN;
}

bool Json::Has(const std::string& k) const {
    for (auto& e : o_)
        if (e.first == k) return true;
    return false;
}

const Json& Json::operator[](const std::string& k) const {
    for (auto& e : o_)
        if (e.first == k) return e.second;
    return None();
}

Json& Json::Set(const std::string& k, Json v) {
    type_ = kObject;
    for (auto& e : o_) {
        if (e.first == k) {
            e.second = std::move(v);
            return e.second;
        }
    }
    if (v.IsNull()) return const_cast<Json&>(None());
    o_.emplace_back(k, std::move(v));
    return o_.back().second;
}

static void WriteString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            // the game's strings are Latin-1 bytes: anything past ASCII is
            // written as its code point, so the file stays plain ASCII
            if (c < 0x20 || c >= 0x7f) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void Json::Write(std::string& out) const {
    switch (type_) {
    case kNull:
        out += "null";
        break;
    case kBool:
        out += b_ ? "true" : "false";
        break;
    case kNumber: {
        char buf[32];
        if (std::floor(n_) == n_ && std::fabs(n_) < 1e15)
            snprintf(buf, sizeof buf, "%.0f", n_);
        else
            snprintf(buf, sizeof buf, "%.17g", n_);
        out += buf;
        break;
    }
    case kString:
        WriteString(out, s_);
        break;
    case kArray:
        out += '[';
        for (size_t i = 0; i < a_.size(); i++) {
            if (i) out += ',';
            a_[i].Write(out);
        }
        out += ']';
        break;
    case kObject: {
        out += '{';
        bool first = true;
        for (auto& e : o_) {
            if (e.second.IsNull()) continue;
            if (!first) out += ',';
            first = false;
            WriteString(out, e.first);
            out += ':';
            e.second.Write(out);
        }
        out += '}';
        break;
    }
    }
}

std::string Json::Dump() const {
    std::string out;
    Write(out);
    return out;
}

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    [[noreturn]] void Fail(const char* what) {
        throw std::runtime_error(std::string("bad JSON: ") + what + " at " + std::to_string(i));
    }
    void Ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    }
    bool Lit(const char* w) {
        size_t n = strlen(w);
        if (s.compare(i, n, w) == 0) {
            i += n;
            return true;
        }
        return false;
    }
    std::string Str() {
        if (s[i] != '"') Fail("string");
        i++;
        std::string out;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\') {
                if (i >= s.size()) Fail("escape");
                char e = s[i++];
                switch (e) {
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'u': {
                    unsigned cp =
                            static_cast<unsigned>(strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    out += cp < 256 ? static_cast<char>(cp) : '?';
                    break;
                }
                default:
                    out += e;
                }
            } else {
                out += c;
            }
        }
        if (i >= s.size()) Fail("unterminated string");
        i++;
        return out;
    }
    Json Value() {
        Ws();
        if (i >= s.size()) Fail("end");
        char c = s[i];
        if (c == '{') {
            i++;
            Json o = Json::Object();
            Ws();
            if (s[i] == '}') {
                i++;
                return o;
            }
            for (;;) {
                Ws();
                std::string k = Str();
                Ws();
                if (s[i] != ':') Fail("colon");
                i++;
                Json v = Value();
                o.Set(k, std::move(v));
                Ws();
                if (s[i] == ',') {
                    i++;
                    continue;
                }
                if (s[i] == '}') {
                    i++;
                    return o;
                }
                Fail("object");
            }
        }
        if (c == '[') {
            i++;
            Json a = Json::Array();
            Ws();
            if (s[i] == ']') {
                i++;
                return a;
            }
            for (;;) {
                a.Push(Value());
                Ws();
                if (s[i] == ',') {
                    i++;
                    continue;
                }
                if (s[i] == ']') {
                    i++;
                    return a;
                }
                Fail("array");
            }
        }
        if (c == '"') return Json(Str());
        if (Lit("true")) return Json(true);
        if (Lit("false")) return Json(false);
        if (Lit("null")) return Json();
        char* end = nullptr;
        double d = strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) Fail("value");
        i = end - s.c_str();
        return Json(d);
    }
};
}  // namespace

Json Json::Parse(const std::string& text) {
    Parser p{text};
    Json v = p.Value();
    p.Ws();
    if (p.i != text.size()) p.Fail("trailing text");
    return v;
}

}  // namespace w2
