param([string]$BuildDir = "build-codec2")
$ErrorActionPreference = "Stop"
& { . 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1' }
Set-Location (Split-Path -Parent $PSScriptRoot)
idf.py -B $BuildDir build
exit $LASTEXITCODE
