# AF-2.2 host regression. Compiles production src/vorbis_decoder against the
# host shims in shim/. The decoder sources are staged so their relative
# includes resolve to the shims, not to a second copy of the algorithm.
# Compile inputs are repo src/vorbis_decoder plus shim/. Generated output is build/.
# From this directory: powershell -NoProfile -File .\run.ps1
# Isolate each case in its own process: powershell -NoProfile -File .\run.ps1 -Isolate

param([switch]$Isolate)

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
if (-not $compiler) {
    Write-Error 'g++ not found. Set CXX to a C++17 compiler.'
}

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

$exe = Join-Path $here 'build\vorbis_bounds_test.exe'
$src = Join-Path $decDir 'vorbis_decoder.cpp'
$test = Join-Path $here 'vorbis_bounds_test.cpp'
& $compiler -std=c++17 -O0 -g -w "-I$decDir" -o $exe $test $src
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$env:PATH = "$(Split-Path $compiler);$env:PATH"

if ($Isolate) {
    $names = & $exe --list
    $failed = 0
    foreach ($name in $names) {
        if (-not $name) { continue }
        & $exe $name
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-Host "ISOLATED $name exit=$code"
            $failed++
        }
    }
    if ($failed -ne 0) { exit 1 }
    Write-Host 'vorbis_bounds: PASS'
    exit 0
}

& $exe
exit $LASTEXITCODE
