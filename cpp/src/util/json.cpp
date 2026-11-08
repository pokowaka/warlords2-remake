#include "util/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace w2 {

const Json& Json::none() {
    static const Json kN;
    return kN;
}

bool Json::has(const std::string& k) const {
    for (auto& e : o_)
        if (e.first == k) return true;
    return false;
}

const Json& Json::operator[](const std::string& k) const {
    for (auto& e : o_)
        if (e.first == k) return e.second;
    return none();
}

Json& Json::set(const std::string& k, Json v) {
    type_ = Object;
    for (auto& e : o_) {
        if (e.first == k) {
            e.second = std::move(v);
            return e.second;
        }
    }
    if (v.isNull()) return const_cast<Json&>(none());
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

void Json::write(std::string& out) const {
    switch (type_) {
    case Null:
        out += "null";
        break;
    case Bool:
        out += b_ ? "true" : "false";
        break;
    case Number: {
        char buf[32];
        if (std::floor(n_) == n_ && std::fabs(n_) < 1e15)
            snprintf(buf, sizeof buf, "%.0f", n_);
        else
            snprintf(buf, sizeof buf, "%.17g", n_);
        out += buf;
        break;
    }
    case String:
        WriteString(out, s_);
        break;
    case Array:
        out += '[';
        for (size_t i = 0; i < a_.size(); i++) {
            if (i) out += ',';
            a_[i].write(out);
        }
        out += ']';
        break;
    case Object: {
        out += '{';
        bool first = true;
        for (auto& e : o_) {
            if (e.second.isNull()) continue;
            if (!first) out += ',';
            first = false;
            WriteString(out, e.first);
            out += ':';
            e.second.write(out);
        }
        out += '}';
        break;
    }
    }
}

std::string Json::dump() const {
    std::string out;
    write(out);
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
            Json o = Json::object();
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
                o.set(k, std::move(v));
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
            Json a = Json::array();
            Ws();
            if (s[i] == ']') {
                i++;
                return a;
            }
            for (;;) {
                a.push(Value());
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

Json Json::parse(const std::string& text) {
    Parser p{text};
    Json v = p.Value();
    p.Ws();
    if (p.i != text.size()) p.Fail("trailing text");
    return v;
}

}  // namespace w2
