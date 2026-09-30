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
            x86_64|aarch64) ;;
            *) printf 'error: unknown architecture %s\n' "$arch" >&2; exit 1 ;;
        esac
        # make static builds for this machine, and the archive is tested by running it.
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

        make static
        binary=build/isocost-static
        mkdir "$work/$name" "$work/unpacked"
        cp "$binary" "$work/$name/isocost"
        cp LICENSE README.md CHANGELOG.md "$work/$name/"
        cp third_party/dejavu/LICENSE "$work/$name/LICENSE-DejaVu"
        tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
            -czf "$work/$name.tar.gz" -C "$work" "$name"
        tar -xzf "$work/$name.tar.gz" -C "$work/unpacked"

        bin="$work/unpacked/$name/isocost"
        file "$bin" | grep -Eq 'static(ally|-pie) linked'
        actual=$(env -i PATH=/usr/bin:/bin "$bin" --version 2> "$work/stderr")
        printf '%s\n' "$actual"
        test "$actual" = "isocost $version"
        test ! -s "$work/stderr"
        cmp LICENSE "$work/unpacked/$name/LICENSE"
        cmp third_party/dejavu/LICENSE "$work/unpacked/$name/LICENSE-DejaVu"

        # The printed defaults must pass the binary's own check.
        env -i PATH=/usr/bin:/bin "$bin" config > "$work/isocost.toml"
        env -i PATH=/usr/bin:/bin "$bin" config --check "$work/isocost.toml" | grep -q ": valid ("

        # Render every example with the archived binary and with a native build that
        # make test covers. Numbers and vector files must match byte for byte. PNG
        # anti-aliasing may differ by one level in a few pixels, because musl's
        # libm does not round every result the way glibc's does.
        make build/isocost
        for dir in examples/*/; do
            example=$(basename "$dir")
            [[ -f "$dir/isocost.toml" ]] || continue
            env -i PATH=/usr/bin:/bin "$bin" report "$dir/results" -o "$work/static/$example" \
                --format png,svg,pdf -q
            build/isocost report "$dir/results" -o "$work/native/$example" --format png,svg,pdf -q
            cmp "$work/native/$example/summary.json" "$work/static/$example/summary.json"
            test -s "$work/static/$example/report.md"
            for figure in "$work/native/$example"/*.svg "$work/native/$example"/*.pdf; do
                cmp "$figure" "$work/static/$example/$(basename "$figure")"
            done
            for png in "$work/static/$example"/*.png; do
                test "$(head -c 8 "$png" | od -An -tx1 | tr -d ' \n')" = 89504e470d0a1a0a
            done
        done

        mv "$work/$name.tar.gz" "$archive"
        printf 'Tested %s\n' "$archive"
        ;;
    *)
        printf 'usage: release.sh version | notes [TAG] | package x86_64|aarch64 OUTPUT_DIRECTORY\n' >&2
        exit 2
        ;;
esac
