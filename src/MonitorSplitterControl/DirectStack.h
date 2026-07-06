#pragma once

#include <windows.h>
#include <swdevice.h>
#include <cfgmgr32.h>
#include <sddl.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "Control.h"

namespace MonitorSplitter::Control
{
inline constexpr wchar_t kSoftwareDeviceClass[] = L"MonitorSplitterDriver";
inline constexpr wchar_t kInstanceId[] = L"MonitorSplitterDriver";
inline constexpr wchar_t kHardwareIds[] = L"MonitorSplitterDriver\0\0";
inline constexpr wchar_t kCompatibleIds[] = L"MonitorSplitterDriver\0\0";
inline constexpr wchar_t kDescription[] = L"MonitorSplitter Virtual Display Adapter";
inline constexpr wchar_t kStopEventName[] = L"Local\\MonitorSplitter.Stop";
inline constexpr wchar_t kRunningMutexName[] = L"Local\\MonitorSplitter.Running";
inline constexpr wchar_t kReadyEventName[] = L"Local\\MonitorSplitter.Ready";
inline constexpr wchar_t kFailedEventName[] = L"Local\\MonitorSplitter.Failed";
inline constexpr wchar_t kHostRunningMutexName[] = L"Local\\MonitorSplitter.HostRunning";
inline constexpr wchar_t kHostStopEventName[] = L"Local\\MonitorSplitter.HostStop";

struct CreationContext
{
    HANDLE Event = nullptr;
    HRESULT Result = E_FAIL;
    std::wstring InstanceId;
};

struct DevNodeStartStatus
{
    CONFIGRET LocateResult = CR_DEFAULT;
    CONFIGRET StatusResult = CR_DEFAULT;
    CONFIGRET ProblemStatusResult = CR_DEFAULT;
    ULONG Status = 0;
    ULONG Problem = 0;
    LONG ProblemStatus = 0;
    bool Located = false;
    bool Started = false;
    bool DriverLoaded = false;
};

struct DisplayConfigDesktopBounds
{
    bool HasBounds = false;
    LONG Left = 0;
    LONG Top = 0;
    LONG Right = 0;
    LONG Bottom = 0;
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

inline SECURITY_ATTRIBUTES* LocalControlObjectSecurity()
{
    static LocalSecurityDescriptor securityDescriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)");
    if (FAILED(securityDescriptor.Status()))
    {
        return nullptr;
    }
    return securityDescriptor.Attributes();
}

inline HANDLE CreateLocalMutex(PCWSTR name, BOOL initialOwner)
{
    return CreateMutexW(LocalControlObjectSecurity(), initialOwner, name);
}

inline HANDLE CreateLocalEvent(PCWSTR name, BOOL manualReset, BOOL initialState)
{
    return CreateEventW(LocalControlObjectSecurity(), manualReset, initialState, name);
}

inline void SignalIfValid(HANDLE handle)
{
    if (handle != nullptr)
    {
        SetEvent(handle);
    }
}

inline std::wstring HResultText(HRESULT hr)
{
    std::wstringstream stream;
    stream << L"0x" << std::hex << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(hr);
    return stream.str();
}

inline std::wstring DescribeHResult(HRESULT hr)
{
    auto message = HResultText(hr);
    if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED))
    {
        message += L" (access denied)";
    }
    return message;
}

inline std::wstring StatePath()
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

inline void WriteState(const std::wstring& message)
{
    WriteTextFile(StatePath(), message);
}

inline void ClearActiveLayout()
{
    DeleteFileW(ActiveLayoutPath().c_str());
}

inline bool SaveActiveLayout(const Layout& layout)
{
    return WriteTextFile(ActiveLayoutPath(), SerializeLayout(layout));
}

inline bool IsVirtualDeviceRunning()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kRunningMutexName);
    if (mutex == nullptr)
    {
        return false;
    }
    CloseHandle(mutex);
    return true;
}

inline bool IsDirectHostRunning()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kHostRunningMutexName);
    if (mutex == nullptr)
    {
        return false;
    }
    CloseHandle(mutex);
    return true;
}

inline bool RequestDirectHostStop()
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

inline bool WaitUntilDirectHostStopped(DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        if (!IsDirectHostRunning())
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

inline bool RequestVirtualDeviceHolderStop()
{
    HANDLE stopEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (stopEvent == nullptr)
    {
        return false;
    }
    SetEvent(stopEvent);
    CloseHandle(stopEvent);
    return true;
}

inline bool WaitUntilVirtualDeviceStopped(DWORD timeoutMs)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        if (!IsVirtualDeviceRunning())
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

inline bool ShouldCleanupStaleLocalStackBeforeFreshStart(bool directHostRunning, bool virtualDeviceRunning)
{
    return directHostRunning || virtualDeviceRunning;
}

inline bool CleanupStaleLocalStackBeforeFreshStart(DWORD timeoutMs, std::wstring& reason)
{
    const bool directHostRunning = IsDirectHostRunning();
    const bool virtualDeviceRunning = IsVirtualDeviceRunning();
    if (!ShouldCleanupStaleLocalStackBeforeFreshStart(directHostRunning, virtualDeviceRunning))
    {
        reason.clear();
        return true;
    }

    if (directHostRunning)
    {
        RequestDirectHostStop();
    }
    if (virtualDeviceRunning)
    {
        RequestVirtualDeviceHolderStop();
    }

    const DWORD start = GetTickCount();
    for (;;)
    {
        const bool directStopped = !IsDirectHostRunning();
        const bool virtualStopped = !IsVirtualDeviceRunning();
        if (directStopped && virtualStopped)
        {
            reason.clear();
            return true;
        }

        if (GetTickCount() - start >= timeoutMs)
        {
            reason = L"stale MonitorSplitter session objects did not stop within " +
                     std::to_wstring(timeoutMs) +
                     L" ms";
            if (!directStopped)
            {
                reason += L"; direct host is still running";
            }
            if (!virtualStopped)
            {
                reason += L"; virtual device holder is still running";
            }
            return false;
        }

        Sleep(100);
    }
}

inline bool HostStatusReadyForDirectShared(const std::wstring& status, DWORD expectedPid, std::wstring& reason)
{
    return HostStatusHealthy(status, &reason, expectedPid);
}

inline bool WaitForDirectSharedHost(HANDLE process, DWORD expectedPid, DWORD timeoutMs, std::wstring& reason)
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

        if (!IsDirectHostRunning())
        {
            reason = L"direct host is not running";
        }
        else
        {
            const auto status = MonitorSplitter::Trim(ReadTextFile(HostStatusPath()));
            if (HostStatusReadyForDirectShared(status, expectedPid, reason))
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

inline bool WaitForDirectHostSupervisedStartup(HANDLE process, DWORD expectedPid, DWORD timeoutMs, std::wstring& reason)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        if (process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
        {
            DWORD exitCode = 0;
            GetExitCodeProcess(process, &exitCode);
            reason = L"direct host exited before supervised startup with exit code " + std::to_wstring(exitCode);
            return false;
        }

        if (!IsDirectHostRunning())
        {
            reason = L"direct host is not running";
        }
        else
        {
            const auto status = MonitorSplitter::Trim(ReadTextFile(HostStatusPath()));
            if (HostStatusInProgressForSupervision(status, &reason, expectedPid))
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

inline bool WaitForDirectHostReadiness(
    HANDLE process,
    DWORD expectedPid,
    DWORD timeoutMs,
    bool acceptSupervisedStartup,
    std::wstring& reason)
{
    if (acceptSupervisedStartup)
    {
        return WaitForDirectHostSupervisedStartup(process, expectedPid, timeoutMs, reason);
    }
    return WaitForDirectSharedHost(process, expectedPid, timeoutMs, reason);
}

inline VOID WINAPI CreationCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT hrCreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId)
{
    UNREFERENCED_PARAMETER(hSwDevice);
    auto* context = reinterpret_cast<CreationContext*>(pContext);
    context->Result = hrCreateResult;
    if (pszDeviceInstanceId != nullptr)
    {
        context->InstanceId = pszDeviceInstanceId;
    }
    SetEvent(context->Event);
}

inline DevNodeStartStatus QueryDevNodeStartStatus(const std::wstring& instanceId)
{
    DevNodeStartStatus result;
    DEVINST devInst = 0;
    result.LocateResult = CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL);
    if (result.LocateResult != CR_SUCCESS)
    {
        return result;
    }

    result.Located = true;
    result.StatusResult = CM_Get_DevNode_Status(&result.Status, &result.Problem, devInst, 0);
    if (result.StatusResult == CR_SUCCESS)
    {
        result.Started = (result.Status & DN_STARTED) != 0;
        result.DriverLoaded = (result.Status & DN_DRIVER_LOADED) != 0;
    }

    return result;
}

inline std::wstring FormatDevNodeStartStatus(const DevNodeStartStatus& status)
{
    std::wstringstream text;
    text << L"located=" << (status.Located ? L"true" : L"false")
         << L", locateResult=0x" << std::hex << status.LocateResult
         << L", statusResult=0x" << status.StatusResult
         << L", status=0x" << status.Status
         << L", problem=0x" << status.Problem
         << L", problemStatus=0x" << static_cast<ULONG>(status.ProblemStatus)
         << L", started=" << (status.Started ? L"true" : L"false")
         << L", driverLoaded=" << (status.DriverLoaded ? L"true" : L"false");
    return text.str();
}

inline bool WaitForDevNodeStarted(const std::wstring& instanceId, DWORD timeoutMs, DevNodeStartStatus& lastStatus)
{
    const DWORD start = GetTickCount();
    do
    {
        lastStatus = QueryDevNodeStartStatus(instanceId);
        if (lastStatus.Started && lastStatus.DriverLoaded)
        {
            return true;
        }
        Sleep(250);
    } while (GetTickCount() - start < timeoutMs);
    return false;
}

inline bool WaitForDevNodeRemoved(const std::wstring& instanceId, DWORD timeoutMs, DevNodeStartStatus& lastStatus)
{
    if (instanceId.empty())
    {
        return true;
    }

    const DWORD start = GetTickCount();
    do
    {
        lastStatus = QueryDevNodeStartStatus(instanceId);
        if (!lastStatus.Located)
        {
            return true;
        }
        Sleep(250);
    } while (GetTickCount() - start < timeoutMs);
    return false;
}

inline std::wstring SoftwareDeviceInstanceId()
{
    return std::wstring(L"SWD\\") + kSoftwareDeviceClass + L"\\" + kInstanceId;
}

inline bool TryRemoveDevNodeAndWait(const std::wstring& instanceId, DWORD timeoutMs, std::wstring& failure)
{
    failure.clear();
    if (instanceId.empty())
    {
        return true;
    }

    DEVINST devInst = 0;
    CONFIGRET locate = CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL);
    if (locate != CR_SUCCESS)
    {
        return true;
    }

    PNP_VETO_TYPE vetoType = PNP_VetoTypeUnknown;
    wchar_t vetoName[MAX_PATH] = {};
    CONFIGRET remove = CM_Query_And_Remove_SubTreeW(
        devInst,
        &vetoType,
        vetoName,
        ARRAYSIZE(vetoName),
        CM_REMOVE_NO_RESTART);
    if (remove != CR_SUCCESS)
    {
        std::wstringstream text;
        text << L"could not remove virtual display devnode " << instanceId
             << L": cmResult=0x" << std::hex << remove
             << L", vetoType=" << static_cast<int>(vetoType);
        if (vetoName[0] != L'\0')
        {
            text << L", vetoName=" << vetoName;
        }
        failure = text.str();
        return false;
    }

    DevNodeStartStatus removedStatus;
    if (!WaitForDevNodeRemoved(instanceId, timeoutMs, removedStatus))
    {
        failure = L"virtual display devnode did not remove within " +
                  std::to_wstring(timeoutMs) +
                  L" ms: " +
                  FormatDevNodeStartStatus(removedStatus);
        return false;
    }
    return true;
}

inline bool IsMonitorSplitterPath(const DisplayPathSnapshot& path)
{
    return ContainsInsensitive(path.SourceName, L"MonitorSplitter") ||
           ContainsInsensitive(path.SourceName, L"MSplit") ||
           ContainsInsensitive(path.TargetFriendlyName, L"MonitorSplitter") ||
           ContainsInsensitive(path.TargetFriendlyName, L"MSplit") ||
           ContainsInsensitive(path.TargetDevicePath, L"MonitorSplitter") ||
           ContainsInsensitive(path.TargetDevicePath, L"MSplit") ||
           ContainsInsensitive(path.TargetDevicePath, L"DISPLAY#MSP");
}

inline bool DisplayConfigPathHasSourceMode(
    const DISPLAYCONFIG_PATH_INFO& path,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
    DISPLAYCONFIG_SOURCE_MODE** sourceMode)
{
    if (path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
        path.sourceInfo.modeInfoIdx >= modes.size())
    {
        return false;
    }
    auto& mode = const_cast<DISPLAYCONFIG_MODE_INFO&>(modes[path.sourceInfo.modeInfoIdx]);
    if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
    {
        return false;
    }
    *sourceMode = &mode.sourceMode;
    return true;
}

inline void IncludeDisplayConfigSourceModeBounds(
    const DISPLAYCONFIG_SOURCE_MODE& sourceMode,
    DisplayConfigDesktopBounds& bounds)
{
    const LONG left = sourceMode.position.x;
    const LONG top = sourceMode.position.y;
    const LONG right = left + static_cast<LONG>(sourceMode.width);
    const LONG bottom = top + static_cast<LONG>(sourceMode.height);

    if (!bounds.HasBounds)
    {
        bounds.HasBounds = true;
        bounds.Left = left;
        bounds.Top = top;
        bounds.Right = right;
        bounds.Bottom = bottom;
        return;
    }

    bounds.Left = min(bounds.Left, left);
    bounds.Top = min(bounds.Top, top);
    bounds.Right = max(bounds.Right, right);
    bounds.Bottom = max(bounds.Bottom, bottom);
}

inline DisplayConfigDesktopBounds ComputeNonMonitorSplitterDesktopBounds(
    const std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
    const std::vector<DisplayPathSnapshot>& snapshots)
{
    DisplayConfigDesktopBounds bounds;
    for (size_t pathIndex = 0; pathIndex < paths.size() && pathIndex < snapshots.size(); pathIndex++)
    {
        if ((paths[pathIndex].flags & DISPLAYCONFIG_PATH_ACTIVE) == 0)
        {
            continue;
        }
        if (IsMonitorSplitterPath(snapshots[pathIndex]))
        {
            continue;
        }

        DISPLAYCONFIG_SOURCE_MODE* sourceMode = nullptr;
        if (DisplayConfigPathHasSourceMode(paths[pathIndex], modes, &sourceMode))
        {
            IncludeDisplayConfigSourceModeBounds(*sourceMode, bounds);
        }
    }
    return bounds;
}

inline POINT ChooseMonitorSplitterDesktopOrigin(const DisplayConfigDesktopBounds& nonSplitterBounds)
{
    if (!nonSplitterBounds.HasBounds)
    {
        return { 0, 0 };
    }
    return { nonSplitterBounds.Right, nonSplitterBounds.Top };
}

inline bool TryGetCurrentMonitorSplitterDesktopOrigin(
    const std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
    const std::vector<size_t>& pathIndexesByLayout,
    POINT& origin)
{
    if (pathIndexesByLayout.empty() || pathIndexesByLayout[0] >= paths.size())
    {
        return false;
    }

    DISPLAYCONFIG_SOURCE_MODE* sourceMode = nullptr;
    if (!DisplayConfigPathHasSourceMode(paths[pathIndexesByLayout[0]], modes, &sourceMode))
    {
        return false;
    }

    origin = { sourceMode->position.x, sourceMode->position.y };
    return true;
}

inline size_t MatchDisplayConfigPathToLayoutIndex(
    const DISPLAYCONFIG_PATH_INFO& path,
    const DisplayPathSnapshot& snapshot,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
    const Layout& layout,
    const std::vector<bool>& usedLayoutIndexes,
    const std::wstring& edidNameBase)
{
    for (size_t index = 0; index < layout.Monitors.size(); index++)
    {
        if (usedLayoutIndexes[index])
        {
            continue;
        }
        if (MatchesMonitorSplitterEdidName(snapshot.TargetFriendlyName, layout.Monitors.size(), index, edidNameBase))
        {
            return index;
        }
    }

    DISPLAYCONFIG_SOURCE_MODE* sourceMode = nullptr;
    if (!DisplayConfigPathHasSourceMode(path, modes, &sourceMode))
    {
        return layout.Monitors.size();
    }

    for (size_t index = 0; index < layout.Monitors.size(); index++)
    {
        if (usedLayoutIndexes[index])
        {
            continue;
        }
        const auto& expected = layout.Monitors[index];
        if (sourceMode->width == expected.Width && sourceMode->height == expected.Height)
        {
            return index;
        }
    }
    return layout.Monitors.size();
}

inline bool TryApplyMonitorSplitterDesktopLayout(
    const Layout& layout,
    std::wstring& reason,
    bool* changed = nullptr,
    bool forceApply = false,
    bool applyChanges = true)
{
    if (changed != nullptr)
    {
        *changed = false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryActiveDisplayConfigRaw(paths, modes))
    {
        reason = L"QueryDisplayConfig did not return active paths";
        return false;
    }

    std::vector<DisplayPathSnapshot> snapshots;
    snapshots.reserve(paths.size());
    for (const auto& path : paths)
    {
        DisplayPathSnapshot snapshot;
        snapshot.SourceName = GetSourceName(path.sourceInfo);
        snapshot.TargetFriendlyName = GetTargetFriendlyName(path.targetInfo);
        snapshot.TargetDevicePath = GetTargetDevicePath(path.targetInfo);
        snapshot.TargetAdapterId = path.targetInfo.adapterId;
        snapshot.TargetId = path.targetInfo.id;
        snapshots.push_back(std::move(snapshot));
    }

    const auto edidNameBase = LoadEdidNameBase();
    std::vector<bool> usedLayoutIndexes(layout.Monitors.size(), false);
    std::vector<size_t> pathIndexesByLayout(layout.Monitors.size(), paths.size());
    size_t matchedCount = 0;
    for (size_t pathIndex = 0; pathIndex < paths.size(); pathIndex++)
    {
        if (!IsMonitorSplitterPath(snapshots[pathIndex]))
        {
            continue;
        }

        const size_t layoutIndex = MatchDisplayConfigPathToLayoutIndex(
            paths[pathIndex],
            snapshots[pathIndex],
            modes,
            layout,
            usedLayoutIndexes,
            edidNameBase);
        if (layoutIndex >= layout.Monitors.size())
        {
            continue;
        }

        usedLayoutIndexes[layoutIndex] = true;
        pathIndexesByLayout[layoutIndex] = pathIndex;
        matchedCount++;
    }

    if (matchedCount != layout.Monitors.size())
    {
        reason = L"only matched " + std::to_wstring(matchedCount) + L" of " + std::to_wstring(layout.Monitors.size()) + L" active MonitorSplitter paths";
        return false;
    }

    POINT virtualDesktopOrigin = {};
    if (!TryGetCurrentMonitorSplitterDesktopOrigin(paths, modes, pathIndexesByLayout, virtualDesktopOrigin))
    {
        virtualDesktopOrigin = ChooseMonitorSplitterDesktopOrigin(
            ComputeNonMonitorSplitterDesktopBounds(paths, modes, snapshots));
    }

    LONG x = virtualDesktopOrigin.x;
    bool needsApply = false;
    for (size_t layoutIndex = 0; layoutIndex < layout.Monitors.size(); layoutIndex++)
    {
        const size_t pathIndex = pathIndexesByLayout[layoutIndex];
        DISPLAYCONFIG_SOURCE_MODE* sourceMode = nullptr;
        if (!DisplayConfigPathHasSourceMode(paths[pathIndex], modes, &sourceMode))
        {
            reason = L"matched MonitorSplitter path has no source mode index";
            return false;
        }

        if (sourceMode->position.x != x || sourceMode->position.y != virtualDesktopOrigin.y)
        {
            sourceMode->position.x = x;
            sourceMode->position.y = virtualDesktopOrigin.y;
            needsApply = true;
        }
        x += static_cast<LONG>(layout.Monitors[layoutIndex].Width);
    }

    if (!needsApply && !forceApply)
    {
        reason.clear();
        return true;
    }
    if (!applyChanges)
    {
        reason = L"MonitorSplitter desktop layout is not positioned as configured";
        return false;
    }

    const LONG result = SetDisplayConfig(
        static_cast<UINT32>(paths.size()),
        paths.data(),
        static_cast<UINT32>(modes.size()),
        modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES);
    if (result != ERROR_SUCCESS)
    {
        reason = L"SetDisplayConfig failed with Win32 error " + std::to_wstring(result) + L" (" + HResultText(HRESULT_FROM_WIN32(result)) + L")";
        return false;
    }

    reason.clear();
    if (changed != nullptr)
    {
        *changed = true;
    }
    return true;
}

inline bool MonitorSplitterDesktopLayoutActive(const Layout& layout, std::wstring& reason)
{
    bool changed = false;
    return TryApplyMonitorSplitterDesktopLayout(layout, reason, &changed, false, false);
}

inline bool TryExtendDesktopTopology(std::wstring& reason)
{
    const LONG result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND | SDC_SAVE_TO_DATABASE);
    if (result != ERROR_SUCCESS)
    {
        reason = L"SetDisplayConfig topology extend failed with Win32 error " + std::to_wstring(result) + L" (" + HResultText(HRESULT_FROM_WIN32(result)) + L")";
        return false;
    }
    reason.clear();
    return true;
}

inline bool AddReferencedDisplayConfigMode(
    UINT32& modeInfoIdx,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& sourceModes,
    std::vector<DISPLAYCONFIG_MODE_INFO>& targetModes,
    std::vector<size_t>& modeIndexMap,
    std::wstring& reason)
{
    if (modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
    {
        return true;
    }
    if (modeInfoIdx >= sourceModes.size())
    {
        reason = L"DisplayConfig path references an out-of-range mode index";
        return false;
    }

    constexpr size_t unmapped = static_cast<size_t>(-1);
    size_t& mappedIndex = modeIndexMap[modeInfoIdx];
    if (mappedIndex == unmapped)
    {
        mappedIndex = targetModes.size();
        targetModes.push_back(sourceModes[modeInfoIdx]);
    }

    modeInfoIdx = static_cast<UINT32>(mappedIndex);
    return true;
}

inline bool CompactDisplayConfigModesForPaths(
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& sourceModes,
    std::vector<DISPLAYCONFIG_MODE_INFO>& targetModes,
    std::wstring& reason)
{
    constexpr size_t unmapped = static_cast<size_t>(-1);
    std::vector<size_t> modeIndexMap(sourceModes.size(), unmapped);
    targetModes.clear();

    for (auto& path : paths)
    {
        if (!AddReferencedDisplayConfigMode(path.sourceInfo.modeInfoIdx, sourceModes, targetModes, modeIndexMap, reason))
        {
            return false;
        }
        if (!AddReferencedDisplayConfigMode(path.targetInfo.modeInfoIdx, sourceModes, targetModes, modeIndexMap, reason))
        {
            return false;
        }
    }

    reason.clear();
    return true;
}

inline bool TryActivateMonitorSplitterPathsFromDatabase(const Layout& layout, std::wstring& reason)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryDisplayConfigRaw(QDC_DATABASE_CURRENT, paths, modes))
    {
        reason = L"DisplayConfig database did not return paths";
        return false;
    }

    std::vector<DisplayPathSnapshot> snapshots;
    snapshots.reserve(paths.size());
    for (const auto& path : paths)
    {
        DisplayPathSnapshot snapshot;
        snapshot.SourceName = GetSourceName(path.sourceInfo);
        snapshot.TargetFriendlyName = GetTargetFriendlyName(path.targetInfo);
        snapshot.TargetDevicePath = GetTargetDevicePath(path.targetInfo);
        snapshot.TargetAdapterId = path.targetInfo.adapterId;
        snapshot.TargetId = path.targetInfo.id;
        snapshots.push_back(std::move(snapshot));
    }

    const auto edidNameBase = LoadEdidNameBase();
    std::vector<bool> usedLayoutIndexes(layout.Monitors.size(), false);
    std::vector<size_t> pathIndexesByLayout(layout.Monitors.size(), paths.size());
    size_t matchedCount = 0;
    for (size_t pathIndex = 0; pathIndex < paths.size(); pathIndex++)
    {
        if (!IsMonitorSplitterPath(snapshots[pathIndex]))
        {
            continue;
        }

        const size_t layoutIndex = MatchDisplayConfigPathToLayoutIndex(
            paths[pathIndex],
            snapshots[pathIndex],
            modes,
            layout,
            usedLayoutIndexes,
            edidNameBase);
        if (layoutIndex >= layout.Monitors.size())
        {
            continue;
        }

        usedLayoutIndexes[layoutIndex] = true;
        pathIndexesByLayout[layoutIndex] = pathIndex;
        matchedCount++;
    }

    if (matchedCount != layout.Monitors.size())
    {
        reason = L"only matched " + std::to_wstring(matchedCount) + L" of " + std::to_wstring(layout.Monitors.size()) + L" MonitorSplitter paths in the DisplayConfig database";
        return false;
    }

    POINT virtualDesktopOrigin = ChooseMonitorSplitterDesktopOrigin(
        ComputeNonMonitorSplitterDesktopBounds(paths, modes, snapshots));

    std::vector<DISPLAYCONFIG_PATH_INFO> pathsToApply;
    pathsToApply.reserve(paths.size());
    for (size_t pathIndex = 0; pathIndex < paths.size() && pathIndex < snapshots.size(); pathIndex++)
    {
        if ((paths[pathIndex].flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 ||
            IsMonitorSplitterPath(snapshots[pathIndex]))
        {
            continue;
        }
        pathsToApply.push_back(paths[pathIndex]);
    }

    LONG x = virtualDesktopOrigin.x;
    for (size_t layoutIndex = 0; layoutIndex < layout.Monitors.size(); layoutIndex++)
    {
        const size_t pathIndex = pathIndexesByLayout[layoutIndex];
        paths[pathIndex].flags |= DISPLAYCONFIG_PATH_ACTIVE;

        DISPLAYCONFIG_SOURCE_MODE* sourceMode = nullptr;
        if (DisplayConfigPathHasSourceMode(paths[pathIndex], modes, &sourceMode))
        {
            sourceMode->position.x = x;
            sourceMode->position.y = virtualDesktopOrigin.y;
        }
        x += static_cast<LONG>(layout.Monitors[layoutIndex].Width);
        pathsToApply.push_back(paths[pathIndex]);
    }

    if (pathsToApply.empty())
    {
        reason = L"DisplayConfig database activation produced no paths to apply";
        return false;
    }

    std::vector<DISPLAYCONFIG_MODE_INFO> modesToApply;
    if (!CompactDisplayConfigModesForPaths(pathsToApply, modes, modesToApply, reason))
    {
        return false;
    }

    const LONG result = SetDisplayConfig(
        static_cast<UINT32>(pathsToApply.size()),
        pathsToApply.data(),
        static_cast<UINT32>(modesToApply.size()),
        modesToApply.empty() ? nullptr : modesToApply.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES);
    if (result != ERROR_SUCCESS)
    {
        reason = L"SetDisplayConfig database activation failed with Win32 error " + std::to_wstring(result) + L" (" + HResultText(HRESULT_FROM_WIN32(result)) + L")";
        return false;
    }

    reason.clear();
    return true;
}

inline bool TryResolvePreferredRenderAdapterFromDisplayConfigDatabase(
    const std::wstring& selector,
    LUID& adapterLuid,
    UINT32& targetId,
    bool& databaseAvailable,
    std::wstring& reason)
{
    adapterLuid = {};
    targetId = 0;
    databaseAvailable = false;
    reason.clear();

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryDisplayConfigRaw(QDC_DATABASE_CURRENT, paths, modes))
    {
        reason = L"DisplayConfig database is not available";
        return false;
    }
    databaseAvailable = true;

    size_t matchCount = 0;
    LUID matchedLuid = {};
    UINT32 matchedTargetId = 0;
    for (const auto& path : paths)
    {
        DisplayPathSnapshot snapshot;
        snapshot.SourceName = GetSourceName(path.sourceInfo);
        snapshot.TargetFriendlyName = GetTargetFriendlyName(path.targetInfo);
        snapshot.TargetDevicePath = GetTargetDevicePath(path.targetInfo);
        snapshot.TargetAdapterId = path.targetInfo.adapterId;
        snapshot.TargetId = path.targetInfo.id;

        if (IsMonitorSplitterPath(snapshot) || !SelectorMatchesDisplayPath(selector, snapshot))
        {
            continue;
        }

        matchCount++;
        matchedLuid = path.targetInfo.adapterId;
        matchedTargetId = path.targetInfo.id;
    }

    if (matchCount != 1)
    {
        reason = matchCount == 0
            ? L"host target selector did not resolve in the DisplayConfig database"
            : L"host target selector matched multiple DisplayConfig database targets";
        return false;
    }
    if (matchedLuid.LowPart == 0 && matchedLuid.HighPart == 0)
    {
        reason = L"resolved DisplayConfig database target has no adapter LUID";
        return false;
    }

    adapterLuid = matchedLuid;
    targetId = matchedTargetId;
    return true;
}

inline bool WaitForAndApplyMonitorSplitterDesktopLayout(
    const Layout& layout,
    DWORD timeoutMs,
    std::wstring& lastReason,
    bool forceApply = false)
{
    const DWORD start = GetTickCount();
    DWORD lastExtendAttempt = 0;
    std::wstring lastExtendReason;
    do
    {
        bool changed = false;
        if (TryApplyMonitorSplitterDesktopLayout(layout, lastReason, &changed, forceApply))
        {
            if (changed)
            {
                Sleep(1500);
            }
            return true;
        }

        const DWORD now = GetTickCount();
        if (lastExtendAttempt == 0 || now - lastExtendAttempt >= 2000)
        {
            lastExtendAttempt = now;
            std::wstring extendReason;
            if (TryActivateMonitorSplitterPathsFromDatabase(layout, extendReason))
            {
                lastExtendReason = L"MonitorSplitter DisplayConfig database activation requested";
            }
            else if (TryExtendDesktopTopology(extendReason))
            {
                lastExtendReason = L"topology extend requested";
            }
            else
            {
                lastExtendReason = extendReason;
            }
        }

        Sleep(500);
    } while (GetTickCount() - start < timeoutMs);

    if (!lastExtendReason.empty())
    {
        lastReason += L"; " + lastExtendReason;
    }
    return false;
}

inline bool TryResolvePreferredRenderAdapterLuid(const Layout& layout, LUID& adapterLuid, std::wstring& reason)
{
    adapterLuid = {};
    reason.clear();

    const auto selector = LoadHostTarget();
    if (MonitorSplitter::Trim(selector).empty())
    {
        reason = L"no host target configured";
        return false;
    }

    UINT32 targetId = 0;
    bool displayConfigDatabaseAvailable = false;
    std::wstring displayConfigDatabaseReason;
    if (TryResolvePreferredRenderAdapterFromDisplayConfigDatabase(
            selector,
            adapterLuid,
            targetId,
            displayConfigDatabaseAvailable,
            displayConfigDatabaseReason))
    {
        return true;
    }

    size_t matchCount = 0;
    LUID matchedLuid = {};
    for (const auto& candidate : EnumerateMonitorCandidates())
    {
        if (candidate.Width < static_cast<int>(layout.HostWidth) ||
            candidate.Height < static_cast<int>(layout.Height))
        {
            continue;
        }
        if (!SelectorMatchesCandidate(selector, candidate))
        {
            continue;
        }
        matchCount++;
        matchedLuid = candidate.TargetAdapterId;
    }

    if (matchCount != 1)
    {
        reason = matchCount == 0
            ? L"host target selector did not resolve to a physical host monitor"
            : L"host target selector matched multiple physical host monitors";
        if (!displayConfigDatabaseReason.empty())
        {
            reason += L"; " + displayConfigDatabaseReason;
        }
        if (!displayConfigDatabaseAvailable &&
            TryParseSelectorAdapterIdentity(selector, adapterLuid, &targetId) &&
            (adapterLuid.LowPart != 0 || adapterLuid.HighPart != 0))
        {
            reason.clear();
            return true;
        }
        return false;
    }
    if (matchedLuid.LowPart == 0 && matchedLuid.HighPart == 0)
    {
        reason = L"resolved host target has no active display-config adapter LUID";
        return false;
    }

    adapterLuid = matchedLuid;
    return true;
}

inline std::wstring SiblingExecutablePath(const std::wstring& fileName)
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) == 0)
    {
        return {};
    }

    std::wstring path = exePath;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return fileName;
    }

    path.resize(slash + 1);
    path += fileName;
    return path;
}

inline bool StartDirectHostProcess(
    PROCESS_INFORMATION& process,
    std::wstring& reason,
    bool forceApply = false,
    bool allowModeApply = true)
{
    const std::wstring hostPath = SiblingExecutablePath(L"MonitorSplitterHost.exe");
    if (hostPath.empty())
    {
        reason = L"could not resolve MonitorSplitterHost.exe path";
        return false;
    }

    std::wstring commandLine = L"\"" + hostPath + L"\"";
    if (forceApply)
    {
        commandLine += L" --force-apply";
    }
    else if (!allowModeApply)
    {
        commandLine += L" --no-mode-apply";
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);

    if (!CreateProcessW(
            hostPath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            DETACHED_PROCESS,
            nullptr,
            nullptr,
            &startup,
            &process))
    {
        reason = L"CreateProcess for MonitorSplitterHost.exe failed: " + std::to_wstring(GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    process.hThread = nullptr;
    reason.clear();
    return true;
}

class DirectStack
{
public:
    ~DirectStack()
    {
        std::wstring ignored;
        if (m_SessionOnly)
        {
            StopSession(5000, ignored);
        }
        else
        {
            Stop(5000, ignored);
        }
    }

    bool StartVirtualDeviceOnly(std::wstring& reason)
    {
        m_SessionOnly = false;

        PreparedConfiguration config;
        if (!PrepareSavedConfiguration(config, reason))
        {
            return false;
        }

        if (const auto blockReason = HostPanelEnableBlockReason(); !blockReason.empty())
        {
            reason = blockReason;
            return false;
        }

        const auto& layout = config.LayoutValue;
        const auto& layoutSpec = config.LayoutSpec;
        const auto& edidNameBase = config.EdidNameBase;
        if (m_SwDevice != nullptr || !m_DeviceInstanceId.empty())
        {
            if (layoutSpec == m_StartedLayoutSpec && edidNameBase == m_StartedEdidNameBase)
            {
                reason.clear();
                return true;
            }

            if (!StopVirtualDeviceOnly(5000, reason))
            {
                return false;
            }
        }

        return CreateVirtualDevice(layout, layoutSpec, edidNameBase, reason);
    }

    bool StopVirtualDeviceOnly(DWORD timeoutMs, std::wstring& reason)
    {
        std::wstring stopFailure;
        if (m_SwDevice != nullptr || !m_DeviceInstanceId.empty())
        {
            std::wstring deviceRemovalFailure;
            if (!CloseSoftwareDeviceAndWait(timeoutMs, deviceRemovalFailure))
            {
                stopFailure = deviceRemovalFailure;
            }
        }

        ClearActiveLayout();
        m_StartedLayoutSpec.clear();
        m_StartedEdidNameBase.clear();
        m_DeviceInstanceId.clear();
        CloseOwnedHandles();
        reason = stopFailure;
        return stopFailure.empty();
    }

    bool StartSession(std::wstring& reason)
    {
        m_SessionOnly = true;

        PreparedConfiguration config;
        if (!PrepareSavedConfiguration(config, reason))
        {
            return false;
        }

        if (const auto blockReason = HostPanelEnableBlockReason(); !blockReason.empty())
        {
            reason = blockReason;
            return false;
        }

        if (TryAdoptRunningSession(config, reason))
        {
            return true;
        }

        std::wstring layoutApplyReason;
        if (!WaitForAndApplyMonitorSplitterDesktopLayout(config.LayoutValue, 10 * 1000, layoutApplyReason))
        {
            reason = L"MonitorSplitter desktop layout was not applied: " + layoutApplyReason;
            return false;
        }

        if (!RestartSessionDirectHostAndStabilize(130 * 1000, reason))
        {
            return false;
        }

        m_StartedLayoutSpec = config.LayoutSpec;
        m_StartedEdidNameBase = config.EdidNameBase;
        return true;
    }

    bool RecoverSession(
        std::wstring& reason,
        bool forceRestart = false,
        bool forceModeApply = false)
    {
        m_SessionOnly = true;

        PreparedConfiguration config;
        if (!PrepareSavedConfiguration(config, reason))
        {
            return false;
        }

        const auto& layout = config.LayoutValue;
        const auto& layoutSpec = config.LayoutSpec;
        const auto& edidNameBase = config.EdidNameBase;
        if (!forceRestart && TryAdoptRunningSession(config, reason))
        {
            return true;
        }

        if (!forceRestart &&
            (layoutSpec != m_StartedLayoutSpec || edidNameBase != m_StartedEdidNameBase))
        {
            return StartSession(reason);
        }

        const DWORD expectedPid = ActiveHostProcessId();
        const auto status = ReadTextFile(HostStatusPath());
        if (!forceRestart &&
            IsDirectHostRunning() &&
            HostStatusInProgressForSupervision(status, nullptr, expectedPid))
        {
            reason.clear();
            return true;
        }

        std::wstring layoutApplyReason;
        if (!WaitForAndApplyMonitorSplitterDesktopLayout(layout, 10 * 1000, layoutApplyReason, forceModeApply))
        {
            reason = L"MonitorSplitter desktop layout was not reapplied: " + layoutApplyReason;
            return false;
        }

        if (HostStatusLikelyStuckInModeChange(status))
        {
            reason = L"DisplayCore mode change is stuck; reboot before continuing recovery.";
            return false;
        }

        if (!RestartSessionDirectHostAndStabilize(130 * 1000, reason, forceModeApply, true))
        {
            return false;
        }

        m_StartedLayoutSpec = layoutSpec;
        m_StartedEdidNameBase = edidNameBase;
        return true;
    }

    bool StopSession(DWORD timeoutMs, std::wstring& reason)
    {
        RequestDirectHostStop();
        bool directHostStopped = WaitUntilDirectHostStopped(timeoutMs);
        std::wstring stopFailure;

        if (m_HostProcess.hProcess != nullptr)
        {
            if (!directHostStopped)
            {
                TerminateProcess(m_HostProcess.hProcess, ERROR_SERVICE_REQUEST_TIMEOUT);
                WaitForSingleObject(m_HostProcess.hProcess, 3000);
                directHostStopped = WaitUntilDirectHostStopped(3000);
                if (!directHostStopped)
                {
                    stopFailure = L"direct host did not stop within " + std::to_wstring(timeoutMs) + L" ms";
                }
            }
            CloseHandle(m_HostProcess.hProcess);
            m_HostProcess = {};
        }
        else if (!directHostStopped)
        {
            stopFailure = L"direct host did not stop within " + std::to_wstring(timeoutMs) + L" ms";
        }

        m_StartedLayoutSpec.clear();
        m_StartedEdidNameBase.clear();
        reason = stopFailure;
        return stopFailure.empty();
    }

    bool Start(std::wstring& reason)
    {
        m_SessionOnly = false;

        PreparedConfiguration config;
        if (!PrepareSavedConfiguration(config, reason))
        {
            return false;
        }

        if (const auto blockReason = HostPanelEnableBlockReason(); !blockReason.empty())
        {
            reason = blockReason;
            return false;
        }

        const auto& layout = config.LayoutValue;
        const auto& layoutSpec = config.LayoutSpec;
        const auto& edidNameBase = config.EdidNameBase;
        if (m_SwDevice != nullptr)
        {
            if (layoutSpec != m_StartedLayoutSpec || edidNameBase != m_StartedEdidNameBase)
            {
                if (!Stop(5000, reason))
                {
                    return false;
                }
            }
            else if (const DWORD expectedPid = ActiveHostProcessId();
                     expectedPid != 0 && IsDirectHostRunning() && HostStatusHealthy(ReadTextFile(HostStatusPath()), nullptr, expectedPid))
            {
                reason.clear();
                return true;
            }
            else
            {
                if (HostStatusLikelyStuckInModeChange(ReadTextFile(HostStatusPath())))
                {
                    reason = L"DisplayCore mode change is stuck; reboot before continuing recovery.";
                    return false;
                }
                return RestartDirectHost(10 * 1000, reason, false, false, true);
            }
        }

        LUID preferredRenderAdapter = {};
        std::wstring preferredRenderAdapterReason;
        const bool hasPreferredRenderAdapter = TryResolvePreferredRenderAdapterLuid(layout, preferredRenderAdapter, preferredRenderAdapterReason);

        std::wstring staleCleanupReason;
        if (!CleanupStaleLocalStackBeforeFreshStart(10 * 1000, staleCleanupReason))
        {
            reason = staleCleanupReason;
            return false;
        }

        m_RunningMutex = CreateLocalMutex(kRunningMutexName, TRUE);
        if (m_RunningMutex == nullptr)
        {
            reason = L"CreateMutex failed: " + std::to_wstring(GetLastError());
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            reason = L"virtual device holder is already running in this session";
            CloseOwnedHandles();
            return false;
        }

        m_ReadyEvent = CreateLocalEvent(kReadyEventName, TRUE, FALSE);
        m_FailedEvent = CreateLocalEvent(kFailedEventName, TRUE, FALSE);
        m_StopEvent = CreateLocalEvent(kStopEventName, TRUE, FALSE);
        if (m_ReadyEvent == nullptr || m_FailedEvent == nullptr || m_StopEvent == nullptr)
        {
            reason = L"CreateEvent failed: " + std::to_wstring(GetLastError());
            CloseOwnedHandles();
            return false;
        }
        ResetEvent(m_ReadyEvent);
        ResetEvent(m_FailedEvent);
        ResetEvent(m_StopEvent);
        ClearActiveLayout();

        CreationContext creation;
        creation.Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (creation.Event == nullptr)
        {
            reason = L"CreateEvent failed: " + std::to_wstring(GetLastError());
            CloseOwnedHandles();
            return false;
        }

        SW_DEVICE_CREATE_INFO createInfo = {};
        createInfo.cbSize = sizeof(createInfo);
        createInfo.pszzCompatibleIds = kCompatibleIds;
        createInfo.pszInstanceId = kInstanceId;
        createInfo.pszzHardwareIds = kHardwareIds;
        createInfo.pszDeviceDescription = kDescription;
        createInfo.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                                     SWDeviceCapabilitiesSilentInstall |
                                     SWDeviceCapabilitiesDriverRequired;

        DEVPROPERTY layoutProperty = {};
        layoutProperty.CompKey.Key = MonitorSplitter::kLayoutPropertyKey;
        layoutProperty.CompKey.Store = DEVPROP_STORE_SYSTEM;
        layoutProperty.Type = DEVPROP_TYPE_STRING;
        layoutProperty.BufferSize = static_cast<ULONG>((layoutSpec.size() + 1) * sizeof(wchar_t));
        layoutProperty.Buffer = const_cast<wchar_t*>(layoutSpec.c_str());

        std::vector<DEVPROPERTY> properties;
        properties.push_back(layoutProperty);

        DEVPROPERTY edidNameBaseProperty = {};
        if (!edidNameBase.empty())
        {
            edidNameBaseProperty.CompKey.Key = MonitorSplitter::kEdidNameBasePropertyKey;
            edidNameBaseProperty.CompKey.Store = DEVPROP_STORE_SYSTEM;
            edidNameBaseProperty.Type = DEVPROP_TYPE_STRING;
            edidNameBaseProperty.BufferSize = static_cast<ULONG>((edidNameBase.size() + 1) * sizeof(wchar_t));
            edidNameBaseProperty.Buffer = const_cast<wchar_t*>(edidNameBase.c_str());
            properties.push_back(edidNameBaseProperty);
        }

        DEVPROPERTY renderAdapterProperty = {};
        if (hasPreferredRenderAdapter)
        {
            renderAdapterProperty.CompKey.Key = MonitorSplitter::kRenderAdapterLuidPropertyKey;
            renderAdapterProperty.CompKey.Store = DEVPROP_STORE_SYSTEM;
            renderAdapterProperty.Type = DEVPROP_TYPE_BINARY;
            renderAdapterProperty.BufferSize = sizeof(preferredRenderAdapter);
            renderAdapterProperty.Buffer = &preferredRenderAdapter;
            properties.push_back(renderAdapterProperty);
        }

        HRESULT hr = SwDeviceCreate(
            kSoftwareDeviceClass,
            L"HTREE\\ROOT\\0",
            &createInfo,
            static_cast<ULONG>(properties.size()),
            properties.data(),
            CreationCallback,
            &creation,
            &m_SwDevice);
        if (FAILED(hr))
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
            {
                CloseHandle(creation.Event);
                creation.Event = nullptr;

                const auto existingInstanceId = SoftwareDeviceInstanceId();
                m_DeviceInstanceId = existingInstanceId;
                DevNodeStartStatus existingStatus;
                if (WaitForDevNodeStarted(existingInstanceId, 10 * 1000, existingStatus))
                {
                    if (!SaveActiveLayout(layout))
                    {
                        reason = L"could not write active layout config for existing virtual display device: " + ActiveLayoutPath();
                        CleanupFailedStart(reason);
                        return false;
                    }

                    auto enabledState = L"MonitorSplitter adopted existing virtual display device " +
                        existingInstanceId +
                        L" with layout " +
                        layoutSpec;
                    if (hasPreferredRenderAdapter)
                    {
                        enabledState += L" preferredRenderAdapter=" +
                            std::to_wstring(static_cast<unsigned long>(preferredRenderAdapter.LowPart)) +
                            L":" +
                            std::to_wstring(preferredRenderAdapter.HighPart);
                    }
                    else if (!preferredRenderAdapterReason.empty())
                    {
                        enabledState += L" preferredRenderAdapter=none (" + preferredRenderAdapterReason + L")";
                    }
                    if (!edidNameBase.empty())
                    {
                        enabledState += L" edidNameBase=" + edidNameBase;
                    }
                    WriteState(enabledState);

                    reason.clear();
                    m_StartedLayoutSpec = layoutSpec;
                    m_StartedEdidNameBase = edidNameBase;
                    SignalIfValid(m_ReadyEvent);
                    return true;
                }

                reason = L"existing MonitorSplitter virtual display devnode is not started: " +
                    FormatDevNodeStartStatus(existingStatus);
                SignalIfValid(m_FailedEvent);
                CleanupFailedStart(reason);
                return false;
            }

            reason = L"SwDeviceCreate failed: " + DescribeHResult(hr);
            SignalIfValid(m_FailedEvent);
            CloseHandle(creation.Event);
            CleanupFailedStart(reason);
            return false;
        }

        DWORD waitResult = WaitForSingleObject(creation.Event, 10 * 1000);
        if (waitResult != WAIT_OBJECT_0)
        {
            reason = L"timed out waiting for software device creation";
            SignalIfValid(m_FailedEvent);
            CloseHandle(creation.Event);
            CleanupFailedStart(reason);
            return false;
        }
        CloseHandle(creation.Event);

        if (FAILED(creation.Result))
        {
            reason = L"software device creation callback failed: " + DescribeHResult(creation.Result);
            SignalIfValid(m_FailedEvent);
            CleanupFailedStart(reason);
            return false;
        }
        m_DeviceInstanceId = creation.InstanceId;

        DevNodeStartStatus devNodeStatus;
        if (!WaitForDevNodeStarted(creation.InstanceId, 10 * 1000, devNodeStatus))
        {
            reason = L"MonitorSplitter devnode did not start: " + FormatDevNodeStartStatus(devNodeStatus);
            SignalIfValid(m_FailedEvent);
            CleanupFailedStart(reason);
            return false;
        }

        if (!SaveActiveLayout(layout))
        {
            reason = L"could not write active layout config: " + ActiveLayoutPath();
            SignalIfValid(m_FailedEvent);
            CleanupFailedStart(reason);
            return false;
        }

        std::wstring layoutApplyReason;
        if (!WaitForAndApplyMonitorSplitterDesktopLayout(layout, 10 * 1000, layoutApplyReason))
        {
            reason = L"MonitorSplitter desktop layout was not applied: " + layoutApplyReason;
            SignalIfValid(m_FailedEvent);
            CleanupFailedStart(reason);
            return false;
        }

        auto enabledState = L"MonitorSplitter enabled as " + creation.InstanceId + L" with layout " + layoutSpec;
        if (hasPreferredRenderAdapter)
        {
            enabledState += L" preferredRenderAdapter=" +
                std::to_wstring(static_cast<unsigned long>(preferredRenderAdapter.LowPart)) +
                L":" +
                std::to_wstring(preferredRenderAdapter.HighPart);
        }
        else if (!preferredRenderAdapterReason.empty())
        {
            enabledState += L" preferredRenderAdapter=none (" + preferredRenderAdapterReason + L")";
        }
        if (!edidNameBase.empty())
        {
            enabledState += L" edidNameBase=" + edidNameBase;
        }
        WriteState(enabledState);

        std::wstring hostStartReason;
        if (!StartDirectHostProcess(m_HostProcess, hostStartReason))
        {
            reason = hostStartReason;
            CleanupFailedStart(reason);
            return false;
        }

        std::wstring readinessReason;
        if (!WaitForDirectSharedHost(m_HostProcess.hProcess, m_HostProcess.dwProcessId, 130 * 1000, readinessReason))
        {
            reason = L"timed out waiting for MonitorSplitter direct host to reach direct-shared mode: " + readinessReason;
            CleanupFailedStart(reason);
            return false;
        }

        reason.clear();
        m_StartedLayoutSpec = layoutSpec;
        m_StartedEdidNameBase = edidNameBase;
        SignalIfValid(m_ReadyEvent);
        return true;
    }

    bool Recover(std::wstring& reason)
    {
        m_SessionOnly = false;

        PreparedConfiguration config;
        if (!PrepareSavedConfiguration(config, reason))
        {
            return false;
        }

        const auto& layout = config.LayoutValue;
        const auto& layoutSpec = config.LayoutSpec;
        const auto& edidNameBase = config.EdidNameBase;
        if (m_SwDevice == nullptr)
        {
            if (!Start(reason))
            {
                return false;
            }

            const DWORD expectedPid = ActiveHostProcessId();
            if (expectedPid != 0 && IsDirectHostRunning() && HostStatusHealthy(ReadTextFile(HostStatusPath()), nullptr, expectedPid))
            {
                reason.clear();
                return true;
            }

            return RestartDirectHost(10 * 1000, reason);
        }

        if (layoutSpec != m_StartedLayoutSpec || edidNameBase != m_StartedEdidNameBase)
        {
            return Start(reason);
        }

        std::wstring layoutApplyReason;
        if (!WaitForAndApplyMonitorSplitterDesktopLayout(layout, 10 * 1000, layoutApplyReason))
        {
            std::wstring stopReason;
            if (!Stop(10 * 1000, stopReason))
            {
                reason = L"MonitorSplitter desktop layout was not reapplied: " + layoutApplyReason +
                         L"; full virtual display restart could not stop the current stack: " + stopReason;
                return false;
            }

            std::wstring startReason;
            if (!Start(startReason))
            {
                reason = L"MonitorSplitter desktop layout was not reapplied: " + layoutApplyReason +
                         L"; full virtual display restart failed: " + startReason;
                return false;
            }

            reason.clear();
            return true;
        }

        return RestartDirectHost(10 * 1000, reason);
    }

    DWORD ActiveHostProcessId() const
    {
        if (m_HostProcess.hProcess == nullptr || m_HostProcess.dwProcessId == 0)
        {
            return 0;
        }
        return WaitForSingleObject(m_HostProcess.hProcess, 0) == WAIT_TIMEOUT ? m_HostProcess.dwProcessId : 0;
    }

    bool WaitForStopRequest(DWORD timeoutMs = INFINITE) const
    {
        if (m_StopEvent == nullptr)
        {
            return false;
        }
        return WaitForSingleObject(m_StopEvent, timeoutMs) == WAIT_OBJECT_0;
    }

    bool RestartDirectHost(
        DWORD timeoutMs,
        std::wstring& reason,
        bool acceptSupervisedStartup = false,
        bool forceApply = false,
        bool allowModeApply = true)
    {
        if (m_SwDevice == nullptr)
        {
            reason = L"virtual display device is not running";
            return false;
        }

        RequestDirectHostStop();
        if (!WaitUntilDirectHostStopped(timeoutMs))
        {
            if (m_HostProcess.hProcess != nullptr)
            {
                TerminateProcess(m_HostProcess.hProcess, ERROR_SERVICE_REQUEST_TIMEOUT);
                WaitForSingleObject(m_HostProcess.hProcess, 3000);
            }
            if (!WaitUntilDirectHostStopped(3000))
            {
                reason = L"direct host did not stop within " + std::to_wstring(timeoutMs) + L" ms";
                return false;
            }
        }

        if (m_HostProcess.hProcess != nullptr)
        {
            CloseHandle(m_HostProcess.hProcess);
            m_HostProcess = {};
        }

        std::wstring hostStartReason;
        if (!StartDirectHostProcess(m_HostProcess, hostStartReason, forceApply, allowModeApply))
        {
            reason = hostStartReason;
            return false;
        }

        std::wstring readinessReason;
        if (!WaitForDirectHostReadiness(
                m_HostProcess.hProcess,
                m_HostProcess.dwProcessId,
                130 * 1000,
                acceptSupervisedStartup,
                readinessReason))
        {
            reason = acceptSupervisedStartup
                ? L"timed out waiting for MonitorSplitter direct host to enter supervised startup: " + readinessReason
                : L"timed out waiting for MonitorSplitter direct host to reach direct-shared mode: " + readinessReason;
            RequestDirectHostStop();
            WaitUntilDirectHostStopped(5 * 1000);
            return false;
        }

        reason.clear();
        return true;
    }

    bool RestartSessionDirectHostAndStabilize(
        DWORD timeoutMs,
        std::wstring& reason,
        bool forceApply = false,
        bool allowModeApply = true)
    {
        if (!RestartSessionDirectHost(timeoutMs, reason, false, forceApply, allowModeApply))
        {
            return false;
        }

        // A freshly started DisplayCore direct host can report healthy scanout
        // while the physical panel remains black. A second host-only start after
        // the first one reaches direct-shared has been the reliable stabilizer:
        // the virtual monitors and Windows layout stay untouched, and the new
        // host only reads the already-active direct mode.
        std::wstring restartReason;
        if (!RestartSessionDirectHost(timeoutMs, restartReason, false, false, false))
        {
            reason = L"post-start direct host restart failed: " + restartReason;
            return false;
        }

        reason.clear();
        return true;
    }

    bool RestartSessionDirectHost(
        DWORD timeoutMs,
        std::wstring& reason,
        bool acceptSupervisedStartup = true,
        bool forceApply = false,
        bool allowModeApply = true)
    {
        RequestDirectHostStop();
        if (!WaitUntilDirectHostStopped(timeoutMs))
        {
            if (m_HostProcess.hProcess != nullptr)
            {
                TerminateProcess(m_HostProcess.hProcess, ERROR_SERVICE_REQUEST_TIMEOUT);
                WaitForSingleObject(m_HostProcess.hProcess, 3000);
            }
            if (!WaitUntilDirectHostStopped(3000))
            {
                reason = L"direct host did not stop within " + std::to_wstring(timeoutMs) + L" ms";
                return false;
            }
        }

        if (m_HostProcess.hProcess != nullptr)
        {
            CloseHandle(m_HostProcess.hProcess);
            m_HostProcess = {};
        }

        std::wstring hostStartReason;
        if (!StartDirectHostProcess(m_HostProcess, hostStartReason, forceApply, allowModeApply))
        {
            reason = hostStartReason;
            return false;
        }

        std::wstring readinessReason;
        if (!WaitForDirectHostReadiness(
                m_HostProcess.hProcess,
                m_HostProcess.dwProcessId,
                130 * 1000,
                acceptSupervisedStartup,
                readinessReason))
        {
            reason = acceptSupervisedStartup
                ? L"timed out waiting for MonitorSplitter direct host to enter supervised startup: " + readinessReason
                : L"timed out waiting for MonitorSplitter direct host to reach direct-shared mode: " + readinessReason;
            RequestDirectHostStop();
            WaitUntilDirectHostStopped(5 * 1000);
            return false;
        }

        reason.clear();
        return true;
    }

    bool Stop(DWORD timeoutMs, std::wstring& reason)
    {
        RequestDirectHostStop();
        bool directHostStopped = WaitUntilDirectHostStopped(timeoutMs);
        std::wstring stopFailure;

        if (m_HostProcess.hProcess != nullptr)
        {
            if (!directHostStopped)
            {
                TerminateProcess(m_HostProcess.hProcess, ERROR_SERVICE_REQUEST_TIMEOUT);
                WaitForSingleObject(m_HostProcess.hProcess, 3000);
                directHostStopped = WaitUntilDirectHostStopped(3000);
                if (!directHostStopped)
                {
                    stopFailure = L"direct host did not stop within " + std::to_wstring(timeoutMs) + L" ms";
                }
            }
            CloseHandle(m_HostProcess.hProcess);
            m_HostProcess = {};
        }
        else if (!directHostStopped)
        {
            stopFailure = L"direct host did not stop within " + std::to_wstring(timeoutMs) + L" ms";
        }

        if (m_SwDevice != nullptr || !m_DeviceInstanceId.empty())
        {
            std::wstring deviceRemovalFailure;
            if (!CloseSoftwareDeviceAndWait(timeoutMs, deviceRemovalFailure) && stopFailure.empty())
            {
                stopFailure = deviceRemovalFailure;
            }
        }
        else
        {
            RequestVirtualDeviceHolderStop();
            if (!WaitUntilVirtualDeviceStopped(timeoutMs))
            {
                reason = L"virtual device holder did not stop within " + std::to_wstring(timeoutMs) + L" ms";
                return false;
            }
        }
        ClearActiveLayout();
        m_StartedLayoutSpec.clear();
        m_StartedEdidNameBase.clear();
        m_DeviceInstanceId.clear();
        CloseOwnedHandles();
        reason = stopFailure;
        return stopFailure.empty();
    }

private:
    bool TryAdoptRunningSession(const PreparedConfiguration& config, std::wstring& reason)
    {
        const auto status = ReadTextFile(HostStatusPath());
        if (!HostStatusLayoutMatches(status, config.LayoutSpec))
        {
            reason = L"direct host status does not match the configured layout";
            return false;
        }
        if (!HostStatusInProgressForSupervision(status, &reason))
        {
            return false;
        }
        if (!HostStatusProcessIsAlive(status, &reason))
        {
            return false;
        }

        m_StartedLayoutSpec = config.LayoutSpec;
        m_StartedEdidNameBase = config.EdidNameBase;
        reason.clear();
        return true;
    }

    bool CreateVirtualDevice(
        const Layout& layout,
        const std::wstring& layoutSpec,
        const std::wstring& edidNameBase,
        std::wstring& reason)
    {
        LUID preferredRenderAdapter = {};
        std::wstring preferredRenderAdapterReason;
        const bool hasPreferredRenderAdapter = TryResolvePreferredRenderAdapterLuid(layout, preferredRenderAdapter, preferredRenderAdapterReason);

        std::wstring staleCleanupReason;
        if (!CleanupStaleLocalStackBeforeFreshStart(10 * 1000, staleCleanupReason))
        {
            reason = staleCleanupReason;
            return false;
        }

        m_RunningMutex = CreateLocalMutex(kRunningMutexName, TRUE);
        if (m_RunningMutex == nullptr)
        {
            reason = L"CreateMutex failed: " + std::to_wstring(GetLastError());
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            reason = L"virtual device holder is already running in this session";
            CloseOwnedHandles();
            return false;
        }

        m_ReadyEvent = CreateLocalEvent(kReadyEventName, TRUE, FALSE);
        m_FailedEvent = CreateLocalEvent(kFailedEventName, TRUE, FALSE);
        m_StopEvent = CreateLocalEvent(kStopEventName, TRUE, FALSE);
        if (m_ReadyEvent == nullptr || m_FailedEvent == nullptr || m_StopEvent == nullptr)
        {
            reason = L"CreateEvent failed: " + std::to_wstring(GetLastError());
            CloseOwnedHandles();
            return false;
        }
        ResetEvent(m_ReadyEvent);
        ResetEvent(m_FailedEvent);
        ResetEvent(m_StopEvent);
        ClearActiveLayout();

        CreationContext creation;
        creation.Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (creation.Event == nullptr)
        {
            reason = L"CreateEvent failed: " + std::to_wstring(GetLastError());
            CloseOwnedHandles();
            return false;
        }

        SW_DEVICE_CREATE_INFO createInfo = {};
        createInfo.cbSize = sizeof(createInfo);
        createInfo.pszzCompatibleIds = kCompatibleIds;
        createInfo.pszInstanceId = kInstanceId;
        createInfo.pszzHardwareIds = kHardwareIds;
        createInfo.pszDeviceDescription = kDescription;
        createInfo.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                                     SWDeviceCapabilitiesSilentInstall |
                                     SWDeviceCapabilitiesDriverRequired;

        DEVPROPERTY layoutProperty = {};
        layoutProperty.CompKey.Key = MonitorSplitter::kLayoutPropertyKey;
        layoutProperty.CompKey.Store = DEVPROP_STORE_SYSTEM;
        layoutProperty.Type = DEVPROP_TYPE_STRING;
        layoutProperty.BufferSize = static_cast<ULONG>((layoutSpec.size() + 1) * sizeof(wchar_t));
        layoutProperty.Buffer = const_cast<wchar_t*>(layoutSpec.c_str());

        std::vector<DEVPROPERTY> properties;
        properties.push_back(layoutProperty);

        DEVPROPERTY edidNameBaseProperty = {};
        if (!edidNameBase.empty())
        {
            edidNameBaseProperty.CompKey.Key = MonitorSplitter::kEdidNameBasePropertyKey;
            edidNameBaseProperty.CompKey.Store = DEVPROP_STORE_SYSTEM;
            edidNameBaseProperty.Type = DEVPROP_TYPE_STRING;
            edidNameBaseProperty.BufferSize = static_cast<ULONG>((edidNameBase.size() + 1) * sizeof(wchar_t));
            edidNameBaseProperty.Buffer = const_cast<wchar_t*>(edidNameBase.c_str());
            properties.push_back(edidNameBaseProperty);
        }

        DEVPROPERTY renderAdapterProperty = {};
        if (hasPreferredRenderAdapter)
        {
            renderAdapterProperty.CompKey.Key = MonitorSplitter::kRenderAdapterLuidPropertyKey;
            renderAdapterProperty.CompKey.Store = DEVPROP_STORE_SYSTEM;
            renderAdapterProperty.Type = DEVPROP_TYPE_BINARY;
            renderAdapterProperty.BufferSize = sizeof(preferredRenderAdapter);
            renderAdapterProperty.Buffer = &preferredRenderAdapter;
            properties.push_back(renderAdapterProperty);
        }

        HRESULT hr = SwDeviceCreate(
            kSoftwareDeviceClass,
            L"HTREE\\ROOT\\0",
            &createInfo,
            static_cast<ULONG>(properties.size()),
            properties.data(),
            CreationCallback,
            &creation,
            &m_SwDevice);
        if (FAILED(hr))
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
            {
                CloseHandle(creation.Event);
                creation.Event = nullptr;

                const auto existingInstanceId = SoftwareDeviceInstanceId();
                m_DeviceInstanceId = existingInstanceId;
                DevNodeStartStatus existingStatus;
                if (WaitForDevNodeStarted(existingInstanceId, 10 * 1000, existingStatus))
                {
                    if (!SaveActiveLayout(layout))
                    {
                        reason = L"could not write active layout config for existing virtual display device: " + ActiveLayoutPath();
                        CleanupFailedStart(reason);
                        return false;
                    }

                    auto enabledState = L"MonitorSplitter adopted existing virtual display device " +
                        existingInstanceId +
                        L" with layout " +
                        layoutSpec;
                    if (hasPreferredRenderAdapter)
                    {
                        enabledState += L" preferredRenderAdapter=" +
                            std::to_wstring(static_cast<unsigned long>(preferredRenderAdapter.LowPart)) +
                            L":" +
                            std::to_wstring(preferredRenderAdapter.HighPart);
                    }
                    else if (!preferredRenderAdapterReason.empty())
                    {
                        enabledState += L" preferredRenderAdapter=none (" + preferredRenderAdapterReason + L")";
                    }
                    if (!edidNameBase.empty())
                    {
                        enabledState += L" edidNameBase=" + edidNameBase;
                    }
                    WriteState(enabledState);

                    reason.clear();
                    m_StartedLayoutSpec = layoutSpec;
                    m_StartedEdidNameBase = edidNameBase;
                    SignalIfValid(m_ReadyEvent);
                    return true;
                }

                reason = L"existing MonitorSplitter virtual display devnode is not started: " +
                    FormatDevNodeStartStatus(existingStatus);
                SignalIfValid(m_FailedEvent);
                CleanupFailedStart(reason);
                return false;
            }

            reason = L"SwDeviceCreate failed: " + DescribeHResult(hr);
            SignalIfValid(m_FailedEvent);
            CloseHandle(creation.Event);
            CleanupFailedStart(reason);
            return false;
        }

        DWORD waitResult = WaitForSingleObject(creation.Event, 10 * 1000);
        if (waitResult != WAIT_OBJECT_0)
        {
            reason = L"timed out waiting for software device creation";
            SignalIfValid(m_FailedEvent);
            CloseHandle(creation.Event);
            CleanupFailedStart(reason);
            return false;
        }
        CloseHandle(creation.Event);

        if (FAILED(creation.Result))
        {
            reason = L"software device creation callback failed: " + DescribeHResult(creation.Result);
            SignalIfValid(m_FailedEvent);
            CleanupFailedStart(reason);
            return false;
        }
        m_DeviceInstanceId = creation.InstanceId;

        DevNodeStartStatus devNodeStatus;
        if (!WaitForDevNodeStarted(creation.InstanceId, 10 * 1000, devNodeStatus))
        {
            reason = L"MonitorSplitter devnode did not start: " + FormatDevNodeStartStatus(devNodeStatus);
            SignalIfValid(m_FailedEvent);
            CleanupFailedStart(reason);
            return false;
        }

        if (!SaveActiveLayout(layout))
        {
            reason = L"could not write active layout config: " + ActiveLayoutPath();
            SignalIfValid(m_FailedEvent);
            CleanupFailedStart(reason);
            return false;
        }

        auto enabledState = L"MonitorSplitter enabled as " + creation.InstanceId + L" with layout " + layoutSpec;
        if (hasPreferredRenderAdapter)
        {
            enabledState += L" preferredRenderAdapter=" +
                std::to_wstring(static_cast<unsigned long>(preferredRenderAdapter.LowPart)) +
                L":" +
                std::to_wstring(preferredRenderAdapter.HighPart);
        }
        else if (!preferredRenderAdapterReason.empty())
        {
            enabledState += L" preferredRenderAdapter=none (" + preferredRenderAdapterReason + L")";
        }
        if (!edidNameBase.empty())
        {
            enabledState += L" edidNameBase=" + edidNameBase;
        }
        WriteState(enabledState);

        reason.clear();
        m_StartedLayoutSpec = layoutSpec;
        m_StartedEdidNameBase = edidNameBase;
        SignalIfValid(m_ReadyEvent);
        return true;
    }

    void CleanupFailedStart(std::wstring& reason)
    {
        RequestDirectHostStop();
        bool directHostStopped = WaitUntilDirectHostStopped(3000);

        if (m_HostProcess.hProcess != nullptr)
        {
            if (!directHostStopped)
            {
                TerminateProcess(m_HostProcess.hProcess, ERROR_SERVICE_REQUEST_TIMEOUT);
                WaitForSingleObject(m_HostProcess.hProcess, 3000);
                directHostStopped = WaitUntilDirectHostStopped(3000);
            }
            CloseHandle(m_HostProcess.hProcess);
            m_HostProcess = {};
        }

        if (!directHostStopped && IsDirectHostRunning())
        {
            reason += L"; cleanup warning: direct host did not stop";
        }

        std::wstring deviceRemovalFailure;
        if (!CloseSoftwareDeviceAndWait(3000, deviceRemovalFailure))
        {
            reason += L"; cleanup warning: " + deviceRemovalFailure;
        }

        ClearActiveLayout();
        m_StartedLayoutSpec.clear();
        m_StartedEdidNameBase.clear();
        m_DeviceInstanceId.clear();
        CloseOwnedHandles();
    }

    bool CloseSoftwareDeviceAndWait(DWORD timeoutMs, std::wstring& failure)
    {
        failure.clear();
        const auto deviceInstanceId = m_DeviceInstanceId;
        if (m_SwDevice == nullptr)
        {
            if (deviceInstanceId.empty())
            {
                return true;
            }
            return TryRemoveDevNodeAndWait(deviceInstanceId, timeoutMs, failure);
        }

        SwDeviceClose(m_SwDevice);
        m_SwDevice = nullptr;

        if (deviceInstanceId.empty())
        {
            return true;
        }

        DevNodeStartStatus removedStatus;
        if (!WaitForDevNodeRemoved(deviceInstanceId, timeoutMs, removedStatus))
        {
            failure = L"virtual display devnode did not remove within " +
                      std::to_wstring(timeoutMs) +
                      L" ms: " +
                      FormatDevNodeStartStatus(removedStatus);
            return false;
        }

        return true;
    }

    void CloseOwnedHandles()
    {
        if (m_StopEvent != nullptr)
        {
            CloseHandle(m_StopEvent);
            m_StopEvent = nullptr;
        }
        if (m_ReadyEvent != nullptr)
        {
            CloseHandle(m_ReadyEvent);
            m_ReadyEvent = nullptr;
        }
        if (m_FailedEvent != nullptr)
        {
            CloseHandle(m_FailedEvent);
            m_FailedEvent = nullptr;
        }
        if (m_RunningMutex != nullptr)
        {
            CloseHandle(m_RunningMutex);
            m_RunningMutex = nullptr;
        }
    }

    HSWDEVICE m_SwDevice = nullptr;
    HANDLE m_RunningMutex = nullptr;
    HANDLE m_ReadyEvent = nullptr;
    HANDLE m_FailedEvent = nullptr;
    HANDLE m_StopEvent = nullptr;
    PROCESS_INFORMATION m_HostProcess = {};
    std::wstring m_StartedLayoutSpec;
    std::wstring m_StartedEdidNameBase;
    std::wstring m_DeviceInstanceId;
    bool m_SessionOnly = false;
};
}
