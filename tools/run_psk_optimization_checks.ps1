param([string]$Python = 'C:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$gcc = 'C:/cygwin64/bin/gcc.exe'
$env:PATH = 'C:/cygwin64/bin;' + $env:PATH
$out = Join-Path $root 'build/host-tools'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$common = @('-std=c11', '-O3', '-Wall', '-Wextra', '-I', 'main', '-I', 'components/psk_rtty/include')
$receiver = @('main/psk_rx.c', 'main/psk_modes.c', 'main/psk_codec.c', 'components/psk_rtty/psk_rtty.c')
Push-Location $root
try {
    & $gcc @common tools/host_viterbi_check.c main/psk_codec.c -lm -o "$out/host_viterbi_check.exe"
    if ($LASTEXITCODE) { throw 'Viterbi oracle build failed' }
    & "$out/host_viterbi_check.exe"
    if ($LASTEXITCODE) { throw 'Viterbi/deinterleaver oracle failed' }

    & $gcc @common -DPAYLOAD_BYTES=512 tools/host_rate_check.c main/audio_decim.c main/psk_tx.c @receiver -lm -o "$out/host_k16_check.exe"
    if ($LASTEXITCODE) { throw 'K16 test build failed' }
    foreach ($mode in @('8PSK125F', '8PSK250F')) {
        foreach ($drops in @(0, 1)) {
            & "$out/host_k16_check.exe" $mode 0 $drops
            if ($LASTEXITCODE) { throw "$mode payload/reacquisition check failed" }
        }
    }

} finally { Pop-Location }
