#!/usr/bin/env bash
# Run the dedicated CMake analysis preset.
#
# clang-tidy needs a real compilation command, so it is never run from an
# ordinary commit hook. This is the explicitly invoked manual path that
# delegates to the same presets CI uses.
set -euo pipefail

preset="ci-tidy-linux"
if [[ "$(uname -s)" == "Darwin" ]]; then
    preset="ci-tidy-macos"
fi

echo "Configuring and building with preset '${preset}'..."
cmake --preset "${preset}"
cmake --build --preset "${preset}"
