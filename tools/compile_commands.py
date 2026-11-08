#!/usr/bin/env python3
"""Writes compile_commands.json for clangd and clang-tidy from Bazel's own
compile actions (bazel aquery), so the editor and the linter see exactly the
flags the build uses.

    python3 tools/compile_commands.py          # from anywhere in the repository

The file goes to the repository root. External code (SDL, Vorbis) is left
out: only our own sources under cpp/ are listed.
"""

import json
import os
import subprocess
import sys


def main() -> int:
    root = subprocess.run(["bazel", "info", "workspace"], check=True, capture_output=True,
                          text=True).stdout.strip()
    execroot = subprocess.run(["bazel", "info", "execution_root"], check=True, cwd=root,
                              capture_output=True, text=True).stdout.strip()
    # build first, so generated headers and external repositories exist
    subprocess.run(["bazel", "build", "//cpp/..."], check=True, cwd=root)
    query = subprocess.run(
        ["bazel", "aquery", 'mnemonic("CppCompile", //cpp/...)', "--output=jsonproto"],
        check=True, cwd=root, capture_output=True, text=True).stdout
    actions = json.loads(query).get("actions", [])

    # On macOS Bazel compiles through apple_support's wrapper, with
    # placeholders it fills in at run time; tools need the real compiler and
    # paths, or they can't find the standard library.
    substitutions = {}
    compiler = None
    if sys.platform == "darwin":
        def xcrun(*args):
            return subprocess.run(["xcrun", *args], check=True, capture_output=True,
                                  text=True).stdout.strip()
        substitutions = {
            "__BAZEL_XCODE_SDKROOT__": xcrun("--show-sdk-path"),
            "__BAZEL_XCODE_DEVELOPER_DIR__": subprocess.run(
                ["xcode-select", "-p"], check=True, capture_output=True, text=True).stdout.strip(),
            "__BAZEL_EXECUTION_ROOT__": execroot,
        }
        compiler = xcrun("--find", "clang++")

    def fill(argument):
        for placeholder, value in substitutions.items():
            argument = argument.replace(placeholder, value)
        return argument

    entries = []
    for action in actions:
        arguments = [fill(arg) for arg in action["arguments"]]
        if compiler and "wrapped_clang" in arguments[0]:
            arguments[0] = compiler
        source = next((arg for arg in arguments[1:] if arg.endswith(".cpp")), None)
        if source is None or not source.startswith("cpp/"):
            continue
        entries.append({
            "directory": execroot,
            "file": os.path.join(root, source),
            "arguments": arguments,
        })

    out = os.path.join(root, "compile_commands.json")
    with open(out, "w", encoding="utf-8") as handle:
        json.dump(entries, handle, indent=1)
    print(f"wrote {len(entries)} entries to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
