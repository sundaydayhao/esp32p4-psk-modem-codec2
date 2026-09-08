$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gcc = 'C:/cygwin64/bin/gcc.exe'
$env:PATH = 'C:/cygwin64/bin;' + $env:PATH
$out = Join-Path $root 'build/host-tools'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $root
try {
    & $gcc -std=c11 -O2 -Wall -Wextra -I main -I components/psk_rtty/include tools/host_content_check.c main/modem_image.c main/voice_protocol.c main/psk_rx.c main/psk_modes.c main/psk_codec.c main/psk_tx.c components/psk_rtty/psk_rtty.c -lm -o "$out/host_content_check.exe"
    if ($LASTEXITCODE) { throw 'Content test build failed' }
    & "$out/host_content_check.exe"
    if ($LASTEXITCODE) { throw 'Content protocol or PSK roundtrip failed' }
    & python tools/run_voice_checks.py
    if ($LASTEXITCODE) { throw 'Codec2 checks failed' }
} finally { Pop-Location }
