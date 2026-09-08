$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:/cygwin64/bin;' + $env:PATH
Push-Location $root
try {
    New-Item -ItemType Directory -Force -Path 'build/host-tools' | Out-Null
    & C:/cygwin64/bin/gcc.exe -std=c11 -O2 -Wall -Wextra -I main -I components/psk_rtty/include tools/host_rsid_check.c main/rsid.c main/voice_resample.c main/psk_modes.c -lm -o build/host-tools/host_rsid_check.exe
    if ($LASTEXITCODE) { throw 'RsID build failed' }
    & build/host-tools/host_rsid_check.exe
    if ($LASTEXITCODE) { throw 'RsID audio/vector checks failed' }
} finally { Pop-Location }
