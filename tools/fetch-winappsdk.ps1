<#
.SYNOPSIS
    Fetch the pinned Windows App SDK packages Morph.App.exe is built against.

.DESCRIPTION
    Section 12 asks for WinUI 3, and src/app/winappsdk.cmake explains at length
    why that needs no MSBuild -- only the .winmd metadata, the headers, the
    bootstrapper import library, and cppwinrt.exe. All four arrive in NuGet
    packages, and until this script existed they were read out of whatever the
    local NuGet cache happened to contain. That builds on a machine that has
    opened a WinUI project before and nowhere else, which is why MORPH_BUILD_APP
    defaulted to OFF.

    This is fetch-pdfium.ps1's shape, and for the same reasons: a pinned version
    and a committed SHA-256, so that a build's output does not depend on the day
    it ran and a tampered or truncated download fails loudly rather than
    producing a subtly different binary.

    THE PINS ARE NOT IN THIS FILE. They are in third_party/winappsdk-packages.txt,
    which src/app/winappsdk.cmake and tools/scan-licenses.ps1 also read -- three
    copies of five version numbers is how a build ends up linking one release of
    the SDK against the metadata of another.

    A .nupkg is a zip. It is unpacked into the same layout the NuGet cache uses
    (third_party/winappsdk/<id-lowercase>/<version>/...) so that CMake's search
    is one list of roots rather than two shapes of path.

.PARAMETER Force
    Re-download even when a package is already unpacked and stamped.

.EXAMPLE
    .\tools\fetch-winappsdk.ps1
#>
[CmdletBinding()]
param([switch]$Force)

$ErrorActionPreference = 'Stop'

$repo     = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $repo 'third_party\winappsdk-packages.txt'
$root     = Join-Path $repo 'third_party\winappsdk'

if (-not (Test-Path $manifest)) { throw "The package pin manifest is missing: $manifest" }

# --- The pins -----------------------------------------------------------------
$packages = @()
foreach ($line in Get-Content $manifest) {
    $text = $line.Trim()
    if (-not $text -or $text.StartsWith('#')) { continue }
    $fields = $text -split '\|'
    if ($fields.Count -ne 5) { throw "Malformed line in ${manifest}:`n  $text" }
    $packages += [PSCustomObject]@{
        Id          = $fields[0].Trim()
        Version     = $fields[1].Trim()
        Sha256      = $fields[2].Trim().ToUpperInvariant()
        Licence     = $fields[3].Trim()
        LicenceFile = $fields[4].Trim()
    }
}
if ($packages.Count -eq 0) { throw "No packages listed in $manifest." }

New-Item -ItemType Directory -Path $root -Force | Out-Null

$fetched = 0
foreach ($package in $packages) {
    $lower  = $package.Id.ToLowerInvariant()
    $target = Join-Path $root "$lower\$($package.Version)"
    $stamp  = Join-Path $target '.morph-sha256'

    # --- Already present? -----------------------------------------------------
    # Stamped with the hash rather than the version, because the version is
    # already in the path: what a stamp has to answer is "is what is unpacked
    # here the thing the manifest currently pins", and after a re-pin of the same
    # version -- which happens when upstream re-tags -- the version cannot.
    if ((-not $Force) -and (Test-Path $stamp)) {
        if ((Get-Content $stamp -Raw).Trim() -eq $package.Sha256) {
            Write-Host "$($package.Id) $($package.Version) already present" -ForegroundColor DarkGray
            continue
        }
        Write-Host "$($package.Id) $($package.Version) is pinned to a different hash; refetching" -ForegroundColor Yellow
    }

    # --- Download -------------------------------------------------------------
    # The v3 flat container. Both the id and the version are lowercased in the
    # path; the service is case-sensitive about it and answers 404 otherwise.
    $url = "https://api.nuget.org/v3-flatcontainer/$lower/$($package.Version.ToLowerInvariant())/$lower.$($package.Version.ToLowerInvariant()).nupkg"

    $scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("morph-winappsdk-" + [System.Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch -Force | Out-Null
    $archive = Join-Path $scratch "$lower.nupkg"

    try {
        Write-Host "Downloading $($package.Id) $($package.Version)" -ForegroundColor Cyan
        Write-Host "  $url" -ForegroundColor DarkGray

        # Invoke-WebRequest's progress bar makes this roughly ten times slower,
        # and the WinUI package is 56 MB.
        $previousProgress = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue'
        try {
            Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing -MaximumRedirection 5
        } finally {
            $ProgressPreference = $previousProgress
        }

        # --- Verify before unpacking ------------------------------------------
        $actual = (Get-FileHash -Path $archive -Algorithm SHA256).Hash
        if ($actual -ne $package.Sha256) {
            throw @"
$($package.Id) $($package.Version) checksum mismatch. Nothing was unpacked.
  expected $($package.Sha256)
  actual   $actual
Either the pinned package was replaced upstream, or the download was tampered
with. Do not update the checksum in third_party\winappsdk-packages.txt without
establishing which.
"@
        }
        Write-Host "  SHA-256 verified" -ForegroundColor DarkGray

        # --- Unpack ------------------------------------------------------------
        $extracted = Join-Path $scratch 'unpacked'
        # A .nupkg is an ordinary zip with a .nupkg extension, and
        # Expand-Archive goes by extension rather than by content.
        $asZip = Join-Path $scratch "$lower.zip"
        Move-Item -Path $archive -Destination $asZip
        Expand-Archive -Path $asZip -DestinationPath $extracted -Force

        # Section 14: the licence text has to ARRIVE, not merely be named in a
        # table. Checked here rather than in the scanner alone, so that a package
        # whose layout changed upstream fails at the fetch with the reason
        # attached instead of during a release.
        $licence = Join-Path $extracted $package.LicenceFile
        if (-not (Test-Path $licence)) {
            throw "$($package.Id) $($package.Version) does not contain $($package.LicenceFile); the package layout may have changed."
        }

        if (Test-Path $target) { Remove-Item -Path $target -Recurse -Force }
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Move-Item -Path $extracted -Destination $target

        Set-Content -Path (Join-Path $target '.morph-sha256') -Value $package.Sha256 -NoNewline
        Write-Host "  unpacked to third_party\winappsdk\$lower\$($package.Version) ($($package.Licence))" -ForegroundColor DarkGray
        $fetched++
    }
    finally {
        if (Test-Path $scratch) { Remove-Item -Path $scratch -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

if ($fetched -eq 0) {
    Write-Host "The Windows App SDK is already pinned and present in third_party\winappsdk" -ForegroundColor Green
} else {
    Write-Host "Fetched $fetched of $($packages.Count) Windows App SDK packages into third_party\winappsdk" -ForegroundColor Green
}
Write-Host "  Morph.App.exe builds by default now; configure with -DMORPH_BUILD_APP=OFF to skip it." -ForegroundColor DarkGray
