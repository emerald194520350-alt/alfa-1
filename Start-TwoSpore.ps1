$ErrorActionPreference = 'Stop'

$primaryLauncher = 'C:\ProgramData\SPORE ModAPI Launcher Kit\Spore ModAPI Launcher.exe'
$primaryLauncherRoot = 'C:\ProgramData\SPORE ModAPI Launcher Kit'
$gameExecutable = 'C:\Games\SPORE Collection\SporebinEP1\SporeApp.exe'
$secondLauncherScript = Join-Path $PSScriptRoot 'Start-SporeCoopInstance.ps1'
$serverScript = Join-Path $PSScriptRoot 'Start-SporeCoopServer.ps1'
$coopServer = '127.0.0.1'
$coopPort = 5523
$hostToken = 'local-host-012345678901234567890123456789'
$guestToken = 'local-guest-012345678901234567890123456789'
$builtMod = Join-Path $PSScriptRoot 'bin\SporeCoop.Probe.dll'
$primaryMod = 'C:\ProgramData\SPORE ModAPI Launcher Kit\mLibs\SporeCoop.Probe.dll'
$secondMod = 'C:\ProgramData\SPORE ModAPI Launcher Kit 2\mLibs\SporeCoop.Probe.dll'
$primaryProfile = Join-Path $env:APPDATA 'Spore'
$secondProfile = Join-Path $env:APPDATA 'SporeCoop2'

if (-not (Test-Path -LiteralPath $primaryLauncher)) {
    throw "Основной ModAPI Launcher не найден: $primaryLauncher"
}
if (-not (Test-Path -LiteralPath $secondLauncherScript)) {
    throw "Скрипт второго окна не найден: $secondLauncherScript"
}
if (-not (Test-Path -LiteralPath $serverScript)) {
    throw "Скрипт сервера не найден: $serverScript"
}

# CREATE_SUSPENDED, which ModAPI needs for injection, fails when Windows forces
# SporeApp.exe through the RUNASADMIN compatibility shim. Remove only that token
# and preserve any other compatibility flags selected by the user.
$compatibilityKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers'
$compatibilityFlags = $null
try {
    $compatibilityFlags = Get-ItemPropertyValue -LiteralPath $compatibilityKey `
        -Name $gameExecutable -ErrorAction Stop
}
catch [System.Management.Automation.ItemNotFoundException] { }
catch [System.Management.Automation.PSArgumentException] { }

if ($compatibilityFlags) {
    $remainingFlags = @(($compatibilityFlags -split '\s+') |
        Where-Object { $_ -and $_ -ne 'RUNASADMIN' })
    if ($remainingFlags.Count -ne (($compatibilityFlags -split '\s+') |
            Where-Object { $_ }).Count) {
        if ($remainingFlags.Count) {
            Set-ItemProperty -LiteralPath $compatibilityKey -Name $gameExecutable `
                -Value ($remainingFlags -join ' ')
        }
        else {
            Remove-ItemProperty -LiteralPath $compatibilityKey -Name $gameExecutable
        }
        Write-Host 'Отключён несовместимый с инжектором флаг RUNASADMIN для SporeApp.exe.' -ForegroundColor Yellow
    }
}

& $serverScript -Listen $coopServer -Port $coopPort `
    -HostToken $hostToken -GuestToken $guestToken

$instances = @(Get-Process -Name SporeApp -ErrorAction SilentlyContinue)
if ($instances.Count -ge 2) {
    Write-Host 'Два окна Spore уже запущены.' -ForegroundColor Green
    Start-Sleep -Seconds 2
    exit 0
}

if ($instances.Count -eq 0) {
    if (Test-Path -LiteralPath $builtMod) {
        foreach ($target in @($primaryMod, $secondMod)) {
            $targetDirectory = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $targetDirectory)) {
                New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
            }
            Copy-Item -LiteralPath $builtMod -Destination $target -Force
        }
        Write-Host 'Установлена последняя сборка кооперативного мода.' -ForegroundColor Green
    }

    if (Test-Path -LiteralPath $primaryProfile) {
        if (-not (Test-Path -LiteralPath $secondProfile)) {
            New-Item -ItemType Directory -Path $secondProfile -Force | Out-Null
        }
        $primaryGames = Join-Path $primaryProfile 'Games'
        $secondGames = Join-Path $secondProfile 'Games'
        if (Test-Path -LiteralPath $primaryGames) {
            if (-not (Test-Path -LiteralPath $secondGames)) {
                New-Item -ItemType Directory -Path $secondGames -Force | Out-Null
            }
            Get-ChildItem -LiteralPath $primaryGames -Force | ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination $secondGames -Recurse -Force
            }
        }
        foreach ($fileName in @('Planets.package', 'Pollination.package',
                'EditorSaves.package', 'RigblockInfo.package')) {
            $source = Join-Path $primaryProfile $fileName
            if (Test-Path -LiteralPath $source) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $secondProfile $fileName) -Force
            }
        }
        Write-Host 'Актуальный мир первого профиля скопирован во второй профиль.' -ForegroundColor Green
    }

    Write-Host 'Запускаю первое окно Spore прямым инжектором ModAPI...'
    & $secondLauncherScript -Profile 1 -Role host `
        -LauncherRoot $primaryLauncherRoot -Server $coopServer `
        -Port $coopPort -Token $hostToken

    $first = $null
    for ($i = 0; $i -lt 60 -and -not $first; $i++) {
        Start-Sleep -Milliseconds 500
        $first = Get-Process -Name SporeApp -ErrorAction SilentlyContinue |
            Select-Object -First 1
    }
    if (-not $first) { throw 'Первое окно Spore не запустилось за 30 секунд.' }
    Start-Sleep -Seconds 5
}

Write-Host 'Запускаю второе изолированное окно Spore...'
& $secondLauncherScript -Profile 2 -Role guest `
    -Server $coopServer -Port $coopPort -Token $guestToken

$instances = @()
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    $instances = @(Get-Process -Name SporeApp -ErrorAction SilentlyContinue)
    if ($instances.Count -ge 2) { break }
}

if ($instances.Count -lt 2) { throw 'Второе окно Spore не запустилось за 30 секунд.' }

Write-Host 'Готово: два окна запущены. Хост загружает мир, открывает Esc и нажимает Invite friend to co-op; во втором окне нужно принять приглашение.' -ForegroundColor Green
Start-Sleep -Seconds 3
