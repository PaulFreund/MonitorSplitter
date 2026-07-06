param(
    [switch] $Install,
    [switch] $Uninstall,
    [switch] $Start,
    [switch] $Stop,
    [switch] $Enable,
    [switch] $Disable,
    [switch] $DisableOnInstall,
    [switch] $KeepPanelState,
    [switch] $NoTrayLaunch,
    [switch] $SelfTest,
    [string] $BinDir = ""
)

$ErrorActionPreference = "Stop"

$ServiceName = "MonitorSplitterService"
$ServiceDisplayName = "MonitorSplitter Service"
$ServiceDescription = "Keeps MonitorSplitter virtual displays and direct ultrawide scanout running across users, logons, and power resume."
$ProgramDataDir = Join-Path $env:ProgramData "MonitorSplitter"
$DesiredPath = Join-Path $ProgramDataDir "service-enabled.txt"
$TrayRunKey = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Run"
$TrayRunValue = "MonitorSplitterConfig"
$ServiceWakeEventName = "Global\MonitorSplitter.ServiceWake"
$AgentWakeEventName = "Global\MonitorSplitter.AgentWake"
$EdidNameBaseSelectorMetadataPrefix = "msp:edid-name-base="

function Resolve-DefaultBinDir {
    $scriptParent = Split-Path -Parent $PSScriptRoot
    if (Test-Path -LiteralPath (Join-Path $scriptParent "bin") -PathType Container) {
        return Join-Path $scriptParent "bin"
    }
    return Join-Path $scriptParent "out\bin"
}

if ([string]::IsNullOrWhiteSpace($BinDir)) {
    $BinDir = Resolve-DefaultBinDir
}

$ServiceExe = Join-Path $BinDir "MonitorSplitterService.exe"
$ConfigExe = Join-Path $BinDir "MonitorSplitterConfig.exe"
$CtlExe = Join-Path $BinDir "MonitorSplitterCtl.exe"

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this script from an elevated PowerShell session."
    }
}

function Get-InstalledService {
    Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
}

function Invoke-NativeChecked {
    param(
        [Parameter(Mandatory=$true)][string] $FilePath,
        [string[]] $Arguments = @()
    )

    & $FilePath @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function Read-TextFileTrimmed {
    param([Parameter(Mandatory=$true)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }

    return ([string] (Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue)).Trim()
}

function Write-AtomicTextFile {
    param(
        [Parameter(Mandatory=$true)][string] $Path,
        [Parameter(Mandatory=$true)][string] $Value
    )

    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    $temp = "$Path.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
    $backup = "$temp.bak"
    try {
        [IO.File]::WriteAllText($temp, $Value + [Environment]::NewLine, [Text.Encoding]::ASCII)
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            [IO.File]::Replace($temp, $Path, $backup, $true)
            if (Test-Path -LiteralPath $backup -PathType Leaf) {
                Remove-Item -LiteralPath $backup -Force
            }
        }
        else {
            [IO.File]::Move($temp, $Path)
        }
    }
    catch {
        if (Test-Path -LiteralPath $temp -PathType Leaf) {
            Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
        }
        throw
    }
}

function Get-SelectorEdidNameBaseMetadata {
    param([string] $Selector)

    foreach ($line in ($Selector -split "\r?\n")) {
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith($EdidNameBaseSelectorMetadataPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            $value = $trimmed.Substring($EdidNameBaseSelectorMetadataPrefix.Length).Trim()
            if (-not [string]::IsNullOrWhiteSpace($value)) {
                return $value
            }
        }
    }

    return ""
}

function Set-SelectorEdidNameBaseMetadata {
    param(
        [string] $Selector,
        [string] $NameBase
    )

    $trimmedNameBase = $NameBase.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmedNameBase)) {
        return $Selector.Trim()
    }

    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($Selector -split "\r?\n")) {
        $trimmed = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed) -or
            $trimmed.StartsWith($EdidNameBaseSelectorMetadataPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }

        $exists = $false
        foreach ($existing in $lines) {
            if ([string]::Equals($existing, $trimmed, [StringComparison]::OrdinalIgnoreCase)) {
                $exists = $true
                break
            }
        }
        if (-not $exists) {
            $lines.Add($trimmed) | Out-Null
        }
    }

    $lines.Add("${EdidNameBaseSelectorMetadataPrefix}${trimmedNameBase}") | Out-Null
    return [string]::Join("`n", $lines)
}

function Convert-TargetDevicePathToEdidRegistryPath {
    param([string] $TargetDevicePath)

    $value = $TargetDevicePath.Trim()
    if ([string]::IsNullOrWhiteSpace($value)) {
        return ""
    }

    $prefix = '\\?\'
    if ($value.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        $value = $value.Substring($prefix.Length)
    }

    $classGuidSeparator = $value.IndexOf('#{', [StringComparison]::OrdinalIgnoreCase)
    if ($classGuidSeparator -ge 0) {
        $value = $value.Substring(0, $classGuidSeparator)
    }

    $value = $value.Replace('#', '\')
    if (-not $value.StartsWith('DISPLAY\', [StringComparison]::OrdinalIgnoreCase)) {
        return ""
    }

    return "HKLM:\SYSTEM\CurrentControlSet\Enum\$value\Device Parameters"
}

function Get-EdidNameFromBytes {
    param([byte[]] $Edid)

    if ($null -eq $Edid -or $Edid.Length -lt 128) {
        return ""
    }

    foreach ($offset in @(54, 72, 90, 108)) {
        if ($offset + 18 -gt $Edid.Length) {
            continue
        }
        if ($Edid[$offset] -ne 0x00 -or
            $Edid[$offset + 1] -ne 0x00 -or
            $Edid[$offset + 2] -ne 0x00 -or
            $Edid[$offset + 3] -ne 0xFC -or
            $Edid[$offset + 4] -ne 0x00) {
            continue
        }

        $chars = New-Object System.Collections.Generic.List[char]
        for ($index = 0; $index -lt 13; $index++) {
            $byte = $Edid[$offset + 5 + $index]
            if ($byte -eq 0x00 -or $byte -eq 0x0A) {
                break
            }
            if ($byte -ge 0x20 -and $byte -le 0x7E) {
                $chars.Add([char]$byte) | Out-Null
            }
        }

        return ([string]::new($chars.ToArray())).Trim()
    }

    return ""
}

function Get-EdidNameFromTargetDevicePath {
    param([string] $TargetDevicePath)

    $registryPath = Convert-TargetDevicePathToEdidRegistryPath $TargetDevicePath
    if ([string]::IsNullOrWhiteSpace($registryPath)) {
        return ""
    }

    try {
        $edid = (Get-ItemProperty -LiteralPath $registryPath -Name EDID -ErrorAction Stop).EDID
    }
    catch {
        return ""
    }

    return Get-EdidNameFromBytes $edid
}

function Repair-EdidNameBaseConfig {
    $hostTargetPath = Join-Path $ProgramDataDir "host-target.txt"
    $edidNameBasePath = Join-Path $ProgramDataDir "edid-name-base.txt"

    $hostTargetValue = Read-TextFileTrimmed $hostTargetPath
    if ([string]::IsNullOrWhiteSpace($hostTargetValue)) {
        return
    }

    $edidNameBaseValue = Read-TextFileTrimmed $edidNameBasePath
    $metadataValue = Get-SelectorEdidNameBaseMetadata $hostTargetValue

    if ([string]::IsNullOrWhiteSpace($edidNameBaseValue) -and
        [string]::IsNullOrWhiteSpace($metadataValue)) {
        foreach ($line in ($hostTargetValue -split "\r?\n")) {
            $trimmed = $line.Trim()
            if ([string]::IsNullOrWhiteSpace($trimmed) -or
                $trimmed.StartsWith($EdidNameBaseSelectorMetadataPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                continue
            }

            $registryName = Get-EdidNameFromTargetDevicePath $trimmed
            if (-not [string]::IsNullOrWhiteSpace($registryName)) {
                $edidNameBaseValue = $registryName
                Write-AtomicTextFile -Path $edidNameBasePath -Value $edidNameBaseValue
                Write-Host "Restored edid-name-base.txt from saved target EDID."
                break
            }
        }
    }

    if ([string]::IsNullOrWhiteSpace($edidNameBaseValue) -and
        -not [string]::IsNullOrWhiteSpace($metadataValue)) {
        Write-AtomicTextFile -Path $edidNameBasePath -Value $metadataValue
        Write-Host "Restored edid-name-base.txt from host-target metadata."
        return
    }

    if (-not [string]::IsNullOrWhiteSpace($edidNameBaseValue) -and
        ([string]::IsNullOrWhiteSpace($metadataValue) -or
            -not [string]::Equals($edidNameBaseValue, $metadataValue, [StringComparison]::OrdinalIgnoreCase))) {
        $repaired = Set-SelectorEdidNameBaseMetadata $hostTargetValue $edidNameBaseValue
        Write-AtomicTextFile -Path $hostTargetPath -Value $repaired
        Write-Host "Repaired EDID name base metadata in host-target.txt."
    }
}

function Assert-SelfTest {
    param(
        [bool] $Condition,
        [Parameter(Mandatory=$true)][string] $Message
    )

    if (-not $Condition) {
        throw "Service installer self-test failed: $Message"
    }
}

function Invoke-ServiceInstallerSelfTest {
    $originalProgramDataDir = $ProgramDataDir
    $root = Join-Path ([IO.Path]::GetTempPath()) "MonitorSplitterServiceInstallerSelfTest.$PID.$([Guid]::NewGuid().ToString('N'))"

    try {
        $script:ProgramDataDir = $root
        New-Item -ItemType Directory -Force -Path $script:ProgramDataDir | Out-Null

        $hostTargetPath = Join-Path $script:ProgramDataDir "host-target.txt"
        $edidNameBasePath = Join-Path $script:ProgramDataDir "edid-name-base.txt"

        Set-Content -LiteralPath $hostTargetPath -Value "adapter:100:0:4353" -Encoding ascii
        Set-Content -LiteralPath $edidNameBasePath -Value "C49RG9x" -Encoding ascii
        Repair-EdidNameBaseConfig
        $hostTarget = Get-Content -LiteralPath $hostTargetPath -Raw
        Assert-SelfTest ($hostTarget -match "(?im)^msp:edid-name-base=C49RG9x\s*$") "repair did not embed the EDID name base in host-target.txt"
        Assert-SelfTest ((Get-SelectorEdidNameBaseMetadata $hostTarget) -eq "C49RG9x") "embedded metadata could not be parsed"

        Remove-Item -LiteralPath $edidNameBasePath -Force
        Repair-EdidNameBaseConfig
        Assert-SelfTest ((Get-Content -LiteralPath $edidNameBasePath -Raw).Trim() -eq "C49RG9x") "repair did not restore edid-name-base.txt from metadata"

        Set-Content -LiteralPath $hostTargetPath -Value "msp:edid-name-base=OldName`nadapter:100:0:4353" -Encoding ascii
        Set-Content -LiteralPath $edidNameBasePath -Value "C49RG9x" -Encoding ascii
        Repair-EdidNameBaseConfig
        $repaired = Get-Content -LiteralPath $hostTargetPath -Raw
        Assert-SelfTest ((Get-SelectorEdidNameBaseMetadata $repaired) -eq "C49RG9x") "repair did not update stale selector metadata"
        Assert-SelfTest (@($repaired -split "`n" | Where-Object { $_.Trim().StartsWith($EdidNameBaseSelectorMetadataPrefix, [StringComparison]::OrdinalIgnoreCase) }).Count -eq 1) "repair duplicated selector metadata"

        $targetPath = "\\?\DISPLAY#SAM0F9C#5&499c1e7&0&UID4353#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}"
        $registryPath = Convert-TargetDevicePathToEdidRegistryPath $targetPath
        Assert-SelfTest (
            $registryPath -eq "HKLM:\SYSTEM\CurrentControlSet\Enum\DISPLAY\SAM0F9C\5&499c1e7&0&UID4353\Device Parameters"
        ) "target device path was not converted to the expected EDID registry path"

        $edid = New-Object byte[] 128
        $edid[54] = 0x00
        $edid[55] = 0x00
        $edid[56] = 0x00
        $edid[57] = 0xFC
        $edid[58] = 0x00
        $nameBytes = [Text.Encoding]::ASCII.GetBytes("C49RG9x")
        [Array]::Copy($nameBytes, 0, $edid, 59, $nameBytes.Length)
        Assert-SelfTest ((Get-EdidNameFromBytes $edid) -eq "C49RG9x") "EDID monitor-name descriptor was not decoded"

        $atomicPath = Join-Path $script:ProgramDataDir "atomic-write.txt"
        Write-AtomicTextFile -Path $atomicPath -Value "alpha"
        Assert-SelfTest ((Get-Content -LiteralPath $atomicPath -Raw).Trim() -eq "alpha") "atomic write did not create the expected file"
        Write-AtomicTextFile -Path $atomicPath -Value "beta"
        Assert-SelfTest ((Get-Content -LiteralPath $atomicPath -Raw).Trim() -eq "beta") "atomic write did not replace the existing file"
        Assert-SelfTest ($ServiceWakeEventName -ne $AgentWakeEventName) "service and agent wake events must stay distinct"
        Assert-SelfTest ((Get-ServiceDisablePlan -KeepPanelStateRequested:$false -CtlAvailable:$true) -eq "ctl-disable") "default service disable should use ctl for panel restore when available"
        Assert-SelfTest ((Get-ServiceDisablePlan -KeepPanelStateRequested:$true -CtlAvailable:$true) -eq "desired-state-only") "KeepPanelState should bypass ctl panel restore"
        Assert-SelfTest ((Get-ServiceDisablePlan -KeepPanelStateRequested:$false -CtlAvailable:$false) -eq "blocked-missing-ctl") "default service disable should not silently continue without ctl"

        Write-Host "Service installer self-test passed."
    }
    finally {
        $script:ProgramDataDir = $originalProgramDataDir
        if (Test-Path -LiteralPath $root) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
    }
}

function Ensure-ProgramDataConfig {
    New-Item -ItemType Directory -Force -Path $ProgramDataDir | Out-Null
    Invoke-NativeChecked icacls.exe @($ProgramDataDir, "/grant", "*S-1-5-32-545:(OI)(CI)(M)")

    $localDir = Join-Path $env:LOCALAPPDATA "MonitorSplitter"
    if (-not (Test-Path -LiteralPath $localDir)) {
        Repair-EdidNameBaseConfig
        return
    }

    foreach ($name in @("layout.txt", "active-layout.txt", "host-target.txt", "direct-target.txt", "edid-name-base.txt")) {
        $source = Join-Path $localDir $name
        $destination = Join-Path $ProgramDataDir $name
        if ((Test-Path -LiteralPath $source) -and -not (Test-Path -LiteralPath $destination)) {
            Copy-Item -LiteralPath $source -Destination $destination
            Write-Host "Copied $name to $ProgramDataDir"
        }
    }

    Repair-EdidNameBaseConfig
}

function Install-TrayStartup {
    if (-not (Test-Path -LiteralPath $ConfigExe)) {
        Write-Host "Skipping tray startup because $ConfigExe does not exist."
        return
    }

    New-Item -Path $TrayRunKey -Force | Out-Null
    Set-ItemProperty -Path $TrayRunKey -Name $TrayRunValue -Value "`"$ConfigExe`" --tray"
    Write-Host "Installed MonitorSplitter tray startup for all users."
}

function Start-TrayNow {
    if (-not (Test-Path -LiteralPath $ConfigExe)) {
        Write-Host "Skipping tray launch because $ConfigExe does not exist."
        return
    }

    $existing = Get-Process -Name "MonitorSplitterConfig" -ErrorAction SilentlyContinue
    if ($null -ne $existing) {
        Write-Host "MonitorSplitter tray is already running."
        return
    }

    Start-Process -FilePath $ConfigExe -ArgumentList "--tray"
    Write-Host "Started MonitorSplitter tray for the current user."
}

function Stop-TrayProcesses {
    $processes = @(Get-Process -Name "MonitorSplitterConfig" -ErrorAction SilentlyContinue)
    if ($processes.Count -eq 0) {
        Write-Host "MonitorSplitter tray is not running."
        return
    }

    foreach ($process in $processes) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    Write-Host "Stopped MonitorSplitter tray process(es)."
}

function Remove-TrayStartup {
    if (Test-Path -LiteralPath $TrayRunKey) {
        Remove-ItemProperty -Path $TrayRunKey -Name $TrayRunValue -ErrorAction SilentlyContinue
    }
}

function Set-DesiredState {
    param([bool] $Enabled)

    Ensure-ProgramDataConfig
    Write-AtomicTextFile -Path $DesiredPath -Value ($(if ($Enabled) { "1" } else { "0" }))
    Write-Host "MonitorSplitter service desired state set to $(if ($Enabled) { 'enabled' } else { 'disabled' })."
    Signal-ServiceWake
    Signal-AgentWake
}

function Get-ServiceDisablePlan {
    param(
        [bool] $KeepPanelStateRequested,
        [bool] $CtlAvailable
    )

    if ($KeepPanelStateRequested) {
        return "desired-state-only"
    }
    if ($CtlAvailable) {
        return "ctl-disable"
    }
    return "blocked-missing-ctl"
}

function Disable-MonitorSplitterServiceState {
    param([bool] $KeepPanelStateRequested)

    $plan = Get-ServiceDisablePlan `
        -KeepPanelStateRequested $KeepPanelStateRequested `
        -CtlAvailable (Test-Path -LiteralPath $CtlExe -PathType Leaf)

    if ($plan -eq "blocked-missing-ctl") {
        throw "Cannot safely disable MonitorSplitter because MonitorSplitterCtl.exe is missing: $CtlExe. Rerun with -KeepPanelState only if you intentionally want to leave the physical panel in its current Windows Settings state."
    }

    if ($plan -eq "ctl-disable") {
        Invoke-NativeChecked $CtlExe @("disable")
    }

    Set-DesiredState -Enabled $false
    if ($KeepPanelStateRequested) {
        Write-Host "MonitorSplitter stop was requested; the physical panel state was left unchanged."
    }
    else {
        Write-Host "MonitorSplitter stop was requested; physical panel restore was handled by MonitorSplitterCtl.exe."
    }
}

function Signal-ServiceWake {
    try {
        $event = [Threading.EventWaitHandle]::OpenExisting($ServiceWakeEventName)
        try {
            $event.Set() | Out-Null
        }
        finally {
            $event.Dispose()
        }
    }
    catch {
        # The service may not be installed or running yet; it also polls desired state.
    }
}

function Signal-AgentWake {
    try {
        $event = [Threading.EventWaitHandle]::OpenExisting($AgentWakeEventName)
        try {
            $event.Set() | Out-Null
        }
        finally {
            $event.Dispose()
        }
    }
    catch {
        # The session agent may not exist yet; the service also polls desired state.
    }
}

function Start-MonitorSplitterService {
    $service = Get-InstalledService
    if ($null -eq $service) {
        throw "$ServiceName is not installed."
    }
    if ($service.Status -ne "Running") {
        Start-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus([System.ServiceProcess.ServiceControllerStatus]::Running, [TimeSpan]::FromSeconds(30))
    }
    Write-Host "$ServiceName is running."
}

function Stop-MonitorSplitterService {
    $service = Get-InstalledService
    if ($null -eq $service) {
        Write-Host "$ServiceName is not installed."
        return
    }
    if ($service.Status -ne "Stopped") {
        Stop-Service -Name $ServiceName -Force
        (Get-Service -Name $ServiceName).WaitForStatus([System.ServiceProcess.ServiceControllerStatus]::Stopped, [TimeSpan]::FromSeconds(30))
    }
    Write-Host "$ServiceName is stopped."
}

function Install-MonitorSplitterService {
    if (-not (Test-Path -LiteralPath $ServiceExe)) {
        throw "Missing service executable: $ServiceExe. Run scripts\package-driver.ps1 first."
    }

    Stop-TrayProcesses
    Ensure-ProgramDataConfig
    Set-DesiredState -Enabled (-not $DisableOnInstall)

    $binaryPath = "`"$ServiceExe`""
    $service = Get-InstalledService
    if ($null -eq $service) {
        New-Service `
            -Name $ServiceName `
            -BinaryPathName $binaryPath `
            -DisplayName $ServiceDisplayName `
            -StartupType Automatic | Out-Null
        Invoke-NativeChecked sc.exe @("description", $ServiceName, $ServiceDescription)
        Write-Host "Installed $ServiceName."
    }
    else {
        Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName" -Name ImagePath -Value $binaryPath
        Set-Service -Name $ServiceName -StartupType Automatic
        Invoke-NativeChecked sc.exe @("description", $ServiceName, $ServiceDescription)
        Write-Host "Updated existing $ServiceName."
    }

    Invoke-NativeChecked sc.exe @("failure", $ServiceName, "reset=", "86400", "actions=", "restart/5000/restart/30000/restart/60000")
    Invoke-NativeChecked sc.exe @("failureflag", $ServiceName, "1")
    Install-TrayStartup
    if (-not $NoTrayLaunch) {
        Start-TrayNow
    }
    Start-MonitorSplitterService
}

function Uninstall-MonitorSplitterService {
    Disable-MonitorSplitterServiceState -KeepPanelStateRequested $KeepPanelState
    Remove-TrayStartup
    Stop-TrayProcesses
    Stop-MonitorSplitterService

    $service = Get-InstalledService
    if ($null -ne $service) {
        Invoke-NativeChecked sc.exe @("delete", $ServiceName)
        Write-Host "Deleted $ServiceName."
    }
}

if ($SelfTest) {
    Invoke-ServiceInstallerSelfTest
    return
}

Assert-Admin

if (-not ($Install -or $Uninstall -or $Start -or $Stop -or $Enable -or $Disable)) {
    throw "Specify one or more actions: -Install, -Uninstall, -Start, -Stop, -Enable, or -Disable."
}

if ($Uninstall) {
    Uninstall-MonitorSplitterService
}
if ($Install) {
    Install-MonitorSplitterService
}
if ($Enable) {
    Set-DesiredState -Enabled $true
    Start-MonitorSplitterService
}
if ($Disable) {
    Disable-MonitorSplitterServiceState -KeepPanelStateRequested $KeepPanelState
}
if ($Stop) {
    Stop-MonitorSplitterService
}
if ($Start) {
    Start-MonitorSplitterService
}
