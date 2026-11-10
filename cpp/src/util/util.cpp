#include "util/util.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "absl/base/no_destructor.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace fs = std::filesystem;

namespace w2 {

std::string Sfmt(std::string_view fmt_str, const std::vector<FmtArg>& args) {
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
        while (pos < fmt_str.size() && std::strchr("-+ 0#", fmt_str[pos]) != nullptr) {
            flags += fmt_str[pos++];
        }
        int width = 0;
        while (pos < fmt_str.size() && std::isdigit(static_cast<unsigned char>(fmt_str[pos]))) {
            width = width * 10 + (fmt_str[pos++] - '0');
        }
        int prec = -1;
        if (pos < fmt_str.size() && fmt_str[pos] == '.') {
            prec = 0;
            pos++;
            while (pos < fmt_str.size() && std::isdigit(static_cast<unsigned char>(fmt_str[pos]))) {
                prec = prec * 10 + (fmt_str[pos++] - '0');
            }
        }
        while (pos < fmt_str.size() && (fmt_str[pos] == 'l' || fmt_str[pos] == 'h')) pos++;
        if (pos >= fmt_str.size()) {
            out.append(fmt_str.substr(i));
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
            long val = arg.is_str ? ToInt(arg.str_val).value_or(0) : arg.num_val;
            unsigned long mag = val < 0 ? 0UL - static_cast<unsigned long>(val)
                                        : static_cast<unsigned long>(val);
            formatted = std::to_string(mag);
            if (prec >= 0) {
                while (static_cast<int>(formatted.size()) < prec) formatted = "0" + formatted;
            }
            if (val < 0)
                formatted = "-" + formatted;
            else if (flags.contains('+'))
                formatted = "+" + formatted;
            break;
        }
        case 'x':
            formatted =
                    absl::StrFormat("%x", static_cast<unsigned long>(arg.is_str ? 0 : arg.num_val));
            break;
        case 'X':
            formatted =
                    absl::StrFormat("%X", static_cast<unsigned long>(arg.is_str ? 0 : arg.num_val));
            break;
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
            if (flags.contains('-')) {
                formatted += std::string(width - formatted.size(), ' ');
            } else if (flags.contains('0') && conv != 's') {
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

using DirListingMap = absl::flat_hash_map<std::string, std::vector<std::string>>;

class DirectoryListingCache {
  public:
    std::vector<std::string> Listing(const std::string& dir) ABSL_LOCKS_EXCLUDED(dir_cache_mutex_) {
        absl::MutexLock dir_cache_lock(&dir_cache_mutex_);
        auto found_it = entries_.find(dir);
        if (found_it != entries_.end()) return found_it->second;
        std::vector<std::string> names;
        std::error_code err_code;
        for (const auto& entry : fs::directory_iterator(dir.empty() ? "." : dir, err_code)) {
            names.push_back(entry.path().filename().string());
        }
        entries_.emplace(dir, names);
        return names;
    }

    void Clear() ABSL_LOCKS_EXCLUDED(dir_cache_mutex_) {
        absl::MutexLock dir_cache_lock(&dir_cache_mutex_);
        entries_.clear();
    }

  private:
    absl::Mutex dir_cache_mutex_;
    DirListingMap entries_ ABSL_GUARDED_BY(dir_cache_mutex_);
};

DirectoryListingCache& GlobalDirCache() {
    static absl::NoDestructor<DirectoryListingCache> cache;
    return *cache;
}

absl::Status OsIoError(std::string_view operation, const fs::path& path, int err_num,
                       std::string_view hint) {
    std::string detail = std::strerror(err_num);
    std::string msg = absl::StrFormat("Failed to %s '%s': %s (errno=%d); %s", operation,
                                      path.string(), detail, err_num, hint);
    if (err_num == ENOENT) return absl::NotFoundError(msg);
    if (err_num == EACCES || err_num == EPERM) return absl::PermissionDeniedError(msg);
    return absl::InternalError(msg);
}

}  // namespace

fs::path ResolvePath(const fs::path& path) {
    std::error_code err_code;
    if (fs::exists(path, err_code)) return path;
    std::string norm_path = path.string();
    std::replace(norm_path.begin(), norm_path.end(), '\\', '/');
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
            std::string upper_part = Upper(part);
            for (const auto& name : GlobalDirCache().Listing(cur)) {
                if (name.size() == part.size() && Upper(name) == upper_part) {
                    found = name;
                    break;
                }
            }
            if (found.empty()) return {};
            next = cur.empty() ? found : (cur.back() == '/' ? cur + found : cur + "/" + found);
        }
        cur = next;
    }
    return cur.empty() ? fs::path{} : fs::path(cur);
}

absl::StatusOr<std::vector<uint8_t>> ReadBytes(const fs::path& path) {
    fs::path real = ResolvePath(path);
    if (real.empty()) {
        return absl::NotFoundError(absl::StrFormat(
                "Failed to read '%s': file does not exist (checked case-insensitively); verify "
                "the file exists and --data points to a valid Warlords II directory.",
                path.string()));
    }
    errno = 0;
    std::ifstream file(real, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        int err_num = errno != 0 ? errno : EIO;
        return OsIoError("open file for reading", real, err_num,
                         "check file permissions and verify the path is a readable file.");
    }
    std::streamsize file_size = file.tellg();
    if (file_size < 0) {
        int err_num = errno != 0 ? errno : EIO;
        return OsIoError("determine file size of", real, err_num,
                         "ensure the path refers to a regular file rather than a directory.");
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    file.seekg(0, std::ios::beg);
    if (file_size > 0 && !file.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        int err_num = errno != 0 ? errno : EIO;
        return OsIoError("read bytes from", real, err_num,
                         "check disk health and ensure the file is not truncated concurrently.");
    }
    return buffer;
}

absl::StatusOr<std::string> ReadFile(const fs::path& path) {
    auto bytes = ReadBytes(path);
    if (!bytes.ok()) return bytes.status();
    return std::string(bytes->begin(), bytes->end());
}

bool FileExists(const fs::path& path) {
    return !ResolvePath(path).empty();
}

absl::Status WriteBytes(const fs::path& path, absl::Span<const uint8_t> bytes) {
    fs::path real = ResolvePath(path);
    if (real.empty()) real = path;
    fs::path tmp = real;
    tmp += ".tmp";
    {
        errno = 0;
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            int err_num = errno != 0 ? errno : EIO;
            return OsIoError("open temporary file for writing", tmp, err_num,
                             "ensure the parent directory exists and is writable.");
        }
        if (!bytes.empty()) {
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (!file) {
                int err_num = errno != 0 ? errno : EIO;
                return OsIoError("write bytes to", tmp, err_num,
                                 "check available disk space and directory permissions.");
            }
        }
    }
    std::error_code err_code;
    fs::rename(tmp, real, err_code);
    if (err_code) {
        std::error_code cleanup_err;
        fs::remove(tmp, cleanup_err);
        return absl::InternalError(absl::StrFormat(
                "Failed to atomically replace '%s' from '%s': %s; verify write permissions on the "
                "destination directory.",
                real.string(), tmp.string(), err_code.message()));
    }
    GlobalDirCache().Clear();
    return absl::OkStatus();
}

absl::Status WriteFile(const fs::path& path, std::string_view content) {
    const auto* raw = reinterpret_cast<const uint8_t*>(content.data());
    return WriteBytes(path, absl::MakeConstSpan(raw, content.size()));
}

absl::Status RemoveFile(const fs::path& path) {
    fs::path real = ResolvePath(path);
    if (real.empty()) {
        return absl::NotFoundError(absl::StrFormat(
                "Failed to remove '%s': file does not exist; verify the path before deleting.",
                path.string()));
    }
    std::error_code err_code;
    bool removed = fs::remove(real, err_code);
    GlobalDirCache().Clear();
    if (err_code || !removed) {
        return absl::InternalError(absl::StrFormat(
                "Failed to remove '%s': %s; check directory write permissions.", real.string(),
                err_code ? err_code.message() : "fs::remove returned false"));
    }
    return absl::OkStatus();
}

// ----------------------------------------------------------------- strings

std::string Upper(std::string_view str) {
    std::string out(str);
    for (char& chr : out) chr = static_cast<char>(std::toupper(static_cast<unsigned char>(chr)));
    return out;
}

std::string Lower(std::string_view str) {
    std::string out(str);
    for (char& chr : out) chr = static_cast<char>(std::tolower(static_cast<unsigned char>(chr)));
    return out;
}

std::string TrimRight(std::string_view str) {
    size_t end_pos = str.size();
    while (end_pos > 0 && std::isspace(static_cast<unsigned char>(str[end_pos - 1]))) end_pos--;
    return std::string(str.substr(0, end_pos));
}

std::string Trim(std::string_view str) {
    size_t start_pos = 0;
    while (start_pos < str.size() && std::isspace(static_cast<unsigned char>(str[start_pos]))) {
        start_pos++;
    }
    return TrimRight(str.substr(start_pos));
}

std::vector<std::string> SplitLines(std::string_view str) {
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

std::string Latin1ToUtf8(std::string_view str) {
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

std::string Utf8ToLatin1(std::string_view str) {
    std::string out;
    for (size_t i = 0; i < str.size();) {
        auto lead = static_cast<unsigned char>(str[i]);
        if (lead < 0x80) {
            out += static_cast<char>(lead);
            i++;
            continue;
        }
        int seq_len = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
        unsigned code_point = lead & (0x3f >> (seq_len - 1));
        for (int offset = 1; offset < seq_len && i + offset < str.size(); offset++) {
            code_point = (code_point << 6) | (static_cast<unsigned char>(str[i + offset]) & 0x3f);
        }
        out += code_point < 256 ? static_cast<char>(code_point) : '?';
        i += seq_len;
    }
    return out;
}

std::optional<long> ToInt(std::string_view str) {
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
        str.remove_suffix(1);
    }
    if (str.empty()) return std::nullopt;
    if (str.front() == '+') {
        str.remove_prefix(1);
        if (str.empty() || str.front() == '-' || str.front() == '+') return std::nullopt;
    }
    long val = 0;
    const char* begin = str.data();
    const char* end = str.data() + str.size();
    auto [ptr, err_code] = std::from_chars(begin, end, val, 10);
    if (err_code != std::errc{} || ptr != end) return std::nullopt;
    return val;
}

double Now() {
    static const absl::Time kProcessStart = absl::Now();
    return absl::ToDoubleSeconds(absl::Now() - kProcessStart);
}

std::vector<std::string> ParseCliFlags(int argc, char** argv, std::string_view usage_message) {
    if (!usage_message.empty()) {
        absl::SetProgramUsageMessage(usage_message);
    }
    // Preserve signed numeric positional arguments (such as "-1" in w2aidiag) by
    // replacing them with a sentinel token during flag parsing and restoring them
    // into the returned positional list, while normalizing "--kebab-case" flags
    // to "--snake_case" so existing shell scripts work verbatim.
    constexpr std::string_view kNegPrefix = "__w2_positional_neg__:";
    std::vector<std::string> owned_args;
    owned_args.reserve(static_cast<size_t>(std::max(1, argc)));
    bool past_double_dash = false;
    for (int i = 0; i < argc; i++) {
        std::string arg = argv[i] ? argv[i] : "";
        if (i > 0 && !past_double_dash) {
            if (arg == "--") {
                past_double_dash = true;
            } else if (arg.starts_with("--")) {
                size_t eq_pos = arg.find('=');
                size_t end_pos = eq_pos == std::string::npos ? arg.size() : eq_pos;
                for (size_t idx = 2; idx < end_pos; idx++) {
                    if (arg[idx] == '-') arg[idx] = '_';
                }
                std::string_view flag_head(arg.data(), end_pos);
                if (flag_head == "--export") {
                    arg = "--export_dir" + (eq_pos == std::string::npos ? "" : arg.substr(eq_pos));
                }
            } else if (arg.size() >= 2 && arg[0] == '-' &&
                       std::isdigit(static_cast<unsigned char>(arg[1]))) {
                arg = std::string(kNegPrefix) + arg;
            }
        }
        owned_args.push_back(std::move(arg));
    }
    std::vector<char*> mutable_argv;
    mutable_argv.reserve(owned_args.size());
    for (std::string& item : owned_args) mutable_argv.push_back(item.data());

    std::vector<char*> remaining =
            absl::ParseCommandLine(static_cast<int>(mutable_argv.size()), mutable_argv.data());
    std::vector<std::string> positional;
    for (size_t i = 1; i < remaining.size(); i++) {
        std::string_view token = remaining[i];
        if (token.starts_with(kNegPrefix)) token.remove_prefix(kNegPrefix.size());
        positional.emplace_back(token);
    }
    return positional;
}

}  // namespace w2
