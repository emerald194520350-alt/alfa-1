$ErrorActionPreference = 'Stop'

$logDirectory = Join-Path $env:LOCALAPPDATA 'SporeCoop'
$logPath = Join-Path $logDirectory 'launcher.log'
$startScript = Join-Path $PSScriptRoot 'Start-TwoSpore.ps1'

New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null

try {
    "`r`n[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] Запуск двух окон SPORE" |
        Out-File -LiteralPath $logPath -Encoding utf8 -Append
    & $startScript *>&1 | Out-File -LiteralPath $logPath -Encoding utf8 -Append
}
catch {
    $details = $_ | Out-String
    $details | Out-File -LiteralPath $logPath -Encoding utf8 -Append

    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show(
        "Не удалось запустить два окна SPORE.`n`n$($_.Exception.Message)`n`nПодробный журнал:`n$logPath",
        'SPORE Coop — ошибка запуска',
        [System.Windows.MessageBoxButton]::OK,
        [System.Windows.MessageBoxImage]::Error) | Out-Null
    exit 1
}
