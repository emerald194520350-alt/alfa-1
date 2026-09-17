param([string]$NodePath)
$ErrorActionPreference = 'Stop'
if (-not $NodePath) {
    $nodeCommand = Get-Command node -ErrorAction SilentlyContinue
    if ($nodeCommand) { $NodePath = $nodeCommand.Source }
    else {
        $runtimeRoot = Join-Path $env:LOCALAPPDATA 'OpenAI\Codex\runtimes'
        $NodePath = Get-ChildItem -LiteralPath $runtimeRoot -Recurse -File -Filter node.exe -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $NodePath) { throw 'Node.js 22 or later is required to run the protocol tests.' }
& (Join-Path $PSScriptRoot 'Build-Server.ps1')
& $NodePath (Join-Path $PSScriptRoot 'tests\server.test.mjs')
if ($LASTEXITCODE -ne 0) { throw 'Protocol tests failed.' }
