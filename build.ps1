# Build script for Fractal Additive (Windows / Visual Studio bundled toolchain).
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1 [-Config Debug|Release]
param(
    [ValidateSet('Debug','Release')]
    [string]$Config = 'Debug'
)

$ErrorActionPreference = 'Stop'

$vsRoot = 'C:\Program Files\Microsoft Visual Studio\18\Community'
$cmake  = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

Set-Location $PSScriptRoot

$buildDir = 'build'

# Use CMake's default generator (the native Visual Studio one). It is multi-config,
# so the build type is chosen at build time with --config.
& $cmake -B $buildDir
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

& $cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Write-Host ""
Write-Host "==== BUILD OK ($Config) ====" -ForegroundColor Green
$artefacts = Join-Path $buildDir 'FractalAdditive_artefacts'
Get-ChildItem -Path $artefacts -Recurse -Include *.exe,*.vst3 -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host $_.FullName }
