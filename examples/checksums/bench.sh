#!/usr/bin/env bash
# Measure six checksum tools on three file sizes with Zebrac.
#
#   examples/checksums/bench.sh
#
# Needs: zebrac and GNU coreutils (md5sum, sha1sum, sha256sum, sha512sum, b2sum, cksum).
# Inputs are random bytes under data/ (ignored by Git): 4 MiB, 64 MiB, 256 MiB.
set -euo pipefail
cd -- "$(dirname -- "$0")"
ZEBRAC_FLAGS=(--quiet --duration "${DURATION_MS:-2000}" --min-samples "${MIN_SAMPLES:-10}" --warmup 2)
mkdir -p data results/checksum
for mib in 4 64 256; do
    f=data/random-${mib}m.bin
    [[ -s $f ]] || (set +o pipefail; head -c $((mib * 1024 * 1024)) /dev/urandom > "$f")
    zebrac "${ZEBRAC_FLAGS[@]}" --json "results/checksum/random-${mib}m.json" -- \
        "md5sum $f" "sha1sum $f" "sha256sum $f" "sha512sum $f" "b2sum $f" "cksum $f"
done
echo "checksums: wrote results/checksum"
