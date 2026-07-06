param(
    [switch] $Install,
    [switch] $Uninstall,
    [string] $InstallDir = (Join-Path $env:ProgramFiles "MonitorSplitter"),
    [string] $EnableAfterInstall = "Preserve",
    [switch] $SkipVerify,
    [string] $RemoveConfig = "0",
    [string] $SkipPanelRestore = "1",
    [switch] $SelfTest
)

$ErrorActionPreference = "Stop"

$ProgramDataDir = Join-Path $env:ProgramData "MonitorSplitter"
$DesiredPath = Join-Path $ProgramDataDir "service-enabled.txt"

if (-not [string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = [IO.Path]::GetFullPath($InstallDir)
}

function Read-DesiredState {
    if (-not (Test-Path -LiteralPath $DesiredPath -PathType Leaf)) {
        return $false
    }

    $raw = Get-Content -LiteralPath $DesiredPath -Raw
    if ($null -eq $raw) {
        $raw = ""
    }

    $value = $raw.Trim().ToLowerInvariant()
    return $value -in @("1", "true", "enabled", "enable", "on")
}

function Resolve-EnableAfterInstall {
    param([string] $Value)

    if ($null -eq $Value) {
        $Value = ""
    }

    switch ($Value.Trim().ToLowerInvariant()) {
        "preserve" { return (Read-DesiredState) }
        "" { return (Read-DesiredState) }
        "1" { return $true }
        "true" { return $true }
        "enabled" { return $true }
        "enable" { return $true }
        "on" { return $true }
        "0" { return $false }
        "false" { return $false }
        "disabled" { return $false }
        "disable" { return $false }
        "off" { return $false }
        default {
            throw "Invalid -EnableAfterInstall value '$Value'. Use Preserve, Enable, or Disable."
        }
    }
}

function Resolve-MsiBoolean {
    param([string] $Value)

    if ($null -eq $Value) {
        return $false
    }

    switch ($Value.Trim().ToLowerInvariant()) {
        "" { return $false }
        "0" { return $false }
        "false" { return $false }
        "disabled" { return $false }
        "disable" { return $false }
        "off" { return $false }
        "no" { return $false }
        "1" { return $true }
        "true" { return $true }
        "enabled" { return $true }
        "enable" { return $true }
        "on" { return $true }
        "yes" { return $true }
        default {
            throw "Invalid MSI boolean value '$Value'. Use 1/0 or true/false."
        }
    }
}

function Invoke-ScriptChecked {
    param(
        [Parameter(Mandatory=$true)][string] $Path,
        [string[]] $Arguments = @()
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required script is missing: $Path"
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Path $($Arguments -join ' ')"
    }
}

function Get-ServiceInstallArguments {
    param([bool] $EnableAfterInstallResolved)

    $arguments = @("-Install", "-BinDir", (Join-Path $InstallDir "bin"), "-NoTrayLaunch")
    if (-not $EnableAfterInstallResolved) {
        $arguments += "-DisableOnInstall"
    }
    return $arguments
}

function Get-ServiceUninstallArguments {
    param([bool] $SkipPanelRestoreResolved)

    # MSI runs unattended and must never trigger the interactive panel-restore flow.
    return @("-Uninstall", "-KeepPanelState")
}

function Remove-ProgramDataConfig {
    if (-not (Resolve-MsiBoolean $RemoveConfig)) {
        return
    }

    $fullProgramData = [IO.Path]::GetFullPath($env:ProgramData).TrimEnd('\')
    $fullTarget = [IO.Path]::GetFullPath($ProgramDataDir).TrimEnd('\')
    $expectedTarget = [IO.Path]::GetFullPath((Join-Path $env:ProgramData "MonitorSplitter")).TrimEnd('\')
    if ($fullTarget -ine $expectedTarget -or -not $fullTarget.StartsWith($fullProgramData + "\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove unexpected ProgramData directory: $fullTarget"
    }

    if (Test-Path -LiteralPath $ProgramDataDir) {
        Remove-Item -LiteralPath $ProgramDataDir -Recurse -Force
        Write-Host "Removed MonitorSplitter ProgramData config."
    }
}

function Install-MonitorSplitterFromMsiFiles {
    $scriptsDir = Join-Path $InstallDir "scripts"
    $driverDir = Join-Path $InstallDir "driver-package"
    $driverScript = Join-Path $scriptsDir "install-driver.ps1"
    $serviceScript = Join-Path $scriptsDir "install-service.ps1"
    $verifyScript = Join-Path $scriptsDir "verify-install.ps1"
    $enableAfterInstallResolved = Resolve-EnableAfterInstall $EnableAfterInstall

    Invoke-ScriptChecked -Path $driverScript -Arguments @("-PackageDir", $driverDir, "-Install", "-RemoveExisting")
    Invoke-ScriptChecked -Path $serviceScript -Arguments @(Get-ServiceInstallArguments $enableAfterInstallResolved)

    if (-not $SkipVerify) {
        $verifyArguments = @("-InstallDir", $InstallDir, "-SkipRuntimeStatus")
        if ($enableAfterInstallResolved) {
            $verifyArguments += "-RequireHealthyDirectHost"
        }
        Invoke-ScriptChecked -Path $verifyScript -Arguments $verifyArguments
    }

    Write-Host "MonitorSplitter MSI install action completed."
}

function Uninstall-MonitorSplitterFromMsiFiles {
    $scriptsDir = Join-Path $InstallDir "scripts"
    $serviceScript = Join-Path $scriptsDir "install-service.ps1"
    $driverScript = Join-Path $scriptsDir "install-driver.ps1"
    $skipPanelRestoreResolved = Resolve-MsiBoolean $SkipPanelRestore

    Invoke-ScriptChecked -Path $serviceScript -Arguments @(Get-ServiceUninstallArguments $skipPanelRestoreResolved)
    Invoke-ScriptChecked -Path $driverScript -Arguments @("-Uninstall")
    Remove-ProgramDataConfig
    Write-Host "MonitorSplitter MSI uninstall action completed."
}

function Assert-SelfTest {
    param(
        [bool] $Condition,
        [Parameter(Mandatory=$true)][string] $Message
    )

    if (-not $Condition) {
        throw "MSI custom-action self-test failed: $Message"
    }
}

if ($SelfTest) {
    $originalDesiredPath = $DesiredPath
    $root = Join-Path ([IO.Path]::GetTempPath()) "MonitorSplitterMsiCustomActionSelfTest.$PID.$([Guid]::NewGuid().ToString('N'))"
    try {
        New-Item -ItemType Directory -Force -Path $root | Out-Null
        $script:DesiredPath = Join-Path $root "service-enabled.txt"
        Assert-SelfTest ((Resolve-EnableAfterInstall "Preserve") -eq $false) "fresh preserve should default to disabled"
        Set-Content -LiteralPath $script:DesiredPath -Encoding ascii -Value "1"
        Assert-SelfTest ((Resolve-EnableAfterInstall "Preserve") -eq $true) "preserve should read enabled desired state"
        Assert-SelfTest ((Resolve-EnableAfterInstall "Disable") -eq $false) "Disable should resolve false"
        Assert-SelfTest ((Resolve-EnableAfterInstall "Enable") -eq $true) "Enable should resolve true"
        Assert-SelfTest ((Resolve-MsiBoolean "1") -eq $true) "1 should resolve true"
        Assert-SelfTest ((Resolve-MsiBoolean "0") -eq $false) "0 should resolve false"

        $script:InstallDir = "C:\Program Files\MonitorSplitter"
        $disabledArguments = @(Get-ServiceInstallArguments $false)
        Assert-SelfTest ($disabledArguments -contains "-DisableOnInstall") "disabled service install should pass -DisableOnInstall"
        $enabledArguments = @(Get-ServiceInstallArguments $true)
        Assert-SelfTest (-not ($enabledArguments -contains "-DisableOnInstall")) "enabled service install should not pass -DisableOnInstall"
        Assert-SelfTest ($enabledArguments -contains "-NoTrayLaunch") "MSI service install should not launch tray from elevated custom action"
        Assert-SelfTest ((Resolve-MsiBoolean $SkipPanelRestore) -eq $true) "default MSI uninstall should skip interactive panel restore"
        Assert-SelfTest ((Get-ServiceUninstallArguments $false) -contains "-KeepPanelState") "MSI uninstall should keep panel state even when an old package passes SkipPanelRestore=0"
        Assert-SelfTest ((Get-ServiceUninstallArguments $true) -contains "-KeepPanelState") "MSI uninstall should keep panel state"

        $argHelper = Join-Path $root "arg-helper.ps1"
        $argCapture = Join-Path $root "arg-capture.txt"
        Set-Content -LiteralPath $argHelper -Value 'Set-Content -LiteralPath $env:MONITOR_SPLITTER_ARG_CAPTURE -Value ($args -join "|") -Encoding ascii' -Encoding ascii
        $env:MONITOR_SPLITTER_ARG_CAPTURE = $argCapture
        Invoke-ScriptChecked -Path $argHelper -Arguments @("-PackageDir", "C:\Program Files\MonitorSplitter\driver-package", "-Install", "-RemoveExisting")
        Assert-SelfTest ((Get-Content -LiteralPath $argCapture -Raw).Trim() -eq "-PackageDir|C:\Program Files\MonitorSplitter\driver-package|-Install|-RemoveExisting") "Invoke-ScriptChecked did not forward named script arguments"
        Remove-Item Env:\MONITOR_SPLITTER_ARG_CAPTURE -ErrorAction SilentlyContinue

        Write-Host "MSI custom-action self-test passed."
    }
    finally {
        $script:DesiredPath = $originalDesiredPath
        Remove-Item Env:\MONITOR_SPLITTER_ARG_CAPTURE -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $root) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
    }
    return
}

$actionCount = @(@($Install, $Uninstall) | Where-Object { $_ }).Count
if ($actionCount -ne 1) {
    throw "Specify exactly one action: -Install or -Uninstall."
}

if ($Install) {
    Install-MonitorSplitterFromMsiFiles
}
elseif ($Uninstall) {
    Uninstall-MonitorSplitterFromMsiFiles
}
