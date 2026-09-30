#!/usr/bin/env bash
set -euo pipefail

wiki_dir=${1:-wiki}
repository=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}
server_url=${GITHUB_SERVER_URL:-https://github.com}
: "${WIKI_TOKEN:?WIKI_TOKEN is required}"
source_sha=$(git rev-parse HEAD)
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
bash "$script_dir/wiki_check.sh" "$wiki_dir"

work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/isocost-wiki.XXXXXX")
trap 'rm -rf -- "$work"' EXIT

# Keep the token out of Git arguments and the clone's saved remote URL.
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0="http.${server_url%/}/.extraheader"
GIT_CONFIG_VALUE_0="AUTHORIZATION: basic $(printf 'x-access-token:%s' "$WIKI_TOKEN" | base64 | tr -d '\n')"
export GIT_CONFIG_VALUE_0
export GIT_TERMINAL_PROMPT=0

wiki_repo="$work/wiki"
if ! git clone --quiet --depth 1 "${server_url%/}/${repository}.wiki.git" "$wiki_repo"; then
    echo "cannot clone the GitHub Wiki; enable it and create its first page in GitHub" >&2
    exit 1
fi
wiki_branch=$(git -C "$wiki_repo" symbolic-ref --quiet --short HEAD)
rsync --archive --delete --exclude='.git/' "$wiki_dir/" "$wiki_repo/"

git -C "$wiki_repo" add --all
if git -C "$wiki_repo" diff --cached --quiet --exit-code; then
    echo "wiki is already synchronized"
    exit 0
fi
git -C "$wiki_repo" diff --cached --name-status
git -C "$wiki_repo" config user.name 'github-actions[bot]'
git -C "$wiki_repo" config user.email '41898282+github-actions[bot]@users.noreply.github.com'
git -C "$wiki_repo" commit --quiet -m "docs: sync wiki from ${source_sha:0:12}"
git -C "$wiki_repo" push --quiet origin "HEAD:$wiki_branch"

local_sha=$(git -C "$wiki_repo" rev-parse HEAD)
remote_sha=$(git -C "$wiki_repo" ls-remote origin "refs/heads/$wiki_branch" | awk 'NR == 1 { print $1 }')
if [[ "$remote_sha" != "$local_sha" ]]; then
    echo "wiki push verification failed: local=$local_sha remote=${remote_sha:-missing}" >&2
    exit 1
fi
echo "published wiki commit $local_sha from source $source_sha"
