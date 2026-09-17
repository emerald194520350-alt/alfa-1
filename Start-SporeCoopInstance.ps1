param(
    [ValidateSet('1', '2')]
    [string]$Profile = '2',
    [ValidateSet('host', 'guest')]
    [string]$Role = 'guest',
    [string]$LauncherRoot = 'C:\ProgramData\SPORE ModAPI Launcher Kit 2',
    [string]$GameRoot = 'C:\Games\SPORE Collection',
    [string]$Server = '127.0.0.1',
    [int]$Port = 5523,
    [string]$Token = 'local-guest-012345678901234567890123456789'
)

$ErrorActionPreference = 'Stop'

if (-not [Environment]::Is64BitProcess) {
    $launcherPath = Join-Path $LauncherRoot 'Spore ModAPI Launcher.exe'
    $commonPath = Join-Path $LauncherRoot 'ModAPI.Common.dll'
    $executablePath = Join-Path $GameRoot 'SporebinEP1\SporeApp.exe'
    $sporebinPath = Split-Path -Parent $executablePath

    if (-not (Test-Path -LiteralPath $launcherPath)) { throw "Launcher not found: $launcherPath" }
    if (-not (Test-Path -LiteralPath $commonPath)) { throw "ModAPI.Common not found: $commonPath" }
    if (-not (Test-Path -LiteralPath $executablePath)) { throw "Game not found: $executablePath" }

    Set-Location -LiteralPath $LauncherRoot
    [Environment]::CurrentDirectory = $LauncherRoot
    [Environment]::SetEnvironmentVariable('SPORE_COOP_PROFILE', $Profile, 'Process')
    [Environment]::SetEnvironmentVariable('SPORE_COOP_ROLE', $Role, 'Process')
    [Environment]::SetEnvironmentVariable('SPORE_COOP_SERVER', $Server, 'Process')
    [Environment]::SetEnvironmentVariable('SPORE_COOP_PORT', [string]$Port, 'Process')
    [Environment]::SetEnvironmentVariable('SPORE_COOP_TOKEN', $Token, 'Process')

    $commonAssembly = [Reflection.Assembly]::LoadFrom($commonPath)
    $launcherAssembly = [Reflection.Assembly]::LoadFrom($launcherPath)
    $programType = $launcherAssembly.GetType('SporeModAPI_Launcher.Program', $true)
    $program = [Activator]::CreateInstance($programType, $true)
    $flags = [Reflection.BindingFlags]'Public,NonPublic,Instance'

    $programType.GetField('LauncherKitPath', $flags).SetValue($program, $LauncherRoot)
    $programType.GetField('SporebinPath', $flags).SetValue($program, $sporebinPath)
    $programType.GetField('ExecutablePath', $flags).SetValue($program, $executablePath)

    $gameVersionType = $commonAssembly.GetType('ModAPI.Common.GameVersionType', $true)
    $gameVersion = [Enum]::Parse($gameVersionType, 'Steam_March2017')
    $programType.GetField('ExecutableType', $flags).SetValue($program, $gameVersion)

    $startupType = $commonAssembly.GetType('ModAPI.Common.NativeTypes+STARTUPINFO', $true)
    $startupInfo = [Activator]::CreateInstance($startupType)
    $startupType.GetField('cb').SetValue(
        $startupInfo, [uint32][Runtime.InteropServices.Marshal]::SizeOf($startupInfo))
    $programType.GetField('StartupInfo', $flags).SetValue($program, $startupInfo)

    $inject = $programType.GetMethod('InjectSporeProcess', [Reflection.BindingFlags]'NonPublic,Instance')

    try {
        $inject.Invoke($program, @('steam_patched'))

        $processInfo = $programType.GetField('ProcessInfo', $flags).GetValue($program)
        $pidValue = $processInfo.GetType().GetField('dwProcessId').GetValue($processInfo)
        "Started Spore profile $Profile with PID $pidValue through $LauncherRoot"
    }
    catch {
        if ($_.Exception.InnerException) { throw $_.Exception.InnerException }
        throw
    }
    exit
}

$x86PowerShell = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
& $x86PowerShell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath `
    -Profile $Profile -Role $Role -LauncherRoot $LauncherRoot -GameRoot $GameRoot `
    -Server $Server -Port $Port -Token $Token
if ($LASTEXITCODE -ne 0) { throw "32-bit launcher helper exited with code $LASTEXITCODE" }
