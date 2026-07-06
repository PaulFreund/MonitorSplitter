param(
    [string] $DriverPackageDir = "",
    [switch] $RequireMicrosoft,
    [switch] $Json,
    [switch] $SelfTest
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($DriverPackageDir)) {
    $DriverPackageDir = Join-Path $RepoRoot "out\driver-package"
}

function Test-MicrosoftCertificateText {
    param([string] $Text)
    return ($Text -match "(?i)\bMicrosoft\b")
}

function Test-TestCertificateText {
    param([string] $Text)
    return ($Text -match "(?i)(WDKTestCert|Test Certificate|TestCert|Code Signing Test)")
}

function New-SignatureResult {
    param(
        [Parameter(Mandatory=$true)][string] $Path,
        [Parameter(Mandatory=$true)][bool] $RequireMicrosoftSignature
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]@{
            path = $Path
            ok = $false
            status = "Missing"
            subject = ""
            issuer = ""
            microsoft = $false
            testCertificate = $false
            details = "file is missing"
        }
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    $subject = ""
    $issuer = ""
    if ($null -ne $signature.SignerCertificate) {
        $subject = [string] $signature.SignerCertificate.Subject
        $issuer = [string] $signature.SignerCertificate.Issuer
    }

    $certificateText = "$subject $issuer"
    $isMicrosoft = Test-MicrosoftCertificateText $certificateText
    $isTestCertificate = Test-TestCertificateText $certificateText
    $failures = New-Object System.Collections.Generic.List[string]

    if ($signature.Status -ne "Valid") {
        $failures.Add("signature status is $($signature.Status)") | Out-Null
    }
    if ($RequireMicrosoftSignature -and -not $isMicrosoft) {
        $failures.Add("signer is not Microsoft") | Out-Null
    }
    if ($RequireMicrosoftSignature -and $isTestCertificate) {
        $failures.Add("signer looks like a test certificate") | Out-Null
    }

    return [pscustomobject]@{
        path = $Path
        ok = ($failures.Count -eq 0)
        status = [string] $signature.Status
        subject = $subject
        issuer = $issuer
        microsoft = $isMicrosoft
        testCertificate = $isTestCertificate
        details = $(if ($failures.Count -eq 0) { "signature accepted" } else { $failures -join "; " })
    }
}

function Invoke-SelfTest {
    if (-not (Test-MicrosoftCertificateText "CN=Microsoft Windows Third Party Component CA 2014")) {
        throw "Microsoft certificate detector rejected a Microsoft subject"
    }
    if (Test-MicrosoftCertificateText "CN=Example Publisher") {
        throw "Microsoft certificate detector accepted a non-Microsoft subject"
    }
    if (-not (Test-TestCertificateText "CN=WDKTestCert freun,123")) {
        throw "test certificate detector rejected WDKTestCert"
    }
    if (Test-TestCertificateText "CN=Microsoft Windows Third Party Component CA 2014") {
        throw "test certificate detector accepted Microsoft certificate text"
    }

    Write-Host "Driver signature verification self-test passed."
}

if ($SelfTest) {
    Invoke-SelfTest
    return
}

$DriverPackageDir = [IO.Path]::GetFullPath($DriverPackageDir)
$files = @(
    "MonitorSplitterDriver.dll",
    "monitorsplitterdriver.cat"
)

$results = @($files | ForEach-Object {
    New-SignatureResult -Path (Join-Path $DriverPackageDir $_) -RequireMicrosoftSignature:([bool] $RequireMicrosoft)
})

if ($Json) {
    $results | ConvertTo-Json -Depth 4
}
else {
    foreach ($result in $results) {
        $prefix = if ($result.ok) { "OK  " } else { "FAIL" }
        Write-Host "$prefix $([IO.Path]::GetFileName($result.path)) - $($result.details); status=$($result.status); subject=$($result.subject)"
    }
}

if ($results | Where-Object { -not $_.ok }) {
    exit 1
}
