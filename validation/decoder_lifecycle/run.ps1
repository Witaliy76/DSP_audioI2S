# AF-2.6A host regression. Uses production src/decoder_lifecycle.h.
# Prefer g++/clang++ when present; otherwise PlatformIO platform=native.
# From this directory: powershell -NoProfile -File .\run.ps1

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $here 'decoder_lifecycle_test.cpp'
$outDir = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'decoder_lifecycle_test.exe'

$compiler = $null
if ($env:CXX -and (Get-Command $env:CXX -ErrorAction SilentlyContinue)) {
    $compiler = $env:CXX
} elseif (Get-Command g++ -ErrorAction SilentlyContinue) {
    $compiler = 'g++'
} elseif (Get-Command clang++ -ErrorAction SilentlyContinue) {
    $compiler = 'clang++'
}

if ($compiler) {
    & $compiler -std=c++20 -pthread -Wall -Wextra -Werror -o $exe $src
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $exe
    exit $LASTEXITCODE
}

$pioSrcDir = Join-Path $here 'src'
New-Item -ItemType Directory -Force -Path $pioSrcDir | Out-Null
$pioSrc = Join-Path $pioSrcDir 'decoder_lifecycle_test.cpp'
$contents = Get-Content -Raw -Path $src
$contents = $contents.Replace('#include "../../src/decoder_lifecycle.h"', '#include "../../../src/decoder_lifecycle.h"')
Set-Content -Path $pioSrc -Value $contents -NoNewline -Encoding utf8

$pio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
& $pio run -d $here -e native
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$built = Get-ChildItem (Join-Path $here '.pio\build\native') -Filter 'program.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $built) {
    $built = Get-ChildItem (Join-Path $here '.pio\build\native') -Filter '*.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $built) { Write-Error 'native build produced no exe'; exit 1 }
Copy-Item $built $exe -Force
& $exe
exit $LASTEXITCODE
