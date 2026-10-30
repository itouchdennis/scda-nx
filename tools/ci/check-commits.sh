#!/usr/bin/env bash
# Every commit's first line in Conventional Commits form (CONTRIBUTING.md): type(scope)!: subject
#   tools/ci/check-commits.sh <base> <head>     the commits in base..head
#   tools/ci/check-commits.sh "" <head>         head alone (a new branch, or a push that rewrote history)
set -euo pipefail

BASE="${1:-}"
HEAD="${2:?head commit}"
TYPES="feat|fix|docs|style|refactor|perf|test|build|ci|chore"
PATTERN="^($TYPES)(\([a-z0-9._/-]+\))?!?: [^ ].*"

if [ -n "$BASE" ] && [ "$BASE" != 0000000000000000000000000000000000000000 ] &&
    git merge-base --is-ancestor "$BASE" "$HEAD" 2>/dev/null; then
    range="$BASE..$HEAD"
else
    range="-1 $HEAD"
fi

bad=0
# shellcheck disable=SC2086  # range is one or two arguments
while IFS= read -r line; do
    sha="${line%% *}"
    subject="${line#* }"
    if [[ "$subject" =~ $PATTERN ]]; then
        echo "ok   $sha $subject"
    else
        echo "bad  $sha $subject"
        bad=1
    fi
done < <(git log --no-merges --format='%h %s' $range)

if [ "$bad" = 1 ]; then
    echo
    echo "Commit messages start with type(scope): subject, the type one of: ${TYPES//|/, } (CONTRIBUTING.md)."
    exit 1
fi
