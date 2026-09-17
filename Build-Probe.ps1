param(
    [Parameter(Mandatory = $true)][string]$LauncherRoot,
    [string]$SdkRoot = (Join-Path $PSScriptRoot 'vendor\Spore-ModAPI')
)
$ErrorActionPreference = 'Stop'
$environment = & (Join-Path $PSScriptRoot 'Check-Environment.ps1') | ConvertFrom-Json
if (-not $environment.ModAPICompatible) {
    throw 'Install and launch Spore: Galactic Adventures from Steam first. Base Spore is incompatible with this ModAPI SDK.'
}
if (-not $environment.MSBuildWithCpp) { throw 'Visual Studio 2022 Build Tools with MSVC v143 and Windows SDK is required.' }
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
$LauncherRoot = (Resolve-Path -LiteralPath $LauncherRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $LauncherRoot 'coreLibs\SporeModAPI.lib'))) {
    throw 'The selected folder is missing Launcher Kit coreLibs\SporeModAPI.lib.'
}
& $environment.MSBuildWithCpp (Join-Path $SdkRoot 'Spore ModAPI\Spore ModAPI.vcxproj') /nologo /m /p:Configuration=Release /p:Platform=Win32
if ($LASTEXITCODE -ne 0) { throw 'SDK static library build failed.' }
& $environment.MSBuildWithCpp (Join-Path $PSScriptRoot 'native\SporeCoop.Probe.vcxproj') /nologo /m /p:Configuration=Release /p:Platform=Win32 "/p:SdkRoot=$SdkRoot" "/p:LauncherRoot=$LauncherRoot"
if ($LASTEXITCODE -ne 0) { throw 'Native probe build failed.' }
Write-Output 'Built bin\SporeCoop.Probe.dll. This is an experimental NPC probe, not the full game adapter.'
