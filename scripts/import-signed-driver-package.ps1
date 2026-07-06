param(
    [string] $SignedPackagePath = "",
    [string] $PackageRoot = "",
    [string] $DriverPackageDir = "",
    [switch] $RequireMicrosoftSignature,
    [switch] $SelfTest
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-FullPath {
    param([Parameter(Mandatory=$true)][string] $Path)
    return [IO.Path]::GetFullPath($Path)
}

function Resolve-FirstExistingPath {
    param(
        [Parameter(Mandatory=$true)][string] $Name,
        [Parameter(Mandatory=$true)][string[]] $Candidates
    )

    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return $candidate
        }
    }

    throw "$Name not found. Checked: $($Candidates -join '; ')"
}

function Expand-SignedPackage {
    param(
        [Parameter(Mandatory=$true)][string] $Path,
        [Parameter(Mandatory=$true)][string] $Destination
    )

    if (Test-Path -LiteralPath $Path -PathType Container) {
        return (Resolve-FullPath $Path)
    }

    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $extension = [IO.Path]::GetExtension($Path).ToLowerInvariant()
    if ($extension -eq ".zip") {
        Expand-Archive -LiteralPath $Path -DestinationPath $Destination -Force
        return $Destination
    }
    if ($extension -eq ".cab") {
        $expand = Resolve-FirstExistingPath "expand.exe" @((Get-Command expand.exe -ErrorAction SilentlyContinue).Source, (Join-Path $env:SystemRoot "System32\expand.exe"))
        & $expand $Path -F:* $Destination | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Could not extract CAB package: $Path"
        }
        return $Destination
    }

    throw "Unsupported signed package type: $Path. Pass a directory, .zip, or .cab."
}

function Find-RequiredFile {
    param(
        [Parameter(Mandatory=$true)][string] $Root,
        [Parameter(Mandatory=$true)][string] $FileName
    )

    $matches = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Filter $FileName)
    if ($matches.Count -eq 0) {
        throw "Signed package is missing $FileName under $Root"
    }
    if ($matches.Count -gt 1) {
        $exact = @($matches | Where-Object { $_.Directory.Name -eq "MonitorSplitterDriver" })
        if ($exact.Count -eq 1) {
            return $exact[0].FullName
        }
        throw "Signed package contains multiple $FileName files; pass a package with a single MonitorSplitter driver package."
    }

    return $matches[0].FullName
}

function Invoke-SelfTest {
    $root = Join-Path ([IO.Path]::GetTempPath()) "MonitorSplitterImportSignedSelfTest.$PID.$([Guid]::NewGuid().ToString('N'))"
    try {
        $source = Join-Path $root "signed\MonitorSplitterDriver"
        $target = Join-Path $root "out\driver-package"
        New-Item -ItemType Directory -Force -Path $source, $target | Out-Null
        foreach ($fileName in @("MonitorSplitterDriver.inf", "MonitorSplitterDriver.dll", "monitorsplitterdriver.cat")) {
            Set-Content -LiteralPath (Join-Path $source $fileName) -Encoding ascii -Value $fileName
        }
        $global:LASTEXITCODE = 0
        & $PSCommandPath -SignedPackagePath (Split-Path -Parent $source) -PackageRoot (Join-Path $root "out") -DriverPackageDir $target | Out-Null
        foreach ($fileName in @("MonitorSplitterDriver.inf", "MonitorSplitterDriver.dll", "monitorsplitterdriver.cat")) {
            if (-not (Test-Path -LiteralPath (Join-Path $target $fileName) -PathType Leaf)) {
                throw "self-test did not import $fileName"
            }
        }

        Write-Host "Signed driver package import self-test passed."
    }
    finally {
        if (Test-Path -LiteralPath $root) {
            Remove-Item -LiteralPath $root -Recurse -Force
        }
    }
}

if ($SelfTest) {
    Invoke-SelfTest
    return
}

if ([string]::IsNullOrWhiteSpace($SignedPackagePath)) {
    throw "Pass -SignedPackagePath with the Microsoft-signed package directory, .zip, or .cab."
}

if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = Join-Path $RepoRoot "out"
}
if ([string]::IsNullOrWhiteSpace($DriverPackageDir)) {
    $DriverPackageDir = Join-Path $PackageRoot "driver-package"
}

$PackageRoot = Resolve-FullPath $PackageRoot
$DriverPackageDir = Resolve-FullPath $DriverPackageDir
$SignedPackagePath = Resolve-FullPath $SignedPackagePath
New-Item -ItemType Directory -Force -Path $DriverPackageDir | Out-Null

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) "MonitorSplitterSignedPackage.$PID.$([Guid]::NewGuid().ToString('N'))"
try {
    $sourceRoot = Expand-SignedPackage -Path $SignedPackagePath -Destination $tempRoot
    $sources = @{
        "MonitorSplitterDriver.inf" = Find-RequiredFile -Root $sourceRoot -FileName "MonitorSplitterDriver.inf"
        "MonitorSplitterDriver.dll" = Find-RequiredFile -Root $sourceRoot -FileName "MonitorSplitterDriver.dll"
        "monitorsplitterdriver.cat" = Find-RequiredFile -Root $sourceRoot -FileName "monitorsplitterdriver.cat"
    }

    foreach ($entry in $sources.GetEnumerator()) {
        Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $DriverPackageDir $entry.Key) -Force
    }

    if ($RequireMicrosoftSignature) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "verify-driver-signature.ps1") -DriverPackageDir $DriverPackageDir -RequireMicrosoft
        if ($LASTEXITCODE -ne 0) {
            throw "Imported driver package did not pass Microsoft signature verification."
        }
    }

    [pscustomobject]@{
        ok = $true
        driverPackageDir = $DriverPackageDir
        importedFiles = @($sources.Keys)
        microsoftSignatureRequired = [bool] $RequireMicrosoftSignature
    } | ConvertTo-Json -Depth 3
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
