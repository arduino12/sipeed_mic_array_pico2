# Open ODAS Studio and stream the mic array into it through ODAS (running in WSL).
# The board must be in 8-channel mode. Ctrl+C stops the stream; close the
# Studio window when done.
#
#   powershell -ExecutionPolicy Bypass -File odas\run_odas_studio.ps1          # live room
#   powershell -ExecutionPolicy Bypass -File odas\run_odas_studio.ps1 -Play    # + test noise from the left speaker
param([switch]$Play,
      [string]$Studio = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'odas_studio'))
$root = Split-Path -Parent $PSScriptRoot

# Shells started from VS Code inherit ELECTRON_RUN_AS_NODE=1, which would make
# Electron run as plain Node and crash at startup.
Remove-Item Env:ELECTRON_RUN_AS_NODE -ErrorAction SilentlyContinue

if (-not (Get-NetTCPConnection -State Listen -LocalPort 9000 -ErrorAction SilentlyContinue)) {
    $exe = Join-Path $Studio 'node_modules\electron\dist\electron.exe'
    if (-not (Test-Path $exe)) { throw "ODAS Studio not installed: run odas\install_odas_studio.ps1" }
    Start-Process -FilePath $exe -ArgumentList '.' -WorkingDirectory $Studio
    for ($i = 0; $i -lt 30 -and -not (Get-NetTCPConnection -State Listen -LocalPort 9000 -ErrorAction SilentlyContinue); $i++) {
        Start-Sleep -Milliseconds 500
    }
}

$bridgeArgs = @((Join-Path $root 'tools\odas_bridge.py'), '--studio', '--seconds', '0')
if ($Play) { $bridgeArgs += '--play' }
python @bridgeArgs
