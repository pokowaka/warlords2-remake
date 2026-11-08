// What Lua gave the remake for free and C++ does not: string.format, Lua
// 5.1's table.sort, files read whole, and a few string helpers.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace w2 {

/** string.format, as printf. */
std::string Fmt(const char* f, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 1, 2)))
#endif
        ;

/**
 * Lua 5.1's table.sort, as LuaJIT runs it (lib_table.c's auxsort): not
 * stable, so arrays with equal keys come out in the order the Lua remake
 * leaves them, which keeps a seeded game the same in both. `lt(a, b)` is "a
 * sorts before b".
 */
template <class T, class Lt>
void LuaSort(std::vector<T>& v, Lt lt) {
    // 1-based, as the original algorithm is written
    auto get = [&](long i) -> T& { return v[i - 1]; };
    auto swap = [&](long i, long j) { std::swap(v[i - 1], v[j - 1]); };
    std::function<void(long, long)> auxsort = [&](long l, long u) {
        while (l < u) {
            if (lt(get(u), get(l))) swap(l, u);
            if (u - l == 1) break;
            long i = (l + u) / 2;
            if (lt(get(i), get(l)))
                swap(i, l);
            else if (lt(get(u), get(i)))
                swap(i, u);
            if (u - l == 2) break;
            T p = get(i);
            swap(i, u - 1);
            i = l;
            long j = u - 1;
            for (;;) {
                while (lt(get(++i), p)) {
                    if (i > u) throw std::runtime_error("invalid order function for sorting");
                }
                while (lt(p, get(--j))) {
                    if (j < l) throw std::runtime_error("invalid order function for sorting");
                }
                if (j < i) break;
                swap(i, j);
            }
            swap(u - 1, i);
            if (i - l < u - i) {
                j = l;
                i = i - 1;
                l = i + 2;
            } else {
                j = i + 1;
                i = u;
                u = j - 2;
            }
            auxsort(j, i);
        }
    };
    auxsort(1, static_cast<long>(v.size()));
}

template <class T>
void LuaSort(std::vector<T>& v) {
    luaSort(v, [](const T& a, const T& b) { return a < b; });
}

/** An argument to sfmt: a number or a string. */
struct FmtArg {
    bool str = false;
    long n = 0;
    std::string s;
    explicit FmtArg(int v) : n(v) {}
    explicit FmtArg(long v) : n(v) {}
    explicit FmtArg(unsigned v) : n(static_cast<long>(v)) {}
    explicit FmtArg(unsigned long v) : n(static_cast<long>(v)) {}
    explicit FmtArg(long long v) : n(static_cast<long>(v)) {}
    explicit FmtArg(const std::string& v) : str(true), s(v) {}
    explicit FmtArg(const char* v) : str(true), s(v ? v : "") {}
};
/** string.format for a format string from the game's own files: %d, %s, %x,
 *  %c and %%, with flags and widths, read safely whatever the arguments. */
std::string Sfmt(const std::string& f, const std::vector<FmtArg>& args);
template <class... A>
std::string Format(const std::string& f, A&&... a) {
    return Sfmt(f, {FmtArg(std::forward<A>(a))...});
}

/** Lua's floor division and modulo, for numbers that may be negative. */
inline long FloorDiv(long a, long b) {
    long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
inline long LuaMod(long a, long b) {
    long m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b;
    return m;
}

// Files. A path is looked up ignoring case, component by component, as DOS
// did -- the data files' names are written every which way.

/** The real path a DOS-style path resolves to, or "" if there is none. */
std::string ResolvePath(const std::string& path);
/** The whole file, or nothing. */
std::optional<std::string> ReadFile(const std::string& path);
/** The whole file, or an exception naming it. */
std::string MustRead(const std::string& path);
bool FileExists(const std::string& path);
bool WriteFile(const std::string& path, const std::string& bytes);
/** Put a file in memory, where readFile and fileExists find it before the
 *  disk: a random world's scenario, which would otherwise overwrite the last
 *  one the original made. Matched ignoring case, as on disk. */
void InstallFile(const std::string& path, const std::string& bytes);
bool RemoveFile(const std::string& path);

// Strings: bytes, one character a byte, as Lua strings are.
inline uint8_t ByteAt(const std::string& s, size_t i) {
    return static_cast<uint8_t>(s[i]);
}
std::string Upper(std::string s);
std::string Lower(std::string s);
std::string TrimRight(const std::string& s);
std::string Trim(const std::string& s);
/** Split on runs of CR and LF, dropping empty lines. */
std::vector<std::string> SplitLines(const std::string& s);
bool StartsWith(const std::string& s, const std::string& p);
bool EndsWith(const std::string& s, const std::string& p);
bool Contains(const std::string& s, const std::string& p);
/** Latin-1 to UTF-8, and back (characters past Latin-1 become '?'). */
std::string Latin1ToUtf8(const std::string& s);
std::string Utf8ToLatin1(const std::string& s);
/** Lua's tonumber for a decimal integer, or nothing. */
std::optional<long> ToInt(const std::string& s);

/** Wall time in seconds, for what plays out by the clock. */
double Now();

}  // namespace w2
