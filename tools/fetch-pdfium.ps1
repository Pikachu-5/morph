<#
.SYNOPSIS
    Fetch the prebuilt PDFium binaries this project links against.

.DESCRIPTION
    PDFium has no vcpkg port and building it from source pulls in the entire
    Chromium build system, so the architecture (section 13.2) ships it as a
    prebuilt DLL. This script fetches one specific, pinned release and verifies
    its SHA-256 before unpacking anything.

    THE PIN AND THE CHECKSUM ARE THE POINT. A build that downloads "the latest"
    of anything is a build whose output depends on the day it ran, and a download
    without a checksum trusts the network and the host equally. Both values below
    are committed, so a tampered or truncated download fails loudly instead of
    producing a subtly different binary.

    The non-V8 build is deliberate. The V8 variant embeds a JavaScript engine and
    would execute JavaScript embedded in a PDF; section 15's threat model is that
    every input file is hostile, and this removes that entire class of attack for
    a capability Morph does not use.

.PARAMETER Force
    Re-download even when the target directory already looks complete.

.EXAMPLE
    .\tools\fetch-pdfium.ps1
#>
[CmdletBinding()]
param([switch]$Force)

$ErrorActionPreference = 'Stop'

# --- The pin -----------------------------------------------------------------
# Update all four together, never one alone.
$Release  = 'chromium/8035'
$Version  = '154.0.8035.0'
$Asset    = 'pdfium-win-x64.tgz'
$Sha256   = '61513D611AD200A383456140739BE77D156F1E3A2EEF22BD89F6C3BDA79BDD41'

$repo    = Split-Path -Parent $PSScriptRoot
$target  = Join-Path $repo 'third_party\pdfium'
$stamp   = Join-Path $target 'VERSION'

# --- Already present? --------------------------------------------------------
if ((-not $Force) -and (Test-Path $stamp)) {
    $installed = (Get-Content $stamp -Raw).Trim()
    if ($installed -eq $Version -and (Test-Path (Join-Path $target 'bin\pdfium.dll'))) {
        Write-Host "PDFium $Version already present in third_party\pdfium" -ForegroundColor DarkGray
        exit 0
    }
    Write-Host "Replacing PDFium $installed with $Version" -ForegroundColor DarkGray
}

# --- Download ----------------------------------------------------------------
# The release tag contains a slash, which has to survive as %2F in the URL.
$encoded = $Release.Replace('/', '%2F')
$url = "https://github.com/bblanchon/pdfium-binaries/releases/download/$encoded/$Asset"

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("morph-pdfium-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$archive = Join-Path $scratch $Asset

try {
    Write-Host "Downloading PDFium $Version" -ForegroundColor Cyan
    Write-Host "  $url" -ForegroundColor DarkGray

    # Invoke-WebRequest's progress bar makes this roughly ten times slower.
    $previousProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing -MaximumRedirection 5
    } finally {
        $ProgressPreference = $previousProgress
    }

    # --- Verify before unpacking ---------------------------------------------
    $actual = (Get-FileHash -Path $archive -Algorithm SHA256).Hash
    if ($actual -ne $Sha256) {
        throw @"
PDFium checksum mismatch. Nothing was unpacked.
  expected $Sha256
  actual   $actual
Either the pinned release was re-tagged upstream, or the download was tampered
with. Do not update the checksum without establishing which.
"@
    }
    Write-Host "  SHA-256 verified" -ForegroundColor DarkGray

    # --- Unpack ---------------------------------------------------------------
    # bsdtar ships with Windows 10 1803 and later, so no extra dependency.
    $extracted = Join-Path $scratch 'unpacked'
    New-Item -ItemType Directory -Path $extracted -Force | Out-Null
    tar -xzf $archive -C $extracted
    if ($LASTEXITCODE -ne 0) { throw "Could not unpack $Asset (tar exit code $LASTEXITCODE)." }

    if (-not (Test-Path (Join-Path $extracted 'bin\pdfium.dll'))) {
        throw "The archive did not contain bin\pdfium.dll; the asset layout may have changed."
    }

    if (Test-Path $target) { Remove-Item -Path $target -Recurse -Force }
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Move-Item -Path $extracted -Destination $target

    Set-Content -Path $stamp -Value $Version -NoNewline

    Write-Host "PDFium $Version installed to third_party\pdfium" -ForegroundColor Green
    Write-Host "  LICENSE: $(Join-Path $target 'LICENSE') (BSD-3-Clause, see architecture section 14)" -ForegroundColor DarkGray
}
finally {
    if (Test-Path $scratch) { Remove-Item -Path $scratch -Recurse -Force -ErrorAction SilentlyContinue }
}
