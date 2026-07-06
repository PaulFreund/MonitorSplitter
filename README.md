# MonitorSplitter [![Implemented with Codex](https://img.shields.io/badge/Implemented%20with-Codex-6A5ACD?logo=openai&logoColor=white)](https://github.com/openai/codex)

> [!WARNING]
> MonitorSplitter is alpha software that changes Windows display topology and direct scanout state. If you test it on a system with only one physical monitor, a failure can leave you without a visible image until you recover through reboot, Safe Mode, remote access, or another display. Keep a reliable recovery path available before enabling it.

MonitorSplitter exposes one physical ultrawide as multiple Windows desktop monitors.

The working path is:

1. The UMDF/IddCx driver exposes configurable virtual monitors.
2. The shared `MonitorSplitterControl` stack creates or removes the software display device and starts the direct scanout host.
3. `MonitorSplitterHost.exe` acquires the physical panel after Windows has removed it from the desktop, then scans the virtual monitor frames back to that panel through DisplayCore.
4. `MonitorSplitterService.exe` runs as an automatic Windows service. It uses the shared stack directly and keeps the working path alive across logons, user switches, host crashes, and power resume. After the desired state becomes active in a user session, the service agent also performs one delayed stabilization recovery so boot/login display churn gets the same reapply-and-restart path as the configurator's Apply button.
5. `MonitorSplitterConfig.exe` runs in each logged-in user's session as the tray/configuration app. It edits shared machine config and signals the service directly instead of shelling out to the CLI.

`MonitorSplitterCtl.exe` remains available for development, scripting, and recovery. The CLI, tray app, and service use the same shared control code for config paths, monitor discovery, desired state, and stack startup.

## What Stays

Required user-facing operations:

- Configure the split layout and monitor count.
- Select the physical ultrawide target.
- Put the physical panel into split-ready mode or back to native Windows desktop mode.
- Enable and disable MonitorSplitter.
- Install and uninstall through the MSI.
- Configure and monitor the system from a tray app.
- Query status for recovery.

The physical-panel controls are necessary because Windows only lets the direct host own a panel after the user has enabled **Remove display from desktop** for that monitor in Settings.

## Layout

Layout specs use:

```text
hostWidthxheight@refresh:splitWidth1,splitWidth2,...
```

Examples:

```powershell
out\bin\MonitorSplitterCtl.exe layout "5120x1440@120:1280,2560,1280"
out\bin\MonitorSplitterCtl.exe layout "5120x1440@120:1600,1920,1600"
```

The host width must equal the sum of split widths. The driver currently supports 1 to 8 virtual monitors.

## Build

Prerequisites:

- Visual Studio 2022 MSBuild/MSVC `v143`
- Windows SDK/WDK 10.0.26100
- Visual Studio WDK platform toolset `WindowsUserModeDriver10.0`

Package the driver and tools:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-driver.ps1
```

Outputs:

- `out\driver-package`: driver INF, DLL, and catalog.
- `out\bin`: Release builds of `MonitorSplitterCtl.exe`, `MonitorSplitterHost.exe`, `MonitorSplitterService.exe`, and `MonitorSplitterConfig.exe`.
- `out\scripts`: MSI custom action, driver/service installer implementation scripts, verification script, and MSI packaging script.
- `out\installer`: WiX source used to build the MSI installer.
- `out\symbols`: driver symbols used for attestation CAB generation when available.
- `out\README.md` and `out\LICENSE`: package documentation and license.

Release tool binaries are built with the static MSVC runtime; the package step rejects Release tools that import `MSVCP140`, `VCRUNTIME140`, or UCRT redistributable DLLs. Package generation also parses the packaged PowerShell scripts and runs the packaged CLI, service, and installer selftests before reporting success.

## Install And Uninstall

Recommended MSI build after packaging:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-driver.ps1
powershell -ExecutionPolicy Bypass -File scripts\package-msi.ps1
```

`scripts\package-msi.ps1` requires WiX Toolset v4 on `PATH`. It installs the `WixToolset.Util.wixext` extension into the WiX extension cache if needed and writes `out\MonitorSplitter-0.1.0-x64.msi` by default.

One-time WiX setup:

```powershell
dotnet tool install --global wix --version 4.*
```

Install the MSI from an elevated PowerShell session:

```powershell
$msi = Join-Path $PWD "out\MonitorSplitter-0.1.0-x64.msi"
$log = Join-Path $PWD "out\msi-install.log"
$msiArgs = @("/i", "`"$msi`"", "/l*v", "`"$log`"")
$process = Start-Process -FilePath msiexec.exe -ArgumentList $msiArgs -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "MonitorSplitter MSI install failed with exit code $($process.ExitCode). See $log"
}
```

The MSI installs files into `C:\Program Files\MonitorSplitter`, installs the driver package, installs the automatic service, registers the tray app for future sign-ins, and runs install verification. The service starts the tray app in the active user session after install and on later sign-ins, unlocks, and wake events. The MSI preserves the previous service desired state by default. On a fresh install that means the service is installed and running, but MonitorSplitter stays disabled until the tray/config app or CLI enables it. Pass `ENABLEAFTERINSTALL=Enable` or `ENABLEAFTERINSTALL=Disable` to override that behavior:

```powershell
$msi = Join-Path $PWD "out\MonitorSplitter-0.1.0-x64.msi"
$log = Join-Path $PWD "out\msi-install.log"
$msiArgs = @("/i", "`"$msi`"", "ENABLEAFTERINSTALL=Enable", "/l*v", "`"$log`"")
$process = Start-Process -FilePath msiexec.exe -ArgumentList $msiArgs -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "MonitorSplitter MSI install failed with exit code $($process.ExitCode). See $log"
}
```

The installer verifies the installed files, installed CLI/service self-tests, service path, service restart policy, tray startup, ProgramData config, and driver-store package. If the desired state after install is enabled, verification also requires the direct host to become healthy before reporting success.
Verify the installed file layout, service registration, tray startup, ProgramData config, and driver-store package:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\verify-install.ps1
```

When the installed desired state is enabled, verification automatically checks live direct scanout health. Use `-RequireHealthyDirectHost` to force that health check even while the desired state is disabled.
If verification reports service or tray paths under the repository `out\bin` directory, the machine is still using the old development install. Rerun the recommended install command above to move the service, tray app, tools, and driver package under `C:\Program Files\MonitorSplitter`.

To validate package generation without touching the currently installed files, write the package into a separate ignored output root:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-driver.ps1 -OutputRoot out\package-verify
```

Package generation also parses the packaged PowerShell scripts, runs packaged `MonitorSplitterCtl.exe selftest`, runs packaged `MonitorSplitterService.exe --selftest`, and runs the packaged script self-tests for `install-driver.ps1`, `install-service.ps1`, `msi-custom-action.ps1`, `package-msi.ps1`, and `verify-install.ps1`.

Recommended uninstall:

```powershell
$msi = Join-Path $PWD "out\MonitorSplitter-0.1.0-x64.msi"
$log = Join-Path $PWD "out\msi-uninstall.log"
$msiArgs = @("/x", "`"$msi`"", "/l*v", "`"$log`"")
$process = Start-Process -FilePath msiexec.exe -ArgumentList $msiArgs -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "MonitorSplitter MSI uninstall failed with exit code $($process.ExitCode). See $log"
}
```

To remove `%ProgramData%\MonitorSplitter` layouts, target selection, logs, and desired state during MSI uninstall, pass `REMOVECONFIG=1`:

```powershell
$msi = Join-Path $PWD "out\MonitorSplitter-0.1.0-x64.msi"
$log = Join-Path $PWD "out\msi-uninstall.log"
$msiArgs = @("/x", "`"$msi`"", "REMOVECONFIG=1", "/l*v", "`"$log`"")
$process = Start-Process -FilePath msiexec.exe -ArgumentList $msiArgs -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "MonitorSplitter MSI uninstall failed with exit code $($process.ExitCode). See $log"
}
```

MSI install, upgrade, and uninstall are non-interactive: they stop the MonitorSplitter service/host/tray processes and remove the driver/service without opening Windows Display Settings or changing the physical panel's Windows desktop state. Use the tray app's explicit Open Display Settings button or `MonitorSplitterCtl.exe panel native` when you want to return the physical monitor to Windows.

The service stores machine-wide configuration in `%ProgramData%\MonitorSplitter`. On first install it copies the current user layout, target, direct target, and EDID name-base files from `%LOCALAPPDATA%\MonitorSplitter` if they exist. Service installation also repairs the target/name pairing: if `edid-name-base.txt` exists, the base name is embedded into `host-target.txt`; if only the embedded metadata exists, `edid-name-base.txt` is restored from it; if an older saved target contains a `\\?\DISPLAY#...` path but no metadata, the installer reads that monitor's EDID from the registry and restores `edid-name-base.txt` from the physical monitor name.

The service installs `MonitorSplitterConfig.exe --tray` under `HKLM\Software\Microsoft\Windows\CurrentVersion\Run`, so each interactive user gets a tray icon on sign-in. The service also starts the tray in the active user session whenever it is missing. The tray app edits the shared machine config and signals the service directly.

For local development with a test-signed package, Secure Boot must be disabled and Windows test-signing must be enabled:

```powershell
bcdedit /set testsigning on
```

Reboot after changing test-signing.

## First-Time Setup

Run these while the physical ultrawide is still a normal Windows desktop monitor:

```powershell
out\bin\MonitorSplitterCtl.exe layout "5120x1440@120:1280,2560,1280"
out\bin\MonitorSplitterCtl.exe hosttarget \\.\DISPLAY1
out\bin\MonitorSplitterCtl.exe panel split
```

`panel split` opens Windows Settings. Select the physical ultrawide and turn **Remove display from desktop** on. Windows persists this designation for that monitor.

To return the physical ultrawide to Windows as a normal desktop monitor:

```powershell
out\bin\MonitorSplitterCtl.exe panel native
```

## Daily Use

Preferred path:

```powershell
out\bin\MonitorSplitterConfig.exe
```

Use the tray/config app to select the physical ultrawide, configure the split widths, apply the layout, and enable or disable MonitorSplitter. The tray app writes `%ProgramData%\MonitorSplitter` config and wakes the service.

CLI fallback:

```powershell
out\bin\MonitorSplitterCtl.exe enable
```

Enable with a new layout:

```powershell
out\bin\MonitorSplitterCtl.exe enable "5120x1440@120:1280,2560,1280"
```

Check status:

```powershell
out\bin\MonitorSplitterCtl.exe status
out\bin\MonitorSplitterCtl.exe hoststatus
```

Disable:

```powershell
out\bin\MonitorSplitterCtl.exe disable
```

`disable` stops the direct host and removes the virtual monitors, then uses the same verified `panel native` flow to return the saved physical panel to the Windows desktop. Use `disable --keep-panel-state` only for low-level recovery when you intentionally want to leave the panel in its current Windows Settings state.

When the service is installed, the tray app and `MonitorSplitterCtl.exe enable|disable` update `%ProgramData%\MonitorSplitter\service-enabled.txt`. They signal the service and session agent immediately; the service also polls that file as a fallback and performs recovery from its own service session. The tray app never opens Display Settings automatically; use its explicit Open Display Settings button when changing the physical panel state. `MonitorSplitterCtl.exe disable` also polls and verifies that the saved physical panel returns to the Windows desktop.

## Commands

```text
MonitorSplitterCtl enable [layout]
MonitorSplitterCtl disable [--keep-panel-state]
MonitorSplitterCtl status
MonitorSplitterCtl layout [spec]
MonitorSplitterCtl hosttarget [selector|clear]
MonitorSplitterCtl edidbase [status|resolve|clear|set <name>]
MonitorSplitterCtl panelstate
MonitorSplitterCtl panel <split|native> [--enable]
MonitorSplitterCtl hoststatus
MonitorSplitterCtl hoststop
MonitorSplitterCtl selftest
```

Use `edidbase resolve` after selecting a host target, or after a reboot/wake issue, to verify that the saved physical panel can still be resolved while it is off the Windows desktop. The expected virtual EDID names are reported as JSON and should stay stable as long as the split count/order and base name do not change. The synthetic EDID product/serial identity is derived from the EDID base name plus each split's count, index, width, height, and refresh, so Windows keeps a stable identity for the same split config but gets a new identity when the split config or base name changes. New target saves also embed the resolved base name in `host-target.txt` as `msp:edid-name-base=...`, so service startup can recover the same EDID names even if `edid-name-base.txt` is missing.

Use `selftest` for non-live internal checks, including synthetic EDID checksum, decoded mode, selector metadata parsing, safe panel-restore decision logic, and the stable/change behavior of the EDID product and serial identity.

`hoststop` is a recovery command. It stops the direct host without unplugging the virtual monitors; normal shutdown should use `disable`.

## Signing For Secure Boot

The local test-signing path is for development only. For Secure Boot-enabled machines, submit the packaged driver through Microsoft's Hardware dashboard for attestation signing or WHCP signing.

Attestation signing is the first release path for this project. It produces a Microsoft-trusted driver package that can load on Windows 10/11 client systems without Windows test-signing mode. It is not WHQL certification and it does not publish the driver through Windows Update.

Prerequisites outside this repository:

- An EV code-signing certificate.
- A Microsoft Partner Center account registered for the Hardware Developer Program with that EV certificate associated.
- Access to the Partner Center Hardware dashboard.

Build the driver package:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-driver.ps1
```

Create the attestation submission CAB:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-attestation-cab.ps1
```

The script writes `out\attestation\MonitorSplitterDriver-attestation.cab`. The CAB keeps files under a `MonitorSplitterDriver\` subfolder, which is required for Partner Center attestation submissions.

If your EV certificate is available from the Windows certificate store, the script can also sign the CAB:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-attestation-cab.ps1 -SignCab -EvCertificateSubject "Your EV certificate subject"
```

Submit the EV-signed CAB in the Partner Center Hardware dashboard with the normal attestation signing option. Leave test-signing options unchecked. After Microsoft returns the signed driver package, import it into the MSI package root:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\import-signed-driver-package.ps1 -SignedPackagePath "C:\Path\To\MicrosoftSignedPackage" -RequireMicrosoftSignature
```

Build the MSI from the Microsoft-signed driver package:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\package-msi.ps1
```

After installing that MSI on a Secure Boot system, verify the installation and require Microsoft driver signatures:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\verify-install.ps1 -RequireMicrosoftSignedDriver
```

## Current Limitations

- The physical panel must be removed from the Windows desktop before direct mode can acquire it.
- Test-signed local development still requires Secure Boot off.
