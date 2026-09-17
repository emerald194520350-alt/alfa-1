param(
    [string]$Listen = '127.0.0.1',
    [int]$Port = 5523,
    [string]$HostToken = 'local-host-012345678901234567890123456789',
    [string]$GuestToken = 'local-guest-012345678901234567890123456789',
    [string]$SavePath = (Join-Path $env:LOCALAPPDATA 'SporeCoop\session.json')
)

$ErrorActionPreference = 'Stop'
$server = Join-Path $PSScriptRoot 'SporeCoop.Server.exe'
if (-not (Test-Path -LiteralPath $server)) {
    & (Join-Path $PSScriptRoot 'Build-Server.ps1')
}

$alreadyListening = $false
$client = New-Object Net.Sockets.TcpClient
try {
    $pending = $client.BeginConnect($Listen, $Port, $null, $null)
    if ($pending.AsyncWaitHandle.WaitOne(200)) {
        $client.EndConnect($pending)
        $alreadyListening = $client.Connected
    }
}
catch { $alreadyListening = $false }
finally { $client.Dispose() }

$pendingServer = Join-Path $PSScriptRoot 'SporeCoop.Server.next.exe'
if ($alreadyListening -and (Test-Path -LiteralPath $pendingServer) -and
        -not (Get-Process -Name SporeApp -ErrorAction SilentlyContinue)) {
    Get-Process -Name 'SporeCoop.Server' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $server } | Stop-Process
    Start-Sleep -Milliseconds 300
    $alreadyListening = $false
}
if ($alreadyListening) {
    Write-Output "SporeCoop server is already listening on ${Listen}:$Port"
    exit 0
}

if (Test-Path -LiteralPath $pendingServer) {
    Move-Item -LiteralPath $pendingServer -Destination $server -Force
    Write-Output 'Installed the pending SporeCoop server update.'
}

$saveDirectory = Split-Path -Parent $SavePath
if (-not (Test-Path -LiteralPath $saveDirectory)) {
    New-Item -ItemType Directory -Path $saveDirectory -Force | Out-Null
}

$arguments = @(
    '--listen', $Listen,
    '--port', [string]$Port,
    '--host-token', $HostToken,
    '--guest-token', $GuestToken,
    '--save', $SavePath
)
$process = Start-Process -FilePath $server -ArgumentList $arguments -WindowStyle Hidden -PassThru

for ($i = 0; $i -lt 50; $i++) {
    Start-Sleep -Milliseconds 100
    if ($process.HasExited) { throw "SporeCoop server exited with code $($process.ExitCode)." }
    $probe = New-Object Net.Sockets.TcpClient
    try {
        $pending = $probe.BeginConnect($Listen, $Port, $null, $null)
        if ($pending.AsyncWaitHandle.WaitOne(100)) {
            $probe.EndConnect($pending)
            if ($probe.Connected) {
                Write-Output "Started SporeCoop server PID $($process.Id) on ${Listen}:$Port"
                exit 0
            }
        }
    }
    catch { }
    finally { $probe.Dispose() }
}

throw 'SporeCoop server did not start listening in time.'
