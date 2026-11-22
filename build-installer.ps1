# Builds TS3SpeakingOverlay-Setup.exe (single-file installer with both plugin DLLs embedded).
#
# Prerequisites: build.ps1 has already produced
#   <BuildDir>\ts3_speaking_overlay_win64.dll
#   <BuildDir32>\ts3_speaking_overlay_win32.dll
# and a MinGW-w64 x64 (msvcrt) toolchain is available for windres + g++.
#
# The installer is compiled in a staging directory outside this repo because
# MinGW needs a writable ASCII path; pass -Stage to change it.

param(
    # MinGW-w64 x64 (msvcrt) bin directory (holds g++.exe / windres.exe / strip.exe)
    [string]$Mingw = 'C:\dev\mingw64\bin',
    # Where build.ps1 left the two plugin DLLs
    [string]$PayloadDir = (Join-Path $env:TEMP 'ts3ovl_build'),
    [string]$PayloadDir32 = (Join-Path $env:TEMP 'ts3ovl_build32'),
    # Staging directory for the resource object + installer
    [string]$Stage = (Join-Path $env:TEMP 'ts3ovl_setup_build')
)

$ErrorActionPreference = 'Stop'

$project = $PSScriptRoot
$payload = Join-Path $PayloadDir 'ts3_speaking_overlay_win64.dll'
$payload32 = Join-Path $PayloadDir32 'ts3_speaking_overlay_win32.dll'
$sources = @(
    (Join-Path $project 'installer\setup.cpp'),
    (Join-Path $project 'installer\setup.rc'),
    (Join-Path $project 'installer\setup.manifest'),
    (Join-Path $project 'installer\setup.ico'),
    (Join-Path $project 'installer\res.h')
)

foreach ($s in $sources) {
    if (-not (Test-Path $s)) { throw "missing source file: $s" }
}
foreach ($p in @($payload, $payload32)) {
    if (-not (Test-Path $p)) { throw "plugin DLL not found: $p (run build.ps1 first, or pass -PayloadDir/-PayloadDir32)" }
}

$gpp     = Join-Path $Mingw 'g++.exe'
$windres = Join-Path $Mingw 'windres.exe'
$strip   = Join-Path $Mingw 'strip.exe'
foreach ($t in @($gpp, $windres)) {
    if (-not (Test-Path $t)) { throw "tool not found: $t (pass -Mingw <dir with g++.exe>)" }
}

if (-not (Test-Path $Stage)) { New-Item -ItemType Directory -Path $Stage -Force | Out-Null }

# Stage everything into the build dir (payload DLLs included, so setup.rc can embed them).
Copy-Item $payload -Destination $Stage -Force
Copy-Item $payload32 -Destination $Stage -Force
foreach ($s in $sources) {
    Copy-Item $s -Destination $Stage -Force
}

foreach ($dllName in @('ts3_speaking_overlay_win64.dll', 'ts3_speaking_overlay_win32.dll')) {
    $dllInfo = Get-Item (Join-Path $Stage $dllName)
    Write-Host ("payload: {0} ({1:N0} bytes)" -f $dllName, $dllInfo.Length)
}

# --- 1. resource object (icon + manifest + embedded DLLs + version info) ---
$resObj = Join-Path $Stage 'setup_res.o'
& $windres -I $Stage -O coff -o $resObj (Join-Path $Stage 'setup.rc')
if ($LASTEXITCODE -ne 0) { throw "windres failed (exit $LASTEXITCODE)" }
Write-Host ("resource object: {0:N0} bytes" -f (Get-Item $resObj).Length)

# --- 2. compile + link the installer ---
$setupExe = Join-Path $Stage 'TS3SpeakingOverlay-Setup.exe'
$gppArgs = @(
    '-std=c++17', '-O2',
    '-municode', '-mwindows',
    '-static', '-static-libgcc', '-static-libstdc++',
    '-Wall', '-Wextra',
    '-D_WIN32_WINNT=0x0A00', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
    '-DUNICODE', '-D_UNICODE',
    ('-I' + $Stage),
    ('-o' + $setupExe),
    (Join-Path $Stage 'setup.cpp'),
    $resObj,
    '-lcomctl32', '-lshell32', '-lole32', '-luser32', '-lgdi32', '-ladvapi32'
)
& $gpp @gppArgs
if ($LASTEXITCODE -ne 0) { throw "installer compile failed (exit $LASTEXITCODE)" }

if (Test-Path $strip) {
    & $strip --strip-all $setupExe
}

$exeInfo = Get-Item $setupExe
Write-Host ""
Write-Host ("OK  {0}" -f $exeInfo.FullName)
Write-Host ("    {0:N0} bytes" -f $exeInfo.Length)

# --- 3. publish into dist/ ----------------------------------------------
$dist = Join-Path $project 'dist'
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist -Force | Out-Null }
$distExe = Join-Path $dist 'TS3SpeakingOverlay-Setup.exe'
if (Test-Path $distExe) { Remove-Item $distExe -Force }
Move-Item $setupExe -Destination $distExe -Force
Write-Host ("    moved to {0}" -f $distExe)

# NOTE: keep the DLLs in dist/ in sync with the embedded ones so a manual install
# from the same folder uses exactly the same binaries.
foreach ($p in @($payload, $payload32)) {
    Copy-Item $p -Destination $dist -Force
}

# A Low integrity label would make the installer unable to touch the TS3 config
# directory (it silently fails to copy the DLL). Warn instead of shipping it.
# Medium/High labels are fine and are not reported.
$label = (& icacls $distExe 2>&1 | Select-String 'Mandatory' | Select-String 'Low Mandatory Level')
if ($label) {
    Write-Host ("    WARNING: {0}" -f $label.ToString().Trim())
    Write-Host "    this EXE inherited a Low integrity label and cannot write into the TS3 folder;"
    Write-Host "    run: icacls `"$([IO.Path]::GetDirectoryName($distExe))`" /setintegritylevel `"(OI)(CI)Medium`" /T"
}
Write-Host "OK"
