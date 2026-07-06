param(
    [string] $PackageRoot = "",
    [string] $OutputPath = "",
    [string] $ProductVersion = "0.1.0",
    [string] $WixExe = "",
    [switch] $SelfTest
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$InstallerSource = Join-Path $RepoRoot "installer\MonitorSplitter.wxs"
$WixUtilExtension = "WixToolset.Util.wixext"
$RequiredPackageFiles = @(
    "bin\MonitorSplitterCtl.exe",
    "bin\MonitorSplitterHost.exe",
    "bin\MonitorSplitterService.exe",
    "bin\MonitorSplitterConfig.exe",
    "driver-package\MonitorSplitterDriver.inf",
    "driver-package\MonitorSplitterDriver.dll",
    "driver-package\monitorsplitterdriver.cat",
    "scripts\verify-install.ps1",
    "scripts\install-driver.ps1",
    "scripts\install-service.ps1",
    "scripts\msi-custom-action.ps1",
    "README.md",
    "LICENSE"
)

function Resolve-FullPath {
    param([Parameter(Mandatory=$true)][string] $Path)
    return [IO.Path]::GetFullPath($Path)
}

function Resolve-DefaultPackageRoot {
    return Join-Path $RepoRoot "out"
}

function Resolve-DefaultOutputPath {
    param([Parameter(Mandatory=$true)][string] $Version)
    return Join-Path (Join-Path $RepoRoot "out") "MonitorSplitter-$Version-x64.msi"
}

function Resolve-WixTool {
    if (-not [string]::IsNullOrWhiteSpace($WixExe)) {
        if (-not (Test-Path -LiteralPath $WixExe -PathType Leaf)) {
            throw "Configured WiX executable was not found: $WixExe"
        }
        return (Resolve-FullPath $WixExe)
    }

    $command = Get-Command wix.exe -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        $command = Get-Command wix -ErrorAction SilentlyContinue
    }
    if ($null -eq $command) {
        throw "WiX v4 was not found on PATH. Install WiX Toolset v4 and the WixToolset.Util.wixext extension, then rerun scripts\package-msi.ps1."
    }
    return $command.Source
}

function Assert-PackageRootComplete {
    param([Parameter(Mandatory=$true)][string] $Root)

    foreach ($relativePath in $RequiredPackageFiles) {
        $path = Join-Path $Root $relativePath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "MSI package root is incomplete; missing $relativePath under $Root. Run scripts\package-driver.ps1 first."
        }
    }
}

function Get-WixBuildArguments {
    param(
        [Parameter(Mandatory=$true)][string] $SourcePath,
        [Parameter(Mandatory=$true)][string] $Root,
        [Parameter(Mandatory=$true)][string] $Destination,
        [Parameter(Mandatory=$true)][string] $Version
    )

    return @(
        "build",
        $SourcePath,
        "-arch", "x64",
        "-ext", $WixUtilExtension,
        "-d", "PackageRoot=$Root",
        "-d", "ProductVersion=$Version",
        "-o", $Destination
    )
}

function Write-CommandOutput {
    param([object[]] $Output)

    foreach ($line in $Output) {
        Write-Host $line
    }
}

function Test-WixMissingExtension {
    param([object[]] $Output)

    $text = ($Output | ForEach-Object { "$_" }) -join "`n"
    return ($text -match "WIX0144" -and $text -match [regex]::Escape($WixUtilExtension))
}

function Add-WixExtension {
    param([Parameter(Mandatory=$true)][string] $ToolPath)

    Write-Host "WiX extension $WixUtilExtension is missing; installing it into the WiX extension cache..."
    $output = & $ToolPath extension add $WixUtilExtension 2>&1
    $exitCode = $LASTEXITCODE
    if ($output) {
        Write-CommandOutput $output
    }
    if ($exitCode -ne 0) {
        throw "Could not install WiX extension $WixUtilExtension. Run '$ToolPath extension add $WixUtilExtension' and rerun scripts\package-msi.ps1."
    }
}

function Invoke-WixBuild {
    param(
        [Parameter(Mandatory=$true)][string] $ToolPath,
        [Parameter(Mandatory=$true)][string[]] $Arguments
    )

    $output = & $ToolPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($output) {
        Write-CommandOutput $output
    }
    if ($exitCode -eq 0) {
        return
    }

    if (Test-WixMissingExtension $output) {
        Add-WixExtension -ToolPath $ToolPath
        $output = & $ToolPath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
        if ($output) {
            Write-CommandOutput $output
        }
        if ($exitCode -eq 0) {
            return
        }
    }

    throw "WiX build failed with exit code ${exitCode}: $ToolPath $($Arguments -join ' ')"
}

function Assert-SelfTest {
    param(
        [bool] $Condition,
        [Parameter(Mandatory=$true)][string] $Message
    )

    if (-not $Condition) {
        throw "MSI packaging self-test failed: $Message"
    }
}

if ($SelfTest) {
    $root = Join-Path ([IO.Path]::GetTempPath()) "MonitorSplitterMsiPackageSelfTest.$PID.$([Guid]::NewGuid().ToString('N'))"
    try {
        foreach ($relativePath in $RequiredPackageFiles) {
            $path = Join-Path $root $relativePath
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
            Set-Content -LiteralPath $path -Encoding ascii -Value "selftest"
        }

        Assert-PackageRootComplete $root
        $arguments = @(Get-WixBuildArguments `
            -SourcePath "D:\src\MonitorSplitter.wxs" `
            -Root $root `
            -Destination "D:\out\MonitorSplitter.msi" `
            -Version "1.2.3")
        Assert-SelfTest ($arguments[0] -eq "build") "WiX command should use the build verb"
        Assert-SelfTest ($arguments -contains $WixUtilExtension) "WiX build must include the Util extension for elevated custom actions"
        Assert-SelfTest ($arguments -contains "PackageRoot=$root") "WiX build must define PackageRoot"
        Assert-SelfTest ($arguments -contains "ProductVersion=1.2.3") "WiX build must define ProductVersion"
        Assert-SelfTest (Test-WixMissingExtension @("wix.exe : error WIX0144: The extension '$WixUtilExtension' could not be found.")) "WiX missing-extension detection should recognize WIX0144"

        Remove-Item -LiteralPath (Join-Path $root "bin\MonitorSplitterCtl.exe") -Force
        $failed = $false
        try {
            Assert-PackageRootComplete $root
        }
        catch {
            $failed = $true
        }
        Assert-SelfTest $failed "incomplete package root should fail validation"

        Write-Host "MSI packaging self-test passed."
    }
    finally {
        if (Test-Path -LiteralPath $root) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
    }
    return
}

if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = Resolve-DefaultPackageRoot
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Resolve-DefaultOutputPath $ProductVersion
}

$PackageRoot = Resolve-FullPath $PackageRoot
$OutputPath = Resolve-FullPath $OutputPath

if (-not (Test-Path -LiteralPath $InstallerSource -PathType Leaf)) {
    throw "WiX source file was not found: $InstallerSource"
}

Assert-PackageRootComplete $PackageRoot
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null

$wix = Resolve-WixTool
$arguments = @(Get-WixBuildArguments `
    -SourcePath $InstallerSource `
    -Root $PackageRoot `
    -Destination $OutputPath `
    -Version $ProductVersion)
Invoke-WixBuild -ToolPath $wix -Arguments $arguments
Write-Host "MSI package written to $OutputPath"
