# Zips the package folder with UTF-8 entry names and '/' separators.
# CMake's own archiver loses non-ASCII names on Windows, so the zip goes
# through .NET instead. Called by cmake/make_dist.cmake.
param(
    [Parameter(Mandatory = $true)][string]$DistDir,
    [Parameter(Mandatory = $true)][string]$Zip,
    [string]$PackageDir = ""
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not $PackageDir) {
    # The package folder is the one subfolder carrying BUILD_INFO.txt; its
    # name never travels on a command line this way.
    $candidates = @(Get-ChildItem -LiteralPath $DistDir -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'BUILD_INFO.txt') })
    if ($candidates.Count -ne 1) {
        throw "expected exactly one package folder under $DistDir, found $($candidates.Count)"
    }
    $PackageDir = $candidates[0].FullName
}

if (Test-Path -LiteralPath $Zip) {
    Remove-Item -LiteralPath $Zip -Force
}

[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $PackageDir, $Zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)

# Read it back: every name must use '/' and keep its non-ASCII characters.
$archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
try {
    $count = $archive.Entries.Count
    $bad = @($archive.Entries | Where-Object { $_.FullName -match '\\' -or $_.FullName -match '\?' })
    $nonAscii = @($archive.Entries | Where-Object { $_.FullName -match '[^\x00-\x7F]' }).Count
} finally {
    $archive.Dispose()
}
if ($bad.Count -gt 0) {
    throw "zip has $($bad.Count) entries with backslashes or '?' in their names"
}
Write-Output "zip: $count entries, $nonAscii with non-ASCII names -> $Zip"
