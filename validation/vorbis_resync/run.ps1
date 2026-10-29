# AF-2.4 host regression. Compiles production src/vorbis_decoder against shim/.
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
$decDir = Join-Path $stage 'vorbis_decoder'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $decDir | Out-Null
Copy-Item (Join-Path $repo 'src\vorbis_decoder\vorbis_decoder.cpp') $decDir
Copy-Item (Join-Path $repo 'src\vorbis_decoder\vorbis_decoder.h') $decDir
Copy-Item (Join-Path $repo 'src\vorbis_decoder\lookup.h') $decDir
Copy-Item (Join-Path $here 'shim\Arduino.h') $decDir
Copy-Item (Join-Path $here 'shim\Audio.h') $stage
Copy-Item (Join-Path $here 'shim\psram_unique_ptr.hpp') $stage

$exe = Join-Path $here 'build\vorbis_resync_test.exe'
& $compiler -std=c++17 -O0 -g -w "-I$decDir" -o $exe (Join-Path $here 'vorbis_resync_test.cpp') (Join-Path $decDir 'vorbis_decoder.cpp')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$env:PATH = "$(Split-Path $compiler);$env:PATH"
& $exe
exit $LASTEXITCODE
