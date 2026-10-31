# AF-2.5R legacy FLAC buffering host contract.
# From this directory: powershell -NoProfile -File .\run.ps1
# Historical AF-2.5 decoder fixture copy: %TEMP%\af25r1\backup_af26\validation\flac_prebuffer\

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

$build = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build 'flac_prebuffer_test.exe'
& $compiler -std=c++17 -O0 -g -w "-I$(Join-Path $repo 'src')" -o $exe (Join-Path $here 'flac_prebuffer_test.cpp')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$env:PATH = "$(Split-Path $compiler);$env:PATH"
& $exe
exit $LASTEXITCODE
