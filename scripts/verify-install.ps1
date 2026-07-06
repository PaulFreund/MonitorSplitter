param(
    [string] $InstallDir = (Join-Path $env:ProgramFiles "MonitorSplitter"),
    [switch] $RequireHealthyDirectHost,
    [switch] $RequireMicrosoftSignedDriver,
    [switch] $SkipRuntimeStatus,
    [switch] $SelfTest,
    [switch] $Json
)

$ErrorActionPreference = "Stop"

$ServiceName = "MonitorSplitterService"
$TrayRunKey = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Run"
$TrayRunValue = "MonitorSplitterConfig"
$ProgramDataDir = Join-Path $env:ProgramData "MonitorSplitter"
$Checks = New-Object System.Collections.Generic.List[object]
$EdidNameBaseSelectorMetadataPrefix = "msp:edid-name-base="
$script:InstalledDesiredEnabled = $false
$script:ExpectedBuildTag = ""

function Add-Check {
    param(
        [Parameter(Mandatory=$true)][string] $Name,
        [Parameter(Mandatory=$true)][bool] $Ok,
        [string] $Details = ""
    )

    $Checks.Add([pscustomobject]@{
        name = $Name
        ok = $Ok
        details = $Details
    }) | Out-Null
}

function Resolve-FullPath {
    param([Parameter(Mandatory=$true)][string] $Path)
    return [IO.Path]::GetFullPath($Path)
}

function Get-TickAgeMilliseconds {
    param([object] $Tick)

    if ($null -eq $Tick) {
        return $null
    }

    try {
        $mask = [int64] 4294967295
        $then = [uint32](([int64] $Tick) -band $mask)
    }
    catch {
        return $null
    }

    $now = [uint32](([int64] [Environment]::TickCount) -band [int64] 4294967295)
    if ($now -ge $then) {
        return ([uint64] $now) - ([uint64] $then)
    }

    return (([uint64] $now) + [uint64] 4294967296) - ([uint64] $then)
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

function Test-RequiredFile {
    param([Parameter(Mandatory=$true)][string] $Path)

    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    Add-Check "file:$Path" $exists ($(if ($exists) { "present" } else { "missing" }))
}

function Test-MicrosoftCertificateText {
    param([string] $Text)
    return ($Text -match "(?i)\bMicrosoft\b")
}

function Test-TestCertificateText {
    param([string] $Text)
    return ($Text -match "(?i)(WDKTestCert|Test Certificate|TestCert|Code Signing Test)")
}

function Test-DriverSignatureFile {
    param(
        [Parameter(Mandatory=$true)][string] $Path,
        [Parameter(Mandatory=$true)][string] $Name
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Check "driver-signature:$Name" $false "file is missing: $Path"
        return
    }

    try {
        $signature = Get-AuthenticodeSignature -LiteralPath $Path
    }
    catch {
        Add-Check "driver-signature:$Name" $false "signature check failed: $($_.Exception.Message)"
        return
    }

    $subject = ""
    $issuer = ""
    if ($null -ne $signature.SignerCertificate) {
        $subject = [string] $signature.SignerCertificate.Subject
        $issuer = [string] $signature.SignerCertificate.Issuer
    }

    $certificateText = "$subject $issuer"
    $failures = @()
    if ($signature.Status -ne "Valid") {
        $failures += "signature status is $($signature.Status)"
    }
    if (-not (Test-MicrosoftCertificateText $certificateText)) {
        $failures += "signer is not Microsoft"
    }
    if (Test-TestCertificateText $certificateText) {
        $failures += "signer looks like a test certificate"
    }

    Add-Check "driver-signature:$Name" ($failures.Count -eq 0) $(if ($failures.Count -eq 0) { "Microsoft signature accepted; subject=$subject" } else { "$($failures -join '; '); subject=$subject; issuer=$issuer" })
}

function Test-DriverPackageSignature {
    param([Parameter(Mandatory=$true)][string] $Path)

    Test-DriverSignatureFile -Path (Join-Path $Path "MonitorSplitterDriver.dll") -Name "MonitorSplitterDriver.dll"
    Test-DriverSignatureFile -Path (Join-Path $Path "monitorsplitterdriver.cat") -Name "monitorsplitterdriver.cat"
}

function Test-CtlSelfTest {
    param([Parameter(Mandatory=$true)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Check "ctl-selftest" $false "MonitorSplitterCtl.exe is missing: $Path"
        return
    }

    try {
        $global:LASTEXITCODE = 0
        $output = & $Path selftest 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        Add-Check "ctl-selftest" $false "selftest could not be started: $($_.Exception.Message)"
        return
    }

    $outputText = [string]::Join("`n", @($output))
    if ($exitCode -ne 0) {
        Add-Check "ctl-selftest" $false "selftest exit code ${exitCode}: $outputText"
        return
    }

    try {
        $result = $outputText | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        Add-Check "ctl-selftest" $false "selftest output was not valid JSON: $outputText"
        return
    }

    $ok =
        $result.ok -eq $true -and
        $result.selectorMetadataSelftest -eq $true -and
        $result.panelRestoreSelftest -eq $true -and
        $result.hostHealthSelftest -eq $true -and
        $result.staleStackCleanupSelftest -eq $true -and
        $result.atomicWriteSelftest -eq $true -and
        $result.edidMetadataRepairSelftest -eq $true
    $details = if ($ok) {
        "baselineName=$($result.baselineName); baselineProductCode=$($result.baselineProductCode); baselineSerial=$($result.baselineSerial); selector/panel-restore/host-health/stale-stack-cleanup/atomic-write/edid-metadata-repair selftests passed"
    }
    else {
        $outputText
    }
    Add-Check "ctl-selftest" $ok $details
}

function Test-VersionCommand {
    param([Parameter(Mandatory=$true)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Check "version-command" $false "MonitorSplitterCtl.exe is missing: $Path"
        return
    }

    try {
        $global:LASTEXITCODE = 0
        $output = & $Path version 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        Add-Check "version-command" $false "version command could not be started: $($_.Exception.Message)"
        return
    }

    $outputText = [string]::Join("`n", @($output))
    if ($exitCode -ne 0) {
        Add-Check "version-command" $false "version command exit code ${exitCode}: $outputText"
        return
    }

    try {
        $result = $outputText | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        Add-Check "version-command" $false "version output was not valid JSON: $outputText"
        return
    }

    $buildTag = [string] $result.component.buildTag
    $ok = -not [string]::IsNullOrWhiteSpace($buildTag)
    if ($ok) {
        $script:ExpectedBuildTag = $buildTag
    }
    Add-Check "version-command" $ok $(
        if ($ok) { "expectedBuildTag=$buildTag" } else { "version output did not include component.buildTag: $outputText" }
    )
}

function Test-ServiceSelfTest {
    param([Parameter(Mandatory=$true)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Check "service-selftest" $false "MonitorSplitterService.exe is missing: $Path"
        return
    }

    try {
        $global:LASTEXITCODE = 0
        $output = & $Path --selftest 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        Add-Check "service-selftest" $false "selftest could not be started: $($_.Exception.Message)"
        return
    }

    $outputText = [string]::Join("`n", @($output))
    if ($exitCode -ne 0) {
        Add-Check "service-selftest" $false "selftest exit code ${exitCode}: $outputText"
        return
    }

    try {
        $result = $outputText | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        Add-Check "service-selftest" $false "selftest output was not valid JSON: $outputText"
        return
    }

    $ok =
        $result.ok -eq $true -and
        $result.deferredRecoverySelftest -eq $true -and
        $result.agentExitStopSelftest -eq $true -and
        $result.restartThrottleSelftest -eq $true -and
        $result.startupStabilizationSelftest -eq $true -and
        $result.separateAgentWakeSelftest -eq $true
    $details = if ($ok) {
        "deferred recovery scheduler, agent exit stop, restart throttle, startup stabilization, and separated wake-event selftests passed"
    }
    else {
        $outputText
    }
    Add-Check "service-selftest" $ok $details
}

function Test-ScriptSelfTest {
    param(
        [Parameter(Mandatory=$true)][string] $Path,
        [Parameter(Mandatory=$true)][string] $Name
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Check "script-selftest-$Name" $false "script is missing: $Path"
        return
    }

    try {
        $global:LASTEXITCODE = 0
        $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $Path -SelfTest 2>&1
        $scriptSucceeded = $?
        $exitCode = $LASTEXITCODE
    }
    catch {
        Add-Check "script-selftest-$Name" $false "selftest could not be started: $($_.Exception.Message)"
        return
    }

    $outputText = [string]::Join("`n", @($output))
    $ok = $scriptSucceeded -and $exitCode -eq 0
    Add-Check "script-selftest-$Name" $ok $(
        if ($ok) { "selftest passed" } else { "selftest exit code ${exitCode}: $outputText" }
    )
}

function Get-ServiceExecutablePath {
    param([Parameter(Mandatory=$true)][string] $ImagePath)

    $trimmed = $ImagePath.Trim()
    if ($trimmed.StartsWith('"')) {
        $end = $trimmed.IndexOf('"', 1)
        if ($end -gt 1) {
            return $trimmed.Substring(1, $end - 1)
        }
    }

    return ($trimmed -split "\s+", 2)[0]
}

function Read-UInt32Le {
    param(
        [Parameter(Mandatory=$true)] $Bytes,
        [Parameter(Mandatory=$true)][int] $Offset
    )

    if ($Bytes -is [byte[]]) {
        $buffer = $Bytes
    }
    else {
        $buffer = [byte[]] $Bytes
    }
    if ($Offset -lt 0 -or ($Offset + 4) -gt $buffer.Length) {
        throw "Cannot read UInt32 at offset $Offset from $($buffer.Length) byte buffer"
    }

    $value =
        ([uint64] $buffer[$Offset]) +
        (([uint64] $buffer[$Offset + 1]) * 256) +
        (([uint64] $buffer[$Offset + 2]) * 65536) +
        (([uint64] $buffer[$Offset + 3]) * 16777216)
    return [uint32] $value
}

function Get-FailureActionPolicyCandidate {
    param(
        [Parameter(Mandatory=$true)][byte[]] $Bytes,
        [Parameter(Mandatory=$true)][int] $ActionCountOffset,
        [Parameter(Mandatory=$true)][int] $ActionsOffset
    )

    if ($ActionCountOffset + 4 -gt $Bytes.Length) {
        return $null
    }

    $actionCount = Read-UInt32Le -Bytes $Bytes -Offset $ActionCountOffset
    if ($actionCount -eq 0 -or $actionCount -gt 16) {
        return $null
    }

    $requiredLength = $ActionsOffset + ([int] $actionCount * 8)
    if ($Bytes.Length -lt $requiredLength) {
        return $null
    }

    $actions = @()
    for ($index = 0; $index -lt $actionCount; $index++) {
        $offset = $ActionsOffset + ($index * 8)
        $type = Read-UInt32Le -Bytes $Bytes -Offset $offset
        $delay = Read-UInt32Le -Bytes $Bytes -Offset ($offset + 4)
        if ($type -gt 3 -or $delay -gt 86400000) {
            return $null
        }
        $actions += New-Object PSObject -Property @{
            type = [int64] $type
            delayMs = [int64] $delay
        }
    }

    return @($actions)
}

function Get-ServiceFailureActionPolicy {
    param([Parameter(Mandatory=$true)] $FailureActions)

    if ($FailureActions -is [byte[]]) {
        $bytes = $FailureActions
    }
    else {
        $bytes = [byte[]] $FailureActions
    }
    if ($bytes.Length -lt 16) {
        throw "FailureActions is too short: $($bytes.Length) bytes"
    }

    $resetPeriod = Read-UInt32Le -Bytes $bytes -Offset 0
    $actions = $null
    foreach ($candidate in @(
        @{ Count = 12; Actions = 16 },
        @{ Count = 12; Actions = 20 },
        @{ Count = 24; Actions = 40 }
    )) {
        $actions = Get-FailureActionPolicyCandidate `
            -Bytes $bytes `
            -ActionCountOffset $candidate.Count `
            -ActionsOffset $candidate.Actions
        if ($null -ne $actions) {
            break
        }
    }

    if ($null -eq $actions) {
        foreach ($countOffset in @(12, 24)) {
            if ($countOffset + 4 -le $bytes.Length) {
                $possibleCount = Read-UInt32Le -Bytes $bytes -Offset $countOffset
                if ($possibleCount -gt 0 -and $possibleCount -le 16) {
                    throw "FailureActions has $possibleCount action(s), but no supported complete action array layout"
                }
            }
        }
        $actions = @()
    }

    return New-Object PSObject -Property @{
        resetPeriodSeconds = [int64] $resetPeriod
        actions = @($actions)
    }
}

function Assert-SelfTest {
    param(
        [bool] $Condition,
        [Parameter(Mandatory=$true)][string] $Message
    )

    if (-not $Condition) {
        throw "Verify-install self-test failed: $Message"
    }
}

function Should-TestHealthyDirectHost {
    param(
        [bool] $Force,
        [bool] $DesiredEnabled
    )

    return $Force -or $DesiredEnabled
}

function Get-DriverStoreCheckResult {
    param([object[]] $Packages)

    $packageCount = @($Packages).Count
    if ($packageCount -eq 0) {
        return [pscustomobject]@{
            ok = $false
            details = "MonitorSplitter driver package not found"
        }
    }

    $identifiers = @(
        $Packages |
            ForEach-Object {
                if (-not [string]::IsNullOrWhiteSpace([string] $_.PublishedName)) {
                    [string] $_.PublishedName
                }
                elseif (-not [string]::IsNullOrWhiteSpace([string] $_.OriginalName)) {
                    "OriginalName=$($_.OriginalName)"
                }
                elseif (-not [string]::IsNullOrWhiteSpace([string] $_.ProviderName)) {
                    "ProviderName=$($_.ProviderName)"
                }
            } |
            Where-Object { -not [string]::IsNullOrWhiteSpace([string] $_) }
    )

    if ($identifiers.Count -eq 0) {
        return [pscustomobject]@{
            ok = $false
            details = "MonitorSplitter driver package object(s) found, but none had a usable identifier"
        }
    }

    return [pscustomobject]@{
        ok = $true
        details = "MonitorSplitter driver package(s): " + ($identifiers -join ", ")
    }
}

function Invoke-VerifyInstallSelfTest {
    $policyBytes = [byte[]] @(
        0x80, 0x51, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x88, 0x13, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x30, 0x75, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x60, 0xea, 0x00, 0x00
    )
    $policy = Get-ServiceFailureActionPolicy -FailureActions $policyBytes
    Assert-SelfTest ($policy.resetPeriodSeconds -eq 86400) "reset period was not decoded"
    Assert-SelfTest ($policy.actions.Count -eq 3) "action count was not decoded"
    Assert-SelfTest ([int] $policy.actions[0].type -eq 1 -and [int] $policy.actions[0].delayMs -eq 5000) "first action was not decoded"
    Assert-SelfTest ([int] $policy.actions[1].type -eq 1 -and [int] $policy.actions[1].delayMs -eq 30000) "second action was not decoded"
    Assert-SelfTest ([int] $policy.actions[2].type -eq 1 -and [int] $policy.actions[2].delayMs -eq 60000) "third action was not decoded"

    $policyBytes64 = [byte[]] @(
        0x80, 0x51, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x88, 0x13, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x30, 0x75, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x60, 0xea, 0x00, 0x00
    )
    $policy64 = Get-ServiceFailureActionPolicy -FailureActions $policyBytes64
    Assert-SelfTest ($policy64.actions.Count -eq 3) "64-bit failure action blob was not decoded"

    $shortRejected = $false
    try {
        $null = Get-ServiceFailureActionPolicy -FailureActions ([byte[]] @(0x01, 0x02, 0x03))
    }
    catch {
        $shortRejected = $true
    }
    Assert-SelfTest $shortRejected "short policy blob was not rejected"

    $truncated = [byte[]] @(
        0x80, 0x51, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x88, 0x13, 0x00, 0x00
    )
    $truncatedRejected = $false
    try {
        $null = Get-ServiceFailureActionPolicy -FailureActions $truncated
    }
    catch {
        $truncatedRejected = $true
    }
    Assert-SelfTest $truncatedRejected "truncated policy blob was not rejected"

    Assert-SelfTest (Should-TestHealthyDirectHost -Force:$true -DesiredEnabled:$false) "forced live health check was not requested"
    Assert-SelfTest (Should-TestHealthyDirectHost -Force:$false -DesiredEnabled:$true) "enabled desired state did not request live health check"
    Assert-SelfTest (-not (Should-TestHealthyDirectHost -Force:$false -DesiredEnabled:$false)) "disabled desired state unexpectedly requested live health check"

    $emptyDriverStore = Get-DriverStoreCheckResult @()
    Assert-SelfTest (-not $emptyDriverStore.ok) "empty driver store check unexpectedly passed"

    $blankDriverStore = Get-DriverStoreCheckResult @([pscustomobject]@{ PublishedName = ""; OriginalName = "MonitorSplitterDriver.inf"; ProviderName = "MonitorSplitter" })
    Assert-SelfTest ($blankDriverStore.ok -and $blankDriverStore.details -like "*OriginalName=MonitorSplitterDriver.inf*") "driver store check did not accept an identified package without a published name"

    $unidentifiedDriverStore = Get-DriverStoreCheckResult @([pscustomobject]@{ PublishedName = ""; OriginalName = ""; ProviderName = "" })
    Assert-SelfTest (-not $unidentifiedDriverStore.ok) "driver store check passed without any package identifier"

    $validDriverStore = Get-DriverStoreCheckResult @([pscustomobject]@{ PublishedName = "oem123.inf"; ProviderName = "MonitorSplitter" })
    Assert-SelfTest ($validDriverStore.ok -and $validDriverStore.details -like "*oem123.inf*") "valid driver store package was not accepted"

    $localized = ConvertFrom-PnPUtilDriverList @(
        ("Ver" + [char]0x00f6 + "ffentlichter Name:     oem99.inf"),
        "Originalname:      MonitorSplitterDriver.inf",
        "Anbietername:      MonitorSplitter"
    )
    Assert-SelfTest (@($localized).Count -eq 1) "localized driver package was not detected"
    Assert-SelfTest ($localized[0].PublishedName -eq "oem99.inf") "localized published name was not canonicalized"
    Assert-SelfTest (Test-MicrosoftCertificateText "CN=Microsoft Windows Third Party Component CA 2014") "Microsoft signature text was not recognized"
    Assert-SelfTest (-not (Test-MicrosoftCertificateText "CN=Example Publisher")) "non-Microsoft signature text was accepted"
    Assert-SelfTest (Test-TestCertificateText "CN=WDKTestCert freun,123") "test certificate text was not recognized"
    Assert-SelfTest (-not (Test-TestCertificateText "CN=Microsoft Windows Third Party Component CA 2014")) "Microsoft signature text was treated as a test certificate"

    Write-Host "Verify-install self-test passed."
}

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

    throw "pnputil.exe was not found."
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

function Test-MonitorSplitterDriverPackage {
    param([Parameter(Mandatory=$true)] $Package)

    return $Package.OriginalName -like "*MonitorSplitterDriver.inf*" -or
        $Package.ProviderName -like "*MonitorSplitter*" -or
        $Package.PublishedName -like "*MonitorSplitter*"
}

function ConvertFrom-PnPUtilDriverList {
    param([Parameter(Mandatory=$true)][AllowEmptyString()][string[]] $DriverList)

    $packages = @()
    $current = [ordered]@{}

    foreach ($line in $driverList) {
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

    return @($packages)
}

function Get-MonitorSplitterDriverPackages {
    $pnputil = Resolve-PnPUtil
    $driverList = & $pnputil /enum-drivers 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "$pnputil /enum-drivers failed: $($driverList -join ' ')"
    }

    return ConvertFrom-PnPUtilDriverList -DriverList @($driverList)
}

function Test-DriverPackage {
    $packages = @()
    try {
        $packages = @(Get-MonitorSplitterDriverPackages)
    }
    catch {
        Add-Check "driver-store" $false $_.Exception.Message
        return
    }

    $result = Get-DriverStoreCheckResult $packages
    Add-Check "driver-store" ([bool] $result.ok) $result.details
}

function Test-ServiceInstall {
    $service = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'" -ErrorAction SilentlyContinue
    if ($null -eq $service) {
        Add-Check "service" $false "$ServiceName is not installed"
        return
    }

    Add-Check "service" $true "$ServiceName is installed"
    Add-Check "service-start-mode" ($service.StartMode -eq "Auto") "StartMode=$($service.StartMode)"
    Add-Check "service-state" ($service.State -eq "Running") "State=$($service.State)"

    $expectedServiceExe = Resolve-FullPath (Join-Path $InstallDir "bin\MonitorSplitterService.exe")
    $actualServiceExe = Resolve-FullPath (Get-ServiceExecutablePath $service.PathName)
    Add-Check "service-image-path" ($actualServiceExe -ieq $expectedServiceExe) "ImagePath=$actualServiceExe"

    $serviceRegistryPath = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    try {
        $serviceRegistry = Get-ItemProperty -LiteralPath $serviceRegistryPath -ErrorAction Stop
        $failureActions = $serviceRegistry.FailureActions
        if ($null -eq $failureActions) {
            Add-Check "service-failure-actions" $false "FailureActions registry value is missing"
        }
        else {
            $policy = Get-ServiceFailureActionPolicy -FailureActions $failureActions
            $expectedDelays = @(5000, 30000, 60000)
            $ok =
                $policy.resetPeriodSeconds -eq 86400 -and
                $policy.actions.Count -eq $expectedDelays.Count
            if ($ok) {
                for ($index = 0; $index -lt $expectedDelays.Count; $index++) {
                    if ([int] $policy.actions[$index].type -ne 1 -or
                        [int] $policy.actions[$index].delayMs -ne $expectedDelays[$index]) {
                        $ok = $false
                        break
                    }
                }
            }

            $actionDetails = @($policy.actions | ForEach-Object { "type=$($_.type)/delayMs=$($_.delayMs)" }) -join ", "
            Add-Check "service-failure-actions" $ok "reset=$($policy.resetPeriodSeconds); actions=[$actionDetails]"
        }

        $failureFlag = 0
        if ($null -ne $serviceRegistry.FailureActionsOnNonCrashFailures) {
            $failureFlag = [int] $serviceRegistry.FailureActionsOnNonCrashFailures
        }
        Add-Check "service-failure-flag" ($failureFlag -eq 1) "FailureActionsOnNonCrashFailures=$failureFlag"
    }
    catch {
        Add-Check "service-failure-policy" $false $_.Exception.Message
    }
}

function Test-TrayStartup {
    $expectedConfigExe = Resolve-FullPath (Join-Path $InstallDir "bin\MonitorSplitterConfig.exe")
    try {
        $value = (Get-ItemProperty -Path $TrayRunKey -Name $TrayRunValue -ErrorAction Stop).$TrayRunValue
    }
    catch {
        Add-Check "tray-startup" $false "HKLM Run value $TrayRunValue is missing"
        return
    }

    $ok = $value -like "*$expectedConfigExe*" -and $value -like "*--tray*"
    Add-Check "tray-startup" $ok $value
}

function Test-ProgramData {
    $exists = Test-Path -LiteralPath $ProgramDataDir -PathType Container
    Add-Check "program-data" $exists $ProgramDataDir
    if ($exists) {
        try {
            $usersSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-32-545")
            $acl = Get-Acl -LiteralPath $ProgramDataDir
            $hasUsersModify = $false
            foreach ($rule in $acl.Access) {
                $ruleSid = $null
                try {
                    $ruleSid = $rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier])
                }
                catch {
                    continue
                }

                $hasModify =
                    ($rule.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Modify) -eq
                    [Security.AccessControl.FileSystemRights]::Modify
                if ($ruleSid.Value -eq $usersSid.Value -and
                    $rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
                    $hasModify) {
                    $hasUsersModify = $true
                    break
                }
            }

            Add-Check "program-data-acl" $hasUsersModify "BUILTIN\Users modify access"
        }
        catch {
            Add-Check "program-data-acl" $false $_.Exception.Message
        }
    }

    $desiredPath = Join-Path $ProgramDataDir "service-enabled.txt"
    $desiredExists = Test-Path -LiteralPath $desiredPath -PathType Leaf
    Add-Check "desired-state-file" $desiredExists $desiredPath

    $desiredEnabled = $false
    if ($desiredExists) {
        $desiredValue = ([string] (Get-Content -LiteralPath $desiredPath -Raw -ErrorAction SilentlyContinue)).Trim().ToLowerInvariant()
        $desiredEnabled = $desiredValue -in @("1", "true", "enabled", "enable", "on")
    }
    $script:InstalledDesiredEnabled = $desiredEnabled

    $layoutPath = Join-Path $ProgramDataDir "layout.txt"
    $layoutValue = ""
    if (Test-Path -LiteralPath $layoutPath -PathType Leaf) {
        $layoutValue = ([string] (Get-Content -LiteralPath $layoutPath -Raw -ErrorAction SilentlyContinue)).Trim()
    }
    Add-Check "layout-config" ((-not $desiredEnabled) -or -not [string]::IsNullOrWhiteSpace($layoutValue)) $(
        if ([string]::IsNullOrWhiteSpace($layoutValue)) { "layout not configured" } else { $layoutValue }
    )

    $hostTargetPath = Join-Path $ProgramDataDir "host-target.txt"
    $hostTargetValue = ""
    if (Test-Path -LiteralPath $hostTargetPath -PathType Leaf) {
        $hostTargetValue = ([string] (Get-Content -LiteralPath $hostTargetPath -Raw -ErrorAction SilentlyContinue)).Trim()
    }
    $hostTargetConfigured = -not [string]::IsNullOrWhiteSpace($hostTargetValue)
    Add-Check "host-target-config" ((-not $desiredEnabled) -or $hostTargetConfigured) $(
        if ($hostTargetConfigured) { $hostTargetPath } else { "host target not configured" }
    )

    $edidNameBasePath = Join-Path $ProgramDataDir "edid-name-base.txt"
    $edidNameBaseValue = ""
    if (Test-Path -LiteralPath $edidNameBasePath -PathType Leaf) {
        $edidNameBaseValue = ([string] (Get-Content -LiteralPath $edidNameBasePath -Raw -ErrorAction SilentlyContinue)).Trim()
    }
    $edidNameBaseMetadata = Get-SelectorEdidNameBaseMetadata $hostTargetValue
    $edidNameBaseConfigured =
        -not [string]::IsNullOrWhiteSpace($edidNameBaseValue) -or
        -not [string]::IsNullOrWhiteSpace($edidNameBaseMetadata)
    Add-Check "edid-name-base" ((-not $hostTargetConfigured -and -not $desiredEnabled) -or $edidNameBaseConfigured) $(
        if (-not [string]::IsNullOrWhiteSpace($edidNameBaseValue)) {
            $edidNameBaseValue
        }
        elseif (-not [string]::IsNullOrWhiteSpace($edidNameBaseMetadata)) {
            "host-target metadata: $edidNameBaseMetadata"
        }
        else {
            "EDID name base not configured"
        }
    )
}

function Test-HealthyDirectHost {
    $statusPath = Join-Path $ProgramDataDir "host-status.json"
    if (-not (Test-Path -LiteralPath $statusPath -PathType Leaf)) {
        Add-Check "direct-host-health" $false "host-status.json is missing"
        return
    }

    try {
        $status = Get-Content -LiteralPath $statusPath -Raw | ConvertFrom-Json
    }
    catch {
        Add-Check "direct-host-health" $false "host-status.json is not valid JSON: $($_.Exception.Message)"
        return
    }

    $failures = @()
    $expectedHostExe = Resolve-FullPath (Join-Path $InstallDir "bin\MonitorSplitterHost.exe")
    $hostPid = [uint64] $status.pid
    $hostProcess = $null
    if ($hostPid -gt 0 -and $hostPid -le [uint32]::MaxValue) {
        $hostProcess = Get-Process -Id ([int] $hostPid) -ErrorAction SilentlyContinue
    }
    else {
        $failures += "missing or invalid pid"
    }

    $hostProcessPath = ""
    if ($null -eq $hostProcess) {
        $failures += "host pid is not running"
    }
    else {
        if ($hostProcess.ProcessName -ne "MonitorSplitterHost") {
            $failures += "pid belongs to $($hostProcess.ProcessName)"
        }
        try {
            $hostProcessPath = Resolve-FullPath $hostProcess.Path
            if ($hostProcessPath -ine $expectedHostExe) {
                $failures += "host path is $hostProcessPath"
            }
        }
        catch {
            $failures += "could not read host process path: $($_.Exception.Message)"
        }
    }

    $updatedAge = Get-TickAgeMilliseconds $status.updatedTick
    $submitAge = Get-TickAgeMilliseconds $status.direct.lastSubmitTick
    if ($null -eq $updatedAge -or $updatedAge -gt 15000) {
        $failures += "status is stale"
    }
    if ($null -eq $submitAge -or $submitAge -gt 5000) {
        $failures += "display task submit is stale"
    }

    if ($status.running -ne $true) {
        $failures += "running is false"
    }
    if ($status.mode -ne "direct-shared") {
        $failures += "mode is $($status.mode)"
    }
    if ($status.usingSharedFrames -ne $true) {
        $failures += "shared frames are not active"
    }
    if ($status.direct.targetAcquired -ne $true -or
        $status.direct.deviceCreated -ne $true -or
        $status.direct.sourceCreated -ne $true -or
        $status.direct.taskPoolCreated -ne $true -or
        $status.direct.fenceReady -ne $true) {
        $failures += "direct scanout resources are not all ready"
    }
    if ($status.lastPresentResult -ne "0x00000000" -or $status.lastDisplayTaskResult -ne "0x00000000") {
        $failures += "last present/display task result is not success"
    }
    if ([uint64] $status.expectedSourceCount -eq 0 -or [uint64] $status.sourceCount -lt [uint64] $status.expectedSourceCount) {
        $failures += "not all expected split sources are mapped"
    }
    if ([uint64] $status.healthyFrameSourceCount -lt [uint64] $status.expectedSourceCount) {
        $failures += "not all split frame sources are healthy"
    }
    if ([uint64] $status.publishingFrameSourceCount -lt [uint64] $status.expectedSourceCount) {
        $failures += "not all split frame sources are publishing"
    }
    if ([uint64] $status.direct.displayTaskSubmitAttempts -eq 0 -or [uint64] $status.direct.displayTaskSuccesses -eq 0) {
        $failures += "no successful display task has completed"
    }
    if ([uint64] $status.direct.displayTaskFailures -ne 0) {
        $failures += "display task failures were reported"
    }
    if ([int64] $status.lastDisplayTaskPresentStatus -notin @(0, -1) -or
        [int64] $status.lastDisplayTaskSourceStatus -notin @(0, -1)) {
        $failures += "display task status is not successful"
    }
    if ([uint64] $status.presentedFrames -eq 0) {
        $failures += "no frames have been presented"
    }
    if ($status.mode -in @("starting", "recovering") -and
        $status.direct.sourceCreated -ne $true -and
        [string] $status.direct.lastScanoutCreateResult -eq "0x887a0025" -and
        ([uint64] $status.direct.scanoutCreateAttempts -ge 20 -or
            [uint64] $status.direct.directSetupRetryAttempts -ge 20)) {
        $failures += "direct host is stuck waiting for DisplayCore mode change completion"
    }
    if (-not [string]::IsNullOrWhiteSpace($script:ExpectedBuildTag)) {
        $hostBuildTag = [string] $status.component.buildTag
        if ($hostBuildTag -ne $script:ExpectedBuildTag) {
            $failures += "host build tag is '$hostBuildTag', expected '$script:ExpectedBuildTag'"
        }

        foreach ($frameSource in @($status.frameSources)) {
            if ($frameSource.kind -ne "shared") {
                continue
            }
            if ($frameSource.driverRuntimeMapped -ne $true -or $frameSource.driverRuntimeValid -ne $true) {
                $failures += "driver runtime status for split $($frameSource.index) is missing or invalid"
                continue
            }
            $driverBuildTag = [string] $frameSource.driverBuildTag
            if ($driverBuildTag -ne $script:ExpectedBuildTag) {
                $failures += "driver build tag for split $($frameSource.index) is '$driverBuildTag', expected '$script:ExpectedBuildTag'"
            }
        }
    }

    $ok = $failures.Count -eq 0
    $details = if ($ok) {
        "pid=$hostPid; path=$hostProcessPath; mode=$($status.mode); frameSources=$($status.healthyFrameSourceCount)/$($status.expectedSourceCount) healthy; publishing=$($status.publishingFrameSourceCount)/$($status.expectedSourceCount); presentedFrames=$($status.presentedFrames); displayTaskSuccesses=$($status.direct.displayTaskSuccesses); submitAgeMs=$submitAge"
    }
    else {
        $failures -join "; "
    }
    Add-Check "direct-host-health" $ok $details
}

function Read-JsonFileOrNull {
    param([Parameter(Mandatory=$true)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }

    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        Add-Check "runtime-json:$Path" $false "invalid JSON: $($_.Exception.Message)"
        return $null
    }
}

function Test-RuntimeStatusBuildTag {
    param(
        [Parameter(Mandatory=$true)][string] $Name,
        [Parameter(Mandatory=$true)][string] $Path,
        [switch] $Required
    )

    $status = Read-JsonFileOrNull $Path
    if ($null -eq $status) {
        Add-Check "$Name-runtime-version" (-not $Required) $(
            if ($Required) { "$Path is missing" } else { "not running or no status file" }
        )
        return
    }

    $buildTag = [string] $status.component.buildTag
    $age = Get-TickAgeMilliseconds $status.updatedTick
    $failures = @()
    if ([string]::IsNullOrWhiteSpace($buildTag)) {
        $failures += "missing build tag"
    }
    elseif (-not [string]::IsNullOrWhiteSpace($script:ExpectedBuildTag) -and $buildTag -ne $script:ExpectedBuildTag) {
        $failures += "buildTag=$buildTag expected=$script:ExpectedBuildTag"
    }
    if ($status.running -eq $true -and ($null -eq $age -or $age -gt 30000)) {
        $failures += "status stale ageMs=$age"
    }

    Add-Check "$Name-runtime-version" ($failures.Count -eq 0) $(
        if ($failures.Count -eq 0) { "buildTag=$buildTag; running=$($status.running); ageMs=$age" } else { $failures -join "; " }
    )
}

if ($SelfTest) {
    Invoke-VerifyInstallSelfTest
    return
}

$installBin = Join-Path $InstallDir "bin"
$installDriver = Join-Path $InstallDir "driver-package"
$installScripts = Join-Path $InstallDir "scripts"

foreach ($fileName in @("MonitorSplitterCtl.exe", "MonitorSplitterHost.exe", "MonitorSplitterService.exe", "MonitorSplitterConfig.exe")) {
    Test-RequiredFile (Join-Path $installBin $fileName)
}

$ctlPath = Join-Path $installBin "MonitorSplitterCtl.exe"
Test-CtlSelfTest $ctlPath
Test-VersionCommand $ctlPath
Test-ServiceSelfTest (Join-Path $installBin "MonitorSplitterService.exe")

foreach ($fileName in @("MonitorSplitterDriver.inf", "MonitorSplitterDriver.dll", "monitorsplitterdriver.cat")) {
    Test-RequiredFile (Join-Path $installDriver $fileName)
}

if ($RequireMicrosoftSignedDriver) {
    Test-DriverPackageSignature $installDriver
}

foreach ($fileName in @("verify-install.ps1", "install-driver.ps1", "install-service.ps1", "msi-custom-action.ps1")) {
    Test-RequiredFile (Join-Path $installScripts $fileName)
}

foreach ($fileName in @("install-driver.ps1", "install-service.ps1", "msi-custom-action.ps1", "verify-install.ps1")) {
    Test-ScriptSelfTest -Path (Join-Path $installScripts $fileName) -Name ([IO.Path]::GetFileNameWithoutExtension($fileName))
}

Test-ServiceInstall
Test-TrayStartup
Test-ProgramData
Test-DriverPackage
if (-not $SkipRuntimeStatus) {
    Test-RuntimeStatusBuildTag -Name "service" -Path (Join-Path $ProgramDataDir "service-status.json") -Required
    Test-RuntimeStatusBuildTag -Name "agent" -Path (Join-Path $ProgramDataDir "agent-status.json")
    Test-RuntimeStatusBuildTag -Name "config" -Path (Join-Path $ProgramDataDir "config-status.json")
}
else {
    Add-Check "runtime-status" $true "skipped"
}

if ((-not $SkipRuntimeStatus) -and (Should-TestHealthyDirectHost -Force:([bool] $RequireHealthyDirectHost) -DesiredEnabled:$script:InstalledDesiredEnabled)) {
    Test-HealthyDirectHost
}

if ($Json) {
    $Checks | ConvertTo-Json -Depth 5
}
else {
    foreach ($check in $Checks) {
        $prefix = if ($check.ok) { "OK  " } else { "FAIL" }
        Write-Host "$prefix $($check.name) - $($check.details)"
    }
}

if ($Checks | Where-Object { -not $_.ok }) {
    exit 1
}
