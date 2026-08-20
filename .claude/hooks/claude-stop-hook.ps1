# Runs pre-commit at the end of a Claude Code turn.
# Wired up via .claude/settings.json (Stop hook).
#
# Uses --all-files (not --files <changed>) so pre-existing formatting drift
# in untouched files is also caught — the whole point of the hook is "I never
# have to think about pre-commit", and a scoped run leaves drift unfixed.
# At ~3 seconds on this repo (vendor/mono excluded), the cost is negligible.

$ErrorActionPreference = 'Continue'

$repoRoot = git rev-parse --show-toplevel 2>$null
if (-not $repoRoot) {
    Write-Host "[claude-stop-hook] not in a git repo, skipping"
    exit 0
}

Set-Location $repoRoot

Write-Host "[claude-stop-hook] running pre-commit on all files..."

# Pre-commit exits non-zero when it auto-fixes files — that's expected, not a
# failure. Always exit 0 so the hook doesn't surface as an error in Claude UI;
# pre-commit's own output already tells the user what was fixed.
pre-commit run --all-files | Out-Host

Write-Host "[claude-stop-hook] done"
exit 0
