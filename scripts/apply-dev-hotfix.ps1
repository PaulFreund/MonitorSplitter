param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $InstallDir = (Join-Path $env:ProgramFiles "MonitorSplitter")
)

$ErrorActionPreference = "Stop"

function Stop-MonitorSplitterRuntime {
    $service = Get-Service -Name "MonitorSplitterService" -ErrorAction SilentlyContinue
    if ($null -ne $service -and $service.Status -ne "Stopped") {
        & sc.exe stop "MonitorSplitterService" | Out-Null
    }

    $names = @("MonitorSplitterConfig", "MonitorSplitterHost", "MonitorSplitterService")
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $running = @(Get-Process -Name $names -ErrorAction SilentlyContinue)
        if ($running.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 500
    }

    $serviceProcessId = 0
    $serviceInfo = Get-CimInstance Win32_Service -Filter "Name='MonitorSplitterService'" -ErrorAction SilentlyContinue
    if ($null -ne $serviceInfo) {
        $serviceProcessId = [int] $serviceInfo.ProcessId
    }
    if ($serviceProcessId -ne 0) {
        Stop-Process -Id $serviceProcessId -Force -ErrorAction SilentlyContinue
    }
    Get-Process -Name $names -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

function Copy-RequiredFiles {
    $sourceBin = Join-Path $RepoRoot "out\bin"
    $sourceScripts = Join-Path $RepoRoot "out\scripts"
    $sourceDriver = Join-Path $RepoRoot "out\driver-package"

    $targetBin = Join-Path $InstallDir "bin"
    $targetScripts = Join-Path $InstallDir "scripts"
    $targetDriver = Join-Path $InstallDir "driver-package"

    New-Item -ItemType Directory -Force -Path $targetBin, $targetScripts, $targetDriver | Out-Null
    Copy-Item -Path (Join-Path $sourceBin "*") -Destination $targetBin -Force
    Copy-Item -Path (Join-Path $sourceScripts "*") -Destination $targetScripts -Force
    Copy-Item -Path (Join-Path $sourceDriver "*") -Destination $targetDriver -Force
}

Stop-MonitorSplitterRuntime
Copy-RequiredFiles

$driverScript = Join-Path $InstallDir "scripts\install-driver.ps1"
$driverPackage = Join-Path $InstallDir "driver-package"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $driverScript -Install -RemoveExisting -PackageDir $driverPackage
if ($LASTEXITCODE -ne 0) {
    throw "install-driver.ps1 failed with exit code $LASTEXITCODE"
}

$serviceScript = Join-Path $InstallDir "scripts\install-service.ps1"
$serviceBin = Join-Path $InstallDir "bin"
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $serviceScript -Install -BinDir $serviceBin -NoTrayLaunch
if ($LASTEXITCODE -ne 0) {
    throw "install-service.ps1 failed with exit code $LASTEXITCODE"
}

Write-Host "MonitorSplitter development hotfix applied."
