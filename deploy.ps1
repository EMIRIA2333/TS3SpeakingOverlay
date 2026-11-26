# Deploy ts3-speaking-overlay into a local TeamSpeak 3 client (developer helper).
#
# Copies the built DLL into the TS3 plugin directory and registers it in
# addons.ini. TS3 is stopped first (the DLL cannot be replaced while the client
# holds it open) and restarted afterwards unless -NoRestart is given.
#
# Usage:
#   .\deploy.ps1 -Ts3Dir 'C:\Program Files\TeamSpeak 3 Client'
#   .\deploy.ps1 -Ts3Dir "$env:APPDATA\TS3Client"          # config dir variant
#   .\deploy.ps1 -Ts3Dir 'C:\TeamSpeak3' -BuildDir 'D:\build'
#
# -Ts3Dir may point at either the client folder (contains ts3client_win64.exe
# and config\) or the config folder itself (contains plugins\ and addons.ini).

param(
    # Client folder or config folder of the target TS3 installation
    [string]$Ts3Dir = '',
    # Directory holding ts3_speaking_overlay_win64.dll
    [string]$BuildDir = (Join-Path $env:TEMP 'ts3ovl_build'),
    # Which DLL to install; the 64-bit client needs the win64 one
    [string]$DllName = 'ts3_speaking_overlay_win64.dll',
    [switch]$NoRestart
)

$ErrorActionPreference = 'Stop'

if (-not $Ts3Dir) {
    # Fall back to the per-user config location, which is where a normal
    # (non-portable) install keeps its plugins.
    $Ts3Dir = Join-Path $env:APPDATA 'TS3Client'
}

$dll = Join-Path $BuildDir $DllName
if (-not (Test-Path $dll)) { throw "build output missing: $dll (run build.ps1 first, or pass -BuildDir)" }

# --- resolve config dir vs client dir ------------------------------------
$configDir = $null
if (Test-Path (Join-Path $Ts3Dir 'addons.ini')) {
    $configDir = $Ts3Dir                                     # already the config dir
} elseif (Test-Path (Join-Path $Ts3Dir 'config\addons.ini')) {
    $configDir = Join-Path $Ts3Dir 'config'                   # portable client folder
} elseif (Test-Path (Join-Path $Ts3Dir 'config')) {
    $configDir = Join-Path $Ts3Dir 'config'
} else {
    $configDir = $Ts3Dir
}

$plugins = Join-Path $configDir 'plugins'
$addons  = Join-Path $configDir 'addons.ini'
$entry   = 'SpeakingOverlay\files=' + $DllName

New-Item -ItemType Directory -Force -Path $plugins | Out-Null

# --- stop the client so the DLL is not locked ----------------------------
$proc = Get-Process ts3client_win64, ts3client -ErrorAction SilentlyContinue
if ($proc) {
    Write-Host ("stopping TS3 (pid " + (($proc | ForEach-Object { $_.Id }) -join ', ') + ")")
    $proc | Stop-Process -Force
    Start-Sleep -Seconds 3
}

# --- copy the DLL --------------------------------------------------------
Copy-Item $dll $plugins -Force
$installed = Join-Path $plugins $DllName
Write-Host ("installed {0} ({1} bytes)" -f $installed, (Get-Item $installed).Length)

# --- register in addons.ini ---------------------------------------------
if (-not (Test-Path $addons)) {
    Set-Content -Path $addons -Value "[plugin]" -Encoding Default
}

$lines = @(Get-Content $addons)
if (-not ($lines | Where-Object { $_ -match '^\s*\[plugin\]\s*$' })) {
    $lines = @('[plugin]') + $lines
}

# drop any previous registration of ours, keep everything else
$lines = @($lines | Where-Object { $_ -notmatch '^\s*SpeakingOverlay\\files\s*=' })

# insert directly after the [plugin] header
$out = New-Object System.Collections.Generic.List[string]
$inserted = $false
foreach ($line in $lines) {
    $out.Add($line)
    if (-not $inserted -and $line -match '^\s*\[plugin\]\s*$') {
        $out.Add($entry)
        $inserted = $true
    }
}
if (-not $inserted) { $out.Add($entry) }

Set-Content -Path $addons -Value $out -Encoding Default
Write-Host "registered in $addons as: $entry"

# Keep the user's settings: copy overlay.ini aside for rollback, but never move
# it away - the plugin would start from defaults (window position, game list and
# every switch) after each deploy.
$ini = Join-Path $plugins 'overlay.ini'
if (Test-Path $ini) {
    Copy-Item $ini ($ini + '.bak') -Force -ErrorAction SilentlyContinue
    Write-Host "kept existing overlay.ini (backup: overlay.ini.bak)"
}

# --- restart -------------------------------------------------------------
if (-not $NoRestart) {
    $exe = $null
    foreach ($cand in @('ts3client_win64.exe', 'ts3client.exe')) {
        $p = Join-Path (Split-Path $configDir -Parent) $cand
        if (Test-Path $p) { $exe = $p; break }
    }
    if (-not $exe) {
        Write-Host "client exe not found next to the config dir; start TeamSpeak manually"
    } else {
        Write-Host "starting TS3"
        Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe -Parent)
        Start-Sleep -Seconds 15
        $p = Get-Process ts3client_win64, ts3client -ErrorAction SilentlyContinue
        if ($p) {
            Write-Host ("TS3 running, pid " + (($p | ForEach-Object { $_.Id }) -join ', '))
            $mod = $null
            try { $mod = ($p | Select-Object -First 1).Modules | Where-Object { $_.ModuleName -eq $DllName } } catch { }
            if ($mod) { Write-Host "plugin loaded in-process: $($mod.FileName)" }
            else      { Write-Host "WARNING: plugin module not visible in the process (check the TS3 log)" }
        } else {
            Write-Host "WARNING: TS3 did not start"
        }
    }
}

Write-Host "OK"
