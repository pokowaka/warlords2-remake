/**
 * @file
 * @brief Shared utilities providing Lua-compatible sorting, runtime `STRING.DAT`
 * formatting, case-insensitive filesystem I/O, string helpers, CLI flag parsing,
 * and monotonic timing.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/log/absl_check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"

namespace w2 {

/**
 * @brief Sorts `vec` using Lua 5.1 / LuaJIT's unstable quicksort (`auxsort`).
 *
 * Matches the exact tie-breaking order of the Lua reference so seeded games
 * and world generation remain bit-identical across ports. `less_than(lhs, rhs)`
 * returns true when `lhs` sorts before `rhs`, and must define a strict weak
 * ordering.
 */
template <class T, class Lt>
void LuaSort(std::vector<T>& vec, Lt less_than) {
    // 1-based indexing, matching the Lua 5.1 `auxsort` implementation.
    auto get = [&](long idx) -> T& { return vec[idx - 1]; };
    auto swap = [&](long idx_a, long idx_b) { std::swap(vec[idx_a - 1], vec[idx_b - 1]); };
    std::function<void(long, long)> auxsort = [&](long low, long up_idx) {
        while (low < up_idx) {
            if (less_than(get(up_idx), get(low))) swap(low, up_idx);
            if (up_idx - low == 1) break;
            long mid = (low + up_idx) / 2;
            if (less_than(get(mid), get(low)))
                swap(mid, low);
            else if (less_than(get(up_idx), get(mid)))
                swap(mid, up_idx);
            if (up_idx - low == 2) break;
            T pivot = get(mid);
            swap(mid, up_idx - 1);
            mid = low;
            long right = up_idx - 1;
            for (;;) {
                while (less_than(get(++mid), pivot)) {
                    ABSL_CHECK(mid <= up_idx) << "Invalid strict-weak comparator passed to LuaSort";
                }
                while (less_than(pivot, get(--right))) {
                    ABSL_CHECK(right >= low) << "Invalid strict-weak comparator passed to LuaSort";
                }
                if (right < mid) break;
                swap(mid, right);
            }
            swap(up_idx - 1, mid);
            if (mid - low < up_idx - mid) {
                right = low;
                mid = mid - 1;
                low = mid + 2;
            } else {
                right = mid + 1;
                mid = up_idx;
                up_idx = right - 2;
            }
            auxsort(right, mid);
        }
    };
    auxsort(1, static_cast<long>(vec.size()));
}

/** @brief Sorts `vec` in ascending order using `LuaSort`. */
template <class T>
void LuaSort(std::vector<T>& vec) {
    LuaSort(vec, [](const T& lhs, const T& rhs) { return lhs < rhs; });
}

/**
 * @brief Type-erased argument (integer or string) passed to `Sfmt`.
 */
struct FmtArg {
    bool is_str = false;  ///< True when `str_val` holds the argument; false for `num_val`.
    long num_val = 0;     ///< Integer value when `is_str` is false.
    std::string str_val;  ///< String value when `is_str` is true.
    explicit FmtArg(int val) : num_val(val) {}
    explicit FmtArg(long val) : num_val(val) {}
    explicit FmtArg(unsigned val) : num_val(static_cast<long>(val)) {}
    explicit FmtArg(unsigned long val) : num_val(static_cast<long>(val)) {}
    explicit FmtArg(long long val) : num_val(static_cast<long>(val)) {}
    explicit FmtArg(std::string_view val) : is_str(true), str_val(val) {}
    explicit FmtArg(const std::string& val) : is_str(true), str_val(val) {}
    explicit FmtArg(const char* val) : is_str(true), str_val(val ? val : "") {}
};

/**
 * @brief Formats `fmt_str` safely for runtime format strings loaded from `STRING.DAT`
 * (`%d`, `%s`, `%x`, `%c`, and `%%` with flags and widths).
 */
std::string Sfmt(std::string_view fmt_str, const std::vector<FmtArg>& args);

/** @brief Variadic wrapper around `Sfmt`. */
template <class... A>
std::string Format(std::string_view fmt_str, A&&... fmt_args) {
    return Sfmt(fmt_str, {FmtArg(std::forward<A>(fmt_args))...});
}

/** @brief Computes Lua's floor division `floor(lhs / rhs)` for signed integers. */
inline long FloorDiv(long lhs, long rhs) {
    long quot = lhs / rhs;
    if ((lhs % rhs != 0) && ((lhs < 0) != (rhs < 0))) quot--;
    return quot;
}

/** @brief Computes Lua's floored modulo `lhs - floor(lhs / rhs) * rhs`. */
inline long LuaMod(long lhs, long rhs) {
    long rem = lhs % rhs;
    if (rem != 0 && ((rem < 0) != (rhs < 0))) rem += rhs;
    return rem;
}

// Files. Paths are resolved component-by-component ignoring ASCII case, as DOS
// did, since the original data files mix upper and lower case names.

/**
 * @brief Resolves a DOS-style path case-insensitively on disk.
 * @param path Target filesystem path (forward or backslash separated).
 * @return Resolved path on disk, or an empty `std::filesystem::path` when not found.
 */
std::filesystem::path ResolvePath(const std::filesystem::path& path);

/**
 * @brief Reads the entire text contents of `path` (resolved case-insensitively).
 * @param path Path to the text file on disk.
 * @return File contents as `std::string`, or an actionable `absl::Status` error.
 */
absl::StatusOr<std::string> ReadFile(const std::filesystem::path& path);

/**
 * @brief Reads the entire binary payload of `path` (resolved case-insensitively).
 * @param path Path to the binary file on disk.
 * @return Raw byte vector, or an actionable `absl::Status` error.
 */
absl::StatusOr<std::vector<uint8_t>> ReadBytes(const std::filesystem::path& path);

/** @brief True when `path` resolves to an existing file or directory on disk. */
bool FileExists(const std::filesystem::path& path);

/**
 * @brief Atomically writes `content` to `path` via a temporary sibling file.
 * @param path Destination file path.
 * @param content Text payload to write.
 * @return `absl::OkStatus()` on success, or an actionable error status.
 */
absl::Status WriteFile(const std::filesystem::path& path, std::string_view content);

/**
 * @brief Atomically writes binary `bytes` to `path` via a temporary sibling file.
 * @param path Destination file path.
 * @param bytes Binary payload to write.
 * @return `absl::OkStatus()` on success, or an actionable error status.
 */
absl::Status WriteBytes(const std::filesystem::path& path, absl::Span<const uint8_t> bytes);

/**
 * @brief Removes `path` from disk and invalidates the directory listing cache.
 * @param path File path to remove.
 * @return `absl::OkStatus()` when removed, or an actionable error status.
 */
absl::Status RemoveFile(const std::filesystem::path& path);

// Strings: byte-indexed, one character per byte, matching Lua strings.
/** @brief Returns byte `index` of `str` as an unsigned 8-bit integer. */
inline uint8_t ByteAt(std::string_view str, size_t index) {
    return static_cast<uint8_t>(str[index]);
}
/** @brief Converts ASCII characters in `str` to upper case. */
std::string Upper(std::string_view str);
/** @brief Converts ASCII characters in `str` to lower case. */
std::string Lower(std::string_view str);
/** @brief Strips trailing ASCII whitespace from `str`. */
std::string TrimRight(std::string_view str);
/** @brief Strips leading and trailing ASCII whitespace from `str`. */
std::string Trim(std::string_view str);
/** @brief Splits `str` on runs of CR and LF, dropping empty lines. */
std::vector<std::string> SplitLines(std::string_view str);
/** @brief Converts a Latin-1 byte string to UTF-8. */
std::string Latin1ToUtf8(std::string_view str);
/** @brief Converts a UTF-8 string to Latin-1 (replacing code points > 255 with `'?'`). */
std::string Utf8ToLatin1(std::string_view str);
/** @brief Parses a trimmed decimal integer via `std::from_chars`, or returns `std::nullopt`. */
std::optional<long> ToInt(std::string_view str);

/** @brief Returns elapsed monotonic time in seconds since first call. */
double Now();

/**
 * @brief Parses CLI flags via `absl::ParseCommandLine`, normalizing `--kebab-case`
 * flag names to `--snake_case` while preserving positional arguments (including
 * signed numeric literals such as `-1`).
 * @param argc Argument count from `main`.
 * @param argv Argument vector from `main`.
 * @param usage_message Optional program usage banner registered with Abseil flags.
 * @return Positional arguments after `argv[0]`, in command-line order.
 */
std::vector<std::string> ParseCliFlags(int argc, char** argv, std::string_view usage_message = "");

}  // namespace w2
