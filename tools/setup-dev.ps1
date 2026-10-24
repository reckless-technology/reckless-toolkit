<#
.SYNOPSIS
  Set up a Windows machine to build, test, debug and measure the Reckless Technology toolkit: the toolchain, the
  tools and the editor.

.DESCRIPTION
  Each component is checked first and installed only when it is missing or not the version the repo wants, so the
  script is safe to run again at any time. Everything goes into the user's own profile and needs no administrator,
  except Visual Studio, which winget or the Visual Studio installer changes behind a UAC prompt. tools/setup-dev.sh is
  the same for Linux and macOS. VS Code's "setup environment" and "check environment" tasks run the one for the
  platform.

  What it sets up:
    - Git for Windows
    - Visual Studio (any edition, the Build Tools included) with the x64 C++ tools, a Windows SDK, CMake and Ninja,
      Visual Studio's clang with its llvm-cov (tools/coverage-windows.ps1) and AddressSanitizer
      (tools/coverage-windows.ps1 -Asan)
    - uv, Astral's tool installer, and with it clang-format and gcovr at the versions CI pins (build.yml)
    - the repo's git hooks (core.hooksPath .githooks) and the VS Code extensions .vscode/extensions.json recommends

  Building happens in Visual Studio's developer environment (VsDevCmd.bat), which puts cmake, ninja and cl on the
  PATH: run VS Code or the build from a "Developer PowerShell for VS".

.PARAMETER Check
  Report what is present and what is missing, install nothing, and exit 1 if anything is missing.

.PARAMETER NoPersist
  Do not add anything to the user's PATH for good (only to this process's): for a test run in a scratch profile.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\setup-dev.ps1           # set up
  powershell -ExecutionPolicy Bypass -File tools\setup-dev.ps1 -Check    # report only
#>
param([switch]$Check, [switch]$NoPersist)
# Continue, not Stop: Windows PowerShell 5.1 makes a native command's stderr a terminating error under Stop, even
# redirected (uv and git print their progress there). The cmdlets that must stop say so; native commands' exit codes are
# checked, and their stderr is shown as text ("$_"), not as 5.1's error records.
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
Set-Location (Split-Path -Parent $PSScriptRoot)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---------------------------------------------------------------------------------------------------------------------
# Pins. The versions CI uses are read from where CI reads them, so the two cannot drift.
function Get-Pin([string]$file, [string]$pattern) {
  $match = Select-String -Path $file -Pattern $pattern -ErrorAction Stop | Select-Object -First 1
  if (-not $match) { throw "no pin matching '$pattern' in $file" }
  return $match.Matches[0].Groups[1].Value
}
$ClangFormatVersion = Get-Pin '.github/workflows/build.yml' "clang-format==([0-9.]+)"
$GcovrVersion = Get-Pin '.github/workflows/build.yml' 'gcovr==([0-9.]+)'

# Astral's installer for this uv release, which carries the hash of every uv archive it may fetch.
$UvVersion = '0.12.7'
$UvInstallerSha256 = '993933a785b8d695026f0a7869cc2dda01dc41e7329a4c975df2edfa96f229e7'

# ---------------------------------------------------------------------------------------------------------------------
# Helpers (the same in every reckless-technology repo's setup script).
$script:missing = 0
function Write-Ok([string]$name, [string]$detail) { Write-Host ("  ok       {0,-24} {1}" -f $name, $detail) -ForegroundColor Green }
function Write-Missing([string]$name, [string]$detail) {
  $script:missing++
  Write-Host ("  missing  {0,-24} {1}" -f $name, $detail) -ForegroundColor Yellow
}
function Write-Installing([string]$name, [string]$detail) { Write-Host ("  install  {0,-24} {1}" -f $name, $detail) -ForegroundColor Cyan }

# Put a directory on this process's PATH and, unless -NoPersist, on the user's PATH for every new shell.
function Add-ToPath([string]$dir) {
  if (($env:PATH -split ';') -notcontains $dir) { $env:PATH = "$dir;$env:PATH" }
  if ($NoPersist -or $Check) { return }
  $user = [Environment]::GetEnvironmentVariable('Path', 'User')
  if ((($user -split ';') | Where-Object { $_ }) -notcontains $dir) {
    [Environment]::SetEnvironmentVariable('Path', (@($dir) + (($user -split ';') | Where-Object { $_ })) -join ';', 'User')
  }
}

# Download a file and refuse it unless its SHA-256 is the one given.
function Get-VerifiedFile([string]$url, [string]$sha256, [string]$out) {
  Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -ErrorAction Stop
  $got = (Get-FileHash $out -Algorithm SHA256).Hash
  if ($got -ne $sha256.ToUpper()) { Remove-Item $out; throw "SHA-256 mismatch for $url (got $got, want $sha256)" }
}

function New-ScratchDir {
  $dir = Join-Path ([IO.Path]::GetTempPath()) ("setup-dev-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
  New-Item -ItemType Directory -Path $dir -ErrorAction Stop | Out-Null
  return $dir
}

# The first program of a name on the PATH and the first line it prints for --version, or $null.
function Get-ToolVersion([string]$name, [string]$flag = '--version') {
  $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $command) { return $null }
  $line = (& $command.Source $flag 2>&1 | Select-Object -First 1) -as [string]
  return @{ Path = $command.Source; Version = $line }
}

# Visual Studio (any edition, the Build Tools included) with the x64 C++ tools, found by vswhere.
function Find-VisualStudio([string[]]$components) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { return $null }
  $found = & $vswhere -latest -products * -requires ($components | Select-Object -Unique) -property installationPath
  if ($found) { return ($found | Select-Object -First 1) }
  return $null
}

# Install Visual Studio Build Tools 2022 with the C++ workload, or add the components to the Visual Studio already
# there (its own workload carries whatever its edition calls the C++ one). Both need an administrator: winget and the
# Visual Studio installer raise the UAC prompt themselves.
function Install-VisualStudio([string[]]$components) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  $existing = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath | Select-Object -First 1 }
  if ($existing) {
    Write-Installing 'Visual Studio' "adding the C++ components to $existing (UAC prompt)"
    $setup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    $add = $components | ForEach-Object { "--add $_" }
    $p = Start-Process -FilePath $setup -ArgumentList (@('modify', '--installPath', "`"$existing`"", '--passive', '--norestart', '--includeRecommended') + $add) -Wait -PassThru
  }
  else {
    Write-Installing 'Visual Studio' 'Build Tools 2022 with the C++ workload, through winget (UAC prompt)'
    $add = @('Microsoft.VisualStudio.Workload.VCTools') + $components | ForEach-Object { "--add $_" }
    $override = "--wait --passive --norestart --includeRecommended " + ($add -join ' ')
    $p = Start-Process -FilePath winget -ArgumentList @('install', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '-e',
      '--accept-package-agreements', '--accept-source-agreements', '--override', "`"$override`"") -Wait -PassThru -NoNewWindow
  }
  if ($p.ExitCode -notin 0, 3010) { throw "the Visual Studio installer exited with $($p.ExitCode)" }
}

function Set-GitHooks {
  if (-not (Test-Path '.githooks')) { return }
  $current = git config core.hooksPath
  if ($current -eq '.githooks') { Write-Ok 'git hooks' 'core.hooksPath .githooks'; return }
  if ($Check) { Write-Missing 'git hooks' 'core.hooksPath is not .githooks'; return }
  git config core.hooksPath .githooks
  Write-Installing 'git hooks' 'core.hooksPath .githooks'
}

# The extensions .vscode/extensions.json recommends, when VS Code's command line is on the PATH.
function Install-VSCodeExtensions {
  $code = Get-Command code -ErrorAction SilentlyContinue
  if (-not $code -or -not (Test-Path '.vscode/extensions.json')) { return }
  $json = (Get-Content '.vscode/extensions.json' -Raw) -replace '(?m)^\s*//.*$', '' | ConvertFrom-Json
  $installed = & $code.Source --list-extensions 2>$null
  foreach ($ext in $json.recommendations) {
    if ($installed -contains $ext) { Write-Ok 'VS Code extension' $ext; continue }
    if ($Check) { Write-Missing 'VS Code extension' $ext; continue }
    Write-Installing 'VS Code extension' $ext
    & $code.Source --install-extension $ext --force | Out-Null
  }
}

# A uv tool at a version (exactly, or with -AtLeast that or later), its executable the first of its name on the PATH:
# installed, or reinstalled at the version, with uv tool install into uv's bin directory, ~/.local/bin.
function Test-ToolVersion($tool, [string]$version, [bool]$atLeast) {
  if (-not $tool -or $tool.Version -notmatch '(\d+\.\d+(\.\d+)?)') { return $false }
  if ($atLeast) { return ([version]$Matches[1] -ge [version]$version) }
  return ($Matches[1] -eq $version)
}
function Install-UvTool([string]$name, [string]$version, [string]$why, [switch]$AtLeast) {
  $want = if ($AtLeast) { "$version or later" } else { $version }
  $tool = Get-ToolVersion $name
  if (Test-ToolVersion $tool $version $AtLeast) { Write-Ok $name "$($tool.Version) ($($tool.Path))"; return }
  $found = if ($tool) { "found $($tool.Version) at $($tool.Path)" } else { 'none on the PATH' }
  if ($Check -or -not $script:uv) { Write-Missing $name "$want, $why ($found)"; return }
  Write-Installing $name "$version through uv tool install"
  & $script:uv tool install --force "$name==$version" 2>&1 | ForEach-Object { "$_" } | Out-Host
  if ($LASTEXITCODE -ne 0) { throw "uv tool install $name==$version exited with $LASTEXITCODE" }
  $tool = Get-ToolVersion $name
  if (-not (Test-ToolVersion $tool $version $AtLeast)) {
    Write-Missing $name "$version is installed in $localBin, but $($tool.Path) comes first on the PATH: move it after"
  }
}

# ---------------------------------------------------------------------------------------------------------------------
Write-Host "Reckless Technology toolkit: $(if ($Check) { 'checking' } else { 'setting up' }) the Windows development environment"

$localBin = Join-Path $env:USERPROFILE '.local\bin'

# Git for Windows.
$git = Get-Command git -ErrorAction SilentlyContinue
if ($git) { Write-Ok 'Git for Windows' "$(git --version) ($($git.Source))" }
elseif ($Check) { Write-Missing 'Git for Windows' 'git' }
else {
  Write-Installing 'Git for Windows' 'through winget'
  winget install --id Git.Git -e --accept-package-agreements --accept-source-agreements
  Write-Host '  Git for Windows is installed: open a new terminal and run this script again.'
  exit 0
}

# Visual Studio: the x64 C++ tools (MSVC), CMake and Ninja (the presets' generator), its clang and llvm-cov
# (tools/coverage-windows.ps1) and AddressSanitizer (its -Asan), and a Windows SDK.
$vsCore = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$vsParts = [ordered]@{
  'Microsoft.VisualStudio.Component.VC.CMake.Project' = 'CMake and Ninja'
  'Microsoft.VisualStudio.Component.VC.Llvm.Clang'    = 'clang and llvm-cov'
  'Microsoft.VisualStudio.Component.VC.ASAN'          = 'AddressSanitizer'
}
$vs = Find-VisualStudio @($vsCore)
$sdk = Test-Path "${env:ProgramFiles(x86)}\Windows Kits\10\Include\10.*"
$lacking = @()
if (-not $vs) { $lacking += $vsCore }
foreach ($part in $vsParts.Keys) { if (-not (Find-VisualStudio @($vsCore, $part))) { $lacking += $part } }
if (-not $sdk) { $lacking += 'Microsoft.VisualStudio.Component.Windows11SDK.26100' }
$names = @('MSVC') + @($vsParts.Values) + @('Windows SDK')
if (-not $lacking) { Write-Ok 'Visual Studio C++' "$vs ($($names -join ', '))" }
elseif ($Check) { Write-Missing 'Visual Studio C++' "needs $($lacking -join ', ')" }
elseif ($env:GITHUB_ACTIONS -eq 'true') {
  # A CI runner is a shared machine, and its Visual Studio is its owner's to change: never from a job.
  Write-Missing 'Visual Studio C++' "needs $($lacking -join ', ') (not changed on a CI runner)"
}
else { Install-VisualStudio (@($vsCore) + $lacking | Select-Object -Unique) }

# uv, which installs clang-format and gcovr. Its executables go to ~/.local/bin, spelled out from USERPROFILE so a
# profile redirected for a test run is honoured whatever uv takes the home folder to be.
$env:UV_TOOL_BIN_DIR = $localBin
$script:uv = $null
$uvExe = Join-Path $localBin 'uv.exe'
if (Test-Path $uvExe) { Add-ToPath $localBin }
$found = Get-Command uv -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($found) { $script:uv = $found.Source; Write-Ok 'uv' "$((& $script:uv --version) -as [string]) ($($script:uv))" }
elseif ($Check) { Write-Missing 'uv' 'which installs clang-format and gcovr' }
else {
  Write-Installing 'uv' "Astral's installer for uv $UvVersion"
  $scratch = New-ScratchDir
  $uvInstaller = Join-Path $scratch 'uv-installer.ps1'
  Get-VerifiedFile "https://astral.sh/uv/$UvVersion/install.ps1" $UvInstallerSha256 $uvInstaller
  # The installer runs in a process of its own with the install directory spelled out, so a profile redirected through
  # USERPROFILE is honoured; this script puts the directory on the PATH itself.
  $env:UV_INSTALL_DIR = $localBin
  $env:UV_NO_MODIFY_PATH = '1'
  $shell = (Get-Process -Id $PID).Path
  & $shell -NoProfile -ExecutionPolicy Bypass -File $uvInstaller 2>&1 | ForEach-Object { "$_" } | Out-Host
  $exit = $LASTEXITCODE
  Remove-Item Env:UV_INSTALL_DIR, Env:UV_NO_MODIFY_PATH
  Remove-Item -Recurse -Force $scratch
  if ($exit -ne 0 -or -not (Test-Path $uvExe)) { throw "the uv installer failed ($exit)" }
  Add-ToPath $localBin
  $script:uv = $uvExe
}

# clang-format exactly at CI's version (build.yml): another release restyles code this one leaves alone. gcovr at CI's
# version or later, whose reports agree: a machine with a newer one keeps it.
Install-UvTool 'clang-format' $ClangFormatVersion 'which the pre-commit hook and CI format with'
Install-UvTool 'gcovr' $GcovrVersion "CI's coverage report" -AtLeast

Set-GitHooks
Install-VSCodeExtensions

if ($Check) {
  if ($script:missing) { Write-Host "$($script:missing) missing: run tools\setup-dev.ps1 (VS Code: the 'setup environment' task)."; exit 1 }
  Write-Host 'Everything is in place.'
}
elseif ($script:missing) {
  Write-Host "$($script:missing) still missing (above)."
  exit 1
}
else {
  Write-Host 'Done. Open a new terminal (or restart VS Code) for PATH changes to reach it; then, from a Developer PowerShell'
  Write-Host 'for VS: cmake --preset release && cmake --build --preset release && ctest --preset release'
}
