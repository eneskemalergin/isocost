#!/usr/bin/env bash
# Measure GNU grep and ripgrep on a copy of /usr/include with Zebrac.
#
#   examples/text-search/bench.sh
#
# Needs: zebrac, grep, rg. The corpus is copied to data/include (ignored by Git).
# Four searches: a common literal word, a case-insensitive word, a regular
# expression, and a rare word (found in only a few files, so nearly every byte
# is read without many matches). A pattern with no match cannot be used: both
# tools exit 1 when nothing matches, which Zebrac records as a failed run.
set -euo pipefail
cd -- "$(dirname -- "$0")"
ZEBRAC_FLAGS=(--quiet --duration "${DURATION_MS:-2000}" --min-samples "${MIN_SAMPLES:-10}" --warmup 2)
mkdir -p data results/search
[[ -d data/include ]] || cp -r /usr/include data/include
d=data/include
search() {
    local name=$1 grep_args=$2 rg_args=$3
    zebrac "${ZEBRAC_FLAGS[@]}" --json "results/search/$name.json" -- \
        "grep -r -c $grep_args $d" "rg -c --no-ignore -j1 $rg_args $d" "rg -c --no-ignore $rg_args $d"
}
search literal "malloc" "malloc"
search ignore-case "-i error" "-i error"
search regex "-E 'struct[[:space:]]+[a-z_]+[[:space:]]*\{'" "'struct\s+[a-z_]+\s*\{'"
search rare-word "EOWNERDEAD" "EOWNERDEAD"
echo "text-search: wrote results/search"
