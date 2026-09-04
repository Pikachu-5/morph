<#
.SYNOPSIS
    Configure, build and test Morph from an ordinary PowerShell prompt.

.DESCRIPTION
    The presets use the Ninja generator with MSVC, which needs the Visual Studio
    developer environment on PATH. Without it CMake silently selects whatever
    compiler it finds first -- on a stock machine with Strawberry Perl installed
    that is MinGW g++, which cannot build a Windows App SDK project at all.

    This script locates Visual Studio with vswhere, imports the x64 developer
    environment into the current session, then drives the presets.

.PARAMETER Preset
    A configure preset from CMakePresets.json. Default: x64-debug.

.PARAMETER Target
    configure | build | test | all. Default: all.

.EXAMPLE
    .\tools\build.ps1
    .\tools\build.ps1 -Preset x64-release -Target test
#>
[CmdletBinding()]
param(
    [ValidateSet('x64-debug', 'x64-release', 'x64-asan')]
    [string]$Preset = 'x64-debug',

    [ValidateSet('configure', 'build', 'test', 'all')]
    [string]$Target = 'all',

    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# --- vcpkg -------------------------------------------------------------------
if (-not $env:VCPKG_ROOT) {
    $candidates = @(
        @('C:\Codes\vcpkg', "$env:USERPROFILE\vcpkg", $env:VCPKG_INSTALLATION_ROOT) |
            Where-Object { $_ -and (Test-Path (Join-Path $_ 'scripts\buildsystems\vcpkg.cmake')) })
    if ($candidates.Count -eq 0) {
        throw 'VCPKG_ROOT is not set and no vcpkg installation was found. Clone https://github.com/microsoft/vcpkg and run bootstrap-vcpkg.bat.'
    }
    $env:VCPKG_ROOT = $candidates[0]
    Write-Host "VCPKG_ROOT -> $env:VCPKG_ROOT" -ForegroundColor DarkGray
}

# --- MSVC developer environment ---------------------------------------------
if (-not $env:VSCMD_VER) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Install Visual Studio 2022 with the Desktop development with C++ workload." }

    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsPath) { throw 'No Visual Studio installation with the MSVC v143 toolset was found.' }

    # Run VsDevCmd.bat in a child cmd and copy the resulting environment back.
    # Launch-VsDevShell.ps1 would be the tidier route, but it shells out to
    # vswhere by bare name and fails on a PATH that lacks the Installer folder.
    $vsDevCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $vsDevCmd)) { throw "VsDevCmd.bat not found under $vsPath." }

    Write-Host "Importing MSVC x64 environment from $vsPath" -ForegroundColor DarkGray
    # VsDevCmd.bat invokes vswhere by bare name in a couple of its sub-scripts.
    $env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"
    $captured = & "$env:ComSpec" /s /c "`"$vsDevCmd`" -arch=amd64 -host_arch=amd64 -no_logo && set"
    if ($LASTEXITCODE -ne 0) { throw "VsDevCmd.bat failed with exit code $LASTEXITCODE." }

    foreach ($line in $captured) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2]
        }
    }
    if (-not $env:VSCMD_VER) { throw 'The developer environment did not import correctly.' }
}

Set-Location $repo

function Invoke-Step {
    param([string]$Description, [scriptblock]$Action)
    Write-Host "`n== $Description" -ForegroundColor Cyan
    & $Action
    if ($LASTEXITCODE -ne 0) { throw "$Description failed with exit code $LASTEXITCODE." }
}

if ($Target -in @('configure', 'all')) {
    # Not $args: that is an automatic variable and splatting it silently drops
    # every element.
    $configureArgs = @('--preset', $Preset)
    if ($Fresh) { $configureArgs += '--fresh' }
    Invoke-Step "Configure ($Preset)" { cmake @configureArgs }
}
if ($Target -in @('build', 'all')) {
    Invoke-Step "Build ($Preset)" { cmake --build --preset $Preset }
}
if ($Target -in @('test', 'all')) {
    # The AddressSanitizer runtime is a DLL that ships with MSVC and is not on
    # PATH, so an instrumented binary run without this dies at load with
    # STATUS_DLL_NOT_FOUND (0xC0000135) and no message at all -- which looks
    # exactly like a crash in the code being tested.
    if ($Preset -eq 'x64-asan') {
        $asanDir = if ($env:VCToolsInstallDir) { Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64' } else { $null }
        if ($asanDir -and (Test-Path (Join-Path $asanDir 'clang_rt.asan_dynamic-x86_64.dll'))) {
            $env:PATH = "$asanDir;$env:PATH"
            Write-Host "ASan runtime -> $asanDir" -ForegroundColor DarkGray
        } else {
            Write-Warning 'The AddressSanitizer runtime was not found; the tests will fail to start.'
        }
    }
    Invoke-Step "Test ($Preset)" { ctest --preset $Preset }
}

Write-Host "`nDone." -ForegroundColor Green
