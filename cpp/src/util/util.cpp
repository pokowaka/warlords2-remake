#include "util/util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace w2 {

std::string Fmt(const char* fmt_str, ...) {
    va_list args_primary;
    va_start(args_primary, fmt_str);
    char buf[512];
    va_list args_copy;
    va_copy(args_copy, args_primary);
    int len = vsnprintf(buf, sizeof buf, fmt_str, args_primary);
    va_end(args_primary);
    std::string out;
    if (len < static_cast<int>(sizeof buf)) {
        out.assign(buf, len < 0 ? 0 : len);
    } else {
        out.resize(len + 1);
        vsnprintf(out.data(), len + 1, fmt_str, args_copy);
        out.resize(len);
    }
    va_end(args_copy);
    return out;
}

std::string Sfmt(const std::string& fmt_str, const std::vector<FmtArg>& args) {
    std::string out;
    size_t arg_index = 0;
    for (size_t i = 0; i < fmt_str.size(); i++) {
        char chr = fmt_str[i];
        if (chr != '%' || i + 1 >= fmt_str.size()) {
            out += chr;
            continue;
        }
        size_t pos = i + 1;
        std::string flags;
        while (pos < fmt_str.size() && strchr("-+ 0#", fmt_str[pos])) flags += fmt_str[pos++];
        int width = 0;
        while (pos < fmt_str.size() && isdigit(static_cast<unsigned char>(fmt_str[pos])))
            width = width * 10 + (fmt_str[pos++] - '0');
        int prec = -1;
        if (pos < fmt_str.size() && fmt_str[pos] == '.') {
            prec = 0;
            pos++;
            while (pos < fmt_str.size() && isdigit(static_cast<unsigned char>(fmt_str[pos])))
                prec = prec * 10 + (fmt_str[pos++] - '0');
        }
        while (pos < fmt_str.size() && (fmt_str[pos] == 'l' || fmt_str[pos] == 'h')) pos++;
        if (pos >= fmt_str.size()) {
            out += fmt_str.substr(i);
            break;
        }
        char conv = fmt_str[pos];
        i = pos;
        if (conv == '%') {
            out += '%';
            continue;
        }
        FmtArg arg = arg_index < args.size() ? args[arg_index] : FmtArg("nil");
        arg_index++;
        std::string formatted;
        switch (conv) {
        case 'd':
        case 'i':
        case 'u': {
            long val = arg.is_str ? (ToInt(arg.str_val) ? *ToInt(arg.str_val) : 0) : arg.num_val;
            formatted = std::to_string(val < 0 ? -val : val);
            if (prec >= 0)
                while (static_cast<int>(formatted.size()) < prec) formatted = "0" + formatted;
            if (val < 0)
                formatted = "-" + formatted;
            else if (flags.find('+') != std::string::npos)
                formatted = "+" + formatted;
            break;
        }
        case 'x':
        case 'X': {
            char buf[32];
            snprintf(buf, sizeof buf, conv == 'x' ? "%lx" : "%lX",
                     static_cast<unsigned long>(arg.is_str ? 0 : arg.num_val));
            formatted = buf;
            break;
        }
        case 'c':
            formatted = std::string(
                    1, static_cast<char>(arg.is_str ? (arg.str_val.empty() ? ' ' : arg.str_val[0])
                                                    : arg.num_val));
            break;
        case 's':
        default:
            formatted = arg.is_str ? arg.str_val : std::to_string(arg.num_val);
            if (prec >= 0 && static_cast<int>(formatted.size()) > prec)
                formatted = formatted.substr(0, prec);
            break;
        }
        if (static_cast<int>(formatted.size()) < width) {
            if (flags.find('-') != std::string::npos) {
                formatted += std::string(width - formatted.size(), ' ');
            } else if (flags.find('0') != std::string::npos && conv != 's') {
                bool sign = !formatted.empty() && (formatted[0] == '-' || formatted[0] == '+');
                std::string body = sign ? formatted.substr(1) : formatted;
                formatted = (sign ? formatted.substr(0, 1) : "") +
                            std::string(width - formatted.size(), '0') + body;
            } else {
                formatted = std::string(width - formatted.size(), ' ') + formatted;
            }
        }
        out += formatted;
    }
    return out;
}

// ------------------------------------------------------------------- files

namespace {
std::mutex dir_lock;
std::map<std::string, std::vector<std::string>> dir_cache;

/** @brief Returns the cached filename listing for `dir`. */
const std::vector<std::string>& Listing(const std::string& dir) {
    auto found_it = dir_cache.find(dir);
    if (found_it != dir_cache.end()) return found_it->second;
    std::vector<std::string> names;
    std::error_code err_code;
    for (const auto& entry : fs::directory_iterator(dir.empty() ? "." : dir, err_code))
        names.push_back(entry.path().filename().string());
    return dir_cache[dir] = names;
}
}  // namespace

std::string ResolvePath(const std::string& path) {
    std::error_code err_code;
    if (fs::exists(path, err_code)) return path;
    std::string norm_path = path;
    std::replace(norm_path.begin(), norm_path.end(), '\\', '/');
    std::lock_guard<std::mutex> lock(dir_lock);
    std::string cur;
    size_t pos = 0;
    if (!norm_path.empty() && norm_path[0] == '/') {
        cur = "/";
        pos = 1;
    }
    while (pos <= norm_path.size()) {
        size_t slash = norm_path.find('/', pos);
        if (slash == std::string::npos) slash = norm_path.size();
        std::string part = norm_path.substr(pos, slash - pos);
        pos = slash + 1;
        if (part.empty() || part == ".") continue;
        std::string next = cur.empty() ? part : (cur.back() == '/' ? cur + part : cur + "/" + part);
        if (part != ".." && !fs::exists(next, err_code)) {
            std::string found;
            for (const auto& name : Listing(cur)) {
                if (name.size() == part.size() && Upper(name) == Upper(part)) {
                    found = name;
                    break;
                }
            }
            if (found.empty()) return "";
            next = cur.empty() ? found : (cur.back() == '/' ? cur + found : cur + "/" + found);
        }
        cur = next;
    }
    return cur;
}

namespace {
/** @brief Returns the process-wide in-memory file overlay map. */
std::map<std::string, std::string>& MadeFiles() {
    static std::map<std::string, std::string> made;
    return made;
}
/** @brief Normalizes `path` into an upper-case forward-slash key for `MadeFiles()`. */
std::string MadeKey(const std::string& path) {
    std::string key;
    for (char chr : path) {
        char norm = chr == '\\' ? '/'
                                : static_cast<char>(std::toupper(static_cast<unsigned char>(chr)));
        if (norm == '/' && !key.empty() && key.back() == '/') continue;
        key += norm;
    }
    return key;
}
}  // namespace

void InstallFile(const std::string& path, const std::string& bytes) {
    MadeFiles()[MadeKey(path)] = bytes;
}

std::optional<std::string> ReadFile(const std::string& path) {
    auto made = MadeFiles().find(MadeKey(path));
    if (made != MadeFiles().end()) return made->second;
    std::string real = ResolvePath(path);
    if (real.empty()) return std::nullopt;
    std::ifstream file(real, std::ios::binary);
    if (!file) return std::nullopt;
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::string MustRead(const std::string& path) {
    auto content = ReadFile(path);
    if (!content) throw std::runtime_error("cannot open: " + path);
    return *content;
}

bool FileExists(const std::string& path) {
    return MadeFiles().count(MadeKey(path)) > 0 || !ResolvePath(path).empty();
}

bool WriteFile(const std::string& path, const std::string& bytes) {
    std::string real = ResolvePath(path);
    if (real.empty()) real = path;
    std::string tmp = real + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file) return false;
    }
    std::error_code err_code;
    fs::rename(tmp, real, err_code);
    if (err_code) return false;
    std::lock_guard<std::mutex> lock(dir_lock);
    dir_cache.clear();
    return true;
}

bool RemoveFile(const std::string& path) {
    std::string real = ResolvePath(path);
    if (real.empty()) return false;
    std::error_code err_code;
    bool removed = fs::remove(real, err_code);
    std::lock_guard<std::mutex> lock(dir_lock);
    dir_cache.clear();
    return removed;
}

// ----------------------------------------------------------------- strings

std::string Upper(std::string str) {
    for (auto& chr : str) chr = static_cast<char>(std::toupper(static_cast<unsigned char>(chr)));
    return str;
}
std::string Lower(std::string str) {
    for (auto& chr : str) chr = static_cast<char>(std::tolower(static_cast<unsigned char>(chr)));
    return str;
}
std::string TrimRight(const std::string& str) {
    size_t end_pos = str.size();
    while (end_pos > 0 && std::isspace(static_cast<unsigned char>(str[end_pos - 1]))) end_pos--;
    return str.substr(0, end_pos);
}
std::string Trim(const std::string& str) {
    size_t start_pos = 0;
    while (start_pos < str.size() && std::isspace(static_cast<unsigned char>(str[start_pos])))
        start_pos++;
    return TrimRight(str.substr(start_pos));
}
std::vector<std::string> SplitLines(const std::string& str) {
    std::vector<std::string> out;
    std::string cur;
    for (char chr : str) {
        if (chr == '\r' || chr == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += chr;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
bool StartsWith(const std::string& str, const std::string& prefix) {
    return str.compare(0, prefix.size(), prefix) == 0;
}
bool EndsWith(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool Contains(const std::string& str, const std::string& needle) {
    return str.find(needle) != std::string::npos;
}

std::string Latin1ToUtf8(const std::string& str) {
    std::string out;
    for (unsigned char byte : str) {
        if (byte < 0x80) {
            out += static_cast<char>(byte);
        } else {
            out += static_cast<char>(0xc0 | (byte >> 6));
            out += static_cast<char>(0x80 | (byte & 0x3f));
        }
    }
    return out;
}
std::string Utf8ToLatin1(const std::string& str) {
    std::string out;
    for (size_t i = 0; i < str.size();) {
        unsigned char lead = str[i];
        if (lead < 0x80) {
            out += static_cast<char>(lead);
            i++;
            continue;
        }
        int seq_len = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
        unsigned code_point = lead & (0x3f >> (seq_len - 1));
        for (int offset = 1; offset < seq_len && i + offset < str.size(); offset++)
            code_point = (code_point << 6) | (str[i + offset] & 0x3f);
        out += code_point < 256 ? static_cast<char>(code_point) : '?';
        i += seq_len;
    }
    return out;
}
std::optional<long> ToInt(const std::string& str) {
    std::string trimmed = Trim(str);
    if (trimmed.empty()) return std::nullopt;
    size_t pos = 0;
    bool neg = false;
    if (trimmed[0] == '-' || trimmed[0] == '+') {
        neg = trimmed[0] == '-';
        pos = 1;
    }
    if (pos >= trimmed.size()) return std::nullopt;
    long val = 0;
    for (; pos < trimmed.size(); pos++) {
        if (!std::isdigit(static_cast<unsigned char>(trimmed[pos]))) return std::nullopt;
        val = val * 10 + (trimmed[pos] - '0');
    }
    return neg ? -val : val;
}

double Now() {
    using namespace std::chrono;
    static const auto kStart = steady_clock::now();
    return duration<double>(steady_clock::now() - kStart).count();
}

}  // namespace w2
