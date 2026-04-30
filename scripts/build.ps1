# Build the @TakaroIntegration mod into a shippable PBO.
#
# Requires DayZ Tools installed via Steam (free).
# Looks for AddonBuilder at: %PROGRAMFILES(X86)%\Steam\steamapps\common\DayZ Tools\Bin\AddonBuilder\AddonBuilder.exe
#
# Usage:
#   pwsh scripts/build.ps1
#   pwsh scripts/build.ps1 -Clean       # remove build output first
#   pwsh scripts/build.ps1 -Sign keyPath\my.biprivatekey
#
# The build process:
#   1. Copy src/takaro_integration/ to a temp staging dir (Maven-on-Windows
#      pattern — DayZ Tools cannot read WSL UNC paths reliably).
#   2. Run AddonBuilder against the staging dir, output PBO to
#      @TakaroIntegration/Addons/.
#   3. Optionally sign the PBO with the provided private key.
#   4. Print final file size + path.

[CmdletBinding()]
param(
    [switch] $Clean,
    [string] $Sign,
    [string] $StagingRoot = "$env:TEMP\TakaroIntegration_build",
    [string] $RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
)

$ErrorActionPreference = 'Stop'

$AddonBuilder = Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\DayZ Tools\Bin\AddonBuilder\AddonBuilder.exe'
if (-not (Test-Path $AddonBuilder)) {
    Write-Host "AddonBuilder.exe not found at:" -ForegroundColor Red
    Write-Host "  $AddonBuilder" -ForegroundColor Red
    Write-Host "Install DayZ Tools from Steam (free)." -ForegroundColor Yellow
    exit 1
}

$Source = Join-Path $RepoRoot 'src\takaro_integration'
$OutDir = Join-Path $RepoRoot '@TakaroIntegration\Addons'
$StagingPbo = Join-Path $StagingRoot 'TakaroIntegration'

if (-not (Test-Path $Source)) {
    Write-Host "Source not found: $Source" -ForegroundColor Red
    exit 1
}

if ($Clean -and (Test-Path $StagingRoot)) {
    Write-Host "Cleaning $StagingRoot"
    Remove-Item -Path $StagingRoot -Recurse -Force
}

if (-not (Test-Path $OutDir)) {
    New-Item -Path $OutDir -ItemType Directory -Force | Out-Null
}
if (-not (Test-Path $StagingRoot)) {
    New-Item -Path $StagingRoot -ItemType Directory -Force | Out-Null
}

Write-Host "Staging source -> $StagingPbo"
if (Test-Path $StagingPbo) {
    Remove-Item -Path $StagingPbo -Recurse -Force
}
Copy-Item -Path $Source -Destination $StagingPbo -Recurse -Force

Write-Host "Running AddonBuilder..."
$arguments = @(
    "`"$StagingPbo`"",
    "`"$OutDir`"",
    '-clear',
    '-packonly'
)
if ($Sign) {
    $arguments += "-sign=`"$Sign`""
}

& $AddonBuilder @arguments
if ($LASTEXITCODE -ne 0) {
    Write-Host "AddonBuilder exited with code $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
}

$pbo = Join-Path $OutDir 'TakaroIntegration.pbo'
if (Test-Path $pbo) {
    $size = (Get-Item $pbo).Length
    Write-Host "Built: $pbo ($size bytes)" -ForegroundColor Green
} else {
    Write-Host "PBO not produced — check AddonBuilder output above." -ForegroundColor Red
    exit 2
}
