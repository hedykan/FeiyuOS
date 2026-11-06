#!/usr/bin/env bash
# Build the public release tree from HEAD as a fresh single-commit repository.
#
#   tools/publish_snapshot.sh <empty-output-dir> [message]
#
# 1. `git archive HEAD` (paths marked export-ignore in .gitattributes are left out)
# 2. gitleaks over the exported tree
# 3. every regex in ../.publish_denylist (kept outside the repository: private
#    hostnames, addresses, user names...) must match nothing
# 4. git init + one commit, author $PUBLISH_AUTHOR
# Pushing is left to a human: review the tree, then add a remote and push.
set -euo pipefail
out=${1:?usage: tools/publish_snapshot.sh <empty-output-dir> [message]}
message=${2:-"Initial public release"}
author=${PUBLISH_AUTHOR:-"hedykan <hedykan@outlook.com>"}
root=$(git rev-parse --show-toplevel)
denylist="$root/../.publish_denylist"

[ -e "$out" ] && [ -n "$(ls -A "$out")" ] && { echo "$out is not empty" >&2; exit 1; }
mkdir -p "$out"
git -C "$root" archive HEAD | tar -x -C "$out"

command -v gitleaks >/dev/null || { echo "gitleaks not installed" >&2; exit 1; }
gitleaks dir --no-banner "$out"

if [ -f "$denylist" ]; then
  hits=$(grep -v '^\s*\(#\|$\)' "$denylist" | while read -r pattern; do
    grep -rInE -- "$pattern" "$out" || true
  done)
  [ -n "$hits" ] && { echo "denylist matches:" >&2; echo "$hits" | cut -c1-200 >&2; exit 1; }
else
  echo "warning: no $denylist, only gitleaks ran" >&2
fi

git -C "$out" init -q -b main
git -C "$out" add -A
git -C "$out" -c user.name="${author% <*}" -c user.email="$(sed 's/.*<\(.*\)>/\1/' <<<"$author")" \
  commit -q -m "$message"
git -C "$out" log --stat --oneline | tail -3
echo "ready: $out (no remote configured)"
