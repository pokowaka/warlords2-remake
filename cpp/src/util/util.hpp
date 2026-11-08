// Shared utilities providing Lua-compatible formatting, sorting, case-insensitive
// file lookup, string helpers, and monotonic timing.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace w2 {

/** @brief Formats a string using `printf`-style format specifiers. */
std::string Fmt(const char* fmt_str, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 1, 2)))
#endif
        ;

/**
 * @brief Sorts `vec` using Lua 5.1 / LuaJIT's unstable quicksort (`auxsort`).
 *
 * Matches the exact tie-breaking order of the Lua reference so seeded games
 * and world generation remain bit-identical across ports. `less_than(lhs, rhs)`
 * returns true when `lhs` sorts before `rhs`.
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
                    if (mid > up_idx)
                        throw std::runtime_error("invalid order function for sorting");
                }
                while (less_than(pivot, get(--right))) {
                    if (right < low) throw std::runtime_error("invalid order function for sorting");
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
    explicit FmtArg(const std::string& val) : is_str(true), str_val(val) {}
    explicit FmtArg(const char* val) : is_str(true), str_val(val ? val : "") {}
};
/**
 * @brief Formats `fmt_str` safely for format strings loaded from `STRING.DAT`
 * (`%d`, `%s`, `%x`, `%c`, and `%%` with flags and widths).
 */
std::string Sfmt(const std::string& fmt_str, const std::vector<FmtArg>& args);
/** @brief Variadic wrapper around `Sfmt`. */
template <class... A>
std::string Format(const std::string& fmt_str, A&&... fmt_args) {
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

/** @brief Resolves a DOS-style path case-insensitively on disk, or returns `""` if not found. */
std::string ResolvePath(const std::string& path);
/** @brief Reads the entire contents of `path` (from memory overlay or disk), or `std::nullopt`. */
std::optional<std::string> ReadFile(const std::string& path);
/** @brief Reads the entire contents of `path`, or throws `std::runtime_error` if missing. */
std::string MustRead(const std::string& path);
/** @brief True when `path` exists in the in-memory overlay or on disk. */
bool FileExists(const std::string& path);
/** @brief Atomically writes `bytes` to `path` via a temporary file. */
bool WriteFile(const std::string& path, const std::string& bytes);
/**
 * @brief Installs `bytes` under `path` in the in-memory file overlay, taking
 * precedence over disk files (used for generated random maps).
 */
void InstallFile(const std::string& path, const std::string& bytes);
/** @brief Removes `path` from disk and clears the directory listing cache. */
bool RemoveFile(const std::string& path);

// Strings: byte-indexed, one character per byte, matching Lua strings.
/** @brief Returns byte `index` of `str` as an unsigned 8-bit integer. */
inline uint8_t ByteAt(const std::string& str, size_t index) {
    return static_cast<uint8_t>(str[index]);
}
/** @brief Converts ASCII characters in `str` to upper case. */
std::string Upper(std::string str);
/** @brief Converts ASCII characters in `str` to lower case. */
std::string Lower(std::string str);
/** @brief Strips trailing ASCII whitespace from `str`. */
std::string TrimRight(const std::string& str);
/** @brief Strips leading and trailing ASCII whitespace from `str`. */
std::string Trim(const std::string& str);
/** @brief Splits `str` on runs of CR and LF, dropping empty lines. */
std::vector<std::string> SplitLines(const std::string& str);
/** @brief True when `str` starts with `prefix`. */
bool StartsWith(const std::string& str, const std::string& prefix);
/** @brief True when `str` ends with `suffix`. */
bool EndsWith(const std::string& str, const std::string& suffix);
/** @brief True when `str` contains `needle`. */
bool Contains(const std::string& str, const std::string& needle);
/** @brief Converts a Latin-1 byte string to UTF-8. */
std::string Latin1ToUtf8(const std::string& str);
/** @brief Converts a UTF-8 string to Latin-1 (replacing code points > 255 with `'?'`). */
std::string Utf8ToLatin1(const std::string& str);
/** @brief Parses a trimmed decimal integer (like Lua's `tonumber`), or returns `std::nullopt`. */
std::optional<long> ToInt(const std::string& str);

/** @brief Returns elapsed monotonic time in seconds since process start. */
double Now();

}  // namespace w2
