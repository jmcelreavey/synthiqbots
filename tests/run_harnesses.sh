#!/usr/bin/env bash
# Compile and run every standalone C++ harness (bare g++, no AzerothCore): tests/jev/harness_*.cpp and
# tests/director/harness_*.cpp. Each harness also has -DINJECT_* arms that must FAIL; this runs the plain build.
#
#   tests/run_harnesses.sh
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
failed=0
for source in tests/jev/harness_*.cpp tests/director/harness_*.cpp; do
    name=$(basename "$source" .cpp)
    g++ -std=c++17 -Wall -Wextra -I src -I deps "$source" -o "$out/$name"
    if "$out/$name" > "$out/$name.log" 2>&1; then
        echo "ok    $name"
    else
        echo "FAIL  $name"
        cat "$out/$name.log"
        failed=1
    fi
done
exit $failed
