#!/usr/bin/env bash
# isocost test suite: `make test` or tests/run.sh
#
# Uses only files in this repository: examples/*/results (real Zebrac output)
# and tests/fixtures (hand-written edge cases). Scratch files go to a
# temporary directory that is removed on exit.
# ok always returns 0, so `check && ok ... || bad ...` never runs both.
# shellcheck disable=SC2015,SC2001
set -uo pipefail
export LC_ALL=C

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 1
BIN=build/isocost
SAN=build/isocost-sanitize
WORK=$(mktemp -d "${TMPDIR:-/tmp}/isocost-test.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
make -s build/isocost build/isocost-sanitize || exit 1

pass=0
fail=0
ok() { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n' "$1"; [[ -n ${2:-} ]] && sed 's/^/      /' <<< "$2"; }

# expect NAME STATUS [grep-pattern-on-stderr] -- isocost args...
expect() {
    local name=$1 want=$2 pattern=$3
    shift 4
    local out status=0
    out=$("$BIN" "$@" 2>&1) || status=$?
    if [[ $status != "$want" ]]; then
        bad "$name (exit $status, expected $want)" "$out"
    elif [[ -n $pattern ]] && ! grep -q -- "$pattern" <<< "$out"; then
        bad "$name (message lacks '$pattern')" "$out"
    else
        ok "$name"
    fi
}

# ---- every example renders all views and formats ----------------------------------
for dir in examples/*; do
    [[ -f $dir/isocost.toml ]] || continue
    name=$(basename "$dir")
    expect "example $name" 0 "" -- report "$dir/results" -o "$WORK/ex-$name" -q
    for f in "$WORK/ex-$name"/*-overview.png; do
        base=${f%-overview.png}
        for ext in png svg pdf; do
            [[ -s $base-overview.$ext && -s $base-workloads.$ext ]] || bad "example $name: missing $base-*.$ext"
        done
    done
done

# ---- golden values from tests/expected/golden.tsv -----------------------------------------
expect "golden fixture renders" 0 "" -- report tests/fixtures/golden -o "$WORK/golden" --no-config --baseline z-flate --data -q
diff_out=$(awk -F '\t' '
    NR == FNR { if (FNR > 1) { want[$1 "|" $2] = $3 "|" $4 }; next }
    FNR == 1 { next }
    { key = $2 "|" $3; if (!(key in want)) next; split(want[key], w, "|")
      dx = ($5 - w[1]) / w[1]; dy = ($6 - w[2]) / w[2]; if (dx < 0) dx = -dx; if (dy < 0) dy = -dy
      if (dx > 1e-9 || dy > 1e-9) printf "%s: got %s %s, want %s %s\n", key, $5, $6, w[1], w[2]; seen++ }
    END { if (seen != 9) printf "compared %d of 9 expected rows\n", seen }' tests/expected/golden.tsv "$WORK/golden/points.tsv")
[[ -z $diff_out ]] && ok "golden ratios and geometric means match to 1e-9" || bad "golden values" "$diff_out"

# ---- every config key, non-default ---------------------------------------------------------
expect "full config: every key" 0 "" -- report tests/fixtures/golden --config tests/fixtures/config/full.toml -o "$WORK/full" -q
grep -q 'Custom note text' "$WORK/full/report.md" 2>/dev/null || grep -q 'Golden fixture' "$WORK/full/report.md" && ok "full config: report written" || bad "full config: report"

# ---- config errors name the line and suggest a fix -------------------------------------------
expect "config: unknown key suggests the closest" 2 "did you mean 'title'" -- config --check tests/fixtures/config/unknown-key.toml
expect "config: invalid color" 2 'is not a color' -- config --check tests/fixtures/config/bad-color.toml
expect "config: misspelled shape" 2 'did you mean "circle"' -- config --check tests/fixtures/config/bad-shape.toml
expect "config: unknown size unit" 2 'not px, mm, or in' -- config --check tests/fixtures/config/bad-size.toml
expect "config: TOML syntax error with line and column" 2 'syntax.toml:2:22: unterminated string' -- config --check tests/fixtures/config/syntax.toml
expect "config: duplicate tool id" 2 'also defined on line 1' -- config --check tests/fixtures/config/duplicate-tool.toml
expect "config: error stops a report" 2 'did you mean' -- report tests/fixtures/golden --config tests/fixtures/config/unknown-key.toml -o "$WORK/x"

# ---- the TOML examples in wiki/Configuration.md form one valid config ---------------------------------
awk '/^```toml/{f=1;next} /^```/{f=0} f' wiki/Configuration.md > "$WORK/doc-examples.toml"
expect "wiki/Configuration.md: every TOML example parses" 0 "valid" -- config --check "$WORK/doc-examples.toml"

# ---- printed defaults round-trip through the parser ----------------------------------------------
"$BIN" config > "$WORK/defaults.toml"
"$BIN" config --check "$WORK/defaults.toml" --print > "$WORK/reprinted.toml" 2>&1
if diff <(tail -n +4 "$WORK/defaults.toml") <(tail -n +4 "$WORK/reprinted.toml") > /dev/null; then ok "config: defaults print, parse, and reprint identically"
else bad "config: default round trip" "$(diff <(tail -n +4 "$WORK/defaults.toml") <(tail -n +4 "$WORK/reprinted.toml") | head)"; fi

# ---- input failures -------------------------------------------------------------------------------
expect "unsupported schema_version" 2 'unsupported Zebrac schema_version' -- report tests/fixtures/unsupported-schema -o "$WORK/u" --no-config
expect "truncated JSON names the byte" 2 'truncated.json: at byte' -- report tests/fixtures/malformed -o "$WORK/m" --no-config
mkdir -p "$WORK/empty"
expect "no Zebrac results" 3 'no Zebrac results' -- report "$WORK/empty" -o "$WORK/e" --no-config
expect "unknown reference" 3 "reference 'nope' is not in this figure" -- report tests/fixtures/golden -o "$WORK/nr" --no-config --baseline nope
grep -q 'Not drawn' "$WORK/nr/report.md" && grep -q '"status": "not_drawn"' "$WORK/nr/summary.json" && ok "unknown reference: report.md and summary.json say not drawn" || bad "unknown reference: report files"
mkdir -p "$WORK/stale/results"
printf '{"schema_version":"bench.meta.v1","raw_json":"results/perf_x_20260101_000000/gone.json","suite":"s","section":"perf_x","workload":"w","tool":"t"}\n' > "$WORK/stale/results/metadata.jsonl"
expect "stale sidecar" 3 'points to a missing file' -- report "$WORK/stale/results/metadata.jsonl" -o "$WORK/s" --no-config
mkdir -p "$WORK/dup/results/perf_x_20260101_000000"
cp tests/fixtures/golden/small-fastq--isa-l.json "$WORK/dup/results/perf_x_20260101_000000/a.json"
cp tests/fixtures/golden/small-fastq--lean.json "$WORK/dup/results/perf_x_20260101_000000/b.json"
for f in a b; do printf '{"schema_version":"bench.meta.v1","raw_json":"results/perf_x_20260101_000000/%s.json","suite":"s","section":"perf_x","workload":"w","tool":"same"}\n' "$f"; done > "$WORK/dup/results/metadata.jsonl"
expect "duplicate identity" 3 'duplicate result for workload' -- report "$WORK/dup/results" -o "$WORK/d" --no-config
expect "reader: reordered keys and failed samples" 0 "" -- report tests/fixtures/variants -o "$WORK/v" --no-config -q
expect "existing report needs --force" 2 'pass --force' -- report tests/fixtures/golden -o "$WORK/golden" --no-config
FORCE=$WORK/force
"$BIN" report examples/compression/results -o "$FORCE" --format png,svg --data -q
echo keep > "$FORCE/notes.txt"
"$BIN" report examples/compression/results -o "$FORCE" --format pdf --group compress --force -q
left=$(cd "$FORCE" && echo *)
if [[ $left == "compress-overview.pdf compress-workloads.pdf notes.txt report.md summary.json" ]]; then
    ok "--force removes the previous report's figures and points.tsv, and nothing else"
else
    bad "--force left: $left"
fi
expect "output directory is required" 2 'choose an output directory' -- report tests/fixtures/golden --no-config
expect "unknown option" 2 'unknown option' -- report tests/fixtures/golden -o "$WORK/o" --bogus

# ---- identity rules on the real examples ------------------------------------------------------------
for pair in "fastq-sampling:sidecar=120" "fasta-stats:sidecar=61" "crc32:workload.tsv=42" "compression:config=42" "checksums:config=18" "text-search:config=12"; do
    name=${pair%%:*} want=${pair#*:}
    "$BIN" list "examples/$name/results" -q 2>/dev/null | grep -q "$want" && ok "identity: $name uses $want" || bad "identity: $name ($want)" "$("$BIN" list "examples/$name/results" 2>&1 | tail -2)"
done

# ---- determinism: identical bytes from identical input --------------------------------------------------
"$BIN" report examples/fastq-sampling/results -o "$WORK/det1" -q
"$BIN" report examples/fastq-sampling/results -o "$WORK/det2" -q
same=1
for f in "$WORK/det1"/*.png "$WORK/det1"/*.svg "$WORK/det1"/*.pdf "$WORK/det1/summary.json"; do cmp -s "$f" "$WORK/det2/$(basename "$f")" || { same=0; echo "      differs: $(basename "$f")"; }; done
diff <(grep -v -e 'Made by isocost' -e '^- Command:' "$WORK/det1/report.md") <(grep -v -e 'Made by isocost' -e '^- Command:' "$WORK/det2/report.md") > /dev/null || same=0
((same)) && ok "determinism: PNG, SVG, PDF, summary.json, report.md" || bad "determinism"

# ---- output validity with independent tools ------------------------------------------------------
if command -v qpdf > /dev/null; then
    bad_pdf=$(for f in "$WORK"/ex-*/*.pdf "$WORK/full"/*.pdf; do qpdf --check "$f" > /dev/null 2>&1 || echo "$f"; done)
    [[ -z $bad_pdf ]] && ok "qpdf --check: every PDF" || bad "qpdf --check" "$bad_pdf"
else echo "skip  qpdf not installed"; fi
if command -v xmllint > /dev/null; then
    bad_svg=$(for f in "$WORK"/ex-*/*.svg; do xmllint --noout "$f" 2> /dev/null || echo "$f"; done)
    [[ -z $bad_svg ]] && ok "xmllint: every SVG is well-formed XML" || bad "xmllint" "$bad_svg"
else echo "skip  xmllint not installed"; fi
if command -v python3 > /dev/null && python3 -c 'import PIL' 2> /dev/null; then
    out=$(python3 - "$WORK" <<'PY'
import sys, pathlib, json
from PIL import Image
n = 0
for p in pathlib.Path(sys.argv[1]).rglob("*.png"):
    im = Image.open(p); im.load(); assert im.mode == "RGB", p; n += 1
for p in pathlib.Path(sys.argv[1]).rglob("summary.json"):
    assert json.load(open(p))["schema"] == "isocost.report.v1", p
print(n)
PY
) && ok "Pillow decodes all $out PNGs; every summary.json parses" || bad "PNG or JSON validity" "$out"
else echo "skip  Pillow not installed"; fi

# ---- sanitizers --------------------------------------------------------------------------------------
san_out=""
for args in "examples/fastq-sampling/results" "examples/crc32/results" "tests/fixtures/malformed" "$WORK/dup/results" "tests/fixtures/variants"; do
    status=0
    out=$(ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$SAN" report "$args" -o "$WORK/san-$RANDOM" --format png,svg,pdf --no-config -q 2>&1) || status=$?
    if grep -q -e 'Sanitizer' -e 'runtime error' <<< "$out" || ((status > 3)); then san_out+="$args: $out"$'\n'; fi
done
cfg=$(ASAN_OPTIONS=detect_leaks=1 "$SAN" config --check tests/fixtures/config/full.toml 2>&1)
grep -q -e 'Sanitizer' -e 'runtime error' <<< "$cfg" && san_out+="config: $cfg"
[[ -z $san_out ]] && ok "AddressSanitizer, LeakSanitizer, UBSan: 5 inputs and a config" || bad "sanitizers" "$san_out"

# ---- speed: all examples in one run ----------------------------------------------------------------
start=$(date +%s%N)
for dir in examples/*; do [[ -f $dir/isocost.toml ]] && "$BIN" report "$dir/results" -o "$WORK/speed-$(basename "$dir")" -q --format png; done
ms=$(( ($(date +%s%N) - start) / 1000000 ))
((ms < 2000)) && ok "speed: all six examples, both views, PNG, in $ms ms (limit 2000)" || bad "speed: $ms ms (limit 2000)"

echo
echo "$pass passed, $fail failed"
((fail == 0))
