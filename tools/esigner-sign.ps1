# Authenticode-sign one file with Jonny's code-signing certificate, held in SSL.com's cloud (eSigner), through
# SSL.com's CodeSignTool - what a product's Windows release runs on what it ships signed, an Inno Setup installer and
# its uninstaller among them (the script's SignTool line runs this one on each file, from wherever the product holds
# the toolkit: external/reckless-toolkit/tools/esigner-sign.ps1). The credentials come from the environment, never the
# command line, each product's release workflow holding its own copies of the secrets: ESIGNER_USERNAME and
# ESIGNER_PASSWORD (the SSL.com account), ESIGNER_TOTP_SECRET (the enrolment's OTP secret, so no one need be at the
# keyboard) and, where the account holds more than one certificate, ESIGNER_CREDENTIAL_ID. RECKLESS_CODESIGNTOOL is the
# folder CodeSignTool was unpacked into. The file is signed in place with SSL.com's RFC 3161 timestamp, then held as
# Windows checks it: a valid signature, in Jonathan Reckless's name.
#
# The tool is its own Java run on its jar, from its folder (where it reads conf/), and not its CodeSignTool.bat: a batch
# file hands its arguments through cmd.exe, which mangles a password holding %, ^, &, ! or a quote before the tool sees
# it.
#
# With -Check it signs nothing: it signs in with the username and password and lists the account's certificates
# (get_credential_ids, which spends no signing), failing where the sign-in is refused, where the account holds no
# certificate, where it holds several and ESIGNER_CREDENTIAL_ID names none, or where it names one the account lacks -
# what a release runs before its build, so a wrong secret is told in a minute and not after it. Where
# RECKLESS_SIGN_LOG names a file, the tool's words go there too, the credentials blanked: Inno Setup runs this script
# under /Qp, which shows none of them.
#
# Usage (PowerShell 7):  pwsh -NoProfile -File tools/esigner-sign.ps1 <file>
#                        pwsh -NoProfile -File tools/esigner-sign.ps1 -Check
param([string] $File, [switch] $Check)
$ErrorActionPreference = 'Stop'

$needed = @('ESIGNER_USERNAME', 'ESIGNER_PASSWORD', 'RECKLESS_CODESIGNTOOL')
if (-not $Check) { $needed += 'ESIGNER_TOTP_SECRET' }
foreach ($name in $needed) {
    if (-not [Environment]::GetEnvironmentVariable($name)) { throw "$name is not set: the eSigner secrets (the script's head says which)" }
}
$tool = $env:RECKLESS_CODESIGNTOOL
$java = Get-ChildItem -Path $tool -Filter java.exe -Recurse -File | Where-Object { $_.Directory.Name -eq 'bin' } |
        Select-Object -First 1
$jar = Get-ChildItem -Path (Join-Path $tool 'jar') -Filter 'code_sign_tool-*.jar' -File | Select-Object -First 1
if (-not $java -or -not $jar) { throw "$tool holds no CodeSignTool: no bin\java.exe or jar\code_sign_tool-*.jar" }

# CodeSignTool with `$Arguments`: what it said, the credentials blanked, shown and kept in RECKLESS_SIGN_LOG where that
# is set, and its exit code.
function Invoke-CodeSignTool([string[]] $Arguments) {
    Push-Location $tool
    try {
        $said = & $java.FullName -jar $jar.FullName @Arguments 2>&1 | Out-String
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    foreach ($secret in $env:ESIGNER_PASSWORD, $env:ESIGNER_TOTP_SECRET) {
        if ($secret) { $said = $said -replace [regex]::Escape($secret), '***' }
    }
    Write-Host $said.Trim()
    if ($env:RECKLESS_SIGN_LOG) { Add-Content -Path $env:RECKLESS_SIGN_LOG -Value $said.Trim() }
    return [pscustomobject]@{ Said = $said; Code = $code }
}

if ($Check) {
    $result = Invoke-CodeSignTool @('get_credential_ids', "-username=$env:ESIGNER_USERNAME", "-password=$env:ESIGNER_PASSWORD")
    $pattern = '[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}'
    $ids = @([regex]::Matches($result.Said, $pattern) | ForEach-Object Value | Select-Object -Unique)
    if ($result.Code -ne 0 -or $result.Said -match '(?i)\berror\b' -or $ids.Count -eq 0) {
        throw "eSigner refused the sign-in or holds no certificate: ESIGNER_USERNAME and ESIGNER_PASSWORD are the SSL.com account's"
    }
    if ($env:ESIGNER_CREDENTIAL_ID) {
        if ($ids -notcontains $env:ESIGNER_CREDENTIAL_ID) {
            throw "ESIGNER_CREDENTIAL_ID is none of the account's $($ids.Count) certificate(s)"
        }
    } elseif ($ids.Count -gt 1) {
        throw "the account holds $($ids.Count) certificates: set ESIGNER_CREDENTIAL_ID to the code-signing one's id"
    }
    Write-Host "eSigner: signed in, $($ids.Count) certificate(s), the one to sign with known"
    exit 0
}

if (-not $File) { throw "name the file to sign, or -Check" }
$path = (Resolve-Path $File).Path
# CodeSignTool chooses how to sign by the file's extension, and Inno Setup hands its sign tool the uninstaller (and
# the setup itself while it builds it) under a temporary name ending .tmp, which the tool refuses ("Unsupported file
# format for signing - tmp"). A file of another extension is signed as a copy named .exe in a folder of its own, held
# as Windows checks it there, and copied back over the original.
$signed = $path
$scratch = $null
if ([IO.Path]::GetExtension($path).ToLowerInvariant() -notin @('.exe', '.dll', '.msi', '.sys', '.cab')) {
    $scratch = Join-Path ([IO.Path]::GetTempPath()) ("reckless-sign-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    $signed = Join-Path $scratch ([IO.Path]::GetFileNameWithoutExtension($path) + '.exe')
    Copy-Item -LiteralPath $path -Destination $signed
}
try {
    $arguments = @('sign', "-username=$env:ESIGNER_USERNAME", "-password=$env:ESIGNER_PASSWORD",
                   "-totp_secret=$env:ESIGNER_TOTP_SECRET", "-input_file_path=$signed", '-override')
    if ($env:ESIGNER_CREDENTIAL_ID) { $arguments += "-credential_id=$env:ESIGNER_CREDENTIAL_ID" }
    $result = Invoke-CodeSignTool $arguments
    # The tool can end with 0 having failed: its words are read too.
    if ($result.Code -ne 0 -or $result.Said -match '(?i)\berror\b') { throw "eSigner did not sign $path" }

    $signature = Get-AuthenticodeSignature -FilePath $signed
    if ($signature.Status -ne 'Valid') { throw "$path is not validly signed: $($signature.Status) $($signature.StatusMessage)" }
    if ($signature.SignerCertificate.Subject -notmatch 'CN=Jonathan Reckless') {
        throw "$path is signed by $($signature.SignerCertificate.Subject), not Jonathan Reckless"
    }
    if (-not $signature.TimeStamperCertificate) { throw "$path carries no timestamp" }
    if ($signed -ne $path) { Copy-Item -LiteralPath $signed -Destination $path -Force }
    Write-Host "signed $(Split-Path -Leaf $path): $($signature.SignerCertificate.Subject), timestamped"
} finally {
    if ($scratch) { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $scratch }
}
