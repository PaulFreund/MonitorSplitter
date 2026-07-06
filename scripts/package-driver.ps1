param(
    [string] $Configuration = "Release",
    [string] $DriverConfiguration = "Release",
    [string] $Platform = "x64",
    [string] $DriverVersion = "0.1.0.0",
    [string] $ProductVersion = "0.1.0",
    [string] $BuildTag = "",
    [string] $UmdfVersion = "2.25.0",
    [string] $OutputRoot = "",
    [string] $PackageDir = "",
    [string] $BinDir = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$DriverProject = Join-Path $RepoRoot "src\MonitorSplitterDriver\MonitorSplitterDriver.vcxproj"
$CtlProject = Join-Path $RepoRoot "src\MonitorSplitterCtl\MonitorSplitterCtl.vcxproj"
$HostProject = Join-Path $RepoRoot "src\MonitorSplitterHost\MonitorSplitterHost.vcxproj"
$ServiceProject = Join-Path $RepoRoot "src\MonitorSplitterService\MonitorSplitterService.vcxproj"
$ConfigProject = Join-Path $RepoRoot "src\MonitorSplitterConfig\MonitorSplitterConfig.vcxproj"
$RequiredBinFiles = @(
    "MonitorSplitterCtl.exe",
    "MonitorSplitterHost.exe",
    "MonitorSplitterService.exe",
    "MonitorSplitterConfig.exe"
)
$RequiredDriverFiles = @(
    "MonitorSplitterDriver.inf",
    "MonitorSplitterDriver.dll",
    "monitorsplitterdriver.cat"
)
$PackageScriptFiles = @(
    "verify-install.ps1",
    "install-driver.ps1",
    "install-service.ps1",
    "msi-custom-action.ps1",
    "package-msi.ps1",
    "package-attestation-cab.ps1",
    "import-signed-driver-package.ps1",
    "verify-driver-signature.ps1"
)
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot "out"
}
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = Join-Path $OutputRoot "driver-package"
}
if ([string]::IsNullOrWhiteSpace($BinDir)) {
    $BinDir = Join-Path $OutputRoot "bin"
}

function Resolve-FirstExistingPath {
    param(
        [Parameter(Mandatory=$true)]
        [string] $Name,
        [Parameter(Mandatory=$true)]
        [string[]] $Candidates
    )

    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }

    throw "$Name not found. Checked: $($Candidates -join '; ')"
}

function Get-ProgramFilesPath {
    param([switch] $X86)

    if ($X86) {
        $path = ${env:ProgramFiles(x86)}
        if ([string]::IsNullOrWhiteSpace($path)) {
            return "C:\Program Files (x86)"
        }
        return $path
    }

    if ([string]::IsNullOrWhiteSpace($env:ProgramFiles)) {
        return "C:\Program Files"
    }
    return $env:ProgramFiles
}

function Resolve-MSBuild {
    $programFiles = Get-ProgramFilesPath
    $candidates = @(
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe")
    )

    Resolve-FirstExistingPath "MSBuild.exe" $candidates
}

function Resolve-WindowsKitTool {
    param(
        [Parameter(Mandatory=$true)]
        [string] $SubDirectory,
        [Parameter(Mandatory=$true)]
        [string] $Architecture,
        [Parameter(Mandatory=$true)]
        [string] $ToolName,
        [string] $PreferredVersion = "10.0.26100.0"
    )

    $kitsRoot = Join-Path (Get-ProgramFilesPath -X86) "Windows Kits\10"
    $toolRoot = Join-Path $kitsRoot $SubDirectory
    $candidates = New-Object System.Collections.Generic.List[string]
    $candidates.Add((Join-Path $toolRoot "$PreferredVersion\$Architecture\$ToolName"))

    if (Test-Path -LiteralPath $toolRoot) {
        Get-ChildItem -LiteralPath $toolRoot -Directory |
            Sort-Object Name -Descending |
            ForEach-Object {
                $candidates.Add((Join-Path $_.FullName "$Architecture\$ToolName"))
            }
    }

    Resolve-FirstExistingPath $ToolName @($candidates)
}

$MsBuild = Resolve-MSBuild
$InfVerif = Resolve-WindowsKitTool "Tools" "x64" "infverif.exe"
$Inf2Cat = Resolve-WindowsKitTool "bin" "x86" "Inf2Cat.exe"
$StampInf = Resolve-WindowsKitTool "bin" "x86" "stampinf.exe"

function Resolve-VCTool {
    param(
        [Parameter(Mandatory=$true)]
        [string] $ToolName
    )

    $programFiles = Get-ProgramFilesPath
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($edition in @("Community", "Professional", "Enterprise", "BuildTools")) {
        $msvcRoot = Join-Path $programFiles "Microsoft Visual Studio\2022\$edition\VC\Tools\MSVC"
        if (Test-Path -LiteralPath $msvcRoot) {
            Get-ChildItem -LiteralPath $msvcRoot -Directory |
                Sort-Object Name -Descending |
                ForEach-Object {
                    $candidates.Add((Join-Path $_.FullName "bin\HostX64\x64\$ToolName"))
                }
        }
    }

    Resolve-FirstExistingPath $ToolName @($candidates)
}

$DumpBin = Resolve-VCTool "dumpbin.exe"

function Resolve-BuildTag {
    param(
        [Parameter(Mandatory=$true)][string] $Version,
        [string] $ExplicitBuildTag
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitBuildTag)) {
        return ($ExplicitBuildTag -replace '[^A-Za-z0-9_.+-]', '-')
    }

    $timestamp = (Get-Date).ToUniversalTime().ToString("yyyyMMdd-HHmmss")
    $git = "nogit"
    try {
        $short = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($short)) {
            $git = $short.Trim()
            $dirty = (& git -C $RepoRoot status --porcelain 2>$null)
            if ($LASTEXITCODE -eq 0 -and @($dirty).Count -gt 0) {
                $git += "-dirty"
            }
        }
    }
    catch {
        $git = "nogit"
    }

    return "$Version+$timestamp.$git"
}

function Assert-WdkUserModeToolsetInstalled {
    $programFiles = Get-ProgramFilesPath
    $candidates = @(
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\Platforms\$Platform\PlatformToolsets\WindowsUserModeDriver10.0"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Professional\MSBuild\Microsoft\VC\v170\Platforms\$Platform\PlatformToolsets\WindowsUserModeDriver10.0"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Enterprise\MSBuild\Microsoft\VC\v170\Platforms\$Platform\PlatformToolsets\WindowsUserModeDriver10.0"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\BuildTools\MSBuild\Microsoft\VC\v170\Platforms\$Platform\PlatformToolsets\WindowsUserModeDriver10.0")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return
        }
    }

    throw "The Visual Studio WDK platform toolset WindowsUserModeDriver10.0 is not installed for $Platform. Install the Windows Driver Kit Visual Studio extension/build tools matching SDK 10.0.26100, then rerun this script. Checked: $($candidates -join '; ')"
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

function Resolve-FullPath {
    param([Parameter(Mandatory=$true)][string] $Path)
    return [IO.Path]::GetFullPath($Path)
}

function Reset-PackageDirectory {
    param(
        [Parameter(Mandatory=$true)]
        [string] $Path,
        [Parameter(Mandatory=$true)]
        [string] $OutputRoot
    )

    $fullPath = (Resolve-FullPath $Path).TrimEnd('\')
    $fullOutputRoot = (Resolve-FullPath $OutputRoot).TrimEnd('\')
    if ($fullPath.StartsWith($fullOutputRoot + "\", [StringComparison]::OrdinalIgnoreCase) -and
        $fullPath -ine $fullOutputRoot) {
        if (Test-Path -LiteralPath $fullPath) {
            Remove-Item -LiteralPath $fullPath -Recurse -Force
        }
    }

    New-Item -ItemType Directory -Force $fullPath | Out-Null
}

function Assert-BinaryHasNoVCRuntimeImports {
    param(
        [Parameter(Mandatory=$true)]
        [string] $Path,
        [Parameter(Mandatory=$true)]
        [string] $Description,
        [switch] $ForbidUcrtApiSet
    )

    $output = & $DumpBin /dependents $Path
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $DumpBin /dependents $Path"
    }

    $forbidden = @(
        $output |
            Where-Object {
                $_ -match "(?i)\b(MSVCP140[^\\/\s]*|VCRUNTIME140[^\\/\s]*|ucrtbase|ucrtbased)\.dll\b" -or
                ($ForbidUcrtApiSet -and $_ -match "(?i)\bapi-ms-win-crt-[^\\/\s]+\.dll\b")
            } |
            ForEach-Object { $_.Trim() }
    )
    if ($forbidden.Count -gt 0) {
        throw "$Description imports forbidden VC runtime DLL(s): $($forbidden -join ', '). Build Release package binaries with the static runtime before packaging."
    }
}

function Copy-PackageFile {
    param(
        [Parameter(Mandatory=$true)]
        [string] $Source,
        [Parameter(Mandatory=$true)]
        [string] $Destination
    )

    try {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop
    }
    catch {
        throw "Could not copy package file from $Source to $Destination. If the destination is in use by an installed MonitorSplitter process, stop the service first or pass -OutputRoot to package into a separate directory. $($_.Exception.Message)"
    }
}

function Copy-PackageSupportFiles {
    param([Parameter(Mandatory=$true)][string] $OutputRoot)

    $scriptOutput = Join-Path $OutputRoot "scripts"
    $symbolOutput = Join-Path $OutputRoot "symbols"
    Reset-PackageDirectory -Path $scriptOutput -OutputRoot $OutputRoot
    foreach ($fileName in $PackageScriptFiles) {
        Copy-PackageFile (Join-Path $PSScriptRoot $fileName) $scriptOutput
    }

    Reset-PackageDirectory -Path $symbolOutput -OutputRoot $OutputRoot
    $driverPdb = Join-Path $RepoRoot "src\MonitorSplitterDriver\$Platform\$DriverConfiguration\MonitorSplitterDriver.pdb"
    if (Test-Path -LiteralPath $driverPdb -PathType Leaf) {
        Copy-PackageFile $driverPdb $symbolOutput
    }

    foreach ($fileName in @("README.md", "LICENSE")) {
        $source = Join-Path $RepoRoot $fileName
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-PackageFile $source $OutputRoot
        }
    }

    $installerSource = Join-Path $RepoRoot "installer"
    $installerOutput = Join-Path $OutputRoot "installer"
    if (Test-Path -LiteralPath $installerSource -PathType Container) {
        Reset-PackageDirectory -Path $installerOutput -OutputRoot $OutputRoot
        Copy-Item -LiteralPath (Join-Path $installerSource "MonitorSplitter.wxs") -Destination $installerOutput -Force
    }
}

function Assert-RequiredFiles {
    param(
        [Parameter(Mandatory=$true)]
        [string] $Directory,
        [Parameter(Mandatory=$true)]
        [string[]] $FileNames
    )

    foreach ($fileName in $FileNames) {
        $path = Join-Path $Directory $fileName
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Package output is incomplete; missing required file: $path"
        }
    }
}

function Assert-PackageScriptsParse {
    param([Parameter(Mandatory=$true)][string] $ScriptDirectory)

    foreach ($fileName in $PackageScriptFiles) {
        $path = Join-Path $ScriptDirectory $fileName
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
        if ($errors.Count -gt 0) {
            $formattedErrors = @($errors | ForEach-Object { $_.ToString() }) -join "; "
            throw "Packaged PowerShell script has parse errors: $path - $formattedErrors"
        }
    }
}

function Assert-CtlSelfTest {
    param([Parameter(Mandatory=$true)][string] $Path)

    $global:LASTEXITCODE = 0
    $output = & $Path selftest 2>&1
    $exitCode = $LASTEXITCODE
    $outputText = [string]::Join("`n", @($output))
    if ($exitCode -ne 0) {
        throw "Packaged MonitorSplitterCtl selftest failed with exit code ${exitCode}: $outputText"
    }

    try {
        $result = $outputText | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "Packaged MonitorSplitterCtl selftest did not return valid JSON: $outputText"
    }

    if ($result.ok -ne $true -or
        $result.selectorMetadataSelftest -ne $true -or
        $result.panelRestoreSelftest -ne $true -or
        $result.hostHealthSelftest -ne $true -or
        $result.staleStackCleanupSelftest -ne $true -or
        $result.atomicWriteSelftest -ne $true -or
        $result.edidMetadataRepairSelftest -ne $true) {
        throw "Packaged MonitorSplitterCtl selftest did not report all expected checks: $outputText"
    }

    Write-Host "Packaged MonitorSplitterCtl selftest passed: baselineName=$($result.baselineName), baselineProductCode=$($result.baselineProductCode), baselineSerial=$($result.baselineSerial)"
}

function Assert-ServiceSelfTest {
    param([Parameter(Mandatory=$true)][string] $Path)

    $global:LASTEXITCODE = 0
    $output = & $Path --selftest 2>&1
    $exitCode = $LASTEXITCODE
    $outputText = [string]::Join("`n", @($output))
    if ($exitCode -ne 0) {
        throw "Packaged MonitorSplitterService selftest failed with exit code ${exitCode}: $outputText"
    }

    try {
        $result = $outputText | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "Packaged MonitorSplitterService selftest did not return valid JSON: $outputText"
    }

    if ($result.ok -ne $true -or
        $result.deferredRecoverySelftest -ne $true -or
        $result.agentExitStopSelftest -ne $true -or
        $result.restartThrottleSelftest -ne $true -or
        $result.startupStabilizationSelftest -ne $true -or
        $result.separateAgentWakeSelftest -ne $true) {
        throw "Packaged MonitorSplitterService selftest did not report all expected checks: $outputText"
    }

    Write-Host "Packaged MonitorSplitterService selftest passed."
}

function Assert-PackageScriptSelfTest {
    param(
        [Parameter(Mandatory=$true)][string] $ScriptDirectory,
        [Parameter(Mandatory=$true)][string] $FileName
    )

    $path = Join-Path $ScriptDirectory $FileName
    $global:LASTEXITCODE = 0
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $path -SelfTest 2>&1
    $exitCode = $LASTEXITCODE
    $outputText = [string]::Join("`n", @($output))
    if ($exitCode -ne 0) {
        throw "Packaged $FileName self-test failed with exit code ${exitCode}: $outputText"
    }

    Write-Host "Packaged $FileName self-test passed."
}

Assert-WdkUserModeToolsetInstalled
$ResolvedBuildTag = Resolve-BuildTag -Version $ProductVersion -ExplicitBuildTag $BuildTag
$buildProperties = @(
    "/p:MonitorSplitterProductVersion=$ProductVersion",
    "/p:MonitorSplitterBuildTag=$ResolvedBuildTag"
)
Write-Host "MonitorSplitter build tag: $ResolvedBuildTag"
Invoke-Checked $MsBuild $DriverProject /p:Configuration=$DriverConfiguration /p:Platform=$Platform @buildProperties /m
Invoke-Checked $MsBuild $CtlProject /p:Configuration=$Configuration /p:Platform=$Platform @buildProperties /m
Invoke-Checked $MsBuild $HostProject /p:Configuration=$Configuration /p:Platform=$Platform @buildProperties /m
Invoke-Checked $MsBuild $ServiceProject /p:Configuration=$Configuration /p:Platform=$Platform @buildProperties /m
Invoke-Checked $MsBuild $ConfigProject /p:Configuration=$Configuration /p:Platform=$Platform @buildProperties /m

Reset-PackageDirectory -Path $PackageDir -OutputRoot $OutputRoot
Reset-PackageDirectory -Path $BinDir -OutputRoot $OutputRoot

Copy-PackageFile (Join-Path $RepoRoot "src\MonitorSplitterDriver\$Platform\$DriverConfiguration\MonitorSplitterDriver.dll") $PackageDir
Copy-PackageFile (Join-Path $RepoRoot "src\MonitorSplitterDriver\MonitorSplitterDriver.inf") $PackageDir
Copy-PackageFile (Join-Path $RepoRoot "src\MonitorSplitterCtl\$Platform\$Configuration\MonitorSplitterCtl.exe") $BinDir
Copy-PackageFile (Join-Path $RepoRoot "src\MonitorSplitterHost\$Platform\$Configuration\MonitorSplitterHost.exe") $BinDir
Copy-PackageFile (Join-Path $RepoRoot "src\MonitorSplitterService\$Platform\$Configuration\MonitorSplitterService.exe") $BinDir
Copy-PackageFile (Join-Path $RepoRoot "src\MonitorSplitterConfig\$Platform\$Configuration\MonitorSplitterConfig.exe") $BinDir

$stampInfArguments = @(
    "-f", (Join-Path $PackageDir "MonitorSplitterDriver.inf"),
    "-a", "amd64",
    "-d", "*",
    "-v", $DriverVersion,
    "-u", $UmdfVersion
)
Invoke-Checked -FilePath $StampInf -Arguments $stampInfArguments
Assert-BinaryHasNoVCRuntimeImports (Join-Path $PackageDir "MonitorSplitterDriver.dll") "Driver package"
Invoke-Checked $InfVerif /v (Join-Path $PackageDir "MonitorSplitterDriver.inf")
Invoke-Checked $Inf2Cat /driver:$PackageDir /os:10_X64

Assert-RequiredFiles -Directory $BinDir -FileNames $RequiredBinFiles
Assert-RequiredFiles -Directory $PackageDir -FileNames $RequiredDriverFiles
Copy-PackageSupportFiles $OutputRoot

if ($Configuration -eq "Release") {
    foreach ($fileName in $RequiredBinFiles) {
        Assert-BinaryHasNoVCRuntimeImports (Join-Path $BinDir $fileName) $fileName -ForbidUcrtApiSet
    }
}

Assert-PackageScriptsParse (Join-Path $OutputRoot "scripts")
Assert-CtlSelfTest (Join-Path $BinDir "MonitorSplitterCtl.exe")
Assert-ServiceSelfTest (Join-Path $BinDir "MonitorSplitterService.exe")
foreach ($fileName in $PackageScriptFiles) {
    Assert-PackageScriptSelfTest (Join-Path $OutputRoot "scripts") $fileName
}

Write-Host "Driver package written to $PackageDir (driver configuration: $DriverConfiguration)"
Write-Host "Tools written to $BinDir (tool configuration: $Configuration)"
