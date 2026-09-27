#!/usr/bin/env bash
# Render every example into examples/<name>/generated/ (ignored by Git).
#
#   examples/render.sh [isocost options...]
#
# Extra options are passed to every run, for example `--size paper`.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
BIN=${ISOCOST:-build/isocost}
[[ -x $BIN ]] || make -s build/isocost
status=0
for dir in examples/*; do
    name=$(basename "$dir")
    [[ -f $dir/isocost.toml && -d $dir/results ]] || continue
    if "$BIN" report "$dir/results" -o "$dir/generated" --force -q "$@"; then
        printf '%-16s %s\n' "$name" "$dir/generated/report.md"
    else
        printf '%-16s failed with exit status %d\n' "$name" "$?"
        status=1
    fi
done
exit "$status"
