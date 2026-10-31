<# : batch part, PowerShell reads all of this as a comment
@echo off
setlocal
rem The Switch NRO from your own copy of the game, built inside the scda-nx builder container. Needs only Docker
rem Desktop. Double-click this file and choose your disc image (.iso), or drop the .iso or the extracted disc's folder
rem onto it. The SD card folder is made in out\ beside this file. Everything else is in the PowerShell part below.
set "SCDA_SELF=%~f0"
set "SCDA_ARG=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Invoke-Expression ([IO.File]::ReadAllText($env:SCDA_SELF))"
exit /b %errorlevel%
#>

# Your game is mounted read only into the container and never leaves this computer. The recompiled code and the build
# are kept in the Docker volume scda-nx-cache, so later runs are quicker ("docker volume rm scda-nx-cache" removes it).
#   SCDA_IMAGE          the builder image (default ghcr.io/itouchdennis/scda-nx-builder:latest)
#   OUT                 the output folder (default out\ beside this file)
#   SCDA_ICON           your own 256x256 JPEG for the Homebrew Menu (default icon.jpg or switch\icon.jpg beside this file)
#   SCDA_ANY_RELEASE=1  build from a release other than the tested one
#   SCDA_CODE_OPT=-O2   the game code optimized as before (default -O3), for comparing
#   SCDA_NO_PAUSE=1     close at once at the end instead of waiting for Enter (CI)

# native tools report through exit codes here: with Stop, Windows PowerShell would turn their stderr into errors
$ErrorActionPreference = 'Continue'
$Image = if ($env:SCDA_IMAGE) { $env:SCDA_IMAGE } else { 'ghcr.io/itouchdennis/scda-nx-builder:latest' }
$Here = Split-Path -Parent $env:SCDA_SELF
$Out = if ($env:OUT) { $env:OUT } else { Join-Path $Here 'out' }

function Finish($code) {
    if (-not $env:SCDA_NO_PAUSE) {
        Write-Host ''
        Read-Host 'Press Enter to close this window' | Out-Null
    }
    exit $code
}
function Fail($msg) {
    Write-Host ''
    Write-Host "error: $msg" -ForegroundColor Red
    Finish 1
}
function Step($msg) {
    Write-Host ''
    Write-Host "== $msg" -ForegroundColor Cyan
}
function Quiet($cmdline) {
    cmd /c "$cmdline >nul 2>&1"
    return $LASTEXITCODE -eq 0
}

Write-Host 'scda-nx: builds the Switch NRO from your own copy of Splinter Cell: Double Agent (Xbox)'

# the game: dropped onto this file, or chosen in a dialog
$Game = $env:SCDA_ARG
if (-not $Game) {
    Add-Type -AssemblyName System.Windows.Forms
    $owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true }
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Choose your Splinter Cell: Double Agent disc image (.iso), or default.xbe in the extracted disc'
    $dialog.Filter = 'Xbox disc image or default.xbe (*.iso;*.xiso;default.xbe)|*.iso;*.xiso;default.xbe|All files (*.*)|*.*'
    $picked = $dialog.ShowDialog($owner)
    $owner.Dispose()
    if ($picked -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'no game chosen' }
    $Game = $dialog.FileName
}
if (-not (Test-Path -LiteralPath $Game)) { Fail "no such file or folder: $Game" }
$Game = (Resolve-Path -LiteralPath $Game).ProviderPath
# default.xbe stands for the extracted disc's folder it is in
if ((Split-Path -Leaf $Game) -ieq 'default.xbe') { $Game = Split-Path -Parent $Game }
Write-Host "game: $Game"

Step 'Docker'
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Start-Process 'https://docs.docker.com/desktop/setup/install/windows-install/'
    Fail 'Docker Desktop is not installed. Its download page has just opened: install it, start it once, then run this again.'
}
if (-not (Quiet 'docker info')) {
    $desktop = Join-Path $env:ProgramFiles 'Docker\Docker\Docker Desktop.exe'
    if (Test-Path -LiteralPath $desktop) {
        Write-Host 'starting Docker Desktop, this can take a minute'
        Start-Process -FilePath $desktop
        for ($i = 0; $i -lt 90 -and -not (Quiet 'docker info'); $i++) { Start-Sleep -Seconds 2 }
    }
    if (-not (Quiet 'docker info')) { Fail 'Docker does not answer: start Docker Desktop, wait until it says it is running, then run this again.' }
}
Write-Host 'Docker is running'

Step "Builder image $Image"
Write-Host 'downloading it, or its updates (the first time is several GB)'
docker pull $Image
if ($LASTEXITCODE -ne 0) {
    if (Quiet "docker image inspect $Image") { Write-Host 'could not check for updates, using the image already here' -ForegroundColor Yellow }
    else { Fail "could not download ${Image}: are you online?" }
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).ProviderPath
$mounts = @('--mount', "type=bind,source=$Game,target=/game,readonly",
            '--mount', "type=bind,source=$Out,target=/out",
            '-v', 'scda-nx-cache:/cache')
$Icon = $env:SCDA_ICON
if (-not $Icon) {
    foreach ($c in @((Join-Path $Here 'icon.jpg'), (Join-Path $Here 'switch\icon.jpg'))) {
        if (Test-Path -LiteralPath $c) { $Icon = $c; break }
    }
}
if ($Icon -and (Test-Path -LiteralPath $Icon)) {
    $mounts += @('--mount', "type=bind,source=$((Resolve-Path -LiteralPath $Icon).ProviderPath),target=/icon.jpg,readonly")
}
$any = if ($env:SCDA_ANY_RELEASE) { $env:SCDA_ANY_RELEASE } else { '0' }
$opt = if ($env:SCDA_CODE_OPT) { $env:SCDA_CODE_OPT } else { '-O3' }

# the container runs as its own root: on Windows the files it makes in out\ belong to you either way
docker run --rm -t -e "SCDA_ANY_RELEASE=$any" -e "SCDA_CODE_OPT=$opt" @mounts $Image
if ($LASTEXITCODE -ne 0) { Fail 'the build did not finish, the reason is above' }

Write-Host ''
Write-Host "Your SD card folder: $Out\switch" -ForegroundColor Green
if (Test-Path -LiteralPath (Join-Path $Out 'switch')) { Start-Process explorer.exe (Join-Path $Out 'switch') }
Finish 0
