# SPDX-License-Identifier: GPL-3.0-or-later
# DX11 games only. Overwrite the plugin DLLs; preserve existing INI/model.
# .\tools\deploy.ps1                  list deployment targets
# .\tools\deploy.ps1 -Target bg3      deploy dist\Release as dxgi.dll
# .\tools\deploy.ps1 -Target bg3 -Entry version    use the version.dll entry
# .\tools\deploy.ps1 -Target bg3 -Build
param(
    [string]$Target = '',
    [string]$Path = '',
    [string]$Source = '',
    [string]$Model = '',
    [ValidateSet('dxgi', 'version', 'winmm')][string]$Entry = 'dxgi',
    [switch]$Build
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$games = [ordered]@{
    bg3 = 'F:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin'
    roe = 'H:\Games\RiseOfEros'
    wildlands = "G:\UBisoft\Tom Clancy's Ghost Recon Wildlands"
    bannerlord = 'F:\SteamLibrary\steamapps\common\Mount & Blade II Bannerlord\bin\Win64_Shipping_Client'
    odyssey = 'F:\SteamLibrary\steamapps\common\Assassins Creed Odyssey'
}
# Every entry this plugin can ship. Exactly one belongs in a game folder; the
# others stay free for other mods (or as <name>-original.dll in a chain).
$entryNames = @('dxgi', 'version', 'winmm')
$dlls = @("$Entry.dll", 'nvngx.dll_litedx11.dll')
$iniName = 'DLSSNR_LiteDX11.ini'
$modelName = 'nvngx_dlssnr.dll'
if (-not $Source) { $Source = Join-Path $root 'dist\Release' }
# The model ships in a versioned package folder, never beside the repository.
if (-not $Model) { $Model = Join-Path (Split-Path -Parent $root) 'DLSSNR_Model\nvngx_dlssnr_2026_10\nvngx_dlssnr.dll' }

if (-not $Target -and -not $Path) {
    foreach ($name in $games.Keys) {
        $folder = $games[$name]
        $runtime = Join-Path $folder $dlls[1]
        $state = if (Test-Path -LiteralPath $runtime -PathType Leaf) {
            'runtime ' + (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.Substring(0, 12)
        } else { 'not deployed' }
        $entryState = foreach ($entry in $entryNames) {
            $file = Join-Path $folder "$entry.dll"
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
            $sourceFile = Join-Path $Source "$entry.dll"
            $ours = (Test-Path -LiteralPath $sourceFile -PathType Leaf) -and
                ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash)
            if ($ours) { "our $entry.dll" } else { "foreign $entry.dll" }
        }
        Write-Host ("{0} : {1} ({2}; {3})" -f $name, $folder, $state, ($entryState -join ', '))
    }
    Write-Host "source : $Source"
    return
}
if ($Target -and $Path) { throw 'Use either -Target or -Path, not both.' }
if ($Path) { $folder = $Path }
else {
    if (-not $games.Contains($Target)) { throw "Unknown target: $Target" }
    $folder = $games[$Target]
}
if (-not (Test-Path -LiteralPath $folder -PathType Container)) { throw "Game folder missing: $folder" }
$folder = (Resolve-Path -LiteralPath $folder).Path
if ($Build) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'Build.ps1')
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
}
foreach ($file in $dlls + $iniName) {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $file) -PathType Leaf)) {
        throw "Source file missing: $file"
    }
}
$modelDestination = Join-Path $folder $modelName
if (-not (Test-Path -LiteralPath $modelDestination -PathType Leaf) -and
    -not (Test-Path -LiteralPath $Model -PathType Leaf)) { throw "NR model missing: $Model" }
# One entry per game folder: refuse to stack our own dxgi.dll and version.dll,
# and leave another mod's DLL of the same name untouched.
foreach ($other in ($entryNames | Where-Object { $_ -ne $Entry })) {
    $otherFile = Join-Path $folder "$other.dll"
    if (-not (Test-Path -LiteralPath $otherFile -PathType Leaf)) { continue }
    $otherSource = Join-Path $Source "$other.dll"
    $ours = (Test-Path -LiteralPath $otherSource -PathType Leaf) -and
        ((Get-FileHash -LiteralPath $otherFile -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $otherSource -Algorithm SHA256).Hash)
    if ($ours) { throw "This folder still has our $other.dll; remove it first, only $Entry.dll belongs here." }
    Write-Host "note  kept foreign $other.dll (not this plugin's copy)"
}

Write-Host "Deploy DX11 ($Entry.dll entry): $folder"
foreach ($file in $dlls + $iniName + $modelName) {
    $destination = Join-Path $folder $file
    if (($file -eq $iniName -or $file -eq $modelName) -and
        (Test-Path -LiteralPath $destination -PathType Leaf)) {
        Write-Host "kept  $file"
        continue
    }
    $inputFile = if ($file -eq $modelName) { $Model } else { Join-Path $Source $file }
    $expected = (Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $inputFile -Destination $destination -Force
    $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($actual -ne $expected) { throw "Copied file hash mismatch: $file" }
    Write-Host "wrote $file SHA256=$actual"
}
Write-Host 'Deployment verified. No game was started.'
