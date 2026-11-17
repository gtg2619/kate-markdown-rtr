#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${KATE_MARKDOWN_BUILD_DIR:-$project_dir/build}"
mkdir -p "$build_dir"
build_dir="$(cd -- "$build_dir" && pwd)"
exec 9>"$build_dir/.test.lock"
flock 9
cmake -S "$project_dir" -B "$build_dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build_dir" --parallel "${KATE_MARKDOWN_BUILD_JOBS:-4}"
ctest --test-dir "$build_dir" --output-on-failure --output-junit "$build_dir/test-results.xml"
