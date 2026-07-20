#!/usr/bin/env sh
# POSIX companion to claude-stop-hook.ps1 — runs pre-commit at the end of a
# Claude Code turn on Linux/macOS (and Windows via Git Bash). Wired up via
# .claude/settings.json (Stop hook), which prefers pwsh + the .ps1 when pwsh is
# available and falls back to this script otherwise.
#
# Uses --all-files (not --files <changed>) so pre-existing formatting drift in
# untouched files is also caught — the whole point of the hook is "I never have
# to think about pre-commit", and a scoped run leaves drift unfixed. At a few
# seconds on this repo (vendor/mono excluded), the cost is negligible.

# Always exit 0: pre-commit exits non-zero when it auto-fixes files — that's
# expected, not a failure — and a non-zero Stop hook surfaces as an error in the
# Claude UI. pre-commit's own output already reports what it fixed.

repo_root=$(git rev-parse --show-toplevel 2>/dev/null)
if [ -z "$repo_root" ]; then
    echo "[claude-stop-hook] not in a git repo, skipping"
    exit 0
fi

cd "$repo_root" || exit 0

if ! command -v pre-commit >/dev/null 2>&1; then
    echo "[claude-stop-hook] pre-commit not installed, skipping"
    exit 0
fi

echo "[claude-stop-hook] running pre-commit on all files..."
pre-commit run --all-files
echo "[claude-stop-hook] done"
exit 0
