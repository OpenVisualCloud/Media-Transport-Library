#!/usr/bin/env pwsh

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

<#
.SYNOPSIS
Build and install MTL-patched DPDK in a native MSVC environment on Windows.

.DESCRIPTION
This script is standalone and does not depend on script/build_dpdk.sh or build.sh.
Run it from an MSVC Developer PowerShell or Developer Command Prompt.

It creates script-owned source/build/install directories, applies MTL generic and
Windows DPDK patches in order, then performs Meson configure/compile/install.

.EXAMPLE
./script/build_dpdk_windows.ps1

.EXAMPLE
./script/build_dpdk_windows.ps1 -Force -DpdkVersion 26.03

.EXAMPLE
./script/build_dpdk_windows.ps1 -BuildDirectory "C:\\work\\build\\dpdk" -InstallDirectory "C:\\work\\.local_install\\dpdk"
#>

[CmdletBinding()]
param(
    [switch]$Force,
    [string]$DpdkVersion,
    [string]$BuildDirectory,
    [string]$InstallDirectory,
    [ValidateRange(1, 4096)]
    [int]$MaxLcores = 256,
    [string]$WorkingDirectory,
    [Alias('h')]
    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail {
    param([string]$Message)
    throw "[build_dpdk_windows.ps1] $Message"
}

function Show-Usage {
    @'
Usage:
  .\script\build_dpdk_windows.ps1 [-Force] [-DpdkVersion <ver>] [-BuildDirectory <path>] [-InstallDirectory <path>] [-MaxLcores <n>] [-WorkingDirectory <path>]

Examples:
  .\script\build_dpdk_windows.ps1
  .\script\build_dpdk_windows.ps1 -Force -DpdkVersion 26.03
  .\script\build_dpdk_windows.ps1 -BuildDirectory C:\work\build\dpdk -InstallDirectory C:\work\.local_install\dpdk
'@ | Write-Host
}

function Assert-Command {
    param(
        [string]$Name,
        [string]$Hint
    )

    if (-not (Get-Command -Name $Name -ErrorAction SilentlyContinue)) {
        if ($Hint) {
            Fail "Required tool '$Name' not found. $Hint"
        }
        Fail "Required tool '$Name' not found."
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @()
    )

    Write-Host "> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function Resolve-FullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [switch]$MustExist
    )

    if ($MustExist) {
        return (Resolve-Path -LiteralPath $Path).Path
    }

    if (Test-Path -LiteralPath $Path) {
        return (Resolve-Path -LiteralPath $Path).Path
    }

    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent | Out-Null
    }

    $item = New-Item -ItemType Directory -Path $Path -Force
    return $item.FullName
}

function Get-VersionFromEnvFile {
    param([Parameter(Mandatory = $true)][string]$EnvFile)

    $line = Select-String -Path $EnvFile -Pattern '^DPDK_VER=(.+)$' | Select-Object -First 1
    if (-not $line) {
        Fail "Cannot find DPDK_VER in $EnvFile"
    }

    return $line.Matches[0].Groups[1].Value.Trim()
}

function Convert-PatchLinksToFiles {
    param([Parameter(Mandatory = $true)][string[]]$PatchDirs)

    for ($pass = 1; $pass -le 3; $pass++) {
        foreach ($patchDir in $PatchDirs) {
            if (-not (Test-Path -LiteralPath $patchDir)) {
                continue
            }

            Get-ChildItem -Path $patchDir -Filter '*.patch' -File | ForEach-Object {
                $firstLine = Get-Content -LiteralPath $_.FullName -TotalCount 1 -ErrorAction SilentlyContinue
                if ($firstLine -and $firstLine -match '^\.\./.+\.patch$') {
                    $targetPath = Join-Path -Path $_.DirectoryName -ChildPath $firstLine
                    $resolvedTarget = Resolve-Path -LiteralPath $targetPath -ErrorAction SilentlyContinue
                    if (-not $resolvedTarget) {
                        Fail "Patch link target not found for $($_.FullName): $firstLine"
                    }

                    $targetContent = Get-Content -LiteralPath $resolvedTarget.Path -Raw
                    Set-Content -LiteralPath $_.FullName -Value $targetContent -NoNewline
                }
            }
        }
    }

    $notConverted = @()
    foreach ($patchDir in $PatchDirs) {
        if (-not (Test-Path -LiteralPath $patchDir)) {
            continue
        }

        Get-ChildItem -Path $patchDir -Filter '*.patch' -File | ForEach-Object {
            $firstLine = Get-Content -LiteralPath $_.FullName -TotalCount 1 -ErrorAction SilentlyContinue
            if ($firstLine -and $firstLine -match '^\.\./.+\.patch$') {
                $notConverted += $_.FullName
            }
        }
    }

    if ($notConverted.Count -gt 0) {
        $message = ($notConverted | ForEach-Object { "`n  $_" }) -join ''
        Fail "Patch link conversion incomplete for:$message"
    }
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-FullPath -Path (Join-Path $scriptDir '..') -MustExist
$versionsEnv = Join-Path $repoRoot 'versions.env'

if ($Help) {
    Show-Usage
    exit 0
}

if (-not (Test-Path -LiteralPath $versionsEnv)) {
    Fail "versions.env not found at $versionsEnv"
}

if (-not $DpdkVersion) {
    $DpdkVersion = Get-VersionFromEnvFile -EnvFile $versionsEnv
}

if (-not $WorkingDirectory) {
    $WorkingDirectory = $repoRoot
}
$WorkingDirectory = Resolve-FullPath -Path $WorkingDirectory

$sourceDirectory = Join-Path $WorkingDirectory "dpdk-$DpdkVersion-msvc"
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $WorkingDirectory "build\\dpdk-$DpdkVersion-msvc"
}
if (-not $InstallDirectory) {
    $InstallDirectory = Join-Path $WorkingDirectory ".local_install\\dpdk-$DpdkVersion-msvc"
}

$patchDir = Join-Path $repoRoot "patches\\dpdk\\$DpdkVersion"
$windowsPatchDir = Join-Path $patchDir 'windows'

if (-not (Test-Path -LiteralPath $patchDir)) {
    Fail "Patch directory not found: $patchDir"
}

Assert-Command -Name git -Hint 'Install Git for Windows and ensure git.exe is on PATH.'
Assert-Command -Name meson -Hint 'Install Meson (pip install meson) and ensure meson is on PATH.'
Assert-Command -Name ninja -Hint 'Install Ninja and ensure ninja.exe is on PATH.'
Assert-Command -Name cl -Hint 'Run this script from an MSVC Developer PowerShell/Command Prompt.'
Assert-Command -Name link -Hint 'Run this script from an MSVC Developer PowerShell/Command Prompt.'
Assert-Command -Name lib -Hint 'Run this script from an MSVC Developer PowerShell/Command Prompt.'

if ($Force) {
    foreach ($ownedPath in @($sourceDirectory, $BuildDirectory, $InstallDirectory)) {
        if (Test-Path -LiteralPath $ownedPath) {
            Write-Host "Removing script-owned directory: $ownedPath"
            Remove-Item -LiteralPath $ownedPath -Recurse -Force
        }
    }
}

foreach ($existingPath in @($sourceDirectory, $BuildDirectory, $InstallDirectory)) {
    if (Test-Path -LiteralPath $existingPath) {
        Fail "Path already exists: $existingPath. Re-run with -Force to clean script-owned directories."
    }
}

New-Item -ItemType Directory -Path (Split-Path -Parent $BuildDirectory) -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $InstallDirectory) -Force | Out-Null
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null

Write-Host "Cloning DPDK v$DpdkVersion to $sourceDirectory"
Invoke-Checked -FilePath git -Arguments @(
    'clone',
    '--branch', "v$DpdkVersion",
    '--depth', '1',
    'https://github.com/DPDK/dpdk.git',
    $sourceDirectory
)

$patchWorkRoot = Join-Path $BuildDirectory '_mtl_patch_work'
$genericPatchWorkDir = Join-Path $patchWorkRoot 'generic'
$windowsPatchWorkDir = Join-Path $patchWorkRoot 'windows'

New-Item -ItemType Directory -Path $genericPatchWorkDir -Force | Out-Null
Copy-Item -Path (Join-Path $patchDir '*.patch') -Destination $genericPatchWorkDir -Force

if (Test-Path -LiteralPath $windowsPatchDir) {
    New-Item -ItemType Directory -Path $windowsPatchWorkDir -Force | Out-Null
    Copy-Item -Path (Join-Path $windowsPatchDir '*.patch') -Destination $windowsPatchWorkDir -Force
}

Convert-PatchLinksToFiles -PatchDirs @($genericPatchWorkDir, $windowsPatchWorkDir)

$genericPatches = @(Get-ChildItem -Path $genericPatchWorkDir -Filter '*.patch' -File | Sort-Object Name)
if ($genericPatches.Count -eq 0) {
    Fail "No generic DPDK patches found in $genericPatchWorkDir"
}

$windowsPatches = @()
if (Test-Path -LiteralPath $windowsPatchWorkDir) {
    $windowsPatches = @(Get-ChildItem -Path $windowsPatchWorkDir -Filter '*.patch' -File | Sort-Object Name)
}

Write-Host 'Applying generic MTL DPDK patches with git am...'
$genericPatchPaths = $genericPatches | ForEach-Object { $_.FullName }
Invoke-Checked -FilePath git -Arguments @('-C', $sourceDirectory, 'am') + $genericPatchPaths

if ($windowsPatches.Count -gt 0) {
    Write-Host 'Applying Windows-specific DPDK patches with git apply...'
    $windowsPatchPaths = $windowsPatches | ForEach-Object { $_.FullName }
    Invoke-Checked -FilePath git -Arguments @('-C', $sourceDirectory, 'apply') + $windowsPatchPaths
}

$mesonArgs = @(
    'setup',
    $BuildDirectory,
    $sourceDirectory,
    '--prefix', $InstallDirectory,
    '--default-library=static',
    "-Dmax_lcores=$MaxLcores"
)

Write-Host 'Configuring DPDK with Meson for static libraries...'
Invoke-Checked -FilePath meson -Arguments $mesonArgs

Write-Host 'Compiling DPDK...'
Invoke-Checked -FilePath meson -Arguments @('compile', '-C', $BuildDirectory)

Write-Host 'Installing DPDK...'
Invoke-Checked -FilePath meson -Arguments @('install', '-C', $BuildDirectory)

$installIncludeDir = Join-Path $InstallDirectory 'include'
$rteConfigPath = Join-Path $installIncludeDir 'rte_config.h'
if (-not (Test-Path -LiteralPath $rteConfigPath)) {
    Fail "Expected generated rte_config.h not found at $rteConfigPath"
}

$pkgConfigCandidates = @(
    (Join-Path $InstallDirectory 'lib\\pkgconfig\\libdpdk.pc'),
    (Join-Path $InstallDirectory 'lib64\\pkgconfig\\libdpdk.pc')
)
$pkgConfigPath = $pkgConfigCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $pkgConfigPath) {
    Fail "libdpdk.pc not found under install prefix. Checked: $($pkgConfigCandidates -join ', ')"
}

$libDirs = @(
    (Join-Path $InstallDirectory 'lib'),
    (Join-Path $InstallDirectory 'lib64')
) | Where-Object { Test-Path -LiteralPath $_ }

$staticLibs = @()
foreach ($libDir in $libDirs) {
    $staticLibs += Get-ChildItem -Path $libDir -Recurse -File -Include '*.lib', '*.a'
}

if ($staticLibs.Count -eq 0) {
    Fail "No static libraries (*.lib/*.a) found under install prefix ($InstallDirectory)."
}

Write-Host ''
Write-Host 'DPDK static build/install completed successfully.'
Write-Host "  Source  : $sourceDirectory"
Write-Host "  Build   : $BuildDirectory"
Write-Host "  Install : $InstallDirectory"
Write-Host "  libdpdk.pc: $pkgConfigPath"
Write-Host ''
Write-Host 'For a later native Windows MTL Meson build, expose this DPDK installation with:'
Write-Host "  `$env:PKG_CONFIG_PATH = \"$(Split-Path -Parent $pkgConfigPath);`$env:PKG_CONFIG_PATH\""
Write-Host ''
Write-Host 'Then configure MTL Meson in the same MSVC developer shell.'
