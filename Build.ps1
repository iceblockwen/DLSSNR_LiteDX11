param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Fresh
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere) required' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ tools and Windows SDK required' }
$cmakeRoot = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake'
$cmake = Join-Path $cmakeRoot 'CMake/bin/cmake.exe'
$ninjaRoot = Join-Path $cmakeRoot 'Ninja'
if (-not (Test-Path -LiteralPath $cmake) -or -not (Test-Path -LiteralPath "$ninjaRoot/ninja.exe")) { throw 'Install Visual Studio C++ CMake tools' }
$devCmd = Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'
$savedEnvironment = @{}
Get-ChildItem Env: | ForEach-Object { $savedEnvironment[$_.Name] = $_.Value }
Push-Location $PSScriptRoot
try {
    $devEnvironment = & $env:ComSpec /d /c "call `"$devCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE) { throw 'Visual Studio environment initialization failed' }
    $toolPath = $null
    foreach ($line in $devEnvironment) {
        if ($line -match '^Path=(.*)$') {
            $candidatePath = $Matches[1]
            if (-not $toolPath -or $line -cmatch '^PATH=') { $toolPath = $candidatePath }
            continue
        }
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
    if (-not $toolPath) { throw 'Visual Studio tool PATH missing' }
    $env:PATH = "$ninjaRoot;$toolPath"
    $env:VSLANG = '1033'
    $preset = 'windows-' + $Configuration.ToLowerInvariant()
    $configureArguments = @('--preset', $preset)
    if ($Fresh) { $configureArguments += '--fresh' }
    & $cmake @configureArguments
    if ($LASTEXITCODE) { throw 'CMake configure failed' }
    & $cmake --build --preset $preset
    if ($LASTEXITCODE) { throw 'CMake build failed' }
    $installRoot = "dist/$Configuration"
    & $cmake --install "build/$preset" --prefix $installRoot
    if ($LASTEXITCODE) { throw 'CMake install failed' }
    Write-Output "Ready: $PSScriptRoot/$installRoot"
} finally {
    Pop-Location
    Get-ChildItem Env: | ForEach-Object {
        if (-not $savedEnvironment.ContainsKey($_.Name)) { [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process') }
    }
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}
