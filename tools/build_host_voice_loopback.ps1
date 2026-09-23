# Builds tools/host_voice_loopback.exe (PC realtime voice loopback test tool)
# using the Cygwin gcc host toolchain, reusing the real firmware DSP sources:
#   - components/codec2        (Codec2 700C voice codec)
#   - components/psk_rtty      (PSK modem core)
#   - main/psk_tx.c psk_rx.c psk_codec.c psk_modes.c
#   - main/voice_protocol.c voice_playout.c voice_resample.c audio_decim.c
#
# NOTE: Cygwin gcc is required (same toolchain as tools/run_voice_checks.py).
# MSVC cannot compile the codec2 vendor tree as-is: it uses C99 VLA arrays
# (e.g. float x[nK];), which the MSVC C compiler rejects even under /std:c11.
#
# Output: build/host-tools/host_voice_loopback.exe
param(
    [string]$CygwinGcc = "C:\cygwin64\bin\gcc.exe"
)

$root = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $root "build\host-tools"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outExe = Join-Path $OutDir "host_voice_loopback.exe"

if (-not (Test-Path $CygwinGcc)) {
    Write-Error "Cygwin gcc not found at $CygwinGcc (codec2 host builds need Cygwin, see run_voice_checks.py)"
}

$c2Vendor = Join-Path $root "components\codec2\vendor\src"
$c2Gen = Join-Path $root "components\codec2\generated"
$inc = @(
    "-I$c2Vendor",
    "-I$(Join-Path $root 'components\codec2\include')",
    "-I$(Join-Path $root 'main')",
    "-I$(Join-Path $root 'components\psk_rtty\include')"
)

$codec2Src = @(
    "codec2.c", "codec2_fft.c", "dump.c", "lpc.c", "nlp.c", "postfilter.c",
    "sine.c", "kiss_fft.c", "kiss_fftr.c", "interp.c", "lsp.c", "mbest.c",
    "newamp1.c", "phase.c", "quantise.c", "pack.c"
) | ForEach-Object { Join-Path $c2Vendor $_ }
$codec2Gen = @(
    "codebook.c", "codebookd.c", "codebookjmv.c", "codebookge.c",
    "codebooknewamp1.c", "codebooknewamp1_energy.c"
) | ForEach-Object { Join-Path $c2Gen $_ }
$voiceSrc = @(
    "tools\host_voice_loopback.c",
    "main\voice_protocol.c",
    "main\voice_playout.c",
    "main\voice_resample.c",
    "main\audio_decim.c",
    "main\psk_tx.c",
    "main\psk_rx.c",
    "main\psk_codec.c",
    "main\psk_modes.c",
    "components\psk_rtty\psk_rtty.c"
) | ForEach-Object { Join-Path $root $_ }

$sources = $codec2Src + $codec2Gen + $voiceSrc

Write-Host "Building with Cygwin gcc..."
$env:PATH = "$(Split-Path -Parent $CygwinGcc);$env:PATH"
& $CygwinGcc -std=gnu11 -O2 -Wall -Wextra `
    -DCODEC2_MODE_EN_DEFAULT=0 -DCODEC2_MODE_700C_EN=1 `
    $inc $sources -lwinmm -lm -o $outExe

if ($LASTEXITCODE -ne 0 -or -not (Test-Path $outExe)) {
    Write-Error "build failed: $outExe not produced (gcc exit $LASTEXITCODE)"
}

# Cygwin-built exes need cygwin1.dll; copy it beside the exe so the tool
# runs standalone (Windows loads DLLs from the application directory first).
$cygDll = Join-Path (Split-Path -Parent $CygwinGcc) "cygwin1.dll"
if (Test-Path $cygDll) {
    Copy-Item $cygDll (Join-Path $OutDir "cygwin1.dll") -Force
    Write-Host "copied cygwin1.dll next to exe"
} else {
    Write-Warning "cygwin1.dll not found beside $CygwinGcc; exe needs Cygwin bin on PATH to run"
}
Write-Host "OK: $outExe"
