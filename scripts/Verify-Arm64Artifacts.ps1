<#
.SYNOPSIS
    Verifies that the built executables are genuinely Windows ARM64, and records their size
    and SHA-256.

.DESCRIPTION
    The PE COFF header is read directly from the file rather than trusting the build
    configuration, because the whole point of this repository is to produce ARM64 binaries:
    silently shipping an x64 build as the ARM64 native tool is the failure this guards against.

        offset 0x00  "MZ"                     DOS signature
        offset 0x3C  e_lfanew                 offset of the PE header
        PE + 0x00    "PE\0\0"                 PE signature
        PE + 0x04    Machine       (UInt16)   0xAA64 = ARM64, 0x8664 = x64
        PE + 0x18    OptionalMagic (UInt16)   0x020B = PE32+

    Exits 0 only when every requested file is PE32+ with Machine 0xAA64.

.PARAMETER Path
    Directory containing the executables. Defaults to ./artifacts.

.PARAMETER ReportPath
    Optional path for a JSON report of the results.

.EXAMPLE
    pwsh -File scripts/Verify-Arm64Artifacts.ps1 -Path artifacts -ReportPath artifacts/BUILD_METADATA.json
#>

[CmdletBinding()]
param(
    [string]$Path = 'artifacts',
    [string]$ReportPath,
    [string[]]$Expected = @('whisper-cli.exe', 'clap-vad.exe', 'jarvis-windows-helper.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$EXPECTED_MACHINE = 0xAA64   # IMAGE_FILE_MACHINE_ARM64
$EXPECTED_MAGIC = 0x020B     # PE32+

function Get-PeInfo([string]$FilePath) {
    $bytes = [System.IO.File]::ReadAllBytes($FilePath)
    if ($bytes.Length -lt 0x40) { throw 'file is too small to be a PE image' }
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw 'missing MZ signature' }

    $peOffset = [System.BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -le 0 -or ($peOffset + 26) -gt $bytes.Length) { throw 'e_lfanew is out of range' }

    $signature = [System.Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4)
    if ($signature -ne "PE`0`0") { throw 'missing PE signature' }

    return [pscustomobject]@{
        Machine = [System.BitConverter]::ToUInt16($bytes, $peOffset + 4)
        Magic   = [System.BitConverter]::ToUInt16($bytes, $peOffset + 24)
    }
}

$resolvedPath = Resolve-Path -LiteralPath $Path -ErrorAction SilentlyContinue
if (-not $resolvedPath) {
    Write-Error "Directory not found: $Path"
    exit 1
}

$records = @()
$failures = @()

foreach ($name in $Expected) {
    $filePath = Join-Path $resolvedPath $name
    if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
        $failures += "$name is missing"
        continue
    }

    $file = Get-Item -LiteralPath $filePath
    try {
        $pe = Get-PeInfo $file.FullName
    } catch {
        $failures += "${name}: $($_.Exception.Message)"
        continue
    }

    $machineText = ('0x{0:X4}' -f $pe.Machine)
    $magicText = ('0x{0:X4}' -f $pe.Magic)
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $isArm64 = ($pe.Machine -eq $EXPECTED_MACHINE) -and ($pe.Magic -eq $EXPECTED_MAGIC)

    if ($pe.Machine -ne $EXPECTED_MACHINE) {
        $suffix = if ($pe.Machine -eq 0x8664) { ' (this is an x64 build)' } else { '' }
        $failures += ("{0}: Machine is {1}, expected 0x{2:X4}{3}" -f $name, $machineText, $EXPECTED_MACHINE, $suffix)
    }
    if ($pe.Magic -ne $EXPECTED_MAGIC) {
        $failures += "${name}: OptionalHeader magic is $magicText, expected 0x020B (PE32+)"
    }

    $records += [pscustomobject]@{
        name    = $name
        path    = "tools/win32-arm64/$name"
        size    = $file.Length
        sha256  = $hash
        machine = $machineText
        magic   = $magicText
        format  = if ($isArm64) { 'pe32+-aarch64' } else { 'unexpected' }
        arm64   = $isArm64
    }
}

if ($records.Count -gt 0) {
    $records |
        Select-Object name, machine, magic, format, size, sha256 |
        Format-Table -AutoSize |
        Out-String |
        Write-Host
}

if ($ReportPath) {
    $report = [pscustomobject]@{
        verifiedAt      = (Get-Date).ToUniversalTime().ToString('o')
        osArchitecture  = [string][System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
        expectedMachine = ('0x{0:X4}' -f $EXPECTED_MACHINE)
        allArm64        = ($failures.Count -eq 0)
        files           = $records
    }
    $reportDirectory = Split-Path -Parent $ReportPath
    if ($reportDirectory -and -not (Test-Path -LiteralPath $reportDirectory)) {
        New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null
    }
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding utf8
    Write-Host "report written: $ReportPath"
}

if ($failures.Count -gt 0) {
    Write-Host ''
    foreach ($failure in $failures) { Write-Error $failure }
    Write-Error 'Verification failed. An x64 build must never be shipped as the Windows ARM64 native tool.'
    exit 1
}

Write-Host "All $($records.Count) executables are PE32+ Windows ARM64 (Machine 0xAA64)."
exit 0
