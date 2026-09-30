#!/usr/bin/env bash
# Read the version, extract its changelog entry, or build and test a static release archive.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

version=$(sed -n 's/^#define ISOCOST_VERSION "\([^"]*\)"$/\1/p' src/model.h)
if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    printf 'error: src/model.h must define one major.minor.patch ISOCOST_VERSION\n' >&2
    exit 1
fi

case "${1:-}" in
    version)
        printf '%s\n' "$version"
        ;;
    notes)
        tag="${2:-v$version}"
        if [[ "$tag" != "v$version" ]]; then
            printf 'error: tag %s does not match ISOCOST_VERSION v%s\n' "$tag" "$version" >&2
            exit 1
        fi
        awk -v version="$version" '
            /^## / {
                if (found) exit
                found = ($2 == "[" version "]")
                next
            }
            found {
                print
                if ($0 ~ /[^[:space:]]/ && $0 !~ /^#/) nonempty = 1
            }
            END { if (!found || !nonempty) exit 1 }
        ' CHANGELOG.md || {
            printf 'error: CHANGELOG.md needs a nonempty [%s] entry\n' "$version" >&2
            exit 1
        }
        ;;
    package)
        arch="${2:?usage: release.sh package x86_64|aarch64 OUTPUT_DIRECTORY}"
        output="${3:?usage: release.sh package x86_64|aarch64 OUTPUT_DIRECTORY}"
        case "$arch" in
            x86_64) binary=build/isocost-static ;;
            aarch64) binary=build/isocost-aarch64-linux-musl ;;
            *) printf 'error: unknown architecture %s\n' "$arch" >&2; exit 1 ;;
        esac
        # The archive is tested by running it, so it must match this machine.
        if [[ "$(uname -m)" != "$arch" ]]; then
            printf 'error: cannot test a %s archive on %s\n' "$arch" "$(uname -m)" >&2
            exit 1
        fi
        name="isocost-$version-$arch-linux"
        mkdir -p "$output"
        archive="$(cd "$output" && pwd)/$name.tar.gz"
        if [[ -e "$archive" ]]; then
            printf 'error: archive already exists: %s\n' "$archive" >&2
            exit 1
        fi
        work=$(mktemp -d "${TMPDIR:-/tmp}/isocost-release.XXXXXX")
        trap 'rm -rf -- "$work"' EXIT

        make static TARGET="$arch-linux-musl"
        mkdir "$work/$name" "$work/unpacked"
        cp "$binary" "$work/$name/isocost"
        cp LICENSE THIRD_PARTY_NOTICES.md README.md CHANGELOG.md "$work/$name/"
        tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
            -czf "$work/$name.tar.gz" -C "$work" "$name"
        tar -xzf "$work/$name.tar.gz" -C "$work/unpacked"

        bin="$work/unpacked/$name/isocost"
        file "$bin" | grep -q 'statically linked'
        actual=$(env -i PATH=/usr/bin:/bin "$bin" --version 2> "$work/stderr")
        printf '%s\n' "$actual"
        test "$actual" = "isocost $version"
        test ! -s "$work/stderr"
        cmp LICENSE "$work/unpacked/$name/LICENSE"
        cmp THIRD_PARTY_NOTICES.md "$work/unpacked/$name/THIRD_PARTY_NOTICES.md"

        # The printed defaults must pass the binary's own check.
        env -i PATH=/usr/bin:/bin "$bin" config > "$work/isocost.toml"
        env -i PATH=/usr/bin:/bin "$bin" config --check "$work/isocost.toml" | grep -q ": valid ("

        # Render one example in every format; the PNG must match the README figure byte for byte.
        env -i PATH=/usr/bin:/bin "$bin" report examples/compression/results -o "$work/report" \
            --format png,svg,pdf -q
        for group in compress decompress; do
            for view in overview workloads; do
                for format in png svg pdf; do
                    test -s "$work/report/$group-$view.$format"
                done
            done
        done
        test -s "$work/report/report.md"
        test -s "$work/report/summary.json"
        env -i PATH=/usr/bin:/bin "$bin" report examples/compression/results -o "$work/readme" \
            --group compress --format png -q
        cmp assets/compression-overview.png "$work/readme/compress-overview.png"
        cmp assets/compression-workloads.png "$work/readme/compress-workloads.png"

        mv "$work/$name.tar.gz" "$archive"
        printf 'Tested %s\n' "$archive"
        ;;
    *)
        printf 'usage: release.sh version | notes [TAG] | package x86_64|aarch64 OUTPUT_DIRECTORY\n' >&2
        exit 2
        ;;
esac
