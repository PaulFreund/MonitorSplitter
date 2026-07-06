param(
    [string] $ConfigDir = "C:\ProgramData\MonitorSplitter"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
Set-Content -Path (Join-Path $ConfigDir "service-enabled.txt") -Value "0" -Encoding ASCII

& sc.exe stop MonitorSplitterService | Out-Null
Start-Sleep -Seconds 3

Get-Process MonitorSplitterHost, MonitorSplitterConfig, MonitorSplitterService -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host "All MonitorSplitter runtime processes stopped."
