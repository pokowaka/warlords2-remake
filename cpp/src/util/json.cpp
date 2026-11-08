#include "util/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace w2 {

const Json& Json::None() {
    static const Json kNullJson;
    return kNullJson;
}

bool Json::Has(const std::string& key) const {
    for (const auto& entry : obj_val_)
        if (entry.first == key) return true;
    return false;
}

const Json& Json::operator[](const std::string& key) const {
    for (const auto& entry : obj_val_)
        if (entry.first == key) return entry.second;
    return None();
}

Json& Json::Set(const std::string& key, Json val) {
    type_ = kObject;
    for (auto& entry : obj_val_) {
        if (entry.first == key) {
            entry.second = std::move(val);
            return entry.second;
        }
    }
    if (val.IsNull()) return const_cast<Json&>(None());
    obj_val_.emplace_back(key, std::move(val));
    return obj_val_.back().second;
}

/** @brief Appends `str` to `out` as a quoted, escaped ASCII JSON string. */
static void WriteString(std::string& out, const std::string& str) {
    out += '"';
    for (unsigned char byte : str) {
        switch (byte) {
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
            // The game's strings are Latin-1 bytes: anything outside printable
            // ASCII is escaped as a code point so the file stays plain ASCII.
            if (byte < 0x20 || byte >= 0x7f) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", byte);
                out += buf;
            } else {
                out += static_cast<char>(byte);
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
        out += bool_val_ ? "true" : "false";
        break;
    case kNumber: {
        char buf[32];
        if (std::floor(num_val_) == num_val_ && std::fabs(num_val_) < 1e15)
            snprintf(buf, sizeof buf, "%.0f", num_val_);
        else
            snprintf(buf, sizeof buf, "%.17g", num_val_);
        out += buf;
        break;
    }
    case kString:
        WriteString(out, str_val_);
        break;
    case kArray:
        out += '[';
        for (size_t i = 0; i < arr_val_.size(); i++) {
            if (i) out += ',';
            arr_val_[i].Write(out);
        }
        out += ']';
        break;
    case kObject: {
        out += '{';
        bool first = true;
        for (const auto& entry : obj_val_) {
            if (entry.second.IsNull()) continue;
            if (!first) out += ',';
            first = false;
            WriteString(out, entry.first);
            out += ':';
            entry.second.Write(out);
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
/**
 * @brief Recursive-descent JSON parser over a source string.
 */
struct Parser {
    const std::string& src;  ///< Input JSON text.
    size_t pos = 0;          ///< Current byte cursor into `src`.

    /** @brief Throws a syntax error naming `what` and the current byte offset. */
    [[noreturn]] void Fail(const char* what) const {
        throw std::runtime_error(std::string("bad JSON: ") + what + " at " + std::to_string(pos));
    }

    /** @brief Advances `pos` past any ASCII whitespace. */
    void SkipWhitespace() {
        while (pos < src.size() &&
               (src[pos] == ' ' || src[pos] == '\n' || src[pos] == '\r' || src[pos] == '\t')) {
            pos++;
        }
    }

    /** @brief Consumes `word` if it matches at `pos` and returns true. */
    bool ConsumeLiteral(const char* word) {
        size_t len = strlen(word);
        if (src.compare(pos, len, word) == 0) {
            pos += len;
            return true;
        }
        return false;
    }

    /** @brief Parses a double-quoted JSON string. */
    std::string ParseString() {
        if (src[pos] != '"') Fail("string");
        pos++;
        std::string out;
        while (pos < src.size() && src[pos] != '"') {
            char chr = src[pos++];
            if (chr == '\\') {
                if (pos >= src.size()) Fail("escape");
                char esc = src[pos++];
                switch (esc) {
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
                    unsigned code_point =
                            static_cast<unsigned>(strtoul(src.substr(pos, 4).c_str(), nullptr, 16));
                    pos += 4;
                    out += code_point < 256 ? static_cast<char>(code_point) : '?';
                    break;
                }
                default:
                    out += esc;
                }
            } else {
                out += chr;
            }
        }
        if (pos >= src.size()) Fail("unterminated string");
        pos++;
        return out;
    }

    /** @brief Parses a single JSON value (object, array, string, boolean, null, or number). */
    Json ParseValue() {
        SkipWhitespace();
        if (pos >= src.size()) Fail("end");
        char chr = src[pos];
        if (chr == '{') {
            pos++;
            Json obj = Json::Object();
            SkipWhitespace();
            if (src[pos] == '}') {
                pos++;
                return obj;
            }
            for (;;) {
                SkipWhitespace();
                std::string key = ParseString();
                SkipWhitespace();
                if (src[pos] != ':') Fail("colon");
                pos++;
                Json val = ParseValue();
                obj.Set(key, std::move(val));
                SkipWhitespace();
                if (src[pos] == ',') {
                    pos++;
                    continue;
                }
                if (src[pos] == '}') {
                    pos++;
                    return obj;
                }
                Fail("object");
            }
        }
        if (chr == '[') {
            pos++;
            Json arr = Json::Array();
            SkipWhitespace();
            if (src[pos] == ']') {
                pos++;
                return arr;
            }
            for (;;) {
                arr.Push(ParseValue());
                SkipWhitespace();
                if (src[pos] == ',') {
                    pos++;
                    continue;
                }
                if (src[pos] == ']') {
                    pos++;
                    return arr;
                }
                Fail("array");
            }
        }
        if (chr == '"') return Json(ParseString());
        if (ConsumeLiteral("true")) return Json(true);
        if (ConsumeLiteral("false")) return Json(false);
        if (ConsumeLiteral("null")) return Json();
        char* end = nullptr;
        double num = strtod(src.c_str() + pos, &end);
        if (end == src.c_str() + pos) Fail("value");
        pos = end - src.c_str();
        return Json(num);
    }
};
}  // namespace

Json Json::Parse(const std::string& text) {
    Parser parser{text};
    Json val = parser.ParseValue();
    parser.SkipWhitespace();
    if (parser.pos != text.size()) parser.Fail("trailing text");
    return val;
}

}  // namespace w2
