#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_build=$(mktemp -d "${TMPDIR:-/tmp}/arduremoteid-cn-tests.XXXXXX")
trap 'rm -rf "$test_build"' EXIT
cmake -S libraries/opendroneidcn -B "$test_build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$test_build"
(cd "$test_build" && ctest --output-on-failure)
python3 -m unittest discover -s tests -p 'test_*.py' -v
python3 -m unittest discover -s scripts/tests -p 'test_*.py' -v
