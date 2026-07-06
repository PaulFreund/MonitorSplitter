param(
    [string] $PackageDir = "",
    [switch] $Install,
    [switch] $RemoveExisting,
    [switch] $Uninstall,
    [switch] $SelfTest
)

$ErrorActionPreference = "Stop"

function Resolve-PackageRoot {
    $scriptParent = Split-Path -Parent $PSScriptRoot
    if (Test-Path -LiteralPath (Join-Path $scriptParent "driver-package") -PathType Container) {
        return $scriptParent
    }
    return $scriptParent
}

function Resolve-DefaultPackageDir {
    $packageRoot = Resolve-PackageRoot
    if (Test-Path -LiteralPath (Join-Path $packageRoot "driver-package") -PathType Container) {
        return Join-Path $packageRoot "driver-package"
    }
    return Join-Path $packageRoot "out\driver-package"
}

$RepoRoot = Resolve-PackageRoot
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = Resolve-DefaultPackageDir
}
$Inf = Join-Path $PackageDir "MonitorSplitterDriver.inf"
$script:PnPUtil = ""

function Resolve-PnPUtil {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:WINDIR)) {
        $candidates += (Join-Path $env:WINDIR "Sysnative\pnputil.exe")
        $candidates += (Join-Path $env:WINDIR "System32\pnputil.exe")
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    $command = Get-Command pnputil.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    throw "pnputil.exe was not found. This script must run on Windows with the driver package tools available."
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory=$true)]
        [string] $FilePath,
        [Parameter(ValueFromRemainingArguments=$true)]
        [string[]] $Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
    }
}

function Test-MonitorSplitterDriverPackage {
    param([Parameter(Mandatory=$true)] $Package)

    return $Package.OriginalName -like "*MonitorSplitterDriver.inf*" -or
        $Package.ProviderName -like "*MonitorSplitter*" -or
        $Package.PublishedName -like "*MonitorSplitter*"
}

function ConvertTo-SearchKey {
    param([Parameter(Mandatory=$true)][string] $Value)

    $compact = ($Value -replace "\s+", "").ToLowerInvariant()
    $normalized = $compact.Normalize([Text.NormalizationForm]::FormD)
    $builder = [Text.StringBuilder]::new()
    foreach ($char in $normalized.ToCharArray()) {
        if ([Globalization.CharUnicodeInfo]::GetUnicodeCategory($char) -ne [Globalization.UnicodeCategory]::NonSpacingMark -and
            [char]::IsLetterOrDigit($char)) {
            [void] $builder.Append($char)
        }
    }
    return $builder.ToString()
}

function ConvertTo-CanonicalDriverKey {
    param([Parameter(Mandatory=$true)][string] $Key)

    $searchKey = ConvertTo-SearchKey $Key
    switch ($searchKey) {
        "publishedname" { return "PublishedName" }
        "veroffentlichtername" { return "PublishedName" }
        "verffentlichtername" { return "PublishedName" }
        "originalname" { return "OriginalName" }
        "ursprunglichername" { return "OriginalName" }
        "ursprnglichername" { return "OriginalName" }
        "providername" { return "ProviderName" }
        "anbietername" { return "ProviderName" }
        default { return ($Key.Trim() -replace "\s+", "") }
    }
}

function ConvertFrom-PnPUtilDriverList {
    param([Parameter(Mandatory=$true)][AllowEmptyString()][string[]] $DriverList)

    $packages = @()
    $current = [ordered]@{}

    foreach ($line in $DriverList) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            if ($current.Count -gt 0) {
                $package = [pscustomobject] $current
                if (Test-MonitorSplitterDriverPackage $package) {
                    $packages += $package
                }
                $current = [ordered]@{}
            }
            continue
        }

        if ($line -match "^\s*([^:]+?)\s*:\s*(.*)$") {
            $key = ConvertTo-CanonicalDriverKey $matches[1]
            $current[$key] = $matches[2].Trim()
        }
    }

    if ($current.Count -gt 0) {
        $package = [pscustomobject] $current
        if (Test-MonitorSplitterDriverPackage $package) {
            $packages += $package
        }
    }

    return $packages
}

function Get-MonitorSplitterDriverPackages {
    if ([string]::IsNullOrWhiteSpace($script:PnPUtil)) {
        $script:PnPUtil = Resolve-PnPUtil
    }

    $driverList = & $script:PnPUtil /enum-drivers
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $script:PnPUtil /enum-drivers"
    }

    return ConvertFrom-PnPUtilDriverList -DriverList @($driverList)
}

function Assert-SelfTest {
    param(
        [bool] $Condition,
        [Parameter(Mandatory=$true)][string] $Message
    )

    if (-not $Condition) {
        throw "Driver installer self-test failed: $Message"
    }
}

function Invoke-InstallDriverSelfTest {
    $sample = @(
        "Published Name:     oem12.inf",
        "Original Name:      unrelated.inf",
        "Provider Name:      Example",
        "",
        "Published Name:     oem42.inf",
        "Original Name:      MonitorSplitterDriver.inf",
        "Provider Name:      MonitorSplitter",
        "",
        ("Ver" + [char]0x00f6 + "ffentlichter Name:     oem99.inf"),
        "Originalname:      MonitorSplitterDriver.inf",
        "Anbietername:      MonitorSplitter",
        "",
        "Original Name:      MonitorSplitterDriver.inf",
        "Provider Name:      MonitorSplitter"
    )

    $packages = @(ConvertFrom-PnPUtilDriverList -DriverList $sample)
    Assert-SelfTest ($packages.Count -eq 3) "parser did not return only MonitorSplitter packages"
    Assert-SelfTest ($packages[0].PublishedName -eq "oem42.inf") "parser did not preserve PublishedName"
    Assert-SelfTest ($packages[0].OriginalName -eq "MonitorSplitterDriver.inf") "parser did not preserve OriginalName"
    Assert-SelfTest ($packages[1].PublishedName -eq "oem99.inf") "parser did not canonicalize localized PublishedName"
    Assert-SelfTest ([string]::IsNullOrWhiteSpace($packages[2].PublishedName)) "parser should allow detection of packages missing PublishedName"
    Assert-SelfTest (Test-MonitorSplitterDriverPackage ([pscustomobject]@{ PublishedName = ""; OriginalName = ""; ProviderName = "MonitorSplitter" })) "provider match did not identify MonitorSplitter package"
    Assert-SelfTest (-not (Test-MonitorSplitterDriverPackage ([pscustomobject]@{ PublishedName = "oem7.inf"; OriginalName = "display.inf"; ProviderName = "Microsoft" }))) "unrelated driver was incorrectly matched"

    Write-Host "Driver installer self-test passed."
}

if ($SelfTest) {
    Invoke-InstallDriverSelfTest
    return
}

if (-not $Uninstall -and -not (Test-Path $Inf)) {
    throw "Driver INF not found: $Inf. Run scripts\package-driver.ps1 first."
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Driver package installation requires an elevated PowerShell session."
}

if ($Uninstall -and $Install) {
    throw "-Install and -Uninstall cannot be used together."
}

if ($RemoveExisting -or $Uninstall) {
    $script:PnPUtil = Resolve-PnPUtil
    $packages = Get-MonitorSplitterDriverPackages
    if ($packages.Count -eq 0) {
        Write-Host "No existing MonitorSplitter driver packages found in the driver store."
    }

    foreach ($package in $packages) {
        if ([string]::IsNullOrWhiteSpace($package.PublishedName)) {
            Write-Warning "Found MonitorSplitter package without a PublishedName; skipping."
            continue
        }

        Write-Host "Removing existing driver package $($package.PublishedName)"
        Invoke-Checked $script:PnPUtil /delete-driver $package.PublishedName /uninstall /force
    }

    if ($Uninstall) {
        return
    }
}

$args = @("/add-driver", $Inf)
if ($Install) {
    $args += "/install"
}

$script:PnPUtil = Resolve-PnPUtil
Invoke-Checked $script:PnPUtil @args
