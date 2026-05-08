# DayZ-Takaro-Integration installer.
# Drop this script into the unzipped release folder (next to @TakaroIntegration\
# and DLLs\) and run it from PowerShell. It prompts for a registration token,
# finds the DayZ server install, copies the DLLs and mod, writes the config,
# and tells the operator the one start-line edit they still need to do.

[CmdletBinding()]
param(
    [string] $ServerPath,
    [string] $RegistrationToken
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Read-NonEmpty {
    param([string]$Prompt)
    while ($true) {
        $v = Read-Host $Prompt
        if ($v) { return $v.Trim() }
        Write-Host '  (cannot be empty)' -ForegroundColor Yellow
    }
}

function Show-TokenInstructions {
    Add-Type -AssemblyName PresentationFramework
    $msg = @'
Before continuing, you need a Takaro registration token.

How to get one:
  1. Sign up / log in at https://takaro.io/?via=zach550
  2. Open the "Game Servers" page in the dashboard
  3. Click the "Game server actions" button
  4. For "Server type", select GENERIC
  5. Copy the registrationToken value

Paste that token into the next prompt in the installer.

(Click OK to continue.)
'@
    [System.Windows.MessageBox]::Show($msg, 'DayZ-Takaro Setup — Get your registration token', 'OK', 'Information') | Out-Null
}

# --- Header -----------------------------------------------------------------

Write-Host ''
Write-Host '=== DayZ-Takaro-Integration Installer ===' -ForegroundColor Cyan
Write-Host ''

# --- 1. Token --------------------------------------------------------------

if (-not $RegistrationToken) {
    Show-TokenInstructions
    Write-Host ''
    Write-Host 'Paste the registrationToken from Takaro:' -ForegroundColor Cyan
    $RegistrationToken = Read-NonEmpty '  registrationToken'
}

# --- 2. Server path --------------------------------------------------------

if (-not $ServerPath) {
    Write-Host ''
    Write-Host 'Path to your DayZ server install (folder containing DayZServer_x64.exe):' -ForegroundColor Cyan
    Write-Host '  Common locations:'
    Write-Host '    C:\Steam\steamapps\common\DayZServer'
    Write-Host '    C:\GameServers\dayz'
    Write-Host '    C:\dayzserver'
    $ServerPath = Read-NonEmpty '  server path'
}

if (-not (Test-Path (Join-Path $ServerPath 'DayZServer_x64.exe'))) {
    throw "DayZServer_x64.exe not found in: $ServerPath"
}

# --- 3. Locate installer payload -------------------------------------------

$dllSrc = Join-Path $here 'DLLs'
$modSrc = Join-Path $here '@TakaroIntegration'

foreach ($p in @($dllSrc, $modSrc)) {
    if (-not (Test-Path $p)) {
        throw "Installer payload missing: $p — make sure install.ps1 is in the unzipped release folder."
    }
}

# --- 4. Copy DLLs ----------------------------------------------------------

Write-Host ''
Write-Host 'Installing native DLLs...' -ForegroundColor Cyan
foreach ($dll in 'winmm.dll', 'dayz_takaro.dll') {
    $src = Join-Path $dllSrc $dll
    $dst = Join-Path $ServerPath $dll
    if (-not (Test-Path $src)) { throw "Missing: $src" }
    Copy-Item -Path $src -Destination $dst -Force
    Write-Host "  $dst" -ForegroundColor DarkGray
}

# --- 5. Write takaro_config.txt --------------------------------------------

Write-Host ''
Write-Host 'Writing takaro_config.txt...' -ForegroundColor Cyan
$cfg = @"
registrationToken=$RegistrationToken
serverName=DayZ Server
profilesDir=profiles
"@
Set-Content -Path (Join-Path $ServerPath 'takaro_config.txt') -Value $cfg -Encoding ASCII -NoNewline
Write-Host "  $(Join-Path $ServerPath 'takaro_config.txt')" -ForegroundColor DarkGray

# --- 6. Copy @TakaroIntegration --------------------------------------------

Write-Host ''
Write-Host 'Installing @TakaroIntegration mod...' -ForegroundColor Cyan
$modDst = Join-Path $ServerPath '@TakaroIntegration'
if (Test-Path $modDst) {
    Remove-Item -Path $modDst -Recurse -Force
}
Copy-Item -Path $modSrc -Destination $modDst -Recurse -Force
Write-Host "  $modDst" -ForegroundColor DarkGray

# --- 7. Final instructions -------------------------------------------------

Write-Host ''
Write-Host '==================== DONE ====================' -ForegroundColor Green
Write-Host ''
Write-Host 'One last step: add @TakaroIntegration to your server start command.' -ForegroundColor Yellow
Write-Host ''
Write-Host '  Open your server start file (usually start_server.bat) and add:' -ForegroundColor Yellow
Write-Host ''
Write-Host '      -serverMod="@TakaroIntegration"' -ForegroundColor White
Write-Host ''
Write-Host '  IMPORTANT: use -serverMod=  (NOT -mod=).' -ForegroundColor Red
Write-Host '  -serverMod loads the addon on the server only — clients do NOT need to download it.' -ForegroundColor DarkGray
Write-Host '  -mod would force every player to subscribe to it, which you do not want.' -ForegroundColor DarkGray
Write-Host ''
Write-Host 'Then start the server normally. Within ~5 seconds the gameserver should' -ForegroundColor Green
Write-Host 'show as ONLINE in your Takaro dashboard.' -ForegroundColor Green
Write-Host ''
