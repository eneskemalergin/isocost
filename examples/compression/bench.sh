#!/usr/bin/env bash
# Measure seven common compressors with Zebrac and write results/ for isocost.
#
#   examples/compression/bench.sh
#
# Needs: zebrac, gzip, pigz, zstd, xz, bzip2, lz4, brotli, tar.
# Inputs are built under data/ (ignored by Git, 16 MiB each):
#   source.tar   C headers from /usr/include
#   binary.bin   executables from /usr/bin
#   reads.fastq  sequencing reads from $FASTQ (skipped when unset or missing)
# Each input gets one Zebrac run per direction. Zebrac interleaves the
# commands round by round, so every tool sees the same machine conditions.
set -euo pipefail
cd -- "$(dirname -- "$0")"
ZEBRAC_FLAGS=(--quiet --duration "${DURATION_MS:-2000}" --min-samples "${MIN_SAMPLES:-10}" --warmup 2)
SIZE=$((16 * 1024 * 1024))
mkdir -p data results/compress results/decompress

# head closes each pipe early on purpose, so these pipelines ignore SIGPIPE.
[[ -s data/source.tar ]] || (set +o pipefail; tar -cf - -C /usr/include . 2>/dev/null | head -c "$SIZE" > data/source.tar)
[[ -s data/binary.bin ]] || (set +o pipefail; find /usr/bin -maxdepth 1 -type f -size +200k -print0 | sort -z | xargs -0 cat 2>/dev/null | head -c "$SIZE" > data/binary.bin)
inputs=(source.tar binary.bin)
if [[ -n ${FASTQ:-} && -s ${FASTQ:-} ]]; then
    [[ -s data/reads.fastq ]] || head -c "$SIZE" "$FASTQ" > data/reads.fastq
    inputs+=(reads.fastq)
fi

for input in "${inputs[@]}"; do
    name=${input%.*}
    f=data/$input
    zebrac "${ZEBRAC_FLAGS[@]}" --json "results/compress/$name.json" -- \
        "gzip -6 -c $f" "pigz -6 -c $f" "zstd -3 -c $f" "xz -6 -c $f" \
        "bzip2 -9 -c $f" "lz4 -1 -c $f" "brotli -q 6 -c $f"
    [[ -s $f.gz ]] || gzip -6 -c "$f" > "$f.gz"
    [[ -s $f.zst ]] || zstd -q -3 -c "$f" > "$f.zst"
    [[ -s $f.xz ]] || xz -6 -c "$f" > "$f.xz"
    [[ -s $f.bz2 ]] || bzip2 -9 -c "$f" > "$f.bz2"
    [[ -s $f.lz4 ]] || lz4 -q -1 -c "$f" > "$f.lz4"
    [[ -s $f.br ]] || brotli -q 6 -c "$f" > "$f.br"
    zebrac "${ZEBRAC_FLAGS[@]}" --json "results/decompress/$name.json" -- \
        "gzip -d -c $f.gz" "pigz -d -c $f.gz" "zstd -d -c $f.zst" "xz -d -c $f.xz" \
        "bzip2 -d -c $f.bz2" "lz4 -d -c $f.lz4" "brotli -d -c $f.br"
done
echo "compression: wrote results/compress and results/decompress"
