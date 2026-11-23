# Build ts3-speaking-overlay (TS3 client plugin).
#
# Produces TWO plugin DLLs plus the local test programs:
#   ts3_speaking_overlay_win64.dll  x64, API 26, links msvcrt.dll
#   ts3_speaking_overlay_win32.dll  x86, API 25, links msvcrt.dll
#
# Why msvcrt and not the default UCRT toolchain: the UCRT build imports
# api-ms-win-crt-*.dll apisets, which are missing on Windows 7/8 without the
# Universal CRT update and on several trimmed/modified Windows installs.  When
# those are missing LoadLibrary fails with error 126 and the TS3 client shows
# "Failed to read addon info".  The msvcrt builds only import msvcrt.dll, which
# ships with every Windows.
#
# Why two bitnesses: 64-bit TS3 (3.6.x) needs API 26 while 32-bit TS3 (<= 3.5.x)
# needs API 25/24 and *rejects* a newer number.  The two clients use different
# plugin directories, so both can be installed side by side.
#
# Prerequisites
#   - MinGW-w64 with the **msvcrt** runtime, one x64 and one i686 build:
#       https://github.com/brechtsanders/winlibs_mingw/releases
#       winlibs-x86_64-...-mingw-w64msvcrt-...zip
#       winlibs-i686-...-mingw-w64msvcrt-...zip
#   - The official TeamSpeak 3 client plugin SDK headers:
#       git clone https://github.com/teamspeak/ts3client-pluginsdk
#     point -Sdk at the clone (or at any dir containing include/plugin_definitions.h).
#
# Sources may live anywhere; everything is staged into an ASCII build directory
# first because MinGW's cc1plus refuses to write temp files into directories that
# carry a Low integrity label (some sandboxed workspaces do).

param(
    # MinGW-w64 x64 (msvcrt) root, e.g. C:\dev\mingw64
    [string]$Mingw64 = 'C:\dev\mingw64',
    # MinGW-w64 i686 (msvcrt) root, e.g. C:\dev\mingw32
    [string]$Mingw32 = 'C:\dev\mingw32',
    # Clone of the TS3 plugin SDK (a directory that contains include\plugin_definitions.h)
    [string]$Sdk = '',
    # Staging directories (must be writable by the compiler; ASCII paths only)
    [string]$BuildDir = (Join-Path $env:TEMP 'ts3ovl_build'),
    [string]$BuildDir32 = (Join-Path $env:TEMP 'ts3ovl_build32'),
    # Skip the local test programs
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'

$src = $PSScriptRoot
if (-not $Sdk) {
    # Accept the SDK either inside the repo (sdk\) or next to it (..\ts3client-pluginsdk).
    foreach ($cand in @(
        (Join-Path $src 'sdk'),
        (Join-Path (Split-Path $src -Parent) 'ts3overlay-src'),
        (Join-Path (Split-Path $src -Parent) 'ts3client-pluginsdk'))) {
        if (Test-Path (Join-Path $cand 'include\plugin_definitions.h')) { $Sdk = $cand; break }
    }
}
if (-not $Sdk) {
    throw "TS3 plugin SDK not found. Clone https://github.com/teamspeak/ts3client-pluginsdk and pass -Sdk <path>."
}
$sdkInc = if (Test-Path (Join-Path $Sdk 'include\plugin_definitions.h')) { Join-Path $Sdk 'include' } else { $Sdk }

$out   = Join-Path $BuildDir   'ts3_speaking_overlay_win64.dll'
$out32 = Join-Path $BuildDir32 'ts3_speaking_overlay_win32.dll'

foreach ($pair in @(@('x64', $Mingw64), @('x86', $Mingw32))) {
    if (-not (Test-Path (Join-Path $pair[1] 'bin\g++.exe'))) {
        throw "MinGW-w64 ($($pair[0]), msvcrt) not found: $($pair[1]). Pass -Mingw64 / -Mingw32."
    }
}

New-Item -ItemType Directory -Force -Path $BuildDir, (Join-Path $BuildDir 'src'), (Join-Path $BuildDir 'include') | Out-Null
New-Item -ItemType Directory -Force -Path $BuildDir32, (Join-Path $BuildDir32 'src'), (Join-Path $BuildDir32 'include') | Out-Null

# --- stage sources + SDK headers (both bitnesses share the same sources) ---
foreach ($b in @($BuildDir, $BuildDir32)) {
    Copy-Item (Join-Path $src 'src\*.cpp') (Join-Path $b 'src') -Force
    Copy-Item (Join-Path $src 'src\*.h')   (Join-Path $b 'src') -Force
    Copy-Item (Join-Path $sdkInc '*') (Join-Path $b 'include') -Recurse -Force
}

# --- compile helpers -----------------------------------------------------
$libs = @('-lgdiplus', '-lgdi32', '-luser32', '-lshell32', '-lole32', '-ld2d1', '-ldwrite')
$commonDefs = @('-D_WIN32_WINNT=0x0601', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-DUNICODE', '-D_UNICODE')
$commonFlags = @('-std=c++17', '-O2', '-static', '-static-libgcc', '-static-libstdc++', '-Wall', '-Wextra')

function Build-Plugin {
    param([string]$Gpp, [string]$BuildRoot, [string]$Out, [int]$ApiVersion, [string]$Label)

    # NOTE: each element must be parenthesised. In PowerShell '+' binds tighter
    # than ',', so `'-I' + X, '-I' + Y` collapses into ONE argument.
    $inc = @(('-I' + (Join-Path $BuildRoot 'src')), ('-I' + (Join-Path $BuildRoot 'include')))
    $a = @()
    $a += $commonFlags
    $a += @('-shared', '-o', $Out)
    $a += $inc
    $a += $commonDefs
    $a += @('-DPLUGIN_API_VERSION=' + $ApiVersion)
    $a += @((Join-Path $BuildRoot 'src\plugin.cpp'),
            (Join-Path $BuildRoot 'src\overlay.cpp'),
            (Join-Path $BuildRoot 'src\settings.cpp'))
    $a += $libs

    Write-Host ("compiling [$Label, API $ApiVersion] -> $Out")
    & $Gpp @a 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "$Label compile failed with exit code $LASTEXITCODE" }
}

# --- 1) 64-bit plugin (API 26) ------------------------------------------
Build-Plugin -Gpp (Join-Path $Mingw64 'bin\g++.exe') -BuildRoot $BuildDir `
             -Out $out -ApiVersion 26 -Label 'x64'

# --- 2) 32-bit plugin (API 25) ------------------------------------------
Build-Plugin -Gpp (Join-Path $Mingw32 'bin\g++.exe') -BuildRoot $BuildDir32 `
             -Out $out32 -ApiVersion 25 -Label 'x86'

# --- 3) local test programs (x64 only) ----------------------------------
$gpp = Join-Path $Mingw64 'bin\g++.exe'
$inc = @(('-I' + (Join-Path $BuildDir 'src')), ('-I' + (Join-Path $BuildDir 'include')))
$built = @($out, $out32)

if (-not $SkipTests) {
    $harness = Join-Path $BuildDir 'test_overlay.exe'
    Write-Host "compiling -> $harness"
    $hArgs = @('-std=c++17', '-O2', '-municode', '-mwindows',
               '-static', '-static-libgcc', '-static-libstdc++',
               '-Wall', '-Wextra', '-o', $harness) + $inc + $commonDefs +
             @((Join-Path $BuildDir 'src\test_overlay.cpp'),
               (Join-Path $BuildDir 'src\overlay.cpp')) + $libs
    & $gpp @hArgs 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "harness compile failed with exit code $LASTEXITCODE" }

    # NOTE: no -municode for the console probes: -municode makes the MinGW startup
    # code look for wmain() instead of main() and the link fails.
    $probe = Join-Path $BuildDir 'test_gamedetect.exe'
    Write-Host "compiling -> $probe"
    $pArgs = @('-std=c++17', '-O2',
               '-static', '-static-libgcc', '-static-libstdc++',
               '-Wall', '-Wextra', '-o', $probe) + $inc + $commonDefs +
             @((Join-Path $BuildDir 'src\test_gamedetect.cpp'),
               (Join-Path $BuildDir 'src\overlay.cpp')) + $libs
    & $gpp @pArgs 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "probe compile failed with exit code $LASTEXITCODE" }

    $fakegame = Join-Path $BuildDir 'test_fakegame.exe'
    Write-Host "compiling -> $fakegame"
    $fgArgs = @('-std=c++17', '-O2',
                '-static', '-static-libgcc', '-static-libstdc++',
                '-Wall', '-Wextra', '-o', $fakegame) + $inc + $commonDefs +
              @((Join-Path $BuildDir 'src\test_fakegame.cpp'),
                (Join-Path $BuildDir 'src\overlay.cpp')) + $libs
    & $gpp @fgArgs 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "fakegame compile failed with exit code $LASTEXITCODE" }

    $built += @($harness, $probe, $fakegame)
}

# --- verify the DLLs do not import UCRT apisets --------------------------
Write-Host ""
Write-Host "=== import check (must NOT list api-ms-win-crt-*) ==="
$objdump = Join-Path $Mingw64 'bin\objdump.exe'
foreach ($f in @($out, $out32)) {
    $names = & $objdump -p $f | Select-String -Pattern 'DLL Name:' | ForEach-Object { $_.Line.Split(':')[1].Trim() }
    $bad = @($names | Where-Object { $_ -like 'api-ms-win-crt-*' })
    Write-Host ("  " + (Split-Path $f -Leaf) + " -> " + ($names -join ', '))
    if ($bad.Count -gt 0) { throw ("UCRT apiset dependency still present in " + $f + ": " + ($bad -join ', ')) }
}

Get-Item $built | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
Write-Host "OK"
