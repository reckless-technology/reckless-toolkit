<#
.SYNOPSIS
    The toolkit's line coverage natively on Windows, held to CI's floor; with -Asan, the suites under AddressSanitizer
    too.

.DESCRIPTION
    Windows' counterpart of CI's coverage job (build.yml's "Coverage (gcov)", gcc on Linux), end to end and with no
    WSL. It finds the newest Visual Studio with the x64 C++ tools through vswhere, enters its developer environment and
    configures build\cov from nothing - the toolkit with its suites and without its windowed parts
    (RECKLESS_TOOLKIT_GUI=OFF, so nothing of Skia is fetched), Debug, Ninja - with Visual Studio's own clang++ and clang
    through their GNU driver at --coverage -O0, so the objects carry gcov's notes and the suites write gcov's counters.
    It builds the tree, runs every suite but the device suite (whose test devices are not the floor's business), and
    reads the counters with gcovr through Visual Studio's llvm-cov in its gcov mode (--gcov-executable "llvm-cov gcov"),
    on CI's filters - the parts the toolkit's own suites test: core/, gfx/ (not its Skia canvas), look/, view/, net/,
    update/ and licence/ - failing under CI's floor. gcc's gcov and llvm-cov count a line's coverage a little
    differently, and each platform compiles its own transport, so it is the percentage against the floor that
    compares, not the line counts.

    With -Asan it also configures build\asan from nothing with MSVC itself at RelWithDebInfo (Debug's /RTC1 cannot be
    combined with the sanitizer), /fsanitize=address with /bigobj and /INCREMENTAL:NO, builds it and runs the same suites
    under AddressSanitizer. -SkipCoverage runs the AddressSanitizer pass alone.

    gcovr is the one on the PATH, else uv's tool folder (%USERPROFILE%\.local\bin), where tools\setup-dev.ps1 puts it;
    -Gcovr names one outright. Runs under Windows PowerShell 5.1 and PowerShell 7.

.PARAMETER Asan
    Also build and run the suites under MSVC's AddressSanitizer, in build\asan.

.PARAMETER SkipCoverage
    Leave the coverage pass out (with -Asan, the AddressSanitizer pass alone).

.PARAMETER Jobs
    The build's and the tests' parallelism. Default: the machine's logical processors.

.PARAMETER Gcovr
    The gcovr executable, where it is neither on the PATH nor in %USERPROFILE%\.local\bin.

.PARAMETER CMakeArgs
    More arguments for both configures - a FETCHCONTENT_SOURCE_DIR_<NAME> naming another tree's fetched sources, say.

.EXAMPLE
    tools\coverage-windows.ps1

.EXAMPLE
    tools\coverage-windows.ps1 -Asan -Jobs 16
#>
[CmdletBinding()]
param(
    [switch] $Asan,
    [switch] $SkipCoverage,
    [int] $Jobs = [Environment]::ProcessorCount,
    [string] $Gcovr = '',
    [string[]] $CMakeArgs = @()
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# CI's floor, read from build.yml so the two cannot drift.
$floorLine = Select-String -Path '.github/workflows/build.yml' -Pattern '--fail-under-line ([0-9.]+)' | Select-Object -First 1
if (-not $floorLine) { throw 'no --fail-under-line in .github/workflows/build.yml' }
$floor = $floorLine.Matches[0].Groups[1].Value

# The developer environment of the newest Visual Studio with the x64 C++ tools; its clang, its llvm-cov and, for -Asan,
# its AddressSanitizer are Visual Studio's own components (tools\setup-dev.ps1 installs them).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere - is Visual Studio (or its Build Tools) installed?" }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the x64 C++ tools found: install the "Desktop development with C++" workload.' }
$vsdev = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $vsdev)) { throw "VsDevCmd.bat not found under $vs" }
$llvm = Join-Path $vs 'VC\Tools\Llvm\x64\bin'
$extra = ($CMakeArgs | ForEach-Object { "`"$_`"" }) -join ' '

# Everything cmake and ctest run inside the developer environment, one cmd invocation a step so its exit code is ours.
function Invoke-Dev([string] $command) {
    cmd /c "call `"$vsdev`" -arch=x64 -host_arch=x64 -no_logo >nul 2>&1 && $command"
    if ($LASTEXITCODE -ne 0) { throw "failed (exit $LASTEXITCODE): $command" }
}

# A tree started from nothing: a kept one would add this run's counters to the last run's.
function Reset-Tree([string] $tree) {
    if (Test-Path $tree) {
        Write-Host "== Removing $tree"
        Remove-Item -Recurse -Force $tree
    }
}

if (-not $SkipCoverage) {
    $clangxx = Join-Path $llvm 'clang++.exe'
    $clang = Join-Path $llvm 'clang.exe'
    $llvmCov = Join-Path $llvm 'llvm-cov.exe'
    foreach ($tool in $clangxx, $clang, $llvmCov) {
        if (-not (Test-Path $tool)) {
            throw "$tool not found: add Visual Studio's ""C++ Clang tools for Windows"" (Microsoft.VisualStudio.Component.VC.Llvm.Clang)"
        }
    }
    if (-not $Gcovr) {
        $found = Get-Command gcovr -ErrorAction SilentlyContinue
        if ($found) {
            $Gcovr = $found.Source
        } else {
            $Gcovr = Join-Path $env:USERPROFILE '.local\bin\gcovr.exe'
        }
    }
    if (-not (Test-Path $Gcovr)) { throw "gcovr not found (looked on the PATH and at $Gcovr): run tools\setup-dev.ps1, or pass -Gcovr" }

    $tree = 'build\cov'
    Reset-Tree $tree
    Write-Host "== Configuring $tree (Visual Studio's clang++, --coverage -O0, Debug, no GUI)"
    Invoke-Dev ("cmake -S . -B $tree -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRECKLESS_TOOLKIT_GUI=OFF " +
                "-DCMAKE_C_COMPILER=`"$clang`" -DCMAKE_CXX_COMPILER=`"$clangxx`" " +
                "-DCMAKE_C_FLAGS=`"--coverage -O0`" -DCMAKE_CXX_FLAGS=`"--coverage -O0`" " +
                "-DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage $extra")
    Write-Host "== Building $tree"
    Invoke-Dev "cmake --build $tree -j $Jobs"
    Write-Host "== Testing $tree"
    $env:RECKLESS_UPDATE_CHECK = 'off'
    Invoke-Dev "ctest --test-dir $tree -j $Jobs --output-on-failure -E `"^audio_devices$`""

    # gcovr runs llvm-cov by the name it is given, so Visual Studio's LLVM goes first on the PATH for it. The filters
    # and the floor are CI's coverage job's, word for word.
    Write-Host "== Coverage floor (core, gfx, look, view, net, update, licence: $floor % of lines)"
    $env:PATH = "$llvm;$env:PATH"
    & $Gcovr -r . --gcov-executable 'llvm-cov gcov' --gcov-ignore-parse-errors=suspicious_hits.warn `
        --object-directory $tree --filter '(core|gfx|look|view|net|update|licence)/' --exclude 'gfx/skia/' `
        --exclude '.*/tests/' --gcov-exclude-directories '.*/CMakeFiles/[0-9][^/]*/.*' `
        --gcov-exclude-directories '.*/_deps/.*' --txt --print-summary --fail-under-line $floor $tree
    if ($LASTEXITCODE -ne 0) { throw "gcovr failed (exit $LASTEXITCODE): under the floor, or it could not read the counters" }
}

if ($Asan) {
    $tree = 'build\asan'
    Reset-Tree $tree
    # RelWithDebInfo, since Debug's /RTC1 is refused beside /fsanitize=address; /bigobj for the instrumented objects'
    # sections; no incremental link, which the sanitizer's runtime does not take.
    Write-Host "== Configuring $tree (MSVC, /fsanitize=address, RelWithDebInfo)"
    Invoke-Dev ("cmake -S . -B $tree -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo " +
                "-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl " +
                "-DCMAKE_C_FLAGS=`"/DWIN32 /D_WINDOWS /fsanitize=address`" " +
                "-DCMAKE_CXX_FLAGS=`"/DWIN32 /D_WINDOWS /GR /EHsc /fsanitize=address /bigobj`" " +
                "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=`"/debug /INCREMENTAL:NO`" " +
                "-DCMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO=`"/debug /INCREMENTAL:NO`" $extra")
    Write-Host "== Building $tree"
    Invoke-Dev "cmake --build $tree -j $Jobs"
    # The sanitizer's runtime DLL is in the developer environment's MSVC bin folder, on the PATH ctest runs under.
    Write-Host "== Testing $tree under AddressSanitizer"
    $env:RECKLESS_UPDATE_CHECK = 'off'
    Invoke-Dev "ctest --test-dir $tree -j $Jobs --output-on-failure -E `"^audio_devices$`""
}
Write-Host '== Done'
