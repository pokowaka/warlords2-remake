#!/bin/sh
# Runs clang-tidy (cpp/.clang-tidy) over the C++ and prints a count per
# check, then the findings. From anywhere in the repository:
#
#   sh tools/tidy.sh                  # every file under cpp/
#   sh tools/tidy.sh cpp/src/ui/      # a part of it (a path regex)
#
# Needs LLVM's clang-tidy (brew install llvm) and compile_commands.json,
# which tools/compile_commands.py writes.
set -eu
root=$(git rev-parse --show-toplevel)
llvm=${LLVM:-$(brew --prefix llvm 2>/dev/null || echo /usr)}/bin
[ -f "$root/compile_commands.json" ] || python3 "$root/tools/compile_commands.py"
out=$(mktemp)
# from cpp/, so run-clang-tidy finds cpp/.clang-tidy
cd "$root/cpp"
"$llvm/run-clang-tidy" -clang-tidy-binary "$llvm/clang-tidy" -p "$root" -j 10 -quiet "${1:-cpp/}" 2>&1 |
  grep -E ":[0-9]+:[0-9]+: (error|warning): " | sed -E 's#^.*(cpp/(src|test)/)#\1#' | sort -u > "$out" || true
sed -E 's/.*\[([a-z0-9.-]+)(,-warnings-as-errors)?\]$/\1/' "$out" | sort | uniq -c | sort -rn
cat "$out"
total=$(wc -l < "$out" | tr -d ' ')
rm -f "$out"
echo "$total findings"
[ "$total" -eq 0 ]
