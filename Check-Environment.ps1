param(
    [string]$SporeRoot = 'C:\Program Files (x86)\Steam\steamapps\common\Spore',
    [string]$LauncherRoot = 'C:\ProgramData\SPORE ModAPI Launcher Kit'
)
$ErrorActionPreference = 'Stop'
$baseExe = Join-Path $SporeRoot 'SporeBin\SporeApp.exe'
$candidatePaths = [System.Collections.Generic.List[string]]::new()
$candidatePaths.Add((Join-Path $SporeRoot 'SporebinEP1\SporeApp.exe'))
foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Electronic Arts\SPORE_EP1', 'HKLM:\SOFTWARE\Electronic Arts\SPORE_EP1')) {
    $property = Get-ItemProperty -LiteralPath $key -Name DataDir -ErrorAction SilentlyContinue
    if ($property -and $property.DataDir) {
        $data = $property.DataDir.Trim('"').TrimEnd('\', '/')
        $candidatePaths.Add((Join-Path (Split-Path $data -Parent) 'SporebinEP1\SporeApp.exe'))
    }
}
$gaExe = $candidatePaths | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
$supportedSizes = @(24904192, 24909584, 24885248, 24895536, 25066744)
$gaInfo = if ($gaExe) { Get-Item -LiteralPath $gaExe }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = if (Test-Path -LiteralPath $vswhere) {
    & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
}
$launcherExe = Join-Path $LauncherRoot 'Spore ModAPI Launcher.exe'
$coreDll = Join-Path $LauncherRoot 'coreLibs\SporeModAPI.combined.dll'
$probeDll = Join-Path $PSScriptRoot 'bin\SporeCoop.Probe.dll'
$installedProbeDll = Join-Path $LauncherRoot 'mLibs\SporeCoop.Probe.dll'
$probeLog = Join-Path $env:TEMP 'SporeCoop.Probe.log'
$modApiCompatible = [bool]($gaInfo -and $gaInfo.Length -in $supportedSizes)
[pscustomobject]@{
    BaseSporeExe = if (Test-Path -LiteralPath $baseExe) { $baseExe } else { $null }
    BaseSporeVersion = if (Test-Path -LiteralPath $baseExe) { (Get-Item -LiteralPath $baseExe).VersionInfo.FileVersion } else { $null }
    GalacticAdventuresExe = $gaExe
    GalacticAdventuresVersion = if ($gaInfo) { $gaInfo.VersionInfo.FileVersion } else { $null }
    LauncherKitRoot = if (Test-Path -LiteralPath $launcherExe) { $LauncherRoot } else { $null }
    LauncherKitVersion = if (Test-Path -LiteralPath $launcherExe) { (Get-Item -LiteralPath $launcherExe).VersionInfo.FileVersion } else { $null }
    ModAPIDllVersion = if (Test-Path -LiteralPath $coreDll) { (Get-Item -LiteralPath $coreDll).VersionInfo.FileVersion } else { $null }
    ModAPICompatible = $modApiCompatible
    ModAPIBlockReason = if ($modApiCompatible) { $null } elseif (-not $gaExe) { 'Galactic Adventures is not installed; Launcher Kit cannot target base Spore.' } else { 'The Galactic Adventures executable version or size is unsupported.' }
    MSBuildWithCpp = $msbuild
    NativeProbeInstalledPath = if (Test-Path -LiteralPath $installedProbeDll) { $installedProbeDll } else { $null }
    NativeProbeStatus = if ((Test-Path -LiteralPath $installedProbeDll) -and (Test-Path -LiteralPath $probeLog)) { 'INSTALLED_AND_INITIALIZED; GAMEPLAY_COMMANDS_NOT_TESTED' } elseif (Test-Path -LiteralPath $installedProbeDll) { 'INSTALLED_NOT_INITIALIZED' } elseif (Test-Path -LiteralPath $probeDll) { 'BUILT_NOT_INSTALLED' } else { 'SOURCE_ONLY_NOT_COMPILED' }
} | ConvertTo-Json
