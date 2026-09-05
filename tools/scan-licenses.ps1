<#
.SYNOPSIS
    Fail the build if a dependency's licence is not on the allowlist.

.DESCRIPTION
    Architecture section 14, in its own words:

        No GPL, AGPL, or LGPLv3 dependency enters the tree. Add a
        license-scanning step to the build that reads the vcpkg manifest and
        fails on anything outside an allowlist of MIT, BSD-2, BSD-3, Apache-2.0,
        BSL-1.0, ISC, and Zlib. This takes an hour to write and prevents the one
        mistake that would be genuinely painful to undo six months in.

    The mistake it prevents is not "shipping GPL code" -- that would be noticed.
    It is a transitive dependency arriving under a copyleft licence, going
    unnoticed for six months, and then having to be removed from a codebase that
    has grown around it. The check is cheap and the failure is loud.

    WHAT IT READS. vcpkg writes an SPDX document per installed package, and the
    licence in it is the one the port declares. That is the authority here rather
    than the manifest in the repository, because the manifest lists only direct
    dependencies and the tree contains their dependencies too -- wil arrived that
    way, and a scanner that only read vcpkg.json would never have seen it.

    A package whose SPDX says NOASSERTION is not waved through. It has to appear
    in the exceptions below with a licence a person actually looked up, and the
    comment says where they looked.

    NOT EVERYTHING COMES FROM VCPKG, and for a while this scanner behaved as
    though it did. PDFium is fetched, and was already checked. The Windows App
    SDK is fetched too -- five NuGet packages, three of them under the Microsoft
    Software License Terms rather than anything on section 14's allowlist -- and
    was not checked at all. A required gate with a blind spot over the largest
    binary dependency in the tree is worse than no gate, because it is believed.

    The SDK is read from third_party/winappsdk-packages.txt, the same pin
    manifest tools/fetch-winappsdk.ps1 and src/app/winappsdk.cmake read, so a
    package added to the build is a package this scanner sees.

.PARAMETER Path
    The vcpkg_installed directory to scan. Defaults to the debug preset's.

.PARAMETER Notices
    Also write a ThirdPartyNotices file to this path. Section 14's table lists a
    notice obligation for every component, and the material for it is already in
    the copyright files this scanner reads.

.EXAMPLE
    .\tools\scan-licenses.ps1
    .\tools\scan-licenses.ps1 -Notices .\out\ThirdPartyNotices.txt
#>
[CmdletBinding()]
param(
    [string]$Path,
    [string]$Notices
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# --- The allowlist, from section 14 -------------------------------------------
# Permissive, notice-only licences. Anything not here fails, including licences
# that are perfectly fine in other projects: the list is a decision, not a
# judgement about the licence.
$allowed = @(
    'MIT',
    'BSD-2-Clause',
    'BSD-3-Clause',
    'Apache-2.0',
    'BSL-1.0',
    'ISC',
    'Zlib',
    # Not in section 14's list, and allowed for one reason: it is a public domain
    # dedication with no obligations at all, and it only ever appears here as one
    # branch of an OR with Apache-2.0 (blake3). Refusing it would mean refusing a
    # package that is also available under a licence the list does allow.
    'CC0-1.0'
)

# Spelled out so a near miss is a failure rather than a surprise: LGPL-2.1 is
# acceptable for FFmpeg specifically because section 7.2 consumes it as a
# SEPARATE PROCESS and never links it, which is why FFmpeg is not a vcpkg
# dependency and never will be.
$denied = @('GPL', 'AGPL', 'LGPL-3.0', 'LGPL-2.1', 'SSPL', 'CC-BY-SA')

# Packages whose SPDX metadata says NOASSERTION, with the licence a person
# established by reading the package's own copyright file. Anything not listed
# here and not declared fails the scan.
$exceptions = @{
    # share/lexbor/copyright is the Apache License 2.0 in full. Section 14's
    # table lists it as Apache-2.0 with a notice and NOTICE-file obligation.
    'lexbor' = 'Apache-2.0'
}

# Licences that are NOT on the allowlist and are accepted anyway, each with the
# reason written out. The allowlist exists to keep copyleft out of code that is
# linked into Morph; a proprietary first-party redistributable is a different
# question, and answering it by quietly widening $allowed would lose the
# distinction for the next person.
#
# Keyed by the exact licence string in the pin manifest.
$platformLicences = @{
    'Microsoft Software License Terms' = @'
Microsoft first-party redistributable. The Windows App SDK's terms grant
distribution of the runtime as part of an application (the "Distributable Code"
section), which is what section 13 does with it. It is proprietary, so it is not
on section 14's allowlist and must not be added to it -- the allowlist is about
copyleft obligations propagating into Morph, and these terms impose none of that
kind. What they DO impose is a notice obligation, which is why the licence text
is required to be present and is copied into ThirdPartyNotices.
'@
}

# --- Where to look ------------------------------------------------------------
if (-not $Path) {
    $candidates = @(
        (Join-Path $repo 'out\build\x64-debug\vcpkg_installed'),
        (Join-Path $repo 'out\build\x64-release\vcpkg_installed'),
        (Join-Path $repo 'vcpkg_installed')
    )
    $Path = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Path -or -not (Test-Path $Path)) {
    throw "No vcpkg_installed directory found. Configure a preset first, or pass -Path."
}

$shareDirs = Get-ChildItem -Path $Path -Directory -Filter 'share' -Recurse -Depth 2 -ErrorAction SilentlyContinue
if (-not $shareDirs) { throw "No package metadata under $Path." }

# --- Read what each package says it is ----------------------------------------
function Get-DeclaredLicence {
    param([string]$PackageDir)

    $spdx = Join-Path $PackageDir 'vcpkg.spdx.json'
    if (Test-Path $spdx) {
        $document = Get-Content $spdx -Raw | ConvertFrom-Json
        $package = $document.packages | Select-Object -First 1
        foreach ($field in @($package.licenseConcluded, $package.licenseDeclared)) {
            if ($field -and $field -ne 'NOASSERTION') { return $field }
        }
    }
    return $null
}

# An SPDX expression is a tree of AND and OR. An OR is satisfied by any allowed
# branch -- a package offered under "CC0-1.0 OR Apache-2.0" can be taken under
# either -- while an AND requires all of them. This is a deliberately shallow
# reading: it splits on both operators and requires that at least one term is
# allowed and no term is denied, which is strict enough to catch the mistake the
# check exists for and simple enough to be obviously correct.
function Test-Licence {
    param([string]$Expression)

    $terms = $Expression -split '\s+(?:OR|AND)\s+' |
        ForEach-Object { $_.Trim('(', ')', ' ') } |
        Where-Object { $_ }

    $result = [PSCustomObject]@{ Allowed = $false; Denied = $null; Terms = $terms }
    foreach ($term in $terms) {
        $bare = $term.TrimEnd('+')
        foreach ($bad in $denied) {
            if ($bare -like "*$bad*") {
                $result.Denied = $term
                return $result
            }
        }
        if ($allowed -contains $bare) { $result.Allowed = $true }
    }
    return $result
}

$rows = @()
foreach ($share in $shareDirs) {
    foreach ($packageDir in Get-ChildItem -Path $share.FullName -Directory) {
        $name = $packageDir.Name

        # A directory with neither an SPDX document nor a copyright file is not a
        # package: it is a CMake config shim that another port installed, and
        # unofficial-webview2 is one. Treating it as an unidentified dependency
        # would mean an exceptions list that grows with vcpkg's packaging habits
        # rather than with the tree's actual dependencies.
        $hasSpdx = Test-Path (Join-Path $packageDir.FullName 'vcpkg.spdx.json')
        $hasCopyright = Test-Path (Join-Path $packageDir.FullName 'copyright')
        if (-not $hasSpdx -and -not $hasCopyright) { continue }

        $declared = Get-DeclaredLicence -PackageDir $packageDir.FullName
        $source = 'declared'

        if (-not $declared -and $exceptions.ContainsKey($name)) {
            $declared = $exceptions[$name]
            $source = 'exception'
        }

        $rows += [PSCustomObject]@{
            Package   = $name
            Licence   = if ($declared) { $declared } else { 'UNKNOWN' }
            Source    = $source
            Copyright = Join-Path $packageDir.FullName 'copyright'
        }
    }
}
$rows = $rows | Sort-Object Package -Unique

# --- Verdict ------------------------------------------------------------------
$failures = @()
Write-Host ''
Write-Host ('{0,-46} {1,-34} {2}' -f 'Package', 'Licence', 'Verdict')
Write-Host ('-' * 100)

foreach ($row in $rows) {
    if ($row.Licence -eq 'UNKNOWN') {
        $verdict = 'UNKNOWN -- add it to the exceptions with a licence someone read'
        $failures += $row
    } else {
        $check = Test-Licence -Expression $row.Licence
        if ($check.Denied) {
            $verdict = "DENIED ($($check.Denied))"
            $failures += $row
        } elseif (-not $check.Allowed) {
            $verdict = 'not on the allowlist'
            $failures += $row
        } else {
            $verdict = if ($row.Source -eq 'exception') { 'ok (by exception)' } else { 'ok' }
        }
    }
    Write-Host ('{0,-46} {1,-34} {2}' -f $row.Package, $row.Licence, $verdict)
}

# PDFium is fetched rather than installed by vcpkg, so it has no SPDX document.
# Section 14 lists it as BSD-3 with a notice and no-endorsement obligation, and
# what this checks is that the licence text actually arrived with the binaries.
$pdfiumLicence = Join-Path $repo 'third_party\pdfium\LICENSE'
if (Test-Path (Join-Path $repo 'third_party\pdfium')) {
    if (Test-Path $pdfiumLicence) {
        Write-Host ('{0,-46} {1,-34} {2}' -f 'pdfium', 'BSD-3-Clause', 'ok (fetched)')
    } else {
        Write-Host ('{0,-46} {1,-34} {2}' -f 'pdfium', 'BSD-3-Clause', 'MISSING LICENCE TEXT')
        $failures += [PSCustomObject]@{ Package = 'pdfium'; Licence = 'BSD-3-Clause' }
    }
}

# The Windows App SDK is fetched from NuGet rather than installed by vcpkg, so
# it has no SPDX document either. The pins are read from the same manifest the
# fetcher and the build use, so this cannot fall behind what is actually linked.
$sdkRows = @()
$sdkManifest = Join-Path $repo 'third_party\winappsdk-packages.txt'
if (Test-Path $sdkManifest) {
    $sdkRoots = @(
        (Join-Path $repo 'third_party\winappsdk'),
        (Join-Path $env:USERPROFILE '.nuget\packages')
    )

    foreach ($line in Get-Content $sdkManifest) {
        $text = $line.Trim()
        if (-not $text -or $text.StartsWith('#')) { continue }
        $fields = $text -split '\|'
        if ($fields.Count -ne 5) { throw "Malformed line in ${sdkManifest}:`n  $text" }

        $id = $fields[0].Trim()
        $version = $fields[1].Trim()
        $licence = $fields[3].Trim()
        $licenceFile = $fields[4].Trim()

        $lower = $id.ToLowerInvariant()
        $found = $sdkRoots |
            ForEach-Object { Join-Path $_ "$lower\$version" } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1

        $sdkRows += [PSCustomObject]@{
            Package   = $id
            Version   = $version
            Licence   = $licence
            Copyright = if ($found) { Join-Path $found $licenceFile } else { $null }
            Present   = [bool]$found
        }
    }
}

foreach ($row in $sdkRows) {
    $label = "$($row.Package) $($row.Version)"

    # A pinned package that is not fetched is not a pass. It is a scan that did
    # not happen, and reporting it as ok is exactly the blind spot this section
    # was added to close.
    if (-not $row.Present) {
        Write-Host ('{0,-46} {1,-34} {2}' -f $label, $row.Licence, 'NOT FETCHED -- run tools\fetch-winappsdk.ps1')
        $failures += $row
        continue
    }
    if (-not (Test-Path $row.Copyright)) {
        Write-Host ('{0,-46} {1,-34} {2}' -f $label, $row.Licence, 'MISSING LICENCE TEXT')
        $failures += $row
        continue
    }

    $check = Test-Licence -Expression $row.Licence
    if ($check.Denied) {
        Write-Host ('{0,-46} {1,-34} {2}' -f $label, $row.Licence, "DENIED ($($check.Denied))")
        $failures += $row
    } elseif ($check.Allowed) {
        Write-Host ('{0,-46} {1,-34} {2}' -f $label, $row.Licence, 'ok (fetched)')
    } elseif ($platformLicences.ContainsKey($row.Licence)) {
        Write-Host ('{0,-46} {1,-34} {2}' -f $label, $row.Licence, 'ok (platform component)')
    } else {
        Write-Host ('{0,-46} {1,-34} {2}' -f $label, $row.Licence, 'not on the allowlist and not a declared platform component')
        $failures += $row
    }
}

Write-Host ''

# The reasons, once each rather than once per package: three of the five carry
# the same terms, and a wall of identical paragraphs is a thing people skip.
$statedReasons = $sdkRows |
    Where-Object { $platformLicences.ContainsKey($_.Licence) } |
    Select-Object -ExpandProperty Licence -Unique
foreach ($reason in $statedReasons) {
    Write-Host "$reason -- why this is accepted:" -ForegroundColor DarkGray
    foreach ($sentence in ($platformLicences[$reason] -split "`n")) {
        Write-Host "  $sentence" -ForegroundColor DarkGray
    }
    Write-Host ''
}

# --- Notices ------------------------------------------------------------------
if ($Notices) {
    $lines = @(
        'Third-party notices for Morph',
        '',
        'Morph is MIT licensed. It includes the following components, each under its',
        'own licence. The full text of each licence follows its component.',
        ''
    )
    foreach ($row in $rows) {
        $lines += ('=' * 78)
        $lines += "$($row.Package) -- $($row.Licence)"
        $lines += ('=' * 78)
        $lines += ''
        if (Test-Path $row.Copyright) {
            $lines += (Get-Content $row.Copyright)
        } else {
            $lines += '(no copyright file was installed with this package)'
        }
        $lines += ''
    }
    $written = $rows.Count
    if (Test-Path $pdfiumLicence) {
        $lines += ('=' * 78)
        $lines += 'pdfium -- BSD-3-Clause'
        $lines += ('=' * 78)
        $lines += ''
        $lines += (Get-Content $pdfiumLicence)
        $lines += ''
        $written++
    }

    # The SDK's terms carry a notice obligation of their own, and a notices file
    # that omits the largest redistributable in the package is not a notices
    # file.
    foreach ($row in $sdkRows) {
        if (-not $row.Present -or -not (Test-Path $row.Copyright)) { continue }
        $lines += ('=' * 78)
        $lines += "$($row.Package) $($row.Version) -- $($row.Licence)"
        $lines += ('=' * 78)
        $lines += ''
        $lines += (Get-Content $row.Copyright)
        $lines += ''
        $written++
    }

    $directory = Split-Path -Parent $Notices
    if ($directory -and -not (Test-Path $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    Set-Content -Path $Notices -Value $lines -Encoding UTF8
    Write-Host "Wrote $written notices to $Notices" -ForegroundColor DarkGray
}

if ($failures.Count -gt 0) {
    Write-Host ''
    throw "$($failures.Count) dependency licence(s) are not acceptable. Section 14 keeps GPL, AGPL and LGPLv3 out of this tree; a package that cannot be identified is treated the same way until someone identifies it."
}

$platformCount = ($sdkRows | Where-Object { $platformLicences.ContainsKey($_.Licence) }).Count
$checked = $rows.Count + $sdkRows.Count
if (Test-Path $pdfiumLicence) { $checked++ }
if ($platformCount -gt 0) {
    Write-Host "All $checked dependency licences are acceptable: $($checked - $platformCount) on section 14's allowlist, $platformCount platform components with the reason stated above." -ForegroundColor Green
} else {
    Write-Host "All $checked dependency licences are on section 14's allowlist." -ForegroundColor Green
}
