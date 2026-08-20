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

# Check availability BEFORE announcing the run — otherwise the output reads
# "running pre-commit..." immediately followed by "skipping", which is worse than
# either message alone. On a box without pre-commit this would otherwise emit a raw
# CommandNotFoundException at the end of every turn, reading like a broken hook
# rather than a missing tool.
if (-not (Get-Command pre-commit -ErrorAction SilentlyContinue)) {
    Write-Host "[claude-stop-hook] pre-commit not found on PATH, skipping (install it with: pip install pre-commit)"
    exit 0
}

Write-Host "[claude-stop-hook] running pre-commit on all files..."

# Pre-commit exits non-zero when it auto-fixes files — that's expected, not a
# failure. Always exit 0 so the hook doesn't surface as an error in Claude UI;
# pre-commit's own output already tells the user what was fixed.
pre-commit run --all-files | Out-Host

Write-Host "[claude-stop-hook] done"
exit 0
