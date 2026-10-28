# AF-2.1 host regression. Uses src/audio_url.h; it does not copy the parser.
# Requires g++ with C++17 on PATH. No extra libraries.
# From this directory: powershell -NoProfile -File .\run.ps1

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$compiler = if ($env:CXX) { $env:CXX } else { 'g++' }
$outDir = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'url_validation_test.exe'

& $compiler -std=c++17 -Wall -Wextra -Werror -o $exe (Join-Path $here 'url_validation_test.cpp')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $exe
exit $LASTEXITCODE
