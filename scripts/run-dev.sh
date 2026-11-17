#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${KATE_MARKDOWN_BUILD_DIR:-$project_dir/build}"
if [[ ! -f "$build_dir/bin/kf6/ktexteditor/katemarkdownlive.so" ]]; then
    echo 'Build the plugin first; see README.md.' >&2
    exit 1
fi
export QT_PLUGIN_PATH="$build_dir/bin${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
exec kate --new --startanon "$@"
