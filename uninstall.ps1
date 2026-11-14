[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Programs\TExplorer"),
    [switch]$PurgeData
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    throw "InstallDir cannot be empty."
}

$target = Join-Path $InstallDir "TExplorer.exe"
if (Test-Path -LiteralPath $target) {
    Remove-Item -LiteralPath $target -Force
    Write-Host "Removed $target"
} else {
    Write-Host "Executable not found: $target"
}

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$entries = @($userPath -split ";" | Where-Object {
    $_ -and $_.TrimEnd("\") -ine $InstallDir.TrimEnd("\")
})
if ($entries.Count -ne @($userPath -split ";" | Where-Object { $_ }).Count) {
    [Environment]::SetEnvironmentVariable("Path", ($entries -join ";"), "User")
    Write-Host "Removed the install directory from your user PATH."
}

if (Test-Path -LiteralPath $InstallDir) {
    $remaining = @(Get-ChildItem -LiteralPath $InstallDir -Force)
    if ($remaining.Count -eq 0) {
        Remove-Item -LiteralPath $InstallDir -Force
    } else {
        Write-Host "Kept non-empty install directory: $InstallDir"
    }
}

if ($PurgeData) {
    $trash = Join-Path $env:LOCALAPPDATA "TExplorer\Trash"
    if (Test-Path -LiteralPath $trash) {
        Remove-Item -LiteralPath $trash -Recurse -Force
        Write-Host "Removed TExplorer trash data: $trash"
    }
}

Write-Host "Uninstall complete."
