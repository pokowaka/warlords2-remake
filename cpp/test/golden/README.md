# Golden files

What the C++ port played and made when these were recorded: the refactor's
safety net (`cpp/test/golden.sh`). `bazel test //cpp:golden_test` checks that
the same games and worlds come out; `bazel run //cpp:golden_update` records
them anew, which only a change meant to alter the games or worlds should do.
