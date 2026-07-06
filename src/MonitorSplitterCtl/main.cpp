#include <windows.h>
#include <swdevice.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <sddl.h>
#include <shellapi.h>

#include <winrt/Windows.Devices.Display.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <cwctype>
#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <array>

#include "..\MonitorSplitterCommon\BuildInfo.h"
#include "..\MonitorSplitterCommon\Layout.h"
#include "..\MonitorSplitterCommon\SyntheticEdid.h"
#include "..\MonitorSplitterControl\Control.h"
#include "..\MonitorSplitterControl\DirectStack.h"

namespace displaycore = winrt::Windows::Devices::Display::Core;

namespace
{
constexpr wchar_t kStopEventName[] = L"Local\\MonitorSplitter.Stop";
constexpr wchar_t kRunningMutexName[] = L"Local\\MonitorSplitter.Running";
constexpr wchar_t kReadyEventName[] = L"Local\\MonitorSplitter.Ready";
constexpr wchar_t kFailedEventName[] = L"Local\\MonitorSplitter.Failed";
constexpr wchar_t kHostRunningMutexName[] = L"Local\\MonitorSplitter.HostRunning";
constexpr wchar_t kHostStopEventName[] = L"Local\\MonitorSplitter.HostStop";

struct MonitorSnapshot
{
    HMONITOR Handle = nullptr;
    std::wstring DeviceName;
    RECT MonitorRect = {};
    RECT WorkRect = {};
    DWORD Flags = 0;
};

struct DisplayDeviceSnapshot
{
    std::wstring DeviceName;
    std::wstring DeviceString;
    std::wstring DeviceId;
    DWORD StateFlags = 0;
    std::vector<DisplayDeviceSnapshot> Monitors;
};

struct DisplayConfigPathSnapshot
{
    std::wstring SourceName;
    std::wstring TargetFriendlyName;
    std::wstring TargetDevicePath;
    LUID SourceAdapterId = {};
    UINT32 SourceId = 0;
    LUID TargetAdapterId = {};
    UINT32 TargetId = 0;
    UINT32 PathFlags = 0;
    UINT32 SourceStatusFlags = 0;
    UINT32 TargetStatusFlags = 0;
    UINT32 OutputTechnology = 0;
    bool TargetAvailable = false;
};

bool IsMonitorSplitterPath(const DisplayConfigPathSnapshot& path);
bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right);
int PanelStateCommand();
int PanelCommand(int argc, wchar_t* argv[]);
int EnableDirect();
int DisableCommand(int argc, wchar_t* argv[]);
int SelfTestCommand();

struct MonitorSpecializationSnapshot
{
    LONG Result = ERROR_SUCCESS;
    HRESULT HResult = S_OK;
    bool QuerySucceeded = false;
    bool IsSpecializationEnabled = false;
    bool IsSpecializationAvailableForMonitor = false;
    bool IsSpecializationAvailableForSystem = false;
};

struct DisplayConfigTargetRef
{
    LUID AdapterId = {};
    UINT32 TargetId = 0;
    std::wstring SourceName;
    std::wstring Name;
    std::wstring DevicePath;
    std::wstring StableMonitorId;
    INT32 UsageKind = 0;
    bool FromDisplayCore = false;
};

struct PanelStateSnapshot
{
    bool TargetConfigured = false;
    std::wstring Selector;
    size_t MatchCount = 0;
    bool TargetMatched = false;
    bool TargetAmbiguous = false;
    bool ActiveDesktopPathMatched = false;
    DisplayConfigTargetRef Target;
    MonitorSpecializationSnapshot MonitorSpecialization;
    bool DisplayCoreAvailable = false;
    HRESULT DisplayCoreHResult = S_OK;
    std::wstring DisplayCoreError;
    bool AcquisitionAttempted = false;
    bool AcquisitionSucceeded = false;
    INT32 AcquisitionResult = 0;
    HRESULT AcquisitionHResult = S_OK;
    bool HostRunning = false;
    std::wstring State;
    std::wstring Message;
};

class LocalSecurityDescriptor
{
public:
    explicit LocalSecurityDescriptor(PCWSTR sddl)
    {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl,
                SDDL_REVISION_1,
                &m_Descriptor,
                nullptr))
        {
            m_Error = HRESULT_FROM_WIN32(GetLastError());
        }
    }

    ~LocalSecurityDescriptor()
    {
        if (m_Descriptor != nullptr)
        {
            LocalFree(m_Descriptor);
        }
    }

    HRESULT Status() const
    {
        return m_Error;
    }

    SECURITY_ATTRIBUTES* Attributes()
    {
        m_Attributes.nLength = sizeof(m_Attributes);
        m_Attributes.lpSecurityDescriptor = m_Descriptor;
        m_Attributes.bInheritHandle = FALSE;
        return &m_Attributes;
    }

private:
    PSECURITY_DESCRIPTOR m_Descriptor = nullptr;
    SECURITY_ATTRIBUTES m_Attributes = {};
    HRESULT m_Error = S_OK;
};

std::vector<MonitorSnapshot> g_Monitors;

bool WriteTextFile(const std::wstring& path, const std::wstring& message);
std::wstring ReadTextFile(const std::wstring& path);
std::vector<DisplayConfigPathSnapshot> QueryActiveDisplayConfigPaths();
DisplayConfigTargetRef TargetRefFromPath(const DisplayConfigPathSnapshot& path);
bool TargetRefMatches(const DisplayConfigTargetRef& target, const std::wstring& selector);
bool SameDisplayTarget(const DisplayConfigTargetRef& left, const DisplayConfigTargetRef& right);
MonitorSpecializationSnapshot QueryMonitorSpecialization(const DisplayConfigTargetRef& target);
void PrintMonitorSpecializationJson(const MonitorSpecializationSnapshot& specialization);
void PrintTargetRefJson(const DisplayConfigTargetRef& target);
PanelStateSnapshot QueryPanelState(bool attemptAcquire);
bool PanelReadyForDirect(const PanelStateSnapshot& snapshot);
void SaveDirectTargetFromPanelState(const PanelStateSnapshot& snapshot);
void PrintPanelStateJson(const PanelStateSnapshot& snapshot);

std::wstring GetStatePath()
{
    wchar_t tempPath[MAX_PATH] = {};
    const DWORD length = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
    if (length == 0 || length >= ARRAYSIZE(tempPath))
    {
        return L"MonitorSplitterCtl.state.txt";
    }

    std::wstring path = tempPath;
    path += L"MonitorSplitterCtl.state.txt";
    return path;
}

std::wstring GetConfigDirectory()
{
    return MonitorSplitter::Control::ConfigDirectory();
}

std::wstring GetMachineConfigDirectory()
{
    return MonitorSplitter::Control::ProgramDataConfigDirectory();
}

std::wstring GetServiceDesiredPath()
{
    return MonitorSplitter::Control::DesiredStatePath();
}

std::wstring GetLayoutPath()
{
    return MonitorSplitter::Control::LayoutPath();
}

std::wstring GetActiveLayoutPath()
{
    return MonitorSplitter::Control::ActiveLayoutPath();
}

std::wstring GetHostStatusPath()
{
    return MonitorSplitter::Control::HostStatusPath();
}

std::wstring GetHostTargetPath()
{
    return MonitorSplitter::Control::HostTargetPath();
}

std::wstring GetDirectTargetPath()
{
    return MonitorSplitter::Control::DirectTargetPath();
}

std::string ToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
    {
        return {};
    }

    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], required, nullptr, nullptr);
    result.pop_back();
    return result;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& message)
{
    return MonitorSplitter::Control::WriteTextFile(path, message);
}

std::wstring ReadTextFile(const std::wstring& path)
{
    return MonitorSplitter::Control::ReadTextFile(path);
}

void WriteState(const std::wstring& message)
{
    WriteTextFile(GetStatePath(), message);
}

void SaveServiceDesiredStateIfConfigured(bool enabled)
{
    wchar_t suppress[MAX_PATH] = {};
    const DWORD suppressLength = GetEnvironmentVariableW(L"MONITORSPLITTER_SUPPRESS_DESIRED_STATE", suppress, ARRAYSIZE(suppress));
    if (suppressLength != 0 && suppressLength < ARRAYSIZE(suppress))
    {
        const auto value = MonitorSplitter::Trim(suppress);
        if (!value.empty() && !EqualsIgnoreCase(value, L"0") && !EqualsIgnoreCase(value, L"false") && !EqualsIgnoreCase(value, L"no"))
        {
            return;
        }
    }

    if (!MonitorSplitter::Control::MachineConfigExists())
    {
        return;
    }

    MonitorSplitter::Control::SaveDesiredEnabled(enabled);
    MonitorSplitter::Control::SignalStackWake();
}

bool RequestServiceRestartIfConfigured()
{
    if (!MonitorSplitter::Control::MachineConfigExists())
    {
        return false;
    }

    return MonitorSplitter::Control::RequestServiceRestart(true);
}

std::wstring ReadState()
{
    return ReadTextFile(GetStatePath());
}

bool TryLoadLayoutFile(const std::wstring& path, MonitorSplitter::Layout& layout)
{
    const auto spec = MonitorSplitter::Trim(ReadTextFile(path));
    if (spec.empty())
    {
        return false;
    }

    std::wstring error;
    return MonitorSplitter::ParseLayoutSpec(spec, layout, &error);
}

MonitorSplitter::Layout LoadLayout()
{
    MonitorSplitter::Layout layout;
    if (TryLoadLayoutFile(GetLayoutPath(), layout))
    {
        return layout;
    }

    return MonitorSplitter::DefaultLayout();
}

bool SaveLayout(const MonitorSplitter::Layout& layout)
{
    const auto directory = GetConfigDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);
    return WriteTextFile(GetLayoutPath(), MonitorSplitter::SerializeLayout(layout));
}

bool SaveActiveLayout(const MonitorSplitter::Layout& layout)
{
    const auto directory = GetConfigDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);
    return WriteTextFile(GetActiveLayoutPath(), MonitorSplitter::SerializeLayout(layout));
}

void ClearActiveLayout()
{
    DeleteFileW(GetActiveLayoutPath().c_str());
}

std::wstring LoadHostTarget()
{
    return MonitorSplitter::Trim(ReadTextFile(GetHostTargetPath()));
}

bool StartsWithIgnoreCaseLiteral(const std::wstring& value, const wchar_t* prefix)
{
    const size_t prefixLength = wcslen(prefix);
    return value.size() >= prefixLength &&
           _wcsnicmp(value.c_str(), prefix, prefixLength) == 0;
}

std::wstring LoadDirectTarget()
{
    std::wstring selectors;
    std::wstringstream stream(ReadTextFile(GetDirectTargetPath()));
    std::wstring line;
    while (std::getline(stream, line))
    {
        line = MonitorSplitter::Trim(line);
        if (line.empty())
        {
            continue;
        }

        if (MonitorSplitter::Control::IsSelectorMetadataLine(line))
        {
            continue;
        }

        if (!selectors.empty())
        {
            selectors += L"\n";
        }
        selectors += line;
    }

    return selectors;
}

bool SaveHostTarget(const std::wstring& target)
{
    const auto directory = GetConfigDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);
    return WriteTextFile(GetHostTargetPath(), MonitorSplitter::Trim(target));
}

bool SaveDirectTarget(const std::wstring& target)
{
    const auto directory = GetConfigDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);
    return WriteTextFile(GetDirectTargetPath(), MonitorSplitter::Trim(target));
}

std::wstring JoinArgs(int argc, wchar_t* argv[], int first)
{
    std::wstring value;
    for (int index = first; index < argc; index++)
    {
        if (!value.empty())
        {
            value += L" ";
        }
        value += argv[index];
    }
    return value;
}

std::wstring HResultText(HRESULT hr)
{
    std::wstringstream stream;
    stream << L"0x" << std::hex << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(hr);
    return stream.str();
}

std::wstring ToWString(const winrt::hstring& value)
{
    return std::wstring(value.c_str(), value.size());
}

SECURITY_ATTRIBUTES* LocalControlObjectSecurity()
{
    static LocalSecurityDescriptor securityDescriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)");
    if (FAILED(securityDescriptor.Status()))
    {
        return nullptr;
    }

    return securityDescriptor.Attributes();
}

HANDLE CreateLocalEvent(PCWSTR name, BOOL manualReset, BOOL initialState)
{
    return CreateEventW(LocalControlObjectSecurity(), manualReset, initialState, name);
}

BOOL WINAPI ConsoleHandler(DWORD ControlType)
{
    if (ControlType == CTRL_C_EVENT || ControlType == CTRL_CLOSE_EVENT || ControlType == CTRL_BREAK_EVENT)
    {
        HANDLE stopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
        if (stopEvent != nullptr)
        {
            SetEvent(stopEvent);
            CloseHandle(stopEvent);
        }
        return TRUE;
    }

    return FALSE;
}

bool IsRunning()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kRunningMutexName);
    if (mutex == nullptr)
    {
        return false;
    }

    CloseHandle(mutex);
    return true;
}

bool IsHostRunning()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kHostRunningMutexName);
    if (mutex == nullptr)
    {
        return false;
    }

    CloseHandle(mutex);
    return true;
}

MonitorSplitter::Layout LoadEffectiveLayout()
{
    MonitorSplitter::Layout layout;
    if (IsRunning() && TryLoadLayoutFile(GetActiveLayoutPath(), layout))
    {
        return layout;
    }

    return LoadLayout();
}

bool RequestHostStop()
{
    HANDLE stopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kHostStopEventName);
    if (stopEvent == nullptr)
    {
        return false;
    }

    SetEvent(stopEvent);
    CloseHandle(stopEvent);
    return true;
}

bool WaitUntilStopped(DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        if (!IsRunning())
        {
            return true;
        }

        if (GetTickCount() - start >= timeoutMs)
        {
            return false;
        }

        Sleep(100);
    }
}

bool WaitUntilHostStopped(DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        if (!IsHostRunning())
        {
            return true;
        }

        if (GetTickCount() - start >= timeoutMs)
        {
            return false;
        }

        Sleep(100);
    }
}

bool WaitForDirectSharedHost(HANDLE process, DWORD expectedPid, DWORD timeoutMs, std::wstring& reason)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        if (process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
        {
            DWORD exitCode = 0;
            GetExitCodeProcess(process, &exitCode);
            reason = L"direct host exited before readiness with exit code " + std::to_wstring(exitCode);
            return false;
        }

        if (!IsHostRunning())
        {
            reason = L"direct host is not running";
        }
        else
        {
            const auto status = MonitorSplitter::Trim(ReadTextFile(GetHostStatusPath()));
            if (MonitorSplitter::Control::HostStatusHealthy(status, &reason, expectedPid))
            {
                return true;
            }
        }

        if (GetTickCount() - start >= timeoutMs)
        {
            return false;
        }

        Sleep(100);
    }
}

bool IsEventSignaled(PCWSTR eventName)
{
    HANDLE eventHandle = OpenEventW(SYNCHRONIZE, FALSE, eventName);
    if (eventHandle == nullptr)
    {
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(eventHandle, 0);
    CloseHandle(eventHandle);
    return waitResult == WAIT_OBJECT_0;
}

std::wstring JsonEscape(const std::wstring& value)
{
    std::wstringstream escaped;
    for (wchar_t ch : value)
    {
        switch (ch)
        {
        case L'\\':
            escaped << L"\\\\";
            break;
        case L'"':
            escaped << L"\\\"";
            break;
        case L'\n':
            escaped << L"\\n";
            break;
        case L'\r':
            escaped << L"\\r";
            break;
        case L'\t':
            escaped << L"\\t";
            break;
        default:
            escaped << ch;
            break;
        }
    }

    return escaped.str();
}

void PrintComponentJson(const wchar_t* name)
{
    std::wcout << L"{\"name\":\"" << JsonEscape(name) << L"\"";
    std::wcout << L",\"productVersion\":\"" << JsonEscape(MonitorSplitter::kProductVersionWide) << L"\"";
    std::wcout << L",\"buildTag\":\"" << JsonEscape(MonitorSplitter::kBuildTagWide) << L"\"";
    std::wcout << L",\"pid\":" << GetCurrentProcessId();
    std::wcout << L"}";
}

void PrintRawJsonFileOrNull(const std::wstring& path)
{
    const auto value = MonitorSplitter::Trim(ReadTextFile(path));
    if (value.empty())
    {
        std::wcout << L"null";
    }
    else
    {
        std::wcout << value;
    }
}

std::wstring ToLower(std::wstring value)
{
    for (auto& ch : value)
    {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

bool ContainsIgnoreCase(const std::wstring& value, const std::wstring& needle)
{
    if (needle.empty())
    {
        return true;
    }

    return ToLower(value).find(ToLower(needle)) != std::wstring::npos;
}

bool ContainsIgnoreCase(const std::wstring& value, const wchar_t* needle)
{
    if (needle == nullptr)
    {
        return true;
    }

    return ContainsIgnoreCase(value, std::wstring(needle));
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    return ToLower(left) == ToLower(right);
}

bool SelectorFieldMatches(const std::wstring& field, const std::wstring& selectorItem)
{
    const auto trimmedField = MonitorSplitter::Trim(field);
    const auto trimmedItem = MonitorSplitter::Trim(selectorItem);
    if (trimmedField.empty() || trimmedItem.empty())
    {
        return false;
    }

    return EqualsIgnoreCase(trimmedField, trimmedItem) ||
           ContainsIgnoreCase(trimmedField, trimmedItem);
}

std::vector<std::wstring> SplitSelectorItems(const std::wstring& selector)
{
    std::vector<std::wstring> items;
    std::wstringstream stream(selector);
    std::wstring item;
    while (std::getline(stream, item))
    {
        item = MonitorSplitter::Trim(item);
        if (!item.empty() && !MonitorSplitter::Control::IsSelectorMetadataLine(item))
        {
            items.push_back(std::move(item));
        }
    }
    return items;
}

bool SelectorMatchesAnyField(
    const std::wstring& selector,
    const std::vector<std::wstring>& fields)
{
    for (const auto& item : SplitSelectorItems(selector))
    {
        for (const auto& field : fields)
        {
            if (SelectorFieldMatches(field, item))
            {
                return true;
            }
        }
    }
    return false;
}

std::wstring DisplayTargetAdapterIdentity(LUID adapterId, UINT32 targetId)
{
    std::wstringstream stream;
    stream << L"adapter:" << static_cast<unsigned long>(adapterId.LowPart)
           << L":" << adapterId.HighPart
           << L":" << targetId;
    return stream.str();
}

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
    {
        return TRUE;
    }

    MonitorSnapshot snapshot;
    snapshot.Handle = monitor;
    snapshot.DeviceName = info.szDevice;
    snapshot.MonitorRect = info.rcMonitor;
    snapshot.WorkRect = info.rcWork;
    snapshot.Flags = info.dwFlags;
    g_Monitors.push_back(snapshot);
    return TRUE;
}

HMONITOR FindMonitorHandleByDeviceName(const std::wstring& deviceName)
{
    for (const auto& monitor : g_Monitors)
    {
        if (EqualsIgnoreCase(monitor.DeviceName, deviceName))
        {
            return monitor.Handle;
        }
    }

    return nullptr;
}

std::vector<DisplayDeviceSnapshot> EnumerateDisplayDevices()
{
    std::vector<DisplayDeviceSnapshot> adapters;
    for (DWORD adapterIndex = 0;; adapterIndex++)
    {
        DISPLAY_DEVICEW adapter = {};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0))
        {
            break;
        }

        DisplayDeviceSnapshot adapterSnapshot;
        adapterSnapshot.DeviceName = adapter.DeviceName;
        adapterSnapshot.DeviceString = adapter.DeviceString;
        adapterSnapshot.DeviceId = adapter.DeviceID;
        adapterSnapshot.StateFlags = adapter.StateFlags;

        for (DWORD monitorIndex = 0;; monitorIndex++)
        {
            DISPLAY_DEVICEW monitor = {};
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(adapter.DeviceName, monitorIndex, &monitor, 0))
            {
                break;
            }

            DisplayDeviceSnapshot monitorSnapshot;
            monitorSnapshot.DeviceName = monitor.DeviceName;
            monitorSnapshot.DeviceString = monitor.DeviceString;
            monitorSnapshot.DeviceId = monitor.DeviceID;
            monitorSnapshot.StateFlags = monitor.StateFlags;
            adapterSnapshot.Monitors.push_back(monitorSnapshot);
        }

        adapters.push_back(adapterSnapshot);
    }

    return adapters;
}

bool IsMonitorSplitterDisplayDevice(const DisplayDeviceSnapshot& device)
{
    return ContainsIgnoreCase(device.DeviceName, L"MonitorSplitter") ||
           ContainsIgnoreCase(device.DeviceString, L"MonitorSplitter") ||
           ContainsIgnoreCase(device.DeviceId, L"MonitorSplitter");
}

bool HasAttachedMonitorSplitterAdapter(const std::vector<DisplayDeviceSnapshot>& devices)
{
    for (const auto& adapter : devices)
    {
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && IsMonitorSplitterDisplayDevice(adapter))
        {
            return true;
        }

        for (const auto& monitor : adapter.Monitors)
        {
            if ((monitor.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && IsMonitorSplitterDisplayDevice(monitor))
            {
                return true;
            }
        }
    }

    return false;
}

LONG MonitorWidth(const MonitorSnapshot& monitor)
{
    return monitor.MonitorRect.right - monitor.MonitorRect.left;
}

LONG MonitorHeight(const MonitorSnapshot& monitor)
{
    return monitor.MonitorRect.bottom - monitor.MonitorRect.top;
}

const DisplayDeviceSnapshot* FindDisplayDevice(
    const std::vector<DisplayDeviceSnapshot>& devices,
    const std::wstring& deviceName)
{
    for (const auto& device : devices)
    {
        if (device.DeviceName == deviceName)
        {
            return &device;
        }
    }

    return nullptr;
}

bool IsMonitorSplitterMonitor(
    const MonitorSnapshot& monitor,
    const std::vector<DisplayDeviceSnapshot>& devices)
{
    const auto* adapter = FindDisplayDevice(devices, monitor.DeviceName);
    if (adapter == nullptr)
    {
        return false;
    }

    if (IsMonitorSplitterDisplayDevice(*adapter))
    {
        return true;
    }

    for (const auto& child : adapter->Monitors)
    {
        if (IsMonitorSplitterDisplayDevice(child))
        {
            return true;
        }
    }

    return false;
}

bool IsHostCandidate(
    const MonitorSnapshot& monitor,
    const std::vector<DisplayDeviceSnapshot>& devices,
    const MonitorSplitter::Layout& layout)
{
    return !IsMonitorSplitterMonitor(monitor, devices) &&
           MonitorWidth(monitor) >= static_cast<LONG>(layout.HostWidth) &&
           MonitorHeight(monitor) >= static_cast<LONG>(layout.Height);
}

bool IsSplitSourceCandidate(
    const MonitorSnapshot& monitor,
    const MonitorSplitter::Layout& layout)
{
    if (MonitorHeight(monitor) != static_cast<LONG>(layout.Height))
    {
        return false;
    }

    for (const auto& expected : layout.Monitors)
    {
        if (MonitorWidth(monitor) == static_cast<LONG>(expected.Width))
        {
            return true;
        }
    }

    return false;
}

std::wstring GetSourceName(const DISPLAYCONFIG_PATH_SOURCE_INFO& sourceInfo)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
    sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sourceName.header.size = sizeof(sourceName);
    sourceName.header.adapterId = sourceInfo.adapterId;
    sourceName.header.id = sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
    {
        return {};
    }

    return sourceName.viewGdiDeviceName;
}

std::wstring GetTargetFriendlyName(const DISPLAYCONFIG_PATH_TARGET_INFO& targetInfo)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
    targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    targetName.header.size = sizeof(targetName);
    targetName.header.adapterId = targetInfo.adapterId;
    targetName.header.id = targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS)
    {
        return {};
    }

    return targetName.monitorFriendlyDeviceName;
}

std::wstring GetTargetDevicePath(const DISPLAYCONFIG_PATH_TARGET_INFO& targetInfo)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
    targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    targetName.header.size = sizeof(targetName);
    targetName.header.adapterId = targetInfo.adapterId;
    targetName.header.id = targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS)
    {
        return {};
    }

    return targetName.monitorDevicePath;
}

std::vector<DisplayConfigPathSnapshot> QueryActiveDisplayConfigPaths()
{
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS || pathCount == 0)
    {
        return {};
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    result = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        paths.data(),
        &modeCount,
        modes.data(),
        nullptr);
    if (result != ERROR_SUCCESS)
    {
        return {};
    }

    paths.resize(pathCount);

    std::vector<DisplayConfigPathSnapshot> snapshots;
    for (const auto& path : paths)
    {
        DisplayConfigPathSnapshot snapshot;
        snapshot.SourceName = GetSourceName(path.sourceInfo);
        snapshot.TargetFriendlyName = GetTargetFriendlyName(path.targetInfo);
        snapshot.TargetDevicePath = GetTargetDevicePath(path.targetInfo);
        snapshot.SourceAdapterId = path.sourceInfo.adapterId;
        snapshot.SourceId = path.sourceInfo.id;
        snapshot.TargetAdapterId = path.targetInfo.adapterId;
        snapshot.TargetId = path.targetInfo.id;
        snapshot.PathFlags = path.flags;
        snapshot.SourceStatusFlags = path.sourceInfo.statusFlags;
        snapshot.TargetStatusFlags = path.targetInfo.statusFlags;
        snapshot.OutputTechnology = static_cast<UINT32>(path.targetInfo.outputTechnology);
        snapshot.TargetAvailable = path.targetInfo.targetAvailable ? true : false;
        snapshots.push_back(std::move(snapshot));
    }

    return snapshots;
}

const DisplayConfigPathSnapshot* FindDisplayConfigPathBySourceName(
    const std::vector<DisplayConfigPathSnapshot>& paths,
    const std::wstring& sourceName)
{
    for (const auto& path : paths)
    {
        if (EqualsIgnoreCase(path.SourceName, sourceName))
        {
            return &path;
        }
    }

    return nullptr;
}

std::wstring DisplayConfigPathTargetAdapterIdentity(const DisplayConfigPathSnapshot& path)
{
    return DisplayTargetAdapterIdentity(path.TargetAdapterId, path.TargetId);
}

MonitorSpecializationSnapshot QueryMonitorSpecialization(const DisplayConfigPathSnapshot& path)
{
    MonitorSpecializationSnapshot snapshot;

    DISPLAYCONFIG_GET_MONITOR_SPECIALIZATION specialization = {};
    specialization.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_MONITOR_SPECIALIZATION;
    specialization.header.size = sizeof(specialization);
    specialization.header.adapterId = path.TargetAdapterId;
    specialization.header.id = path.TargetId;

    snapshot.Result = DisplayConfigGetDeviceInfo(&specialization.header);
    snapshot.HResult = HRESULT_FROM_WIN32(snapshot.Result);
    if (snapshot.Result != ERROR_SUCCESS)
    {
        return snapshot;
    }

    snapshot.QuerySucceeded = true;
    snapshot.IsSpecializationEnabled = specialization.isSpecializationEnabled ? true : false;
    snapshot.IsSpecializationAvailableForMonitor = specialization.isSpecializationAvailableForMonitor ? true : false;
    snapshot.IsSpecializationAvailableForSystem = specialization.isSpecializationAvailableForSystem ? true : false;
    return snapshot;
}

MonitorSpecializationSnapshot QueryMonitorSpecialization(const DisplayConfigTargetRef& target)
{
    MonitorSpecializationSnapshot snapshot;

    DISPLAYCONFIG_GET_MONITOR_SPECIALIZATION specialization = {};
    specialization.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_MONITOR_SPECIALIZATION;
    specialization.header.size = sizeof(specialization);
    specialization.header.adapterId = target.AdapterId;
    specialization.header.id = target.TargetId;

    snapshot.Result = DisplayConfigGetDeviceInfo(&specialization.header);
    snapshot.HResult = HRESULT_FROM_WIN32(snapshot.Result);
    if (snapshot.Result != ERROR_SUCCESS)
    {
        return snapshot;
    }

    snapshot.QuerySucceeded = true;
    snapshot.IsSpecializationEnabled = specialization.isSpecializationEnabled ? true : false;
    snapshot.IsSpecializationAvailableForMonitor = specialization.isSpecializationAvailableForMonitor ? true : false;
    snapshot.IsSpecializationAvailableForSystem = specialization.isSpecializationAvailableForSystem ? true : false;
    return snapshot;
}

DisplayConfigTargetRef TargetRefFromPath(const DisplayConfigPathSnapshot& path)
{
    DisplayConfigTargetRef target;
    target.AdapterId = path.TargetAdapterId;
    target.TargetId = path.TargetId;
    target.SourceName = path.SourceName;
    target.Name = path.TargetFriendlyName.empty() ? path.SourceName : path.TargetFriendlyName;
    target.DevicePath = path.TargetDevicePath;
    target.FromDisplayCore = false;
    return target;
}

bool IsMonitorSplitterPath(const DisplayConfigPathSnapshot& path)
{
    return ContainsIgnoreCase(path.SourceName, L"MonitorSplitter") ||
           ContainsIgnoreCase(path.SourceName, L"MSplit") ||
           ContainsIgnoreCase(path.TargetFriendlyName, L"MonitorSplitter") ||
           ContainsIgnoreCase(path.TargetFriendlyName, L"MSplit") ||
           ContainsIgnoreCase(path.TargetDevicePath, L"MonitorSplitter") ||
           ContainsIgnoreCase(path.TargetDevicePath, L"MSplit") ||
           ContainsIgnoreCase(path.TargetDevicePath, L"DISPLAY#MSP");
}

bool TryUseVisibleMonitor(LONG width, LONG height, std::vector<bool>& used)
{
    for (size_t index = 0; index < g_Monitors.size(); index++)
    {
        if (used[index])
        {
            continue;
        }

        const auto& monitor = g_Monitors[index];
        const LONG monitorWidth = monitor.MonitorRect.right - monitor.MonitorRect.left;
        const LONG monitorHeight = monitor.MonitorRect.bottom - monitor.MonitorRect.top;
        if (monitorWidth == width && monitorHeight == height)
        {
            used[index] = true;
            return true;
        }
    }

    return false;
}

DWORD CountExpectedVisibleMonitors(const MonitorSplitter::Layout& layout)
{
    DWORD matches = 0;
    std::vector<bool> used(g_Monitors.size(), false);
    for (const auto& monitor : layout.Monitors)
    {
        if (TryUseVisibleMonitor(static_cast<LONG>(monitor.Width), static_cast<LONG>(monitor.Height), used))
        {
            matches++;
        }
    }

    return matches;
}

void PrintExpectedMonitorsJson(const MonitorSplitter::Layout& layout)
{
    std::wcout << L"[";
    for (size_t index = 0; index < layout.Monitors.size(); index++)
    {
        const auto& monitor = layout.Monitors[index];
        if (index != 0)
        {
            std::wcout << L",";
        }

        std::wcout << L"{\"name\":\"" << JsonEscape(MonitorSplitter::MonitorName(layout.Monitors.size(), index)) << L"\"";
        std::wcout << L",\"width\":" << monitor.Width;
        std::wcout << L",\"height\":" << monitor.Height;
        std::wcout << L",\"refresh\":" << monitor.Refresh;
        std::wcout << L"}";
    }
    std::wcout << L"]";
}

void PrintMonitorSnapshotJson(
    const MonitorSnapshot& monitor,
    const std::vector<DisplayDeviceSnapshot>& devices,
    const std::vector<DisplayConfigPathSnapshot>* displayConfigPaths = nullptr)
{
    const auto* adapter = FindDisplayDevice(devices, monitor.DeviceName);
    const auto* path = displayConfigPaths == nullptr
        ? nullptr
        : FindDisplayConfigPathBySourceName(*displayConfigPaths, monitor.DeviceName);

    std::wcout << L"{\"device\":\"" << JsonEscape(monitor.DeviceName) << L"\"";
    std::wcout << L",\"primary\":" << ((monitor.Flags & MONITORINFOF_PRIMARY) ? L"true" : L"false");
    std::wcout << L",\"x\":" << monitor.MonitorRect.left;
    std::wcout << L",\"y\":" << monitor.MonitorRect.top;
    std::wcout << L",\"width\":" << MonitorWidth(monitor);
    std::wcout << L",\"height\":" << MonitorHeight(monitor);
    std::wcout << L",\"monitorSplitter\":" << (IsMonitorSplitterMonitor(monitor, devices) ? L"true" : L"false");
    std::wcout << L",\"adapterString\":\"" << JsonEscape(adapter == nullptr ? L"" : adapter->DeviceString) << L"\"";
    std::wcout << L",\"adapterId\":\"" << JsonEscape(adapter == nullptr ? L"" : adapter->DeviceId) << L"\"";
    std::wcout << L",\"targetFriendlyName\":\"" << JsonEscape(path == nullptr ? L"" : path->TargetFriendlyName) << L"\"";
    std::wcout << L",\"targetDevicePath\":\"" << JsonEscape(path == nullptr ? L"" : path->TargetDevicePath) << L"\"";
    std::wcout << L",\"targetAdapterLow\":" << (path == nullptr ? 0ul : static_cast<unsigned long>(path->TargetAdapterId.LowPart));
    std::wcout << L",\"targetAdapterHigh\":" << (path == nullptr ? 0 : path->TargetAdapterId.HighPart);
    std::wcout << L",\"targetId\":" << (path == nullptr ? 0 : path->TargetId);
    std::wcout << L",\"targetAdapterIdentity\":\"" << JsonEscape(path == nullptr ? L"" : DisplayConfigPathTargetAdapterIdentity(*path)) << L"\"";
    std::wcout << L"}";
}

void PrintMonitorSpecializationJson(const MonitorSpecializationSnapshot& specialization)
{
    std::wcout << L"{\"querySucceeded\":" << (specialization.QuerySucceeded ? L"true" : L"false");
    std::wcout << L",\"win32Result\":" << specialization.Result;
    std::wcout << L",\"hresult\":\"" << JsonEscape(HResultText(specialization.HResult)) << L"\"";
    std::wcout << L",\"isSpecializationEnabled\":" << (specialization.IsSpecializationEnabled ? L"true" : L"false");
    std::wcout << L",\"isSpecializationAvailableForMonitor\":" << (specialization.IsSpecializationAvailableForMonitor ? L"true" : L"false");
    std::wcout << L",\"isSpecializationAvailableForSystem\":" << (specialization.IsSpecializationAvailableForSystem ? L"true" : L"false");
    std::wcout << L"}";
}

std::vector<std::wstring> RenderAdapterSelectorFields(
    const MonitorSnapshot& monitor,
    const std::vector<DisplayDeviceSnapshot>& devices,
    const std::vector<DisplayConfigPathSnapshot>& displayConfigPaths)
{
    std::vector<std::wstring> fields;
    fields.push_back(monitor.DeviceName);

    const auto* path = FindDisplayConfigPathBySourceName(displayConfigPaths, monitor.DeviceName);
    if (path != nullptr)
    {
        fields.push_back(path->SourceName);
        fields.push_back(path->TargetFriendlyName);
        fields.push_back(path->TargetDevicePath);
        fields.push_back(DisplayConfigPathTargetAdapterIdentity(*path));
    }

    const auto* adapter = FindDisplayDevice(devices, monitor.DeviceName);
    if (adapter != nullptr)
    {
        fields.push_back(adapter->DeviceName);
        fields.push_back(adapter->DeviceString);
        fields.push_back(adapter->DeviceId);
        for (const auto& child : adapter->Monitors)
        {
            fields.push_back(child.DeviceName);
            fields.push_back(child.DeviceString);
            fields.push_back(child.DeviceId);
        }
    }

    return fields;
}

int Host(bool foreground)
{
    MonitorSplitter::Control::DirectStack stack;
    std::wstring reason;
    if (!stack.Start(reason))
    {
        WriteState(reason);
        std::wcerr << reason << L"\n";
        return 1;
    }

    if (foreground)
    {
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
        std::wcout << ReadState() << L". Press Ctrl+C or run disable to stop.\n";
    }

    stack.WaitForStopRequest();

    if (foreground)
    {
        std::wcout << L"Stopping MonitorSplitter.\n";
        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    }
    return 0;
}

bool StopVirtualDeviceHolder(DWORD timeoutMs, std::wstring& reason)
{
    HANDLE stopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (stopEvent == nullptr)
    {
        ClearActiveLayout();
        reason.clear();
        return true;
    }

    SetEvent(stopEvent);
    CloseHandle(stopEvent);
    if (!WaitUntilStopped(timeoutMs))
    {
        reason = L"holder did not stop within " + std::to_wstring(timeoutMs) + L" ms";
        return false;
    }

    ClearActiveLayout();
    reason.clear();
    return true;
}

int EnableVirtualDevice()
{
    if (IsRunning())
    {
        std::wstring healthReason;
        if (IsHostRunning() && MonitorSplitter::Control::HostStatusHealthy(ReadTextFile(GetHostStatusPath()), &healthReason))
        {
            std::wcout << L"MonitorSplitter is already enabled.\n";
            return 0;
        }

        std::wstring stopReason;
        if (!StopVirtualDeviceHolder(10 * 1000, stopReason))
        {
            std::wcerr << L"Existing MonitorSplitter holder is stale, but it could not be stopped: " << stopReason << L".\n";
            return 1;
        }
    }
    DeleteFileW(GetStatePath().c_str());

    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) == 0)
    {
        std::wcerr << L"GetModuleFileName failed: " << GetLastError() << L"\n";
        return 1;
    }

    HANDLE readyEvent = CreateLocalEvent(kReadyEventName, TRUE, FALSE);
    HANDLE failedEvent = CreateLocalEvent(kFailedEventName, TRUE, FALSE);
    if (readyEvent == nullptr || failedEvent == nullptr)
    {
        std::wcerr << L"CreateEvent failed: " << GetLastError() << L"\n";
        if (readyEvent != nullptr)
        {
            CloseHandle(readyEvent);
        }
        if (failedEvent != nullptr)
        {
            CloseHandle(failedEvent);
        }
        return 1;
    }
    ResetEvent(readyEvent);
    ResetEvent(failedEvent);

    std::wstring commandLine = L"\"";
    commandLine += exePath;
    commandLine += L"\" host";

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};

    if (!CreateProcessW(
        exePath,
        &commandLine[0],
        nullptr,
        nullptr,
        FALSE,
        DETACHED_PROCESS,
        nullptr,
        nullptr,
        &startup,
        &process))
    {
        std::wcerr << L"CreateProcess failed: " << GetLastError() << L"\n";
        CloseHandle(readyEvent);
        CloseHandle(failedEvent);
        return 1;
    }

    CloseHandle(process.hThread);

    HANDLE waitHandles[] = { readyEvent, failedEvent, process.hProcess };
    const DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 15 * 1000);
    if (waitResult == WAIT_OBJECT_0)
    {
        CloseHandle(process.hProcess);
        CloseHandle(readyEvent);
        CloseHandle(failedEvent);
        std::wcout << L"MonitorSplitter enabled.\n";
        return 0;
    }
    if (waitResult == WAIT_OBJECT_0 + 1)
    {
        const auto state = ReadState();
        CloseHandle(process.hProcess);
        CloseHandle(readyEvent);
        CloseHandle(failedEvent);
        std::wcerr << L"MonitorSplitter host reported failure.\n";
        if (!state.empty())
        {
            std::wcerr << state;
        }
        return 1;
    }
    if (waitResult == WAIT_OBJECT_0 + 2)
    {
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        CloseHandle(readyEvent);
        CloseHandle(failedEvent);
        std::wcerr << L"MonitorSplitter host exited before becoming ready. Exit code: " << exitCode << L"\n";
        return exitCode == 0 ? 1 : static_cast<int>(exitCode);
    }

    CloseHandle(process.hProcess);
    CloseHandle(readyEvent);
    CloseHandle(failedEvent);
    std::wcerr << L"Timed out waiting for MonitorSplitter host readiness.\n";
    return 1;
}

int EnableDirect()
{
    if (LoadHostTarget().empty())
    {
        std::wcerr
            << L"MonitorSplitter direct enable requires a saved host target. "
            << L"Run MonitorSplitterCtl hosttarget <selector> first.\n";
        return 2;
    }

    auto panelState = QueryPanelState(true);
    if (!PanelReadyForDirect(panelState))
    {
        std::wcerr
            << L"Physical panel is not splitter-ready. Run MonitorSplitterCtl panel split and flip "
            << L"\"Remove display from desktop\" on in Windows Settings.\n";
        PrintPanelStateJson(panelState);
        std::wcout << L"\n";
        return 1;
    }
    SaveDirectTargetFromPanelState(panelState);

    if (MonitorSplitter::Control::MachineConfigExists())
    {
        if (!MonitorSplitter::Control::SaveDesiredEnabled(true))
        {
            std::wcerr << L"Could not write service desired state.\n";
            return 1;
        }
        MonitorSplitter::Control::StartServiceIfNeeded();
        MonitorSplitter::Control::SignalStackWake();
        std::wcout << L"MonitorSplitter enable requested via service.\n";
        return 0;
    }

    SaveServiceDesiredStateIfConfigured(true);

    int result = EnableVirtualDevice();
    if (result != 0)
    {
        return result;
    }

    std::wcout << L"MonitorSplitter direct mode enabled.\n";
    return 0;
}

int StopSplitterStack(bool saveDesiredState)
{
    if (saveDesiredState)
    {
        SaveServiceDesiredStateIfConfigured(false);
    }

    const bool hostWasRunning = IsHostRunning();
    RequestHostStop();
    if (hostWasRunning && !WaitUntilHostStopped(5 * 1000))
    {
        std::wcerr << L"MonitorSplitter host stop requested, but the host did not stop within 5 seconds.\n";
        return 1;
    }

    std::wstring stopReason;
    if (!StopVirtualDeviceHolder(10 * 1000, stopReason))
    {
        std::wcerr << L"MonitorSplitter disable requested, but " << stopReason << L".\n";
        return 1;
    }

    if (!IsRunning())
    {
        std::wcout << L"MonitorSplitter disabled.\n";
        return 0;
    }

    std::wcout << L"MonitorSplitter disabled.\n";
    return 0;
}

int HostStop()
{
    if (!IsHostRunning())
    {
        std::wcout << L"MonitorSplitter host is not running.\n";
        return 0;
    }

    if (!RequestHostStop())
    {
        std::wcerr << L"MonitorSplitter host is running, but its stop event could not be opened.\n";
        return 1;
    }

    if (!WaitUntilHostStopped(5 * 1000))
    {
        std::wcerr << L"MonitorSplitter host stop requested, but the host did not stop within 5 seconds.\n";
        return 1;
    }

    std::wcout << L"MonitorSplitter host stopped.\n";
    return 0;
}

int Status()
{
    const auto state = ReadState();
    const bool running = IsRunning();
    const bool ready = IsEventSignaled(kReadyEventName);
    const bool failed = IsEventSignaled(kFailedEventName);
    const auto savedLayout = LoadLayout();
    MonitorSplitter::Layout activeLayout;
    const bool activeLayoutAvailable = TryLoadLayoutFile(GetActiveLayoutPath(), activeLayout);
    const bool activeLayoutInUse = running && activeLayoutAvailable;
    const auto layout = activeLayoutInUse ? activeLayout : savedLayout;
    const auto layoutSpec = MonitorSplitter::SerializeLayout(layout);

    std::wcout << L"{\"running\":" << (running ? L"true" : L"false");
    std::wcout << L",\"component\":";
    PrintComponentJson(L"MonitorSplitterCtl");
    std::wcout << L",\"ready\":" << (ready ? L"true" : L"false");
    std::wcout << L",\"failed\":" << (failed ? L"true" : L"false");
    std::wcout << L",\"lastState\":\"" << JsonEscape(state) << L"\"";
    std::wcout << L",\"layout\":\"" << JsonEscape(layoutSpec) << L"\"";
    std::wcout << L",\"savedLayout\":\"" << JsonEscape(MonitorSplitter::SerializeLayout(savedLayout)) << L"\"";
    std::wcout << L",\"activeLayoutAvailable\":" << (activeLayoutAvailable ? L"true" : L"false");
    std::wcout << L",\"activeLayoutInUse\":" << (activeLayoutInUse ? L"true" : L"false");
    std::wcout << L",\"activeLayout\":\"" << JsonEscape(activeLayoutAvailable ? MonitorSplitter::SerializeLayout(activeLayout) : L"") << L"\"";
    std::wcout << L",\"hostWidth\":" << layout.HostWidth;
    std::wcout << L",\"height\":" << layout.Height;
    std::wcout << L",\"refresh\":" << layout.Refresh;
    std::wcout << L",\"panelState\":";
    PrintPanelStateJson(QueryPanelState(false));
    std::wcout << L",\"serviceStatus\":";
    PrintRawJsonFileOrNull(MonitorSplitter::Control::ServiceStatusPath());
    std::wcout << L",\"agentStatus\":";
    PrintRawJsonFileOrNull(MonitorSplitter::Control::AgentStatusPath());
    std::wcout << L",\"configStatus\":";
    PrintRawJsonFileOrNull(MonitorSplitter::Control::ConfigStatusPath());
    std::wcout << L",\"monitors\":";
    PrintExpectedMonitorsJson(layout);
    std::wcout << L"}\n";
    return 0;
}

int VersionCommand()
{
    std::wcout << L"{\"component\":";
    PrintComponentJson(L"MonitorSplitterCtl");
    std::wcout << L",\"serviceStatus\":";
    PrintRawJsonFileOrNull(MonitorSplitter::Control::ServiceStatusPath());
    std::wcout << L",\"agentStatus\":";
    PrintRawJsonFileOrNull(MonitorSplitter::Control::AgentStatusPath());
    std::wcout << L",\"configStatus\":";
    PrintRawJsonFileOrNull(MonitorSplitter::Control::ConfigStatusPath());
    std::wcout << L",\"hostStatus\":";
    PrintRawJsonFileOrNull(MonitorSplitter::Control::HostStatusPath());
    std::wcout << L"}\n";
    return 0;
}

int HostStatus()
{
    const bool running = IsHostRunning();
    const auto path = GetHostStatusPath();
    const auto status = MonitorSplitter::Trim(ReadTextFile(path));

    std::wcout << L"{\"running\":" << (running ? L"true" : L"false");
    std::wcout << L",\"path\":\"" << JsonEscape(path) << L"\"";
    std::wcout << L",\"status\":";
    if (status.empty())
    {
        std::wcout << L"null";
    }
    else
    {
        std::wcout << status;
    }
    std::wcout << L"}\n";

    return running ? 0 : 1;
}

bool TargetRefMatches(const DisplayConfigTargetRef& target, const std::wstring& selector)
{
    if (selector.empty())
    {
        return true;
    }

    return SelectorMatchesAnyField(
        selector,
        {
            target.SourceName,
            target.Name,
            target.DevicePath,
            target.StableMonitorId,
            DisplayTargetAdapterIdentity(target.AdapterId, target.TargetId),
        });
}

bool SameDisplayTarget(const DisplayConfigTargetRef& left, const DisplayConfigTargetRef& right)
{
    return left.AdapterId.LowPart == right.AdapterId.LowPart &&
           left.AdapterId.HighPart == right.AdapterId.HighPart &&
           left.TargetId == right.TargetId;
}

void AppendUniqueTarget(std::vector<DisplayConfigTargetRef>& targets, DisplayConfigTargetRef target)
{
    const auto existing = std::find_if(targets.begin(), targets.end(), [&target](const auto& value) {
        return SameDisplayTarget(value, target);
    });
    if (existing == targets.end())
    {
        targets.push_back(std::move(target));
    }
}

std::wstring DirectTargetIdentity(const DisplayConfigTargetRef& target)
{
    if (!target.DevicePath.empty())
    {
        return target.DevicePath;
    }
    if (!target.StableMonitorId.empty())
    {
        return target.StableMonitorId;
    }
    return target.Name;
}

std::wstring DirectTargetAdapterIdentity(const DisplayConfigTargetRef& target)
{
    return DisplayTargetAdapterIdentity(target.AdapterId, target.TargetId);
}

void AppendUniqueSelector(std::vector<std::wstring>& selectors, const std::wstring& selector)
{
    const auto trimmed = MonitorSplitter::Trim(selector);
    if (trimmed.empty())
    {
        return;
    }

    const auto existing = std::find_if(selectors.begin(), selectors.end(), [&trimmed](const std::wstring& value) {
        return EqualsIgnoreCase(value, trimmed);
    });
    if (existing == selectors.end())
    {
        selectors.push_back(trimmed);
    }
}

std::wstring DirectTargetSelectorBundle(const DisplayConfigTargetRef& primary, const DisplayConfigTargetRef& fallback)
{
    std::vector<std::wstring> selectors;
    AppendUniqueSelector(selectors, DirectTargetIdentity(primary));
    AppendUniqueSelector(selectors, primary.StableMonitorId);
    AppendUniqueSelector(selectors, DirectTargetAdapterIdentity(primary));
    AppendUniqueSelector(selectors, primary.SourceName);
    AppendUniqueSelector(selectors, DirectTargetIdentity(fallback));
    AppendUniqueSelector(selectors, fallback.StableMonitorId);
    AppendUniqueSelector(selectors, DirectTargetAdapterIdentity(fallback));
    AppendUniqueSelector(selectors, fallback.SourceName);

    std::wstring result;
    for (const auto& selector : selectors)
    {
        if (!result.empty())
        {
            result += L"\n";
        }
        result += selector;
    }
    return result;
}

void PrintTargetRefJson(const DisplayConfigTargetRef& target)
{
    std::wcout << L"{\"name\":\"" << JsonEscape(target.Name) << L"\"";
    std::wcout << L",\"sourceName\":\"" << JsonEscape(target.SourceName) << L"\"";
    std::wcout << L",\"devicePath\":\"" << JsonEscape(target.DevicePath) << L"\"";
    std::wcout << L",\"stableMonitorId\":\"" << JsonEscape(target.StableMonitorId) << L"\"";
    std::wcout << L",\"adapterLow\":" << static_cast<unsigned long>(target.AdapterId.LowPart);
    std::wcout << L",\"adapterHigh\":" << target.AdapterId.HighPart;
    std::wcout << L",\"targetId\":" << target.TargetId;
    std::wcout << L",\"usageKind\":" << target.UsageKind;
    std::wcout << L",\"fromDisplayCore\":" << (target.FromDisplayCore ? L"true" : L"false");
    std::wcout << L"}";
}

std::wstring LoadPanelTargetSelector()
{
    auto selector = LoadDirectTarget();
    if (selector.empty())
    {
        selector = LoadHostTarget();
    }
    return selector;
}

DisplayConfigTargetRef TargetRefFromDisplayCoreTarget(const displaycore::DisplayTarget& target)
{
    DisplayConfigTargetRef targetRef;
    const auto adapterId = target.Adapter().Id();
    targetRef.AdapterId.LowPart = adapterId.LowPart;
    targetRef.AdapterId.HighPart = adapterId.HighPart;
    targetRef.TargetId = target.AdapterRelativeId();
    targetRef.Name = ToWString(target.DeviceInterfacePath());
    targetRef.DevicePath = ToWString(target.DeviceInterfacePath());
    targetRef.StableMonitorId = ToWString(target.StableMonitorId());
    targetRef.UsageKind = static_cast<INT32>(target.UsageKind());
    targetRef.FromDisplayCore = true;
    return targetRef;
}

void ClassifyPanelState(PanelStateSnapshot& snapshot)
{
    if (!snapshot.TargetConfigured)
    {
        snapshot.State = L"unconfigured";
        snapshot.Message = L"run MonitorSplitterCtl hosttarget <selector> while the physical panel is on the desktop";
        return;
    }
    if (!snapshot.DisplayCoreAvailable)
    {
        snapshot.State = L"unknown";
        snapshot.Message = L"DisplayCore is not available";
        return;
    }
    if (snapshot.TargetAmbiguous)
    {
        snapshot.State = L"ambiguous";
        snapshot.Message = L"multiple connected DisplayCore targets match the saved panel selector";
        return;
    }
    if (!snapshot.TargetMatched)
    {
        snapshot.State = L"missing";
        snapshot.Message = L"no connected DisplayCore target matches the saved panel selector";
        return;
    }
    if (snapshot.HostRunning && !snapshot.ActiveDesktopPathMatched)
    {
        snapshot.State = L"splitter-running";
        snapshot.Message = L"panel is removed from the Windows desktop and currently owned by the MonitorSplitter direct host";
        return;
    }
    if (snapshot.AcquisitionSucceeded)
    {
        snapshot.State = L"splitter-ready";
        snapshot.Message = L"panel is removed from the Windows desktop and free for MonitorSplitter direct output";
        return;
    }
    if (snapshot.ActiveDesktopPathMatched)
    {
        snapshot.State = L"native-desktop";
        snapshot.Message = L"panel is still part of the Windows desktop; run MonitorSplitterCtl panel split";
        return;
    }
    if (snapshot.MonitorSpecialization.QuerySucceeded &&
        snapshot.MonitorSpecialization.IsSpecializationEnabled)
    {
        snapshot.State = L"removed-busy";
        snapshot.Message = L"panel appears removed from the Windows desktop but is not currently acquirable";
        return;
    }

    snapshot.State = L"unknown";
    snapshot.Message = L"panel state could not be proven with public APIs";
}

PanelStateSnapshot QueryPanelState(bool attemptAcquire)
{
    PanelStateSnapshot snapshot;
    snapshot.Selector = LoadPanelTargetSelector();
    snapshot.TargetConfigured = !snapshot.Selector.empty();
    snapshot.HostRunning = IsHostRunning();

    if (!snapshot.TargetConfigured)
    {
        ClassifyPanelState(snapshot);
        return snapshot;
    }

    for (const auto& path : QueryActiveDisplayConfigPaths())
    {
        if (IsMonitorSplitterPath(path))
        {
            continue;
        }

        const auto target = TargetRefFromPath(path);
        if (TargetRefMatches(target, snapshot.Selector))
        {
            snapshot.ActiveDesktopPathMatched = true;
            break;
        }
    }

    try
    {
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        }
        catch (const winrt::hresult_error& error)
        {
            const HRESULT hr = static_cast<HRESULT>(error.code());
            if (hr != RPC_E_CHANGED_MODE)
            {
                throw;
            }
        }

        auto manager = displaycore::DisplayManager::Create(displaycore::DisplayManagerOptions::None);
        displaycore::DisplayTarget matchedTarget{ nullptr };

        for (const auto& target : manager.GetCurrentTargets())
        {
            if (!target.IsConnected())
            {
                continue;
            }

            auto targetRef = TargetRefFromDisplayCoreTarget(target);
            if (!TargetRefMatches(targetRef, snapshot.Selector))
            {
                continue;
            }

            snapshot.MatchCount++;
            if (snapshot.MatchCount == 1)
            {
                snapshot.Target = std::move(targetRef);
                matchedTarget = target;
            }
        }

        snapshot.DisplayCoreAvailable = true;
        snapshot.DisplayCoreHResult = S_OK;
        snapshot.TargetMatched = snapshot.MatchCount == 1;
        snapshot.TargetAmbiguous = snapshot.MatchCount > 1;

        if (snapshot.TargetMatched)
        {
            snapshot.MonitorSpecialization = QueryMonitorSpecialization(snapshot.Target);

            if (attemptAcquire && !snapshot.HostRunning && matchedTarget != nullptr)
            {
                auto targetVector = winrt::single_threaded_vector<displaycore::DisplayTarget>();
                targetVector.Append(matchedTarget);

                snapshot.AcquisitionAttempted = true;
                auto acquireResult = manager.TryAcquireTargetsAndCreateEmptyState(targetVector);
                snapshot.AcquisitionResult = static_cast<INT32>(acquireResult.ErrorCode());
                snapshot.AcquisitionHResult = acquireResult.ExtendedErrorCode();
                snapshot.AcquisitionSucceeded =
                    acquireResult.ErrorCode() == displaycore::DisplayManagerResult::Success &&
                    SUCCEEDED(acquireResult.ExtendedErrorCode());
            }
        }
    }
    catch (const winrt::hresult_error& error)
    {
        snapshot.DisplayCoreAvailable = false;
        snapshot.DisplayCoreHResult = static_cast<HRESULT>(error.code());
        snapshot.DisplayCoreError = ToWString(error.message());
    }
    catch (...)
    {
        snapshot.DisplayCoreAvailable = false;
        snapshot.DisplayCoreHResult = E_FAIL;
        snapshot.DisplayCoreError = L"unknown DisplayCore panel-state failure";
    }

    ClassifyPanelState(snapshot);
    return snapshot;
}

bool PanelReadyForDirect(const PanelStateSnapshot& snapshot)
{
    return snapshot.TargetMatched &&
           !snapshot.TargetAmbiguous &&
           !snapshot.ActiveDesktopPathMatched &&
           (snapshot.AcquisitionSucceeded ||
            snapshot.HostRunning ||
            (snapshot.MonitorSpecialization.QuerySucceeded &&
             snapshot.MonitorSpecialization.IsSpecializationEnabled));
}

bool PanelNativeDesktop(const PanelStateSnapshot& snapshot)
{
    return snapshot.TargetMatched && snapshot.ActiveDesktopPathMatched;
}

void SaveDirectTargetFromPanelState(const PanelStateSnapshot& snapshot)
{
    if (snapshot.TargetMatched)
    {
        SaveDirectTarget(DirectTargetSelectorBundle(snapshot.Target, snapshot.Target));
    }
}

void PrintPanelStateJson(const PanelStateSnapshot& snapshot)
{
    const bool ok = snapshot.TargetConfigured &&
                    snapshot.DisplayCoreAvailable &&
                    snapshot.TargetMatched;

    std::wcout << L"{\"ok\":" << (ok ? L"true" : L"false");
    std::wcout << L",\"state\":\"" << JsonEscape(snapshot.State) << L"\"";
    std::wcout << L",\"message\":\"" << JsonEscape(snapshot.Message) << L"\"";
    std::wcout << L",\"targetConfigured\":" << (snapshot.TargetConfigured ? L"true" : L"false");
    std::wcout << L",\"selector\":\"" << JsonEscape(snapshot.Selector) << L"\"";
    std::wcout << L",\"readyForDirect\":" << (PanelReadyForDirect(snapshot) ? L"true" : L"false");
    std::wcout << L",\"nativeDesktop\":" << (PanelNativeDesktop(snapshot) ? L"true" : L"false");
    std::wcout << L",\"hostRunning\":" << (snapshot.HostRunning ? L"true" : L"false");
    std::wcout << L",\"displayCoreAvailable\":" << (snapshot.DisplayCoreAvailable ? L"true" : L"false");
    std::wcout << L",\"displayCoreHresult\":\"" << JsonEscape(HResultText(snapshot.DisplayCoreHResult)) << L"\"";
    std::wcout << L",\"displayCoreError\":\"" << JsonEscape(snapshot.DisplayCoreError) << L"\"";
    std::wcout << L",\"matchCount\":" << snapshot.MatchCount;
    std::wcout << L",\"targetMatched\":" << (snapshot.TargetMatched ? L"true" : L"false");
    std::wcout << L",\"targetAmbiguous\":" << (snapshot.TargetAmbiguous ? L"true" : L"false");
    std::wcout << L",\"activeDesktopPathMatched\":" << (snapshot.ActiveDesktopPathMatched ? L"true" : L"false");
    std::wcout << L",\"acquisitionAttempted\":" << (snapshot.AcquisitionAttempted ? L"true" : L"false");
    std::wcout << L",\"acquisitionSucceeded\":" << (snapshot.AcquisitionSucceeded ? L"true" : L"false");
    std::wcout << L",\"acquisitionResult\":" << snapshot.AcquisitionResult;
    std::wcout << L",\"acquisitionHresult\":\"" << JsonEscape(HResultText(snapshot.AcquisitionHResult)) << L"\"";
    std::wcout << L",\"target\":";
    if (snapshot.TargetMatched)
    {
        PrintTargetRefJson(snapshot.Target);
    }
    else
    {
        std::wcout << L"null";
    }
    std::wcout << L",\"monitorSpecialization\":";
    PrintMonitorSpecializationJson(snapshot.MonitorSpecialization);
    std::wcout << L"}";
}

int PanelStateCommand()
{
    const auto snapshot = QueryPanelState(true);
    if (PanelReadyForDirect(snapshot))
    {
        SaveDirectTargetFromPanelState(snapshot);
    }
    PrintPanelStateJson(snapshot);
    std::wcout << L"\n";
    return 0;
}

std::wstring PanelDisplayName(const PanelStateSnapshot& snapshot)
{
    if (!snapshot.Target.Name.empty())
    {
        return snapshot.Target.Name;
    }
    if (!snapshot.Target.DevicePath.empty())
    {
        return snapshot.Target.DevicePath;
    }
    return L"the saved physical panel";
}

bool LaunchSettingsUri(const wchar_t* uri)
{
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
}

void PrintPanelToggleInstructions(const PanelStateSnapshot& snapshot, bool split)
{
    std::wcerr
        << L"Settings -> System -> Display -> select "
        << PanelDisplayName(snapshot)
        << L" -> Advanced display -> \"Remove display from desktop\" "
        << (split ? L"ON" : L"OFF")
        << L".\n";
}

int WaitForPanelState(bool split, DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    PanelStateSnapshot lastSnapshot;
    do
    {
        lastSnapshot = QueryPanelState(true);
        if (split && PanelReadyForDirect(lastSnapshot))
        {
            SaveDirectTargetFromPanelState(lastSnapshot);
            PrintPanelStateJson(lastSnapshot);
            std::wcout << L"\n";
            return 0;
        }
        if (!split && PanelNativeDesktop(lastSnapshot))
        {
            PrintPanelStateJson(lastSnapshot);
            std::wcout << L"\n";
            return 0;
        }

        Sleep(750);
    } while (GetTickCount() - start < timeoutMs);

    std::wcerr << L"Timed out waiting for the panel to become " << (split ? L"splitter-ready" : L"native desktop") << L".\n";
    PrintPanelToggleInstructions(lastSnapshot, split);
    PrintPanelStateJson(lastSnapshot);
    std::wcout << L"\n";
    return 1;
}

bool ShouldAttemptNativePanelRestoreAfterDisable(const PanelStateSnapshot& snapshot)
{
    return snapshot.TargetConfigured &&
           snapshot.TargetMatched &&
           !snapshot.TargetAmbiguous &&
           !PanelNativeDesktop(snapshot);
}

int RestoreNativePanelAfterDisable(bool waitForPanel)
{
    auto snapshot = QueryPanelState(true);
    if (PanelNativeDesktop(snapshot))
    {
        std::wcout << L"Physical panel is already part of the Windows desktop.\n";
        return 0;
    }

    if (!snapshot.TargetConfigured)
    {
        std::wcout << L"No physical panel target is configured; panel restore skipped.\n";
        return 0;
    }

    if (!ShouldAttemptNativePanelRestoreAfterDisable(snapshot))
    {
        std::wcerr << L"MonitorSplitter disabled, but the physical panel could not be safely restored automatically: "
                   << snapshot.Message << L"\n";
        PrintPanelStateJson(snapshot);
        std::wcout << L"\n";
        return 1;
    }

    PrintPanelToggleInstructions(snapshot, false);
    return waitForPanel ? WaitForPanelState(false, 180 * 1000) : 1;
}

int DisableCommand(int argc, wchar_t* argv[])
{
    bool restorePanel = true;
    bool waitForPanel = false;
    for (int index = 2; index < argc; index++)
    {
        const std::wstring arg = argv[index];
        if (EqualsIgnoreCase(arg, L"--keep-panel-state") || EqualsIgnoreCase(arg, L"--no-panel-restore"))
        {
            restorePanel = false;
            continue;
        }
        if (EqualsIgnoreCase(arg, L"--wait-panel-restore"))
        {
            waitForPanel = true;
            continue;
        }

        std::wcerr << L"Unknown disable argument: " << arg << L"\n";
        std::wcerr << L"Usage: MonitorSplitterCtl disable [--keep-panel-state] [--wait-panel-restore]\n";
        return 2;
    }

    if (MonitorSplitter::Control::MachineConfigExists())
    {
        if (!MonitorSplitter::Control::SaveDesiredEnabled(false))
        {
            std::wcerr << L"Could not write service desired state.\n";
            return 1;
        }
        MonitorSplitter::Control::RequestServiceRestart();
        MonitorSplitter::Control::SignalStackWake();
        std::wcout << L"MonitorSplitter disable requested via service.\n";

        if (!restorePanel)
        {
            std::wcout << L"Physical panel state was left unchanged.\n";
            return 0;
        }

        return RestoreNativePanelAfterDisable(waitForPanel);
    }

    const int disableResult = StopSplitterStack(true);
    if (disableResult != 0)
    {
        return disableResult;
    }

    if (!restorePanel)
    {
        std::wcout << L"Physical panel state was left unchanged.\n";
        return 0;
    }

    return RestoreNativePanelAfterDisable(waitForPanel);
}

int PanelCommand(int argc, wchar_t* argv[])
{
    if (argc < 3)
    {
        std::wcerr << L"Usage: MonitorSplitterCtl panel <split|native> [--enable] [--open-settings] [--wait]\n";
        return 2;
    }

    const std::wstring action = argv[2];
    const bool split = EqualsIgnoreCase(action, L"split");
    const bool native = EqualsIgnoreCase(action, L"native");
    if (!split && !native)
    {
        std::wcerr << L"panel action must be split or native.\n";
        return 2;
    }

    bool enableAfter = false;
    bool openSettings = false;
    bool waitForPanel = false;
    for (int index = 3; index < argc; index++)
    {
        const std::wstring arg = argv[index];
        if (EqualsIgnoreCase(arg, L"--enable"))
        {
            enableAfter = true;
            continue;
        }
        if (EqualsIgnoreCase(arg, L"--open-settings"))
        {
            openSettings = true;
            continue;
        }
        if (EqualsIgnoreCase(arg, L"--wait"))
        {
            waitForPanel = true;
            continue;
        }

        std::wcerr << L"Unknown panel argument: " << arg << L"\n";
        return 2;
    }

    if (native)
    {
        SaveServiceDesiredStateIfConfigured(false);

        const int disableResult = StopSplitterStack(false);
        if (disableResult != 0)
        {
            return disableResult;
        }
    }

    auto snapshot = QueryPanelState(true);
    if (split && PanelReadyForDirect(snapshot))
    {
        SaveDirectTargetFromPanelState(snapshot);
        PrintPanelStateJson(snapshot);
        std::wcout << L"\n";
        return enableAfter ? EnableDirect() : 0;
    }
    if (native && PanelNativeDesktop(snapshot))
    {
        PrintPanelStateJson(snapshot);
        std::wcout << L"\n";
        return 0;
    }

    PrintPanelToggleInstructions(snapshot, split);
    if (openSettings && !LaunchSettingsUri(L"ms-settings:display-advanced"))
    {
        LaunchSettingsUri(L"ms-settings:display");
    }
    if (!waitForPanel)
    {
        return 1;
    }

    const int waitResult = WaitForPanelState(split, 180 * 1000);
    if (waitResult != 0)
    {
        return waitResult;
    }

    if (split)
    {
        std::wcerr << L"Panel is splitter-ready. Run MonitorSplitterCtl enable to start the splits.\n";
        return enableAfter ? EnableDirect() : 0;
    }

    return 0;
}

struct HostSelectionInputs
{
    MonitorSplitter::Layout Layout;
    std::wstring HostTarget;
};

bool ParseHostSelectionArguments(const wchar_t* commandName, int argc, wchar_t* argv[], HostSelectionInputs& inputs)
{
    inputs.Layout = LoadEffectiveLayout();
    inputs.HostTarget = LoadHostTarget();

    for (int index = 2; index < argc; index++)
    {
        const std::wstring argument = argv[index];
        if (argument == L"--layout")
        {
            if (index + 1 >= argc)
            {
                std::wcerr << commandName << L" --layout requires a layout spec.\n";
                return false;
            }

            MonitorSplitter::Layout layout;
            std::wstring error;
            const std::wstring spec = argv[++index];
            if (!MonitorSplitter::ParseLayoutSpec(spec, layout, &error))
            {
                std::wcerr << L"Invalid layout: " << error << L"\n";
                return false;
            }

            inputs.Layout = layout;
            continue;
        }

        if (argument == L"--hosttarget")
        {
            if (index + 1 >= argc)
            {
                std::wcerr << commandName << L" --hosttarget requires a display selector.\n";
                return false;
            }

            auto target = MonitorSplitter::Trim(argv[++index]);
            if (EqualsIgnoreCase(target, L"clear") ||
                EqualsIgnoreCase(target, L"default") ||
                EqualsIgnoreCase(target, L"none"))
            {
                target.clear();
            }
            inputs.HostTarget = target;
            continue;
        }

        if (argument == L"--no-hosttarget")
        {
            inputs.HostTarget.clear();
            continue;
        }

        std::wcerr << L"Unknown " << commandName << L" argument: " << argument << L"\n";
        return false;
    }

    return true;
}

struct HostTargetResolution
{
    std::vector<size_t> HostCandidates;
    std::vector<size_t> ActiveMatches;
    std::vector<size_t> HostMatches;
};

std::vector<std::wstring> MonitorSelectorFields(
    const MonitorSnapshot& monitor,
    const std::vector<DisplayDeviceSnapshot>& devices,
    const std::vector<DisplayConfigPathSnapshot>& displayConfigPaths)
{
    std::vector<std::wstring> fields;
    fields.push_back(monitor.DeviceName);

    const auto* path = FindDisplayConfigPathBySourceName(displayConfigPaths, monitor.DeviceName);
    if (path != nullptr)
    {
        fields.push_back(path->SourceName);
        fields.push_back(path->TargetFriendlyName);
        fields.push_back(path->TargetDevicePath);
        fields.push_back(DisplayConfigPathTargetAdapterIdentity(*path));
    }

    const auto* adapter = FindDisplayDevice(devices, monitor.DeviceName);
    if (adapter != nullptr)
    {
        fields.push_back(adapter->DeviceName);
        fields.push_back(adapter->DeviceString);
        fields.push_back(adapter->DeviceId);
        for (const auto& child : adapter->Monitors)
        {
            fields.push_back(child.DeviceName);
            fields.push_back(child.DeviceString);
            fields.push_back(child.DeviceId);
        }
    }

    return fields;
}

bool MonitorMatchesHostTargetSelector(
    const MonitorSnapshot& monitor,
    const std::vector<DisplayDeviceSnapshot>& devices,
    const std::vector<DisplayConfigPathSnapshot>& displayConfigPaths,
    const std::wstring& selector)
{
    if (MonitorSplitter::Trim(selector).empty())
    {
        return false;
    }

    return SelectorMatchesAnyField(selector, MonitorSelectorFields(monitor, devices, displayConfigPaths));
}

HostTargetResolution ResolveHostTarget(
    const MonitorSplitter::Layout& layout,
    const std::vector<DisplayDeviceSnapshot>& devices,
    const std::vector<DisplayConfigPathSnapshot>& displayConfigPaths,
    const std::wstring& selector)
{
    HostTargetResolution resolution;
    const bool selectorConfigured = !MonitorSplitter::Trim(selector).empty();
    for (size_t index = 0; index < g_Monitors.size(); index++)
    {
        const bool hostCandidate = IsHostCandidate(g_Monitors[index], devices, layout);
        if (hostCandidate)
        {
            resolution.HostCandidates.push_back(index);
        }

        if (!selectorConfigured)
        {
            continue;
        }

        if (MonitorMatchesHostTargetSelector(g_Monitors[index], devices, displayConfigPaths, selector))
        {
            resolution.ActiveMatches.push_back(index);
            if (hostCandidate)
            {
                resolution.HostMatches.push_back(index);
            }
        }
    }
    return resolution;
}

size_t ResolvedHostIndex(const HostTargetResolution& resolution, bool hostTargetConfigured)
{
    if (hostTargetConfigured)
    {
        return resolution.HostMatches.size() == 1 ? resolution.HostMatches[0] : g_Monitors.size();
    }

    return resolution.HostCandidates.empty() ? g_Monitors.size() : resolution.HostCandidates[0];
}

std::wstring JoinSelectorBundle(const std::vector<std::wstring>& selectors)
{
    std::wstring result;
    for (const auto& selector : selectors)
    {
        if (!result.empty())
        {
            result += L"\n";
        }
        result += selector;
    }
    return result;
}

std::wstring HostTargetSelectorBundle(
    const MonitorSnapshot& monitor,
    const std::vector<DisplayDeviceSnapshot>& devices,
    const std::vector<DisplayConfigPathSnapshot>& displayConfigPaths)
{
    std::vector<std::wstring> selectors;
    const auto* path = FindDisplayConfigPathBySourceName(displayConfigPaths, monitor.DeviceName);
    if (path != nullptr)
    {
        AppendUniqueSelector(selectors, path->TargetDevicePath);
        AppendUniqueSelector(selectors, DisplayConfigPathTargetAdapterIdentity(*path));
        AppendUniqueSelector(selectors, path->TargetFriendlyName);
    }

    if (selectors.empty())
    {
        const auto* adapter = FindDisplayDevice(devices, monitor.DeviceName);
        if (adapter != nullptr)
        {
            for (const auto& child : adapter->Monitors)
            {
                AppendUniqueSelector(selectors, child.DeviceId);
            }
        }
    }

    if (selectors.empty())
    {
        AppendUniqueSelector(selectors, monitor.DeviceName);
    }

    return JoinSelectorBundle(selectors);
}

int LayoutCommand(int argc, wchar_t* argv[])
{
    if (argc == 2)
    {
        const auto layout = LoadLayout();
        std::wcout << MonitorSplitter::SerializeLayout(layout) << L"\n";
        return 0;
    }

    MonitorSplitter::Layout layout;
    std::wstring error;
    const auto spec = JoinArgs(argc, argv, 2);
    if (!MonitorSplitter::ParseLayoutSpec(spec, layout, &error))
    {
        std::wcerr << L"Invalid layout: " << error << L"\n";
        return 2;
    }

    if (!SaveLayout(layout))
    {
        std::wcerr << L"Could not write layout config: " << GetLayoutPath() << L"\n";
        return 1;
    }

    std::wcout << L"Layout set to " << MonitorSplitter::SerializeLayout(layout) << L"\n";
    if (RequestServiceRestartIfConfigured())
    {
        std::wcout << L"Requested MonitorSplitter service restart for the new layout.\n";
    }
    else if (IsRunning())
    {
        std::wcout << L"Restart MonitorSplitter for the new layout to affect Windows display enumeration.\n";
    }
    return 0;
}

int HostTargetCommand(int argc, wchar_t* argv[])
{
    if (argc == 2 || (argc > 2 && std::wstring(argv[2]).rfind(L"--", 0) == 0))
    {
        HostSelectionInputs inputs;
        if (!ParseHostSelectionArguments(L"hosttarget", argc, argv, inputs))
        {
            return 2;
        }

        const auto& layout = inputs.Layout;
        const auto& target = inputs.HostTarget;
        g_Monitors.clear();
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
        const auto devices = EnumerateDisplayDevices();
        const auto displayConfigPaths = QueryActiveDisplayConfigPaths();
        const bool targetConfigured = !target.empty();
        const auto resolution = ResolveHostTarget(layout, devices, displayConfigPaths, target);
        const bool targetAmbiguous = targetConfigured && resolution.HostMatches.size() > 1;
        const bool targetMatched = !targetConfigured || resolution.HostMatches.size() == 1;
        const size_t resolvedIndex = ResolvedHostIndex(resolution, targetConfigured);
        const bool resolved = resolvedIndex < g_Monitors.size();

        std::wcout << L"{\"target\":\"" << JsonEscape(target) << L"\"";
        std::wcout << L",\"configured\":" << (targetConfigured ? L"true" : L"false");
        std::wcout << L",\"targetMatched\":" << (targetMatched ? L"true" : L"false");
        std::wcout << L",\"targetAmbiguous\":" << (targetAmbiguous ? L"true" : L"false");
        std::wcout << L",\"targetActiveMatchCount\":" << resolution.ActiveMatches.size();
        std::wcout << L",\"targetHostMatchCount\":" << resolution.HostMatches.size();
        std::wcout << L",\"hostCandidateCount\":" << resolution.HostCandidates.size();
        std::wcout << L",\"resolvedTarget\":\"" << JsonEscape(resolved ? g_Monitors[resolvedIndex].DeviceName : L"") << L"\"";
        std::wcout << L",\"layout\":\"" << JsonEscape(MonitorSplitter::SerializeLayout(layout)) << L"\"";
        std::wcout << L",\"path\":\"" << JsonEscape(GetHostTargetPath()) << L"\"";
        std::wcout << L",\"hostCandidates\":[";
        bool first = true;
        for (const auto& monitor : g_Monitors)
        {
            if (!IsHostCandidate(monitor, devices, layout))
            {
                continue;
            }
            if (!first)
            {
                std::wcout << L",";
            }
            first = false;
            PrintMonitorSnapshotJson(monitor, devices, &displayConfigPaths);
        }
        std::wcout << L"],\"activeMonitors\":[";
        for (size_t index = 0; index < g_Monitors.size(); index++)
        {
            if (index != 0)
            {
                std::wcout << L",";
            }
            PrintMonitorSnapshotJson(g_Monitors[index], devices, &displayConfigPaths);
        }
        std::wcout << L"]}\n";
        return 0;
    }

    const auto target = MonitorSplitter::Trim(JoinArgs(argc, argv, 2));
    if (EqualsIgnoreCase(target, L"clear") || EqualsIgnoreCase(target, L"default"))
    {
        DeleteFileW(GetHostTargetPath().c_str());
        DeleteFileW(MonitorSplitter::Control::EdidNameBasePath().c_str());
        std::wcout << L"Host target cleared.\n";
        if (RequestServiceRestartIfConfigured())
        {
            std::wcout << L"Requested MonitorSplitter service restart for the cleared target.\n";
        }
        else if (IsHostRunning())
        {
            std::wcout << L"Restart MonitorSplitter for the new target to affect compositor placement and render-adapter preference.\n";
        }
        return 0;
    }

    if (target.empty())
    {
        std::wcerr << L"Host target cannot be empty. Use hosttarget clear to remove the preference.\n";
        return 2;
    }

    const auto layout = LoadEffectiveLayout();
    g_Monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
    const auto devices = EnumerateDisplayDevices();
    const auto displayConfigPaths = QueryActiveDisplayConfigPaths();
    const auto resolution = ResolveHostTarget(layout, devices, displayConfigPaths, target);
    if (resolution.HostMatches.empty())
    {
        if (resolution.ActiveMatches.empty())
        {
            std::wcerr << L"Host target selector did not match any active monitor. Run hosttarget to list available selectors.\n";
        }
        else
        {
            std::wcerr << L"Host target selector matched an active monitor, but it is not a physical monitor at least the configured host size.\n";
        }
        return 2;
    }
    if (resolution.HostMatches.size() != 1)
    {
        std::wcerr << L"Host target selector matched multiple physical host candidates. Use a more specific selector.\n";
        return 2;
    }

    const size_t resolvedIndex = resolution.HostMatches[0];
    auto savedTarget = HostTargetSelectorBundle(g_Monitors[resolvedIndex], devices, displayConfigPaths);
    if (savedTarget.empty())
    {
        savedTarget = target;
    }

    const auto* resolvedPath = FindDisplayConfigPathBySourceName(displayConfigPaths, g_Monitors[resolvedIndex].DeviceName);
    auto edidNameBase = MonitorSplitter::Trim(resolvedPath == nullptr ? L"" : resolvedPath->TargetFriendlyName);
    if (edidNameBase.empty() && resolvedPath != nullptr)
    {
        edidNameBase = MonitorSplitter::Control::ReadPhysicalEdidNameFromTargetDevicePath(resolvedPath->TargetDevicePath);
    }
    savedTarget = MonitorSplitter::Control::SelectorWithEdidNameBaseMetadata(savedTarget, edidNameBase);

    if (!SaveHostTarget(savedTarget))
    {
        std::wcerr << L"Could not write host target config: " << GetHostTargetPath() << L"\n";
        return 1;
    }

    if (!edidNameBase.empty() && !MonitorSplitter::Control::SaveEdidNameBase(edidNameBase))
    {
        std::wcerr << L"Could not write EDID name base config: " << MonitorSplitter::Control::EdidNameBasePath() << L"\n";
        return 1;
    }

    std::wcout << L"Host target set to " << g_Monitors[resolvedIndex].DeviceName << L"\n";
    if (!EqualsIgnoreCase(target, savedTarget))
    {
        std::wcout << L"Saved target selector:\n" << savedTarget << L"\n";
    }
    if (!edidNameBase.empty())
    {
        std::wcout << L"Saved EDID name base: " << edidNameBase << L"\n";
    }
    if (RequestServiceRestartIfConfigured())
    {
        std::wcout << L"Requested MonitorSplitter service restart for the new target.\n";
    }
    else if (IsHostRunning())
    {
        std::wcout << L"Restart MonitorSplitter for the new target to affect compositor placement and render-adapter preference.\n";
    }
    return 0;
}

void PrintEdidBaseJson(
    const MonitorSplitter::Layout& layout,
    const std::wstring& action,
    bool ok,
    const std::wstring& before,
    const std::wstring& after,
    const std::wstring& resolved = {},
    bool restartRequested = false)
{
    std::wcout << L"{\"ok\":" << (ok ? L"true" : L"false");
    std::wcout << L",\"action\":\"" << JsonEscape(action) << L"\"";
    std::wcout << L",\"path\":\"" << JsonEscape(MonitorSplitter::Control::EdidNameBasePath()) << L"\"";
    std::wcout << L",\"layout\":\"" << JsonEscape(MonitorSplitter::SerializeLayout(layout)) << L"\"";
    std::wcout << L",\"hostTargetConfigured\":" << (MonitorSplitter::Trim(LoadHostTarget()).empty() ? L"false" : L"true");
    std::wcout << L",\"before\":\"" << JsonEscape(before) << L"\"";
    std::wcout << L",\"after\":\"" << JsonEscape(after) << L"\"";
    std::wcout << L",\"changed\":" << (_wcsicmp(before.c_str(), after.c_str()) == 0 ? L"false" : L"true");
    std::wcout << L",\"restartRequested\":" << (restartRequested ? L"true" : L"false");
    if (!resolved.empty())
    {
        std::wcout << L",\"resolved\":\"" << JsonEscape(resolved) << L"\"";
    }
    std::wcout << L",\"expectedNames\":[";
    for (size_t index = 0; index < layout.Monitors.size(); index++)
    {
        if (index != 0)
        {
            std::wcout << L",";
        }
        std::wcout << L"\"" << JsonEscape(MonitorSplitter::Control::ExpectedEdidFriendlyName(layout.Monitors.size(), index, after)) << L"\"";
    }
    std::wcout << L"]}\n";
}

int EdidBaseCommand(int argc, wchar_t* argv[])
{
    const auto layout = LoadLayout();
    const auto before = MonitorSplitter::Control::LoadEdidNameBase();
    if (argc == 2 || EqualsIgnoreCase(argv[2], L"status"))
    {
        PrintEdidBaseJson(layout, L"status", true, before, before);
        return 0;
    }

    const std::wstring action = argv[2];
    if (EqualsIgnoreCase(action, L"resolve"))
    {
        const auto resolved = MonitorSplitter::Control::ResolveEdidNameBase(layout, false);
        const auto after = MonitorSplitter::Control::LoadEdidNameBase();
        const bool ok = !MonitorSplitter::Trim(after).empty();
        const bool restartRequested = ok && !EqualsIgnoreCase(before, after) && RequestServiceRestartIfConfigured();
        PrintEdidBaseJson(layout, L"resolve", ok, before, after, resolved, restartRequested);
        return ok ? 0 : 1;
    }

    if (EqualsIgnoreCase(action, L"clear"))
    {
        const BOOL deleted = DeleteFileW(MonitorSplitter::Control::EdidNameBasePath().c_str());
        const DWORD error = deleted ? ERROR_SUCCESS : GetLastError();
        const bool ok = deleted || error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        const auto after = MonitorSplitter::Control::LoadEdidNameBase();
        const bool restartRequested = ok && !EqualsIgnoreCase(before, after) && RequestServiceRestartIfConfigured();
        PrintEdidBaseJson(layout, L"clear", ok, before, after, {}, restartRequested);
        return ok ? 0 : 1;
    }

    if (EqualsIgnoreCase(action, L"set"))
    {
        if (argc < 4)
        {
            std::wcerr << L"edidbase set requires a name.\n";
            return 2;
        }
        const auto nameBase = MonitorSplitter::Trim(JoinArgs(argc, argv, 3));
        if (nameBase.empty())
        {
            std::wcerr << L"edidbase set requires a non-empty name.\n";
            return 2;
        }
        const bool saved = MonitorSplitter::Control::SaveEdidNameBase(nameBase);
        const auto after = MonitorSplitter::Control::LoadEdidNameBase();
        const bool restartRequested = saved && !EqualsIgnoreCase(before, after) && RequestServiceRestartIfConfigured();
        PrintEdidBaseJson(layout, L"set", saved, before, after, after, restartRequested);
        return saved ? 0 : 1;
    }

    std::wcerr << L"Usage: MonitorSplitterCtl edidbase [status|resolve|clear|set <name>]\n";
    return 2;
}

bool RequireSelfTest(bool condition, const wchar_t* message)
{
    if (!condition)
    {
        std::wcerr << L"selftest failed: " << message << L"\n";
        return false;
    }
    return true;
}

WORD EdidProductCode(const std::array<BYTE, MonitorSplitter::kSyntheticEdidSize>& edid)
{
    return static_cast<WORD>(static_cast<WORD>(edid[11]) << 8 | edid[10]);
}

DWORD EdidSerial(const std::array<BYTE, MonitorSplitter::kSyntheticEdidSize>& edid)
{
    return static_cast<DWORD>(edid[12]) |
           (static_cast<DWORD>(edid[13]) << 8) |
           (static_cast<DWORD>(edid[14]) << 16) |
           (static_cast<DWORD>(edid[15]) << 24);
}

std::wstring SelfTestHostStatusJson(
    DWORD tick,
    ULONGLONG runtimeTick,
    DWORD pid,
    unsigned long long sourceCount,
    unsigned long long expectedSourceCount,
    unsigned long long healthyFrameSourceCount,
    unsigned long long publishingFrameSourceCount)
{
    std::wstringstream status;
    status << L"{\"running\":true";
    status << L",\"pid\":" << pid;
    status << L",\"mode\":\"direct-shared\"";
    status << L",\"sourceCount\":" << sourceCount;
    status << L",\"expectedSourceCount\":" << expectedSourceCount;
    status << L",\"healthyFrameSourceCount\":" << healthyFrameSourceCount;
    status << L",\"publishingFrameSourceCount\":" << publishingFrameSourceCount;
    status << L",\"usingSharedFrames\":true";
    status << L",\"lastDisplayTaskResult\":\"0x00000000\"";
    status << L",\"lastDisplayTaskPresentStatus\":0";
    status << L",\"lastDisplayTaskSourceStatus\":0";
    status << L",\"presentedFrames\":1";
    status << L",\"lastPresentResult\":\"0x00000000\"";
    status << L",\"updatedTick\":" << tick;
    status << L",\"direct\":{";
    status << L"\"targetAcquired\":true";
    status << L",\"deviceCreated\":true";
    status << L",\"sourceCreated\":true";
    status << L",\"taskPoolCreated\":true";
    status << L",\"fenceReady\":true";
    status << L",\"displayTaskSubmitAttempts\":1";
    status << L",\"displayTaskSuccesses\":1";
    status << L",\"displayTaskFailures\":0";
    status << L",\"lastSubmitTick\":" << tick;
    status << L"}";
    status << L",\"frameSources\":[";
    for (unsigned long long index = 0; index < expectedSourceCount; index++)
    {
        if (index != 0)
        {
            status << L",";
        }
        status << L"{\"driverRuntimeMapped\":true";
        status << L",\"driverRuntimeValid\":true";
        status << L",\"driverRuntimeVersion\":" << MonitorSplitter::kDriverRuntimeStatusVersion;
        status << L",\"driverRuntimeUpdatedTick\":" << runtimeTick;
        status << L"}";
    }
    status << L"]";
    status << L"}";
    return status.str();
}

int SelfTestCommand()
{
    const MonitorSplitter::MonitorMode left{ 1280, 1440, 120 };
    const MonitorSplitter::MonitorMode center{ 2560, 1440, 120 };
    std::array<BYTE, MonitorSplitter::kSyntheticEdidSize> first = {};
    std::array<BYTE, MonitorSplitter::kSyntheticEdidSize> second = {};
    std::array<BYTE, MonitorSplitter::kSyntheticEdidSize> changedBase = {};
    std::array<BYTE, MonitorSplitter::kSyntheticEdidSize> changedMode = {};
    std::array<BYTE, MonitorSplitter::kSyntheticEdidSize> changedIndex = {};

    if (!RequireSelfTest(MonitorSplitter::BuildSyntheticEdid(left, 3, 0, first, "C49RG9x"), L"could not build baseline EDID") ||
        !RequireSelfTest(MonitorSplitter::BuildSyntheticEdid(left, 3, 0, second, "C49RG9x"), L"could not build repeat EDID") ||
        !RequireSelfTest(MonitorSplitter::BuildSyntheticEdid(left, 3, 0, changedBase, "OdysseyG70D"), L"could not build changed-base EDID") ||
        !RequireSelfTest(MonitorSplitter::BuildSyntheticEdid(center, 3, 0, changedMode, "C49RG9x"), L"could not build changed-mode EDID") ||
        !RequireSelfTest(MonitorSplitter::BuildSyntheticEdid(left, 3, 1, changedIndex, "C49RG9x"), L"could not build changed-index EDID"))
    {
        return 1;
    }

    if (!RequireSelfTest(MonitorSplitter::IsSyntheticEdidChecksumValid(first), L"baseline EDID checksum is invalid") ||
        !RequireSelfTest(MonitorSplitter::IsSyntheticEdidChecksumValid(second), L"repeat EDID checksum is invalid") ||
        !RequireSelfTest(MonitorSplitter::IsSyntheticEdidChecksumValid(changedBase), L"changed-base EDID checksum is invalid") ||
        !RequireSelfTest(MonitorSplitter::IsSyntheticEdidChecksumValid(changedMode), L"changed-mode EDID checksum is invalid") ||
        !RequireSelfTest(MonitorSplitter::IsSyntheticEdidChecksumValid(changedIndex), L"changed-index EDID checksum is invalid"))
    {
        return 1;
    }

    if (!RequireSelfTest(first == second, L"same EDID inputs did not produce a stable binary identity") ||
        !RequireSelfTest(EdidProductCode(first) == EdidProductCode(second), L"same EDID inputs produced different product codes") ||
        !RequireSelfTest(EdidSerial(first) == EdidSerial(second), L"same EDID inputs produced different serials"))
    {
        return 1;
    }

    if (!RequireSelfTest(EdidSerial(first) == EdidSerial(changedBase), L"EDID serial changed when the base name changed") ||
        !RequireSelfTest(EdidProductCode(first) == EdidProductCode(changedBase), L"EDID product code changed when the base name changed") ||
        !RequireSelfTest(EdidSerial(first) == EdidSerial(changedMode), L"EDID serial changed when split geometry changed") ||
        !RequireSelfTest(EdidProductCode(first) == EdidProductCode(changedMode), L"EDID product code changed when split geometry changed") ||
        !RequireSelfTest(EdidSerial(first) != EdidSerial(changedIndex), L"EDID serial did not change when split index changed") ||
        !RequireSelfTest(EdidProductCode(first) != EdidProductCode(changedIndex), L"EDID product code did not change when split index changed"))
    {
        return 1;
    }

    MonitorSplitter::MonitorMode decoded = {};
    if (!RequireSelfTest(MonitorSplitter::DecodeSyntheticEdidMode(first.data(), first.size(), decoded), L"could not decode baseline EDID mode") ||
        !RequireSelfTest(decoded.Width == left.Width && decoded.Height == left.Height && decoded.Refresh == left.Refresh, L"decoded baseline EDID mode does not match input"))
    {
        return 1;
    }

    const std::wstring expectedName = MonitorSplitter::EdidMonitorNameWide(3, 0, L"C49RG9x");
    std::vector<BYTE> edidBytes(first.begin(), first.end());
    const std::wstring decodedName = MonitorSplitter::Control::DecodeEdidMonitorName(edidBytes);
    if (!RequireSelfTest(decodedName == expectedName, L"decoded EDID monitor name does not match expected base/index name"))
    {
        return 1;
    }

    const auto selectorWithMetadata = MonitorSplitter::Control::SelectorWithEdidNameBaseMetadata(
        L"msp:edid-name-base=OldName\n\\\\?\\DISPLAY#SAM0F9C#test\nadapter:100:0:4353",
        L"C49RG9x");
    if (!RequireSelfTest(
            MonitorSplitter::Control::ReadEdidNameBaseFromSelectorMetadata(selectorWithMetadata) == L"C49RG9x",
            L"selector metadata did not preserve the EDID name base"))
    {
        return 1;
    }

    MonitorSplitter::Control::DisplayPathSnapshot selectorPath;
    selectorPath.TargetDevicePath = L"\\\\?\\DISPLAY#SAM0F9C#test";
    selectorPath.TargetFriendlyName = L"C49RG9x";
    selectorPath.TargetAdapterId.LowPart = 100;
    selectorPath.TargetAdapterId.HighPart = 0;
    selectorPath.TargetId = 4353;
    if (!RequireSelfTest(
            MonitorSplitter::Control::SelectorMatchesDisplayPath(selectorWithMetadata, selectorPath),
            L"selector metadata prevented matching the saved display path") ||
        !RequireSelfTest(
            !MonitorSplitter::Control::SelectorMatchesDisplayPath(L"msp:edid-name-base=C49RG9x", selectorPath),
            L"selector metadata was treated as a display-path selector"))
    {
        return 1;
    }

    PanelStateSnapshot unconfiguredPanel;
    PanelStateSnapshot nativePanel;
    nativePanel.TargetConfigured = true;
    nativePanel.TargetMatched = true;
    nativePanel.ActiveDesktopPathMatched = true;
    PanelStateSnapshot splitterReadyPanel;
    splitterReadyPanel.TargetConfigured = true;
    splitterReadyPanel.TargetMatched = true;
    splitterReadyPanel.AcquisitionSucceeded = true;
    PanelStateSnapshot ambiguousPanel = splitterReadyPanel;
    ambiguousPanel.TargetAmbiguous = true;
    if (!RequireSelfTest(
            !ShouldAttemptNativePanelRestoreAfterDisable(unconfiguredPanel),
            L"unconfigured panel should not request native restore after disable") ||
        !RequireSelfTest(
            !ShouldAttemptNativePanelRestoreAfterDisable(nativePanel),
            L"native panel should not request native restore after disable") ||
        !RequireSelfTest(
            ShouldAttemptNativePanelRestoreAfterDisable(splitterReadyPanel),
            L"splitter-ready panel should request native restore after disable") ||
        !RequireSelfTest(
            !ShouldAttemptNativePanelRestoreAfterDisable(ambiguousPanel),
            L"ambiguous panel should not request native restore after disable"))
    {
        return 1;
    }

    constexpr DWORD expectedPid = 1234;
    const DWORD now = GetTickCount();
    const ULONGLONG now64 = GetTickCount64();
    std::wstring healthReason;
    const auto healthyHostStatus = SelfTestHostStatusJson(now, now64, expectedPid, 3, 3, 3, 3);
    if (!RequireSelfTest(
            MonitorSplitter::Control::HostStatusHealthy(healthyHostStatus, &healthReason, expectedPid),
            L"healthy synthetic host status was rejected"))
    {
        return 1;
    }

    const auto unhealthySourceStatus = SelfTestHostStatusJson(now, now64, expectedPid, 3, 3, 2, 3);
    healthReason.clear();
    if (!RequireSelfTest(
            !MonitorSplitter::Control::HostStatusHealthy(unhealthySourceStatus, &healthReason, expectedPid) &&
                healthReason == L"not all split frame sources are healthy",
            L"host health accepted or misreported an unhealthy split source"))
    {
        return 1;
    }

    const auto nonPublishingSourceStatus = SelfTestHostStatusJson(now, now64, expectedPid, 3, 3, 3, 2);
    healthReason.clear();
    if (!RequireSelfTest(
            !MonitorSplitter::Control::HostStatusHealthy(nonPublishingSourceStatus, &healthReason, expectedPid) &&
                healthReason == L"not all split frame sources are publishing frames",
            L"host health accepted or misreported a non-publishing split source"))
    {
        return 1;
    }

    const auto staleRuntimeStatus = SelfTestHostStatusJson(now, now64 - 6000, expectedPid, 3, 3, 3, 3);
    healthReason.clear();
    if (!RequireSelfTest(
            !MonitorSplitter::Control::HostStatusHealthy(staleRuntimeStatus, &healthReason, expectedPid) &&
                healthReason.find(L"split frame producer runtime heartbeat is stale") == 0,
            L"host health accepted or misreported a stale split producer runtime heartbeat"))
    {
        return 1;
    }

    healthReason.clear();
    if (!RequireSelfTest(
            !MonitorSplitter::Control::HostStatusHealthy(healthyHostStatus, &healthReason, expectedPid + 1) &&
                healthReason == L"direct host status belongs to a different process",
            L"host health accepted or misreported a stale process id"))
    {
        return 1;
    }

    if (!RequireSelfTest(
            !MonitorSplitter::Control::ShouldCleanupStaleLocalStackBeforeFreshStart(false, false) &&
                MonitorSplitter::Control::ShouldCleanupStaleLocalStackBeforeFreshStart(true, false) &&
                MonitorSplitter::Control::ShouldCleanupStaleLocalStackBeforeFreshStart(false, true) &&
                MonitorSplitter::Control::ShouldCleanupStaleLocalStackBeforeFreshStart(true, true),
            L"stale local stack cleanup decision selftest failed"))
    {
        return 1;
    }

    wchar_t tempDirectory[MAX_PATH] = {};
    const DWORD tempDirectoryLength = GetTempPathW(ARRAYSIZE(tempDirectory), tempDirectory);
    std::wstring writeSelfTestPath = (tempDirectoryLength > 0 && tempDirectoryLength < ARRAYSIZE(tempDirectory))
        ? std::wstring(tempDirectory)
        : std::wstring(L".\\");
    writeSelfTestPath += L"MonitorSplitterCtl.WriteTextFileSelfTest.";
    writeSelfTestPath += std::to_wstring(GetCurrentProcessId());
    writeSelfTestPath += L".txt";
    DeleteFileW(writeSelfTestPath.c_str());

    const bool writeSelfTestOk =
        MonitorSplitter::Control::WriteTextFile(writeSelfTestPath, L"alpha") &&
        MonitorSplitter::Trim(MonitorSplitter::Control::ReadTextFile(writeSelfTestPath)) == L"alpha" &&
        MonitorSplitter::Control::WriteTextFile(writeSelfTestPath, L"beta") &&
        MonitorSplitter::Trim(MonitorSplitter::Control::ReadTextFile(writeSelfTestPath)) == L"beta";
    DeleteFileW(writeSelfTestPath.c_str());
    if (!RequireSelfTest(writeSelfTestOk, L"atomic text-file write/read selftest failed"))
    {
        return 1;
    }

    std::wstring configSelfTestDir = (tempDirectoryLength > 0 && tempDirectoryLength < ARRAYSIZE(tempDirectory))
        ? std::wstring(tempDirectory)
        : std::wstring(L".\\");
    configSelfTestDir += L"MonitorSplitterCtl.ConfigSelfTest.";
    configSelfTestDir += std::to_wstring(GetCurrentProcessId());

    struct ScopedConfigDirectory
    {
        explicit ScopedConfigDirectory(const std::wstring& path)
        {
            std::vector<wchar_t> buffer(32768, L'\0');
            const DWORD length = GetEnvironmentVariableW(
                L"MONITORSPLITTER_CONFIGDIR",
                buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (length > 0 && length < buffer.size())
            {
                HadPrevious = true;
                Previous.assign(buffer.data(), length);
            }
            SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", path.c_str());
        }

        ~ScopedConfigDirectory()
        {
            SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", HadPrevious ? Previous.c_str() : nullptr);
        }

        bool HadPrevious = false;
        std::wstring Previous;
    };

    bool edidMetadataRepairOk = false;
    if (CreateDirectoryW(configSelfTestDir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        {
            ScopedConfigDirectory scopedConfig(configSelfTestDir);
            const auto hostTargetPath = MonitorSplitter::Control::HostTargetPath();
            const auto edidNameBasePath = MonitorSplitter::Control::EdidNameBasePath();
            DeleteFileW(hostTargetPath.c_str());
            DeleteFileW(edidNameBasePath.c_str());

            edidMetadataRepairOk =
                MonitorSplitter::Control::WriteTextFile(hostTargetPath, L"adapter:100:0:4353") &&
                MonitorSplitter::Control::SaveEdidNameBase(L"C49RG9x") &&
                MonitorSplitter::Control::EnsureHostTargetEdidNameBaseMetadata(L"C49RG9x") &&
                MonitorSplitter::Control::ReadEdidNameBaseFromSelectorMetadata(
                    MonitorSplitter::Control::LoadHostTarget()) == L"C49RG9x";

            if (edidMetadataRepairOk)
            {
                DeleteFileW(edidNameBasePath.c_str());
                const auto resolved = MonitorSplitter::Control::ResolveEdidNameBase(
                    MonitorSplitter::DefaultLayout(),
                    false);
                edidMetadataRepairOk =
                    resolved == L"C49RG9x" &&
                    MonitorSplitter::Control::LoadEdidNameBase() == L"C49RG9x";
            }

            DeleteFileW(hostTargetPath.c_str());
            DeleteFileW(edidNameBasePath.c_str());
        }
        RemoveDirectoryW(configSelfTestDir.c_str());
    }
    if (!RequireSelfTest(edidMetadataRepairOk, L"EDID name-base metadata repair selftest failed"))
    {
        return 1;
    }

    std::wcout << L"{\"ok\":true";
    std::wcout << L",\"baselineName\":\"" << JsonEscape(decodedName) << L"\"";
    std::wcout << L",\"baselineProductCode\":" << EdidProductCode(first);
    std::wcout << L",\"baselineSerial\":" << EdidSerial(first);
    std::wcout << L",\"changedBaseProductCode\":" << EdidProductCode(changedBase);
    std::wcout << L",\"changedBaseSerial\":" << EdidSerial(changedBase);
    std::wcout << L",\"changedModeProductCode\":" << EdidProductCode(changedMode);
    std::wcout << L",\"changedModeSerial\":" << EdidSerial(changedMode);
    std::wcout << L",\"changedIndexProductCode\":" << EdidProductCode(changedIndex);
    std::wcout << L",\"changedIndexSerial\":" << EdidSerial(changedIndex);
    std::wcout << L",\"selectorMetadataSelftest\":true";
    std::wcout << L",\"panelRestoreSelftest\":true";
    std::wcout << L",\"hostHealthSelftest\":true";
    std::wcout << L",\"staleStackCleanupSelftest\":true";
    std::wcout << L",\"atomicWriteSelftest\":true";
    std::wcout << L",\"edidMetadataRepairSelftest\":true";
    std::wcout << L"}\n";
    return 0;
}

bool ApplyOptionalLayoutArgument(int argc, wchar_t* argv[], int first)
{
    if (argc <= first)
    {
        return true;
    }

    MonitorSplitter::Layout layout;
    std::wstring error;
    const auto spec = JoinArgs(argc, argv, first);
    if (!MonitorSplitter::ParseLayoutSpec(spec, layout, &error))
    {
        std::wcerr << L"Invalid layout: " << error << L"\n";
        return false;
    }

    if (!SaveLayout(layout))
    {
        std::wcerr << L"Could not write layout config: " << GetLayoutPath() << L"\n";
        return false;
    }

    return true;
}

bool ParseEnableArguments(int argc, wchar_t* argv[])
{
    std::vector<std::wstring> layoutParts;
    for (int index = 2; index < argc; index++)
    {
        const std::wstring arg = argv[index];
        if (!arg.empty() && arg[0] == L'-')
        {
            std::wcerr << L"Unknown enable argument: " << arg << L"\n";
            return false;
        }

        for (; index < argc; index++)
        {
            layoutParts.push_back(argv[index]);
        }
        break;
    }

    if (layoutParts.empty())
    {
        return true;
    }

    MonitorSplitter::Layout layout;
    std::wstring error;
    std::wstring spec;
    for (const auto& part : layoutParts)
    {
        if (!spec.empty())
        {
            spec += L" ";
        }
        spec += part;
    }

    if (!MonitorSplitter::ParseLayoutSpec(spec, layout, &error))
    {
        std::wcerr << L"Invalid layout: " << error << L"\n";
        return false;
    }

    if (!SaveLayout(layout))
    {
        std::wcerr << L"Could not write layout config: " << GetLayoutPath() << L"\n";
        return false;
    }

    return true;
}

void PrintUsage()
{
    std::wcout << L"Usage: MonitorSplitterCtl <enable [layout]|disable [--keep-panel-state] [--wait-panel-restore]|status|version|layout [spec]|hosttarget [selector|clear]|edidbase [status|resolve|clear|set <name>]|panelstate|panel <split|native> [--enable] [--open-settings] [--wait]|hoststatus|hoststop|selftest>\n";
    std::wcout << L"  enable   Starts the split monitors and direct scanout path; optional layout saves the split before starting.\n";
    std::wcout << L"  disable  Stops the split stack, then guides and verifies returning the physical panel to the Windows desktop.\n";
    std::wcout << L"  status   Prints splitter state as JSON.\n";
    std::wcout << L"  version  Prints CLI, service, agent, host, and driver runtime build tags as JSON.\n";
    std::wcout << L"  layout   Gets or sets layout, e.g. 5120x1440@120:1280,2560,1280.\n";
    std::wcout << L"  hosttarget Gets/sets the physical ultrawide panel selector.\n";
    std::wcout << L"  edidbase Gets, resolves, clears, or manually sets the physical-panel name base used for virtual monitor EDIDs.\n";
    std::wcout << L"  panelstate Prints the saved physical panel state.\n";
    std::wcout << L"  panel    Prints the documented Windows Settings toggle for split or native panel mode; pass --open-settings to open Settings.\n";
    std::wcout << L"  hoststatus Prints compositor running state and last mode status as JSON.\n";
    std::wcout << L"  hoststop Stops the host compositor if it is running, without unplugging virtual monitors.\n";
    std::wcout << L"  selftest Runs non-live internal checks for layout/EDID helper behavior.\n";
}
}

int __cdecl wmain(int argc, wchar_t* argv[])
{
    if (argc < 2)
    {
        PrintUsage();
        return 2;
    }

    std::wstring command = argv[1];
    if (command == L"enable")
    {
        if (!ParseEnableArguments(argc, argv))
        {
            return 2;
        }
        return EnableDirect();
    }
    if (command == L"disable")
    {
        return DisableCommand(argc, argv);
    }
    if (command == L"status")
    {
        return Status();
    }
    if (command == L"version")
    {
        return VersionCommand();
    }
    if (command == L"panelstate")
    {
        return PanelStateCommand();
    }
    if (command == L"panel")
    {
        return PanelCommand(argc, argv);
    }
    if (command == L"hoststatus")
    {
        return HostStatus();
    }
    if (command == L"hoststop")
    {
        return HostStop();
    }
    if (command == L"hosttarget")
    {
        return HostTargetCommand(argc, argv);
    }
    if (command == L"edidbase")
    {
        return EdidBaseCommand(argc, argv);
    }
    if (command == L"layout")
    {
        return LayoutCommand(argc, argv);
    }
    if (command == L"selftest")
    {
        return SelfTestCommand();
    }
    if (command == L"host")
    {
        return Host(false);
    }

    PrintUsage();
    return 2;
}
