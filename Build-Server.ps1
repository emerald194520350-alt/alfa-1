param([string]$OutputPath = (Join-Path $PSScriptRoot 'SporeCoop.Server.exe'))
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw '.NET Framework 4 compiler is missing.' }
& $compiler /nologo /target:exe /optimize+ /platform:anycpu /reference:System.Web.Extensions.dll "/out:$OutputPath" (Join-Path $PSScriptRoot 'server\SessionServer.cs')
if ($LASTEXITCODE -ne 0) { throw 'Server compilation failed.' }
Write-Output "Built $OutputPath"
