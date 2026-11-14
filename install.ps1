[CmdletBinding()]
param(
    [string]$Version,
    [string]$Asset,
    [string]$Repo = "kevin-steve772/TExplorer",
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Programs\TExplorer"),
    [switch]$NoPath
)

$ErrorActionPreference = "Stop"

if ($Repo -notmatch '^[^/]+/[^/]+$') {
    throw "Repository must be in owner/repo format: $Repo"
}
if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    throw "InstallDir cannot be empty."
}

$headers = @{
    Accept = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
}
$token = if ($env:GITHUB_TOKEN) { $env:GITHUB_TOKEN } else { $env:GH_TOKEN }
if ($token) {
    $headers.Authorization = "Bearer $token"
}

$releaseUrl = if ($Version) {
    "https://api.github.com/repos/$Repo/releases/tags/$([uri]::EscapeDataString($Version))"
} else {
    "https://api.github.com/repos/$Repo/releases/latest"
}

try {
    $release = Invoke-RestMethod -Uri $releaseUrl -Headers $headers
} catch {
    throw "Unable to retrieve the GitHub release. Check the repository, version, network, and API rate limit. $($_.Exception.Message)"
}

$releaseAssets = @($release.assets | Where-Object { $_.name -and $_.browser_download_url })
if ($Asset) {
    $selected = $releaseAssets | Where-Object { $_.name -ceq $Asset } | Select-Object -First 1
    if (-not $selected) {
        throw "Release asset not found: $Asset"
    }
} else {
    $arch = if ($env:PROCESSOR_ARCHITEW6432) {
        $env:PROCESSOR_ARCHITEW6432
    } else {
        $env:PROCESSOR_ARCHITECTURE
    }
    $archPattern = switch -Regex ($arch) {
        '^(AMD64|x86_64)$' { 'x86_64|amd64|x64'; break }
        '^(ARM64)$' { 'arm64|aarch64'; break }
        '^(x86|i386)$' { 'i386|i686|win32|(?<![a-z0-9])x86(?![_-]?64)'; break }
        default { $null }
    }

    $candidates = @($releaseAssets | Where-Object {
        $_.name -match '\.exe$' -and
        $_.name -notmatch 'source|setup|installer'
    })
    if ($archPattern) {
        $candidates = @($candidates | Where-Object {
            $name = $_.name.ToLowerInvariant()
            $hasKnownArch = $name -match 'x86_64|amd64|x64|arm64|aarch64|i386|i686|win32'
            -not $hasKnownArch -or $name -match $archPattern
        })
    }
    $selected = $candidates |
        Sort-Object @{
            Expression = { if ($_.name -match '(?i)texplorer') { 1 } else { 0 } }
            Descending = $true
        } |
        Select-Object -First 1
    if (-not $selected) {
        throw "No matching Windows executable was found in the release. Specify an asset with -Asset."
    }
}

if ($selected.name -notmatch '\.exe$') {
    throw "Selected release asset is not an .exe file: $($selected.name)"
}

$tempFile = Join-Path ([System.IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString() + ".exe")
try {
    Write-Host "Downloading $($selected.name)..."
    Invoke-WebRequest -Uri $selected.browser_download_url -Headers $headers -OutFile $tempFile

    $stream = [System.IO.File]::OpenRead($tempFile)
    try {
        if ($stream.Length -lt 2 -or $stream.ReadByte() -ne 0x4D -or $stream.ReadByte() -ne 0x5A) {
            throw "Downloaded file is not a Windows executable."
        }
    } finally {
        $stream.Dispose()
    }

    New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    $target = Join-Path $InstallDir "TExplorer.exe"
    Move-Item -LiteralPath $tempFile -Destination $target -Force

    if (-not $NoPath) {
        $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
        $entries = @($userPath -split ";" | Where-Object { $_ })
        if (-not ($entries | Where-Object { $_.TrimEnd("\") -ieq $InstallDir.TrimEnd("\") })) {
            $newPath = (@($entries) + $InstallDir) -join ";"
            [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        }
    }

    Write-Host "Installed to $target"
    if (-not $NoPath) {
        Write-Host "Added the install directory to your user PATH. Open a new terminal to use 'TExplorer'."
    }
} finally {
    if (Test-Path -LiteralPath $tempFile) {
        Remove-Item -LiteralPath $tempFile -Force
    }
}
