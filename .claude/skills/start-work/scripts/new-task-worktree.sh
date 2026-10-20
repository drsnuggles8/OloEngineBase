#!/usr/bin/env bash
# Create one /start-work task worktree: C:/repos/OloEngine-<slug> on feature/<slug>, cut from
# origin/master WITHOUT an upstream. Prints the absolute worktree path on success.
#
#   bash .claude/skills/start-work/scripts/new-task-worktree.sh <slug>
#
# Run it from the Bash tool: `git worktree add ... origin/master` from the PowerShell tool fails
# with "invalid reference: origin/master" although the ref resolves.
#
# Why each guard exists:
#   - every worktree is a direct child of C:/repos, because ccache shares objects only under its
#     base_dir (C:/repos); a worktree elsewhere caches for itself alone and looks healthy;
#   - the path is never inside the base repo: a mis-joined relative path once produced
#     OloEngineBaseBase/reposOloEngine-<slug>, a worktree nested in the base checkout;
#   - --no-track, so a later bare `git push` cannot push the branch straight to master.
set -euo pipefail

slug="${1:-}"
if [[ ! "$slug" =~ ^[a-z0-9][a-z0-9.-]*[a-z0-9]$ ]]; then
    echo "usage: $0 <slug>   (lowercase letters, digits, '.' and '-'; e.g. vsm-groom-casters-1592)" >&2
    exit 2
fi

parent="C:/repos"
common="$(git rev-parse --path-format=absolute --git-common-dir)"
base="$(git -C "$common/.." rev-parse --path-format=absolute --show-toplevel)"
wt="$parent/OloEngine-$slug"
branch="feature/$slug"

shopt -s nocasematch
[[ "$wt" == "$base"/* ]] && { echo "refusing: $wt is inside the base repo $base" >&2; exit 1; }
shopt -u nocasematch
[[ -e "$wt" ]] && { echo "refusing: $wt already exists" >&2; exit 1; }
git -C "$base" show-ref --verify --quiet "refs/heads/$branch" && { echo "refusing: branch $branch already exists" >&2; exit 1; }
git -C "$base" ls-remote --exit-code --heads origin "$branch" >/dev/null 2>&1 && { echo "refusing: origin/$branch already exists" >&2; exit 1; }

git -C "$base" fetch --quiet origin master
git -C "$base" worktree add --quiet "$wt" -b "$branch" --no-track origin/master

if upstream="$(git -C "$wt" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null)"; then
    echo "error: $branch tracks $upstream; a bare git push would publish there" >&2
    exit 1
fi

echo "$wt"
echo "branch: $branch  base: origin/master @ $(git -C "$wt" rev-parse --short HEAD)" >&2
