param(
    [string] $CtlExe = (Join-Path $env:ProgramFiles "MonitorSplitter\bin\MonitorSplitterCtl.exe"),
    [string] $ConfigDir = (Join-Path $env:ProgramData "MonitorSplitter")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $CtlExe -PathType Leaf)) {
    throw "MonitorSplitterCtl.exe is missing: $CtlExe"
}

$panelStateJson = & $CtlExe panelstate
if ($LASTEXITCODE -ne 0) {
    throw "panelstate failed with exit code $LASTEXITCODE"
}

$panelState = $panelStateJson | ConvertFrom-Json
if ($panelState.ok -ne $true -or $null -eq $panelState.target) {
    throw "panelstate did not return a resolved DisplayCore target: $panelStateJson"
}

$target = $panelState.target
$selectors = @(
    [string] $target.devicePath,
    [string] $target.stableMonitorId,
    ("adapter:{0}:{1}:{2}" -f $target.adapterLow, $target.adapterHigh, $target.targetId)
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

if ($selectors.Count -eq 0) {
    throw "panelstate target did not contain any usable selectors: $panelStateJson"
}

New-Item -ItemType Directory -Force -Path $ConfigDir | Out-Null
$directTargetPath = Join-Path $ConfigDir "direct-target.txt"
Set-Content -LiteralPath $directTargetPath -Encoding ASCII -Value $selectors

Write-Host "Repaired direct target:"
$selectors | ForEach-Object { Write-Host $_ }
