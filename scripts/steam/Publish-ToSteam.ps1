<#
.SYNOPSIS
    Renders the steamcmd app/depot build VDFs from scripts/steam/templates/ and,
    unless -WhatIf is passed, uploads the given GameBuildPipeline output folder
    to a Steam beta branch.

.DESCRIPTION
    See docs/ops/shipping.md#steamcmd-upload-automation for the full reference.

    This script is developer-run, not CI-run — steamcmd needs an interactive
    Steam Guard login on first use per machine, and Steam credentials should
    never sit in a public repo's CI environment. A "release" GitHub Actions
    workflow (.github/workflows/release.yml) validates this script's template
    rendering in -WhatIf mode on synthetic fixtures; it never invokes steamcmd
    for real.

    Never targets the public/default branch without an explicit -Force —
    #894's acceptance criterion is a BETA branch dry run, on purpose.

.PARAMETER BuildPath
    A GameBuildPipeline output folder (must contain game.manifest).

.PARAMETER AppId
    Your Steam App ID. Falls back to $env:STEAM_APP_ID.

.PARAMETER DepotId
    Your Steam Depot ID. Falls back to $env:STEAM_DEPOT_ID.

.PARAMETER BetaBranch
    The branch to set live after upload. Required. "public" or "default"
    additionally requires -Force.

.PARAMETER SteamUser
    The steamcmd login name (not the password — steamcmd prompts for that,
    and for Steam Guard, interactively). Falls back to $env:STEAM_BUILD_ACCOUNT.

.PARAMETER SteamCmdPath
    Path to steamcmd(.exe). Falls back to $env:STEAMCMD_PATH. Only required
    when actually invoking steamcmd (i.e. not under -WhatIf).

.PARAMETER Description
    Optional build description shown on the Steamworks partner site.

.PARAMETER Force
    Required in addition to -BetaBranch public / -BetaBranch default.

.EXAMPLE
    pwsh scripts/steam/Publish-ToSteam.ps1 -BuildPath C:\Builds\Drift\Drift -AppId 480 -DepotId 481 -BetaBranch beta -WhatIf
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildPath,

    [string]$AppId = $env:STEAM_APP_ID,

    [string]$DepotId = $env:STEAM_DEPOT_ID,

    [Parameter(Mandatory = $true)]
    [string]$BetaBranch,

    [string]$SteamUser = $env:STEAM_BUILD_ACCOUNT,

    [string]$SteamCmdPath = $env:STEAMCMD_PATH,

    [string]$Description = "",

    [switch]$Force,

    # Render and validate the VDFs, print them, then stop BEFORE invoking
    # steamcmd. Deliberately a plain switch rather than PowerShell's built-in
    # SupportsShouldProcess mechanism: that mechanism auto-suppresses every
    # ShouldProcess-aware cmdlet in the script (New-Item, Set-Content, …), which
    # would make -WhatIf skip the very template rendering it's supposed to
    # validate. A plain switch keeps local file writes real and gates only the
    # actual steamcmd invocation below.
    [switch]$WhatIf,

    # Directory the rendered VDFs are written to. Overridable so the release
    # workflow's validation job can point this at a scratch directory instead
    # of the git-ignored scripts/steam/generated/ default.
    [string]$OutputDir = (Join-Path $PSScriptRoot "generated")
)

$ErrorActionPreference = "Stop"

function Fail([string]$Message)
{
    Write-Error $Message
    exit 1
}

if ([string]::IsNullOrWhiteSpace($AppId))
{
    Fail "No App ID given. Pass -AppId or set `$env:STEAM_APP_ID."
}
if ([string]::IsNullOrWhiteSpace($DepotId))
{
    Fail "No Depot ID given. Pass -DepotId or set `$env:STEAM_DEPOT_ID."
}
# SteamUser/SteamCmdPath are only needed to actually run steamcmd — checked
# further down, after the -WhatIf short-circuit, so validating and rendering
# the VDFs never requires Steam credentials to be present at all.

# Trim once and use the trimmed value everywhere from here on — including in
# the rendered VDF below — so a copy-pasted "-BetaBranch \" beta \"" doesn't
# pass this guard (which compares the trimmed form) and then silently write
# an untrimmed, unrecognized branch name into the config steamcmd actually
# runs. Only the public/default COMPARISON also lowercases; the branch name
# itself keeps whatever case the caller gave it, since a custom branch name's
# case is significant to steamcmd.
$BetaBranch = $BetaBranch.Trim()
$normalizedBranch = $BetaBranch.ToLowerInvariant()
if (($normalizedBranch -eq "public" -or $normalizedBranch -eq "default") -and -not $Force)
{
    Fail ("Refusing to target the '$BetaBranch' branch without -Force. #894's whole point is a " +
          "BETA branch dry run — publishing straight to what players are currently running should " +
          "never be the accidental result of a copy-pasted command. Pass -Force if you really mean it.")
}
if (($normalizedBranch -eq "public" -or $normalizedBranch -eq "default") -and $Force)
{
    Write-Warning "Targeting the LIVE branch '$BetaBranch' with -Force. This is not a dry run."
}

$resolvedBuildPath = Resolve-Path -Path $BuildPath -ErrorAction SilentlyContinue
if (-not $resolvedBuildPath)
{
    Fail "BuildPath does not exist: $BuildPath"
}
$contentRoot = $resolvedBuildPath.Path

$manifestPath = Join-Path $contentRoot "game.manifest"
if (-not (Test-Path $manifestPath))
{
    Fail ("$contentRoot does not look like a GameBuildPipeline output folder — no game.manifest " +
          "found. Build the game via the editor's Build Game panel first.")
}

$appIdFile = Join-Path $contentRoot "steam_appid.txt"
if (Test-Path $appIdFile)
{
    Fail ("$appIdFile is present inside the content root. That file is the developer-only App 480 " +
          "(Spacewar) affordance (see docs/ops/build.md) and must never ship — remove it from the " +
          "build output and rebuild before publishing.")
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$templateDir = Join-Path $PSScriptRoot "templates"
$appBuildTemplate = Get-Content -Raw (Join-Path $templateDir "app_build.vdf.template")
$depotBuildTemplate = Get-Content -Raw (Join-Path $templateDir "depot_build.vdf.template")

$buildOutputDir = Join-Path $OutputDir "steamcmd_build_output"
New-Item -ItemType Directory -Force -Path $buildOutputDir | Out-Null

$depotVdfName = "depot_build_$DepotId.vdf"
$depotVdfPath = Join-Path $OutputDir $depotVdfName
$appVdfPath = Join-Path $OutputDir "app_build_$AppId.vdf"

# Valve's KeyValues/VDF text format treats both backslash and double-quote as
# special inside a quoted string, so a raw Windows path (backslashes) or a
# free-text description (which could contain a quote) must be escaped before
# insertion — otherwise a contentroot like C:\Builds\Drift or a description
# containing a " silently produces a malformed VDF steamcmd either
# misparses or rejects outright.
function Format-VdfValue([string]$Value)
{
    return $Value.Replace('\', '\\').Replace('"', '\"')
}

# The "preview" field only ever expects "1" (dry-run inside steamcmd itself,
# uploads nothing) or an empty string ("" — real upload). Kept as a token
# rather than a fixed empty string so a future flag can drive it, but this
# script never sets it to "1" itself: -WhatIf short-circuits before steamcmd
# runs at all, which is the stronger guarantee.
$tokens = @{
    "{{APP_ID}}"           = $AppId
    "{{DEPOT_ID}}"         = $DepotId
    "{{BUILD_DESC}}"       = Format-VdfValue $Description
    "{{BUILD_OUTPUT_DIR}}" = Format-VdfValue $buildOutputDir
    "{{CONTENT_ROOT}}"     = Format-VdfValue $contentRoot
    "{{SET_LIVE_BRANCH}}"  = $BetaBranch
    "{{PREVIEW_FLAG}}"     = ""
    "{{DEPOT_VDF_PATH}}"   = Format-VdfValue $depotVdfPath
}

function Expand-Template([string]$Text, [hashtable]$Tokens)
{
    $result = $Text
    foreach ($key in $Tokens.Keys)
    {
        $result = $result.Replace($key, [string]$Tokens[$key])
    }
    return $result
}

$renderedAppVdf = Expand-Template -Text $appBuildTemplate -Tokens $tokens
$renderedDepotVdf = Expand-Template -Text $depotBuildTemplate -Tokens $tokens

foreach ($rendered in @($renderedAppVdf, $renderedDepotVdf))
{
    if ($rendered -match '\{\{[A-Z_]+\}\}')
    {
        Fail "A template token was left unsubstituted: $($Matches[0]). This is a bug in this script, not a config error — every token must have a value above."
    }
}

Set-Content -Path $appVdfPath -Value $renderedAppVdf -NoNewline
Set-Content -Path $depotVdfPath -Value $renderedDepotVdf -NoNewline

Write-Host "Rendered app build config:   $appVdfPath"
Write-Host "Rendered depot build config: $depotVdfPath"
Write-Host "---"
Write-Host $renderedAppVdf
Write-Host "---"
Write-Host $renderedDepotVdf

if ($WhatIf)
{
    Write-Host ""
    Write-Host "-WhatIf: stopping before steamcmd. Nothing was uploaded."
    exit 0
}

if ([string]::IsNullOrWhiteSpace($SteamUser))
{
    Fail "No Steam login name given. Pass -SteamUser or set `$env:STEAM_BUILD_ACCOUNT."
}
if ([string]::IsNullOrWhiteSpace($SteamCmdPath))
{
    Fail "No steamcmd path given. Pass -SteamCmdPath or set `$env:STEAMCMD_PATH."
}
if (-not (Test-Path $SteamCmdPath))
{
    Fail "SteamCmdPath does not exist: $SteamCmdPath"
}

Write-Host ""
Write-Host "Running steamcmd. This will prompt for your Steam password and, on a new machine, Steam Guard."
& $SteamCmdPath +login $SteamUser +run_app_build $appVdfPath +quit
$steamCmdExitCode = $LASTEXITCODE

if ($steamCmdExitCode -ne 0)
{
    Fail "steamcmd exited with code $steamCmdExitCode."
}

Write-Host ""
Write-Host "Upload finished. Verify the build on the Steamworks partner site (SteamPipe -> Builds) and confirm it launches from the '$BetaBranch' branch before promoting it further."
