"""Compiler options for the project's own code."""

# Warnings for our code. The two -Wno- are the CMake build's, kept for now;
# the refactor (docs/STYLE.md) takes them away once the code is clean.
# -Wthread-safety is a Clang-specific warning not supported by GCC.
W2_COPTS = [
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-sign-compare",
    "-DJSON_NOEXCEPTION=1",
] + select({
    "@rules_cc//cc/compiler:clang": ["-Wthread-safety"],
    "//conditions:default": [],
})
