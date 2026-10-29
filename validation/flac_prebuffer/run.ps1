# AF-2.5 host regression. Compiles production src/flac_decoder against shim/.
# From this directory: powershell -NoProfile -File .\run.ps1

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
$compiler = if ($env:CXX) { $env:CXX } else { $null }
if (-not $compiler) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { $compiler = $cmd.Source }
}
if (-not $compiler) {
    $found = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter g++.exe -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($found) { $compiler = $found.FullName }
}
if (-not $compiler) { Write-Error 'g++ not found. Set CXX to a C++17 compiler.' }

$stage = Join-Path $here 'build\stage'
$decDir = Join-Path $stage 'flac_decoder'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $decDir | Out-Null
Copy-Item (Join-Path $repo 'src\flac_decoder\flac_decoder.cpp') $decDir
Copy-Item (Join-Path $repo 'src\flac_decoder\flac_decoder.h') $decDir
Copy-Item (Join-Path $here 'shim\Audio.h') $stage
Copy-Item (Join-Path $here 'shim\psram_unique_ptr.hpp') $stage

$exe = Join-Path $here 'build\flac_prebuffer_test.exe'
& $compiler -std=c++17 -O0 -g -w "-I$decDir" -o $exe (Join-Path $here 'flac_prebuffer_test.cpp') (Join-Path $decDir 'flac_decoder.cpp')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$env:PATH = "$(Split-Path $compiler);$env:PATH"
& $exe
exit $LASTEXITCODE
