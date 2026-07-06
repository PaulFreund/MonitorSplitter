param(
    [string] $ConfigDir = "C:\ProgramData\MonitorSplitter"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
Set-Content -Path (Join-Path $ConfigDir "service-enabled.txt") -Value "0" -Encoding ASCII

function Signal-NamedEvent([string] $Name) {
    try {
        $event = [System.Threading.EventWaitHandle]::OpenExisting($Name)
        try {
            [void] $event.Set()
        }
        finally {
            $event.Dispose()
        }
    }
    catch {
    }
}

Signal-NamedEvent "Global\MonitorSplitter.ServiceWake"
Signal-NamedEvent "Global\MonitorSplitter.AgentWake"

Get-Process MonitorSplitterHost -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Get-Process MonitorSplitterService -ErrorAction SilentlyContinue |
    Where-Object { $_.SessionId -ne 0 } |
    Stop-Process -Force -ErrorAction SilentlyContinue

Start-Sleep -Seconds 2
Signal-NamedEvent "Global\MonitorSplitter.ServiceWake"

Write-Host "MonitorSplitter desired state set to disabled and stuck session processes stopped."
