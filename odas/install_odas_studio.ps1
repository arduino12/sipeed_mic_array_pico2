# Install ODAS Studio (introlab/odas_web, the ODAS desktop GUI) next to this project.
#
#   powershell -ExecutionPolicy Bypass -File odas\install_odas_studio.ps1
#
# The app pins Electron 5 and the native `grpc` module. grpc cannot be built
# here (no C++ compiler), and Electron 5's own installer cannot unzip on
# current Node. It is only needed for the optional Google speech-to-text, so:
#   - npm install --ignore-scripts      (no native builds)
#   - unzip the cached Electron 5 build by hand
#   - make the speech client load lazily, only if transcription is switched on
param([string]$Dest = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'odas_studio'))
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Dest)) { git clone --depth 1 https://github.com/introlab/odas_web.git $Dest }
Push-Location $Dest
try {
    npm install --ignore-scripts --no-audit --no-fund --loglevel=error
    $env:ELECTRON_RUN_AS_NODE = $null
    node node_modules\electron\install.js   # downloads the zip into the cache (its unzip step fails)
    $ver = (Get-Content node_modules\electron\package.json | ConvertFrom-Json).version
    $zip = Get-ChildItem "$env:LOCALAPPDATA\electron\Cache", "$env:USERPROFILE\.electron" -Recurse `
        -Filter "electron-v$ver-win32-x64.zip" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $zip) { throw "Electron $ver zip not found in the cache" }
    Expand-Archive -Path $zip.FullName -DestinationPath node_modules\electron\dist -Force
    Set-Content -Path node_modules\electron\path.txt -Value 'electron.exe' -NoNewline -Encoding ascii

    foreach ($f in 'stream-to-text.js', 'resources\js\speech-to-text.js') {
        $s = Get-Content $f -Raw
        if ($s -notmatch 'loadSpeech') {
            $s = $s.Replace("const speech = require('@google-cloud/speech');",
                "// Loaded on first use: the Google speech client needs native grpc, which is not`n" +
                "// built here, and transcription is optional (off by default).`n" +
                "let speech = null;`n" +
                "const loadSpeech = () => speech || (speech = require('@google-cloud/speech'));")
            $s = $s.Replace('new speech.SpeechClient', 'new (loadSpeech().SpeechClient)')
            Set-Content -Path $f -Value $s -NoNewline -Encoding utf8
        }
    }
    Write-Host "ODAS Studio installed in $Dest"
} finally { Pop-Location }
