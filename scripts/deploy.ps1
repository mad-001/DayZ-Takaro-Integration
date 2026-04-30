# Deploy a freshly-built @TakaroIntegration to a DayZ server install.
#
# Usage:
#   pwsh scripts/deploy.ps1
#   pwsh scripts/deploy.ps1 -ServerRoot '\\SERVER\GameServers\dayz'
#
# This:
#   1. Verifies @TakaroIntegration\Addons\TakaroIntegration.pbo exists.
#   2. Removes any old @TakaroIntegration on the target server.
#   3. Copies the new mod folder over.
#   4. Reminds you to add it to the server start command as -serverMod=, NOT -mod=.

[CmdletBinding()]
param(
    [string] $RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path,
    [string] $ServerRoot = '\\SERVER\GameServers\dayz'
)

$ErrorActionPreference = 'Stop'

$ModSource = Join-Path $RepoRoot '@TakaroIntegration'
$Pbo = Join-Path $ModSource 'Addons\TakaroIntegration.pbo'

if (-not (Test-Path $Pbo)) {
    Write-Host "PBO not built. Run scripts/build.ps1 first." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $ServerRoot)) {
    Write-Host "Server root not reachable: $ServerRoot" -ForegroundColor Red
    exit 2
}

$ModTarget = Join-Path $ServerRoot '@TakaroIntegration'

if (Test-Path $ModTarget) {
    Write-Host "Removing existing $ModTarget"
    Remove-Item -Path $ModTarget -Recurse -Force
}

Write-Host "Copying $ModSource -> $ModTarget"
Copy-Item -Path $ModSource -Destination $ModTarget -Recurse -Force

Write-Host "Done." -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1. Edit the server profile config (created on first boot):"
Write-Host "       $ServerRoot\profiles\TakaroIntegration\config.json"
Write-Host "     and set TakaroApiUrl and either IdentityToken or RegistrationToken."
Write-Host ""
Write-Host "  2. Add @TakaroIntegration to your server start command using -serverMod=, NOT -mod=:"
Write-Host "       -serverMod=`"@TakaroIntegration`""
Write-Host "     (-serverMod loads the mod only on the server, so clients do NOT need it)"
Write-Host ""
Write-Host "  3. Restart the server."
