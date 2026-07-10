#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <iostream>
#include <sstream>
#include <string>

#include "..\MonitorSplitterCommon\BuildInfo.h"
#include "..\MonitorSplitterControl\DirectStack.h"

namespace
{
constexpr wchar_t kServiceDisplayName[] = L"MonitorSplitter Service";
constexpr wchar_t kAgentStopEventPrefix[] = L"Global\\MonitorSplitter.AgentStop.";
constexpr DWORD kCheckIntervalMs = 5000;
constexpr DWORD kRestartCooldownMs = 30000;
constexpr DWORD kFailedRestartCooldownMs = 30000;
constexpr DWORD kAgentRestartRequestCooldownMs = 30000;
constexpr DWORD kMaxConsecutiveRestartFailures = 3;
constexpr DWORD kMaxConsecutiveStuckRecoveries = 3;
constexpr DWORD kTrayLaunchRetryMs = 30000;
constexpr DWORD kResumeRecoveryRetryDelaysMs[] = { 0, 5000, 15000, 30000 };

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};
HANDLE g_stopEvent = nullptr;
HANDLE g_wakeEvent = nullptr;
HANDLE g_externalWakeEvent = nullptr;
volatile LONG g_forceRestart = 0;
ULONGLONG g_lastRestartTick = 0;
bool g_lastRestartSucceeded = true;
DWORD g_consecutiveRestartFailures = 0;
DWORD g_consecutiveStuckRecoveries = 0;
bool g_restartSuppressed = false;
ULONGLONG g_lastAgentRestartRequestTick = 0;
PROCESS_INFORMATION g_agentProcess = {};
DWORD g_agentSessionId = 0xFFFFFFFF;
HANDLE g_agentStopEvent = nullptr;
DWORD g_lastTrayLaunchSessionId = 0xFFFFFFFF;
ULONGLONG g_lastTrayLaunchAttemptTick = 0;

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

void AppendLog(const std::wstring& message);
bool AnyDirectHostStatusHealthy(std::wstring* details);
bool AnySplitterStackHealthy(std::wstring* details);
bool AnyDirectHostStartupInProgress(std::wstring* details);

SECURITY_ATTRIBUTES* ServiceWakeSecurity()
{
    static LocalSecurityDescriptor securityDescriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)");
    if (FAILED(securityDescriptor.Status()))
    {
        return nullptr;
    }

    return securityDescriptor.Attributes();
}

std::wstring AgentStopEventName(DWORD sessionId)
{
    return std::wstring(kAgentStopEventPrefix) + std::to_wstring(sessionId);
}

bool EnableCurrentProcessPrivilege(const wchar_t* privilegeName)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        return false;
    }

    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &privileges.Privileges[0].Luid))
    {
        CloseHandle(token);
        return false;
    }

    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    const bool ok = GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

std::wstring DirectoryName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return {};
    }
    return path.substr(0, slash);
}

bool ProcessStillActive(HANDLE process)
{
    if (process == nullptr)
    {
        return false;
    }

    DWORD exitCode = 0;
    return GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
}

void CloseHandleIfOpen(HANDLE& handle)
{
    if (handle != nullptr)
    {
        CloseHandle(handle);
        handle = nullptr;
    }
}

void CloseWorkerEventHandles()
{
    CloseHandleIfOpen(g_wakeEvent);
    CloseHandleIfOpen(g_externalWakeEvent);
    CloseHandleIfOpen(g_stopEvent);
}

void CloseAgentHandles()
{
    CloseHandleIfOpen(g_agentProcess.hThread);
    CloseHandleIfOpen(g_agentProcess.hProcess);
    CloseHandleIfOpen(g_agentStopEvent);
    g_agentProcess.dwProcessId = 0;
    g_agentProcess.dwThreadId = 0;
    g_agentSessionId = 0xFFFFFFFF;
}

bool IsProcessRunningInSession(const wchar_t* executableName, DWORD sessionId)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, executableName) != 0)
            {
                continue;
            }

            DWORD processSessionId = 0xFFFFFFFF;
            if (ProcessIdToSessionId(entry.th32ProcessID, &processSessionId) && processSessionId == sessionId)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

bool StartUserProcessInSession(
    DWORD sessionId,
    const std::wstring& executable,
    const std::wstring& arguments,
    std::wstring& reason)
{
    reason.clear();
    EnableCurrentProcessPrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
    EnableCurrentProcessPrivilege(SE_INCREASE_QUOTA_NAME);
    EnableCurrentProcessPrivilege(SE_TCB_NAME);

    HANDLE serviceToken = nullptr;
    if (!OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            &serviceToken))
    {
        reason = L"OpenProcessToken failed: " + std::to_wstring(GetLastError());
        return false;
    }

    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(
            serviceToken,
            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            nullptr,
            SecurityIdentification,
            TokenPrimary,
            &primaryToken))
    {
        reason = L"DuplicateTokenEx failed: " + std::to_wstring(GetLastError());
        CloseHandle(serviceToken);
        return false;
    }
    CloseHandle(serviceToken);

    if (!SetTokenInformation(primaryToken, TokenSessionId, &sessionId, sizeof(sessionId)))
    {
        reason = L"SetTokenInformation(TokenSessionId) failed: " + std::to_wstring(GetLastError());
        CloseHandle(primaryToken);
        return false;
    }

    void* environment = nullptr;
    DWORD creationFlags = 0;
    if (CreateEnvironmentBlock(&environment, primaryToken, FALSE))
    {
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    }

    std::wstring commandLine = L"\"" + executable + L"\"";
    if (!arguments.empty())
    {
        commandLine += L" ";
        commandLine += arguments;
    }

    std::wstring workingDirectory = DirectoryName(executable);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessAsUserW(
        primaryToken,
        executable.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        creationFlags,
        environment,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startup,
        &process);

    const DWORD createError = GetLastError();
    if (environment != nullptr)
    {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(primaryToken);

    if (!created)
    {
        reason = L"CreateProcessAsUser failed: " + std::to_wstring(createError);
        return false;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool ShouldAttemptTrayLaunch(DWORD sessionId, ULONGLONG now)
{
    if (g_lastTrayLaunchSessionId != sessionId)
    {
        return true;
    }
    if (g_lastTrayLaunchAttemptTick == 0)
    {
        return true;
    }
    return now - g_lastTrayLaunchAttemptTick >= kTrayLaunchRetryMs;
}

void EnsureTrayInActiveSession()
{
    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF)
    {
        return;
    }

    if (IsProcessRunningInSession(L"MonitorSplitterConfig.exe", sessionId))
    {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (!ShouldAttemptTrayLaunch(sessionId, now))
    {
        return;
    }

    g_lastTrayLaunchSessionId = sessionId;
    g_lastTrayLaunchAttemptTick = now;

    const auto executable = MonitorSplitter::Control::SiblingExecutablePath(L"MonitorSplitterConfig.exe");
    if (executable.empty())
    {
        AppendLog(L"could not resolve MonitorSplitterConfig.exe path for tray launch");
        return;
    }

    std::wstring reason;
    if (StartUserProcessInSession(sessionId, executable, L"--tray", reason))
    {
        AppendLog(L"started tray utility in session " + std::to_wstring(sessionId));
    }
    else
    {
        AppendLog(L"could not start tray utility in session " + std::to_wstring(sessionId) + L": " + reason);
    }
}

bool StartAgentInSession(DWORD sessionId, std::wstring& reason, bool forceFirstRecovery = false)
{
    reason.clear();
    EnableCurrentProcessPrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
    EnableCurrentProcessPrivilege(SE_INCREASE_QUOTA_NAME);
    EnableCurrentProcessPrivilege(SE_TCB_NAME);

    HANDLE queryToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &queryToken))
    {
        reason = L"WTSQueryUserToken failed: " + std::to_wstring(GetLastError());
        return false;
    }

    HANDLE primaryToken = nullptr;
    if (!DuplicateTokenEx(
            queryToken,
            TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
            nullptr,
            SecurityIdentification,
            TokenPrimary,
            &primaryToken))
    {
        reason = L"DuplicateTokenEx for user session failed: " + std::to_wstring(GetLastError());
        CloseHandle(queryToken);
        return false;
    }
    CloseHandle(queryToken);

    const auto executable = MonitorSplitter::Control::SiblingExecutablePath(L"MonitorSplitterService.exe");
    if (executable.empty())
    {
        reason = L"could not resolve MonitorSplitterService.exe path";
        CloseHandle(primaryToken);
        return false;
    }

    g_agentStopEvent = CreateEventW(ServiceWakeSecurity(), TRUE, FALSE, AgentStopEventName(sessionId).c_str());
    if (g_agentStopEvent == nullptr)
    {
        reason = L"CreateEvent for agent stop failed: " + std::to_wstring(GetLastError());
        CloseHandle(primaryToken);
        return false;
    }
    ResetEvent(g_agentStopEvent);

    HANDLE agentWakeEvent = CreateEventW(ServiceWakeSecurity(), TRUE, FALSE, MonitorSplitter::Control::kAgentWakeEventName);
    if (agentWakeEvent != nullptr)
    {
        CloseHandle(agentWakeEvent);
    }

    void* environment = nullptr;
    DWORD creationFlags = DETACHED_PROCESS;
    if (CreateEnvironmentBlock(&environment, primaryToken, FALSE))
    {
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    }

    std::wstring commandLine = L"\"" + executable + L"\" --agent";
    if (forceFirstRecovery)
    {
        commandLine += L" --force-first-recovery";
    }
    std::wstring workingDirectory = DirectoryName(executable);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessAsUserW(
        primaryToken,
        executable.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        creationFlags,
        environment,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startup,
        &process);

    const DWORD createError = GetLastError();
    if (environment != nullptr)
    {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(primaryToken);

    if (!created)
    {
        reason = L"CreateProcessAsUser for session agent failed: " + std::to_wstring(createError);
        CloseAgentHandles();
        return false;
    }

    g_agentProcess = process;
    g_agentSessionId = sessionId;
    return true;
}

void StopAgentProcess(DWORD timeoutMs)
{
    if (g_agentProcess.hProcess == nullptr)
    {
        CloseAgentHandles();
        return;
    }

    if (g_agentStopEvent != nullptr)
    {
        SetEvent(g_agentStopEvent);
    }
    if (g_externalWakeEvent != nullptr)
    {
        SetEvent(g_externalWakeEvent);
    }

    if (WaitForSingleObject(g_agentProcess.hProcess, timeoutMs) != WAIT_OBJECT_0)
    {
        AppendLog(L"session agent did not exit in time; terminating process");
        TerminateProcess(g_agentProcess.hProcess, ERROR_SERVICE_REQUEST_TIMEOUT);
        WaitForSingleObject(g_agentProcess.hProcess, 3000);
    }

    CloseAgentHandles();
}

void RequestAgentRecovery(const wchar_t* reason, bool bypassCooldown = false)
{
    const ULONGLONG now = GetTickCount64();
    if (!bypassCooldown && g_lastAgentRestartRequestTick != 0 && now - g_lastAgentRestartRequestTick < kAgentRestartRequestCooldownMs)
    {
        return;
    }

    g_lastAgentRestartRequestTick = now;
    AppendLog(std::wstring(L"requesting session agent recovery: ") + reason);
    MonitorSplitter::Control::RequestServiceRestart();
    MonitorSplitter::Control::SignalAgentWake();
}

void EnsureSessionAgent(bool forceRestart, bool bypassRecoveryCooldown = false)
{
    const DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF)
    {
        AppendLog(L"no active console session for MonitorSplitter agent");
        return;
    }

    if (g_agentProcess.hProcess != nullptr && !ProcessStillActive(g_agentProcess.hProcess))
    {
        AppendLog(L"session agent exited");
        CloseAgentHandles();
    }

    if (g_agentProcess.hProcess != nullptr && g_agentSessionId != sessionId)
    {
        AppendLog(L"active console session changed; restarting session agent");
        StopAgentProcess(15000);
    }

    if (g_agentProcess.hProcess != nullptr)
    {
        if (forceRestart)
        {
            std::wstring hostReason;
            if (AnySplitterStackHealthy(&hostReason))
            {
                AppendLog(L"skipping session agent recovery; splitter stack is healthy");
                return;
            }
            if (AnyDirectHostStartupInProgress(&hostReason))
            {
                AppendLog(L"skipping session agent recovery; direct host is still starting or recovering");
                return;
            }
            RequestAgentRecovery(L"requested recovery", bypassRecoveryCooldown);
        }
        return;
    }

    std::wstring reason;
    if (StartAgentInSession(sessionId, reason, forceRestart))
    {
        AppendLog(
            L"started session agent in session " +
            std::to_wstring(sessionId) +
            (forceRestart ? L" with immediate first recovery" : L""));
    }
    else
    {
        AppendLog(L"could not start session agent in session " + std::to_wstring(sessionId) + L": " + reason);
    }
}

std::wstring LogPath()
{
    return MonitorSplitter::Control::ProgramDataConfigDirectory() + L"\\service.log";
}

void AppendLog(const std::wstring& message)
{
    MonitorSplitter::Control::EnsureDirectory(MonitorSplitter::Control::ProgramDataConfigDirectory());

    SYSTEMTIME time = {};
    GetLocalTime(&time);

    wchar_t prefix[64] = {};
    swprintf_s(
        prefix,
        L"%04u-%02u-%02u %02u:%02u:%02u ",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);

    const std::string text = MonitorSplitter::Control::ToUtf8(std::wstring(prefix) + message + L"\r\n");
    HANDLE file = CreateFileW(
        LogPath().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

void WriteRuntimeStatus(const wchar_t* role, const wchar_t* state)
{
    MonitorSplitter::Control::EnsureDirectory(MonitorSplitter::Control::ProgramDataConfigDirectory());

    DWORD sessionId = 0xFFFFFFFF;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);

    DWORD agentPid = 0;
    if (g_agentProcess.hProcess != nullptr && ProcessStillActive(g_agentProcess.hProcess))
    {
        agentPid = g_agentProcess.dwProcessId;
    }

    std::wstringstream status;
    status << L"{\"running\":" << (_wcsicmp(state, L"stopped") == 0 ? L"false" : L"true");
    status << L",\"role\":\"" << MonitorSplitter::Control::JsonEscape(role) << L"\"";
    status << L",\"state\":\"" << MonitorSplitter::Control::JsonEscape(state) << L"\"";
    status << L",\"pid\":" << GetCurrentProcessId();
    status << L",\"sessionId\":" << sessionId;
    status << L",\"updatedTick\":" << GetTickCount();
    status << L",\"desiredEnabled\":" << (MonitorSplitter::Control::DesiredEnabled() ? L"true" : L"false");
    status << L",\"agentPid\":" << agentPid;
    status << L",\"agentSessionId\":" << g_agentSessionId;
    status << L",\"component\":{";
    status << L"\"name\":\"MonitorSplitterService\"";
    status << L",\"productVersion\":\"" << MonitorSplitter::Control::JsonEscape(MonitorSplitter::kProductVersionWide) << L"\"";
    status << L",\"buildTag\":\"" << MonitorSplitter::Control::JsonEscape(MonitorSplitter::kBuildTagWide) << L"\"";
    status << L"}}";

    const std::wstring path = _wcsicmp(role, L"agent") == 0
        ? MonitorSplitter::Control::AgentStatusPath()
        : MonitorSplitter::Control::ServiceStatusPath();
    MonitorSplitter::Control::WriteTextFile(path, status.str());
}

int PrintVersionJson()
{
    DWORD sessionId = 0xFFFFFFFF;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    std::wcout << L"{\"name\":\"MonitorSplitterService\"";
    std::wcout << L",\"productVersion\":\"" << MonitorSplitter::Control::JsonEscape(MonitorSplitter::kProductVersionWide) << L"\"";
    std::wcout << L",\"buildTag\":\"" << MonitorSplitter::Control::JsonEscape(MonitorSplitter::kBuildTagWide) << L"\"";
    std::wcout << L",\"pid\":" << GetCurrentProcessId();
    std::wcout << L",\"sessionId\":" << sessionId;
    std::wcout << L"}\n";
    return 0;
}

bool CurrentHostStatusHealthy(DWORD expectedPid, std::wstring* details)
{
    if (!MonitorSplitter::Control::IsDirectHostRunning())
    {
        if (details != nullptr)
        {
            *details = L"direct host is not running";
        }
        return false;
    }

    const std::wstring compact = MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::HostStatusPath());
    std::wstring reason;
    const bool healthy = MonitorSplitter::Control::HostStatusHealthy(compact, &reason, expectedPid);
    if (!healthy && details != nullptr)
    {
        *details = reason.empty() ? L"direct host status is not healthy" : reason;
    }
    return healthy;
}

bool SavedDesktopLayoutActive(std::wstring* details)
{
    MonitorSplitter::Control::PreparedConfiguration prepared;
    std::wstring reason;
    if (!MonitorSplitter::Control::PrepareSavedConfiguration(prepared, reason))
    {
        if (details != nullptr)
        {
            *details = L"saved splitter configuration is incomplete: " + reason;
        }
        return false;
    }

    if (!MonitorSplitter::Control::MonitorSplitterDesktopLayoutActive(prepared.LayoutValue, reason))
    {
        if (details != nullptr)
        {
            *details = L"MonitorSplitter desktop layout is not active: " + reason;
        }
        return false;
    }

    if (details != nullptr)
    {
        details->clear();
    }
    return true;
}

bool CurrentSplitterStackHealthy(DWORD expectedPid, std::wstring* details)
{
    std::wstring reason;
    if (!CurrentHostStatusHealthy(expectedPid, &reason))
    {
        if (details != nullptr)
        {
            *details = reason;
        }
        return false;
    }
    if (!SavedDesktopLayoutActive(&reason))
    {
        if (details != nullptr)
        {
            *details = reason;
        }
        return false;
    }
    if (details != nullptr)
    {
        details->clear();
    }
    return true;
}

bool CurrentHostStartupInProgress(DWORD expectedPid, std::wstring* details)
{
    if (!MonitorSplitter::Control::IsDirectHostRunning())
    {
        if (details != nullptr)
        {
            *details = L"direct host is not running";
        }
        return false;
    }

    const std::wstring compact = MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::HostStatusPath());
    std::wstring reason;
    const bool inProgress = MonitorSplitter::Control::HostStatusStartupInProgress(compact, &reason, expectedPid);
    if (!inProgress && details != nullptr)
    {
        *details = reason.empty() ? L"direct host is not starting or recovering" : reason;
    }
    return inProgress;
}

bool AnyDirectHostStatusHealthy(std::wstring* details)
{
    // The service supervisor runs in session 0 while the direct host is owned by the
    // interactive-session agent. Local named objects used by IsDirectHostRunning()
    // are session-scoped, so session 0 must judge cross-session health from the
    // shared status file instead.
    const std::wstring compact = MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::HostStatusPath());
    std::wstring reason;
    const bool healthy = MonitorSplitter::Control::HostStatusHealthy(compact, &reason);
    if (!healthy && details != nullptr)
    {
        *details = reason.empty() ? L"direct host status is not healthy" : reason;
    }
    return healthy;
}

bool AnySplitterStackHealthy(std::wstring* details)
{
    std::wstring reason;
    if (!AnyDirectHostStatusHealthy(&reason))
    {
        if (details != nullptr)
        {
            *details = reason;
        }
        return false;
    }
    if (!SavedDesktopLayoutActive(&reason))
    {
        if (details != nullptr)
        {
            *details = reason;
        }
        return false;
    }
    if (details != nullptr)
    {
        details->clear();
    }
    return true;
}

bool AnyDirectHostStartupInProgress(std::wstring* details)
{
    // Same cross-session limitation as AnyDirectHostStatusHealthy: rely on the
    // shared status file instead of session-local host mutexes.
    const std::wstring compact = MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::HostStatusPath());
    std::wstring reason;
    const bool inProgress = MonitorSplitter::Control::HostStatusStartupInProgress(compact, &reason);
    if (!inProgress && details != nullptr)
    {
        *details = reason.empty() ? L"direct host is not starting or recovering" : reason;
    }
    return inProgress;
}

bool ShouldCancelDeferredAgentRecovery(bool desired, DWORD deferredRestartStage, bool directHostHealthy)
{
    return desired && deferredRestartStage > 0 && directHostHealthy;
}

bool ShouldStopSplitterOnAgentExit(bool desired, bool directHostRunning, bool virtualDeviceRunning)
{
    return desired || directHostRunning || virtualDeviceRunning;
}

bool ShouldThrottleSplitterRestart(
    ULONGLONG now,
    ULONGLONG lastRestartTick,
    bool lastRestartSucceeded,
    bool bypassCooldown)
{
    if (bypassCooldown || lastRestartTick == 0)
    {
        return false;
    }

    const DWORD cooldownMs = lastRestartSucceeded ? kRestartCooldownMs : kFailedRestartCooldownMs;
    return now - lastRestartTick < cooldownMs;
}

void RestartSplitter(
    MonitorSplitter::Control::DirectStack& stack,
    const wchar_t* reason,
    bool bypassCooldown = false,
    bool forceHostRestart = false,
    bool stuckRecovery = false,
    bool forceModeApply = false)
{
    const ULONGLONG now = GetTickCount64();
    if (g_restartSuppressed && !bypassCooldown)
    {
        return;
    }
    if (ShouldThrottleSplitterRestart(now, g_lastRestartTick, g_lastRestartSucceeded, bypassCooldown))
    {
        return;
    }

    if (stuckRecovery)
    {
        // Repeating an identical restart against a host that keeps ending up
        // stuck in a DisplayCore mode change is a kill loop, not a recovery.
        // Each attempt below is a forced restart with a forced mode reapply;
        // if that recipe fails several times in a row, stop and wait for an
        // explicit request, logon, unlock, or wake event.
        if (g_consecutiveStuckRecoveries >= kMaxConsecutiveStuckRecoveries)
        {
            g_restartSuppressed = true;
            AppendLog(L"stuck-mode-change recovery budget exhausted; waiting for a new explicit restart request, logon, unlock, or wake event");
            return;
        }
        g_consecutiveStuckRecoveries++;
    }

    g_lastRestartTick = now;
    AppendLog(std::wstring(forceHostRestart ? L"restarting splitter: " : L"recovering splitter: ") + reason);
    std::wstring startReason;
    g_lastRestartSucceeded = stack.RecoverSession(startReason, forceHostRestart, forceModeApply);
    if (!g_lastRestartSucceeded)
    {
        g_consecutiveRestartFailures++;
        AppendLog(L"start during restart failed: " + startReason);
        if (g_consecutiveRestartFailures >= kMaxConsecutiveRestartFailures)
        {
            g_consecutiveRestartFailures = 0;
            AppendLog(L"restart budget exhausted; continuing automatic recovery after failed-restart cooldown");
        }
    }
    else
    {
        g_consecutiveRestartFailures = 0;
        g_restartSuppressed = false;
    }
}

void MarkSplitterHealthy()
{
    if (g_lastRestartTick != 0)
    {
        AppendLog(L"splitter is healthy; clearing restart cooldown");
        g_lastRestartTick = 0;
        g_lastRestartSucceeded = true;
        g_consecutiveRestartFailures = 0;
        g_consecutiveStuckRecoveries = 0;
        g_restartSuppressed = false;
    }
}

void EnsureSplitter(
    MonitorSplitter::Control::DirectStack& stack,
    bool recoveryRequested,
    bool bypassCooldown = false,
    bool forceHostRestart = false)
{
    if (recoveryRequested)
    {
        g_consecutiveRestartFailures = 0;
        g_consecutiveStuckRecoveries = 0;
        g_restartSuppressed = false;
        RestartSplitter(
            stack,
            forceHostRestart ? L"forced recovery" : L"requested recovery",
            bypassCooldown,
            forceHostRestart,
            false,
            false);
        return;
    }

    std::wstring reason;
    if (CurrentSplitterStackHealthy(stack.ActiveHostProcessId(), &reason))
    {
        MarkSplitterHealthy();
    }
    else if (CurrentHostStartupInProgress(stack.ActiveHostProcessId(), &reason))
    {
        // The host owns the panel and is still progressing through DisplayCore
        // setup. Do not kill it just because it has not reached direct-shared yet.
    }
    else
    {
        // A host stuck in a DisplayCore mode change gets the tray Apply button
        // treatment automatically: a forced restart whose new host performs a
        // forced mode reapply (see RecoverSession/--force-apply).
        const bool stuck = MonitorSplitter::Control::HostStatusLikelyStuckInModeChange(
            MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::HostStatusPath()));
        RestartSplitter(stack, reason.c_str(), false, stuck, stuck, stuck);
    }
}

void SetServiceStatusValue(DWORD currentState, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0)
{
    static DWORD checkpoint = 1;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = currentState;
    g_status.dwWin32ExitCode = win32ExitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = currentState == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_POWEREVENT | SERVICE_ACCEPT_SESSIONCHANGE
        : 0;
    g_status.dwCheckPoint = (currentState == SERVICE_START_PENDING || currentState == SERVICE_STOP_PENDING) ? checkpoint++ : 0;

    if (g_statusHandle != nullptr)
    {
        SetServiceStatus(g_statusHandle, &g_status);
    }
}

DWORD WINAPI ServiceHandler(DWORD control, DWORD eventType, LPVOID, LPVOID)
{
    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        SetServiceStatusValue(SERVICE_STOP_PENDING, NO_ERROR, 30000);
        if (g_stopEvent != nullptr)
        {
            SetEvent(g_stopEvent);
        }
        return NO_ERROR;
    case SERVICE_CONTROL_POWEREVENT:
        if (eventType == PBT_APMRESUMEAUTOMATIC || eventType == PBT_APMRESUMESUSPEND || eventType == PBT_APMRESUMECRITICAL)
        {
            InterlockedExchange(&g_forceRestart, 1);
            if (g_wakeEvent != nullptr)
            {
                SetEvent(g_wakeEvent);
            }
        }
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (eventType == WTS_SESSION_LOGON || eventType == WTS_SESSION_UNLOCK || eventType == WTS_CONSOLE_CONNECT)
        {
            InterlockedExchange(&g_forceRestart, 1);
            if (g_wakeEvent != nullptr)
            {
                SetEvent(g_wakeEvent);
            }
        }
        return NO_ERROR;
    default:
        return NO_ERROR;
    }
}

void AgentWorkerLoop(bool forceFirstRecovery = false)
{
    const auto configDir = MonitorSplitter::Control::ProgramDataConfigDirectory();
    MonitorSplitter::Control::EnsureDirectory(configDir);
    SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", configDir.c_str());
    MonitorSplitter::Control::DirectStack stack;

    bool haveLastDesired = false;
    bool lastDesired = false;
    bool firstPass = true;
    bool disabledStopComplete = false;
    bool forceNextRecovery = forceFirstRecovery;
    ULONGLONG lastPanelBlockLogTick = 0;
    std::wstring lastRestartRequest = MonitorSplitter::Trim(
        MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::RestartRequestPath()));

    for (;;)
    {
        WriteRuntimeStatus(L"agent", L"running");
        const ULONGLONG now = GetTickCount64();
        const bool desired = MonitorSplitter::Control::DesiredEnabled();
        const bool forced = InterlockedExchange(&g_forceRestart, 0) != 0;
        const auto restartRequest = MonitorSplitter::Trim(
            MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::RestartRequestPath()));
        const bool restartRequested = desired && !restartRequest.empty() && restartRequest != lastRestartRequest;
        if (restartRequested)
        {
            lastRestartRequest = restartRequest;
        }
        const bool forceRequestedRestart = restartRequested &&
            MonitorSplitter::Control::IsForcedRestartRequest(restartRequest);

        if (!desired)
        {
            if (disabledStopComplete &&
                (MonitorSplitter::Control::IsDirectHostRunning() || MonitorSplitter::Control::IsVirtualDeviceRunning()))
            {
                AppendLog(L"desired state is disabled but splitter objects are running; stopping again");
                disabledStopComplete = false;
            }
            if (!disabledStopComplete)
            {
                AppendLog(L"desired state is disabled; stopping splitter");
                std::wstring stopReason;
                disabledStopComplete = stack.StopSession(10000, stopReason);
                if (!disabledStopComplete && !stopReason.empty())
                {
                    AppendLog(L"stop failed: " + stopReason);
                }
            }
            haveLastDesired = true;
            lastDesired = false;
        }
        else
        {
            disabledStopComplete = false;
            const bool desiredTransition = firstPass || !haveLastDesired || !lastDesired;
            const auto panelBlockReason = MonitorSplitter::Control::HostPanelEnableBlockReason();
            if (!panelBlockReason.empty())
            {
                if (lastPanelBlockLogTick == 0 || now - lastPanelBlockLogTick >= 30000)
                {
                    AppendLog(L"enable blocked: " + panelBlockReason);
                    lastPanelBlockLogTick = now;
                }
                if (MonitorSplitter::Control::IsDirectHostRunning() || MonitorSplitter::Control::IsVirtualDeviceRunning())
                {
                    AppendLog(L"panel is not ready for direct scanout; stopping splitter");
                    std::wstring stopReason;
                    if (!stack.StopSession(10000, stopReason) && !stopReason.empty())
                    {
                        AppendLog(L"stop failed: " + stopReason);
                    }
                }
                haveLastDesired = true;
                lastDesired = false;
            }
            else
            {
                lastPanelBlockLogTick = 0;
                const bool oneShotForceRecovery = forceNextRecovery;
                forceNextRecovery = false;
                const bool recoveryRequested = forced || restartRequested || oneShotForceRecovery;
                // A wake/session first pass must restart the direct host even if
                // counters look healthy. Do not turn that into a forced display
                // apply: that path can leave the IddCx producer frozen.
                const bool forceHostRestart = oneShotForceRecovery;
                const bool bypassCooldown = forceRequestedRestart || oneShotForceRecovery;
                EnsureSplitter(
                    stack,
                    desiredTransition || recoveryRequested,
                    bypassCooldown,
                    forceHostRestart);

                haveLastDesired = true;
                lastDesired = true;
            }
        }

        firstPass = false;

        HANDLE handles[] = { g_stopEvent, g_wakeEvent, g_externalWakeEvent };
        const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, kCheckIntervalMs);
        if (wait == WAIT_OBJECT_0)
        {
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1)
        {
            ResetEvent(g_wakeEvent);
        }
        if (wait == WAIT_OBJECT_0 + 2)
        {
            ResetEvent(g_externalWakeEvent);
        }
    }

    if (ShouldStopSplitterOnAgentExit(
            MonitorSplitter::Control::DesiredEnabled(),
            MonitorSplitter::Control::IsDirectHostRunning(),
            MonitorSplitter::Control::IsVirtualDeviceRunning()))
    {
        AppendLog(L"agent exiting; stopping splitter stack");
        std::wstring stopReason;
        if (!stack.StopSession(15000, stopReason) && !stopReason.empty())
        {
            AppendLog(L"agent exit stop failed: " + stopReason);
        }
    }
    WriteRuntimeStatus(L"agent", L"stopped");
}

void ServiceSupervisorLoop()
{
    MonitorSplitter::Control::EnsureDirectory(MonitorSplitter::Control::ProgramDataConfigDirectory());
    SetEnvironmentVariableW(
        L"MONITORSPLITTER_CONFIGDIR",
        MonitorSplitter::Control::ProgramDataConfigDirectory().c_str());

    MonitorSplitter::Control::DirectStack systemStack;
    ULONGLONG deferredRestartDue = 0;
    DWORD deferredRestartStage = 0;
    ULONGLONG lastPanelBlockLogTick = 0;
    std::wstring lastRestartRequest = MonitorSplitter::Trim(
        MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::RestartRequestPath()));
    for (;;)
    {
        WriteRuntimeStatus(L"service", L"running");
        EnsureTrayInActiveSession();

        const bool desired = MonitorSplitter::Control::DesiredEnabled();
        const bool forced = InterlockedExchange(&g_forceRestart, 0) != 0;
        bool immediateWakeRecovery = false;
        const auto restartRequest = MonitorSplitter::Trim(
            MonitorSplitter::Control::ReadTextFile(MonitorSplitter::Control::RestartRequestPath()));
        const bool restartRequested = desired && !restartRequest.empty() && restartRequest != lastRestartRequest;
        if (restartRequested)
        {
            lastRestartRequest = restartRequest;
        }
        const bool forceRequestedRestart = restartRequested &&
            MonitorSplitter::Control::IsForcedRestartRequest(restartRequest);
        if (forced)
        {
            immediateWakeRecovery = true;
            deferredRestartStage = 1;
            deferredRestartDue = GetTickCount64() + kResumeRecoveryRetryDelaysMs[deferredRestartStage];
            AppendLog(L"immediate session agent recovery requested after wake/session event");
        }

        if (deferredRestartDue != 0 && deferredRestartStage > 0)
        {
            std::wstring healthReason;
            if (ShouldCancelDeferredAgentRecovery(desired, deferredRestartStage, AnySplitterStackHealthy(&healthReason)))
            {
                AppendLog(L"canceling deferred session agent recovery; direct host is healthy");
                deferredRestartDue = 0;
                deferredRestartStage = 0;
            }
            else if (!healthReason.empty())
            {
                AppendLog(L"deferred session agent recovery still pending: " + healthReason);
            }
        }

        bool restartDue = false;
        if (deferredRestartDue != 0 && GetTickCount64() >= deferredRestartDue)
        {
            restartDue = true;
            deferredRestartStage++;
            if (deferredRestartStage < ARRAYSIZE(kResumeRecoveryRetryDelaysMs))
            {
                const DWORD previousDelay = kResumeRecoveryRetryDelaysMs[deferredRestartStage - 1];
                const DWORD nextDelay = kResumeRecoveryRetryDelaysMs[deferredRestartStage];
                const DWORD retryDelay = nextDelay > previousDelay + 1000 ? nextDelay - previousDelay : 1000;
                deferredRestartDue = GetTickCount64() + retryDelay;
            }
            else
            {
                deferredRestartDue = 0;
            }
        }
        const bool refreshVirtualDevice = forceRequestedRestart;
        const bool recoverSessionAgent = immediateWakeRecovery || restartDue || forceRequestedRestart;

        if (desired)
        {
            MonitorSplitter::Control::PreparedConfiguration prepared;
            std::wstring configReason;
            if (!MonitorSplitter::Control::PrepareSavedConfiguration(prepared, configReason))
            {
                const ULONGLONG now = GetTickCount64();
                if (lastPanelBlockLogTick == 0 || now - lastPanelBlockLogTick >= 30000)
                {
                    AppendLog(L"enable blocked before virtual device start: " + configReason);
                    lastPanelBlockLogTick = now;
                }
                if (g_agentProcess.hProcess != nullptr)
                {
                    AppendLog(L"configuration is incomplete; stopping session agent");
                    StopAgentProcess(15000);
                }
                std::wstring stopReason;
                if (!systemStack.StopVirtualDeviceOnly(15000, stopReason) && !stopReason.empty())
                {
                    AppendLog(L"virtual display device stop failed: " + stopReason);
                }
            }
            else if (const auto panelBlockReason = MonitorSplitter::Control::HostPanelEnableBlockReason(); !panelBlockReason.empty())
            {
                const ULONGLONG now = GetTickCount64();
                if (lastPanelBlockLogTick == 0 || now - lastPanelBlockLogTick >= 30000)
                {
                    AppendLog(L"enable blocked before virtual device start: " + panelBlockReason);
                    lastPanelBlockLogTick = now;
                }
                if (g_agentProcess.hProcess != nullptr)
                {
                    AppendLog(L"panel is not ready for direct scanout; stopping session agent");
                    StopAgentProcess(15000);
                }
                std::wstring stopReason;
                if (!systemStack.StopVirtualDeviceOnly(15000, stopReason) && !stopReason.empty())
                {
                    AppendLog(L"virtual display device stop failed: " + stopReason);
                }
            }
            else
            {
                lastPanelBlockLogTick = 0;
                if (refreshVirtualDevice)
                {
                    AppendLog(forceRequestedRestart
                        ? L"forced restart requested; refreshing virtual display device"
                        : L"deferred recovery due; refreshing virtual display device");
                    if (g_agentProcess.hProcess != nullptr)
                    {
                        StopAgentProcess(15000);
                    }
                    std::wstring stopReason;
                    if (!systemStack.StopVirtualDeviceOnly(15000, stopReason) && !stopReason.empty())
                    {
                        AppendLog(L"virtual display device stop failed: " + stopReason);
                    }
                }
                else if (restartDue)
                {
                    AppendLog(L"deferred recovery due; requesting session agent recovery");
                }
                if (immediateWakeRecovery && g_agentProcess.hProcess != nullptr)
                {
                    AppendLog(L"wake/session event; restarting session agent immediately");
                    StopAgentProcess(15000);
                }
                std::wstring deviceReason;
                if (systemStack.StartVirtualDeviceOnly(deviceReason))
                {
                    EnsureSessionAgent(recoverSessionAgent, recoverSessionAgent);
                }
                else
                {
                    AppendLog(L"virtual display device start failed: " + deviceReason);
                }
            }
        }
        else
        {
            lastPanelBlockLogTick = 0;
            if (g_agentProcess.hProcess != nullptr)
            {
                AppendLog(L"desired state disabled; stopping session agent");
                StopAgentProcess(15000);
            }
            std::wstring stopReason;
            if (!systemStack.StopVirtualDeviceOnly(15000, stopReason) && !stopReason.empty())
            {
                AppendLog(L"virtual display device stop failed: " + stopReason);
            }
        }

        HANDLE handles[] = { g_stopEvent, g_wakeEvent, g_externalWakeEvent };
        const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, kCheckIntervalMs);
        if (wait == WAIT_OBJECT_0)
        {
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1)
        {
            ResetEvent(g_wakeEvent);
        }
        if (wait == WAIT_OBJECT_0 + 2)
        {
            ResetEvent(g_externalWakeEvent);
        }
    }

    StopAgentProcess(15000);
    std::wstring stopReason;
    if (!systemStack.StopVirtualDeviceOnly(15000, stopReason) && !stopReason.empty())
    {
        AppendLog(L"virtual display device stop failed: " + stopReason);
    }
}

int ServiceSelfTest()
{
    struct Case
    {
        bool Desired;
        DWORD Stage;
        bool Healthy;
        bool Expected;
        const wchar_t* Name;
    };

    const Case cases[] = {
        { true, 0, true, false, L"first recovery must not be canceled" },
        { true, 1, true, true, L"later recovery cancels once healthy" },
        { true, 2, false, false, L"later recovery stays pending while unhealthy" },
        { false, 1, true, false, L"disabled state does not cancel via health" },
    };

    for (const auto& test : cases)
    {
        if (ShouldCancelDeferredAgentRecovery(test.Desired, test.Stage, test.Healthy) != test.Expected)
        {
            std::wcerr << L"service selftest failed: " << test.Name << L"\n";
            return 1;
        }
    }

    struct StopCase
    {
        bool Desired;
        bool DirectHostRunning;
        bool VirtualDeviceRunning;
        bool Expected;
        const wchar_t* Name;
    };

    const StopCase stopCases[] = {
        { true, false, false, true, L"desired enabled should stop stack on agent exit" },
        { false, true, false, true, L"running direct host should stop stack on agent exit" },
        { false, false, true, true, L"running virtual device should stop stack on agent exit" },
        { false, false, false, false, L"idle disabled agent exit should not stop stack" },
    };

    for (const auto& test : stopCases)
    {
        if (ShouldStopSplitterOnAgentExit(test.Desired, test.DirectHostRunning, test.VirtualDeviceRunning) != test.Expected)
        {
            std::wcerr << L"service selftest failed: " << test.Name << L"\n";
            return 1;
        }
    }

    struct RestartThrottleCase
    {
        ULONGLONG Now;
        ULONGLONG LastRestartTick;
        bool LastRestartSucceeded;
        bool BypassCooldown;
        bool Expected;
        const wchar_t* Name;
    };

    const RestartThrottleCase restartThrottleCases[] = {
        { 10000, 0, true, false, false, L"first restart is never throttled" },
        { 10000, 9000, true, false, true, L"successful restart uses long cooldown" },
        { 10000, 4000, true, false, true, L"successful restart is still throttled after failed-retry interval" },
        { 10000, 9000, false, false, true, L"failed restart still avoids immediate tight-loop retry" },
        { 10000, 4000, false, false, true, L"failed restart still uses the long cooldown" },
        { 40000, 9000, false, false, false, L"failed restart retries after long cooldown" },
        { 10000, 9000, true, true, false, L"explicit recovery bypasses cooldown" },
    };

    for (const auto& test : restartThrottleCases)
    {
        if (ShouldThrottleSplitterRestart(
                test.Now,
                test.LastRestartTick,
                test.LastRestartSucceeded,
                test.BypassCooldown) != test.Expected)
        {
            std::wcerr << L"service selftest failed: " << test.Name << L"\n";
            return 1;
        }
    }

    if (wcscmp(MonitorSplitter::Control::kServiceWakeEventName, MonitorSplitter::Control::kAgentWakeEventName) == 0)
    {
        std::wcerr << L"service selftest failed: service and agent wake events must be distinct\n";
        return 1;
    }

    g_lastTrayLaunchSessionId = 7;
    g_lastTrayLaunchAttemptTick = 10000;
    if (!ShouldAttemptTrayLaunch(8, 10001) ||
        ShouldAttemptTrayLaunch(7, 10001) ||
        !ShouldAttemptTrayLaunch(7, 10000 + kTrayLaunchRetryMs))
    {
        std::wcerr << L"service selftest failed: tray launch retry calculation\n";
        return 1;
    }
    g_lastTrayLaunchSessionId = 0xFFFFFFFF;
    g_lastTrayLaunchAttemptTick = 0;

    wchar_t previousConfigDir[MAX_PATH] = {};
    const DWORD previousConfigDirLength = GetEnvironmentVariableW(
        L"MONITORSPLITTER_CONFIGDIR",
        previousConfigDir,
        ARRAYSIZE(previousConfigDir));
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(ARRAYSIZE(tempPath), tempPath) == 0)
    {
        std::wcerr << L"service selftest failed: could not resolve temp path\n";
        return 1;
    }
    const std::wstring selfTestConfigDir =
        std::wstring(tempPath) +
        L"MonitorSplitterServiceSelfTest." +
        std::to_wstring(GetCurrentProcessId()) +
        L"." +
        std::to_wstring(GetTickCount64());
    MonitorSplitter::Control::EnsureDirectory(selfTestConfigDir);
    SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", selfTestConfigDir.c_str());

    const DWORD statusTick = GetTickCount();
    const ULONGLONG runtimeTick = GetTickCount64();
    const std::wstring healthyStatus =
        std::wstring(
        L"{\"running\":true,\"pid\":123,\"mode\":\"direct-shared\","
        L"\"usingSharedFrames\":true,\"sourceCount\":3,\"expectedSourceCount\":3,"
        L"\"healthyFrameSourceCount\":3,\"publishingFrameSourceCount\":3,"
        L"\"updatedTick\":") + std::to_wstring(statusTick) +
        L",\"lastPresentResult\":\"0x00000000\",\"lastDisplayTaskResult\":\"0x00000000\","
        L"\"lastDisplayTaskPresentStatus\":0,\"lastDisplayTaskSourceStatus\":0,"
        L"\"presentedFrames\":1,\"direct\":{\"targetAcquired\":true,"
        L"\"deviceCreated\":true,\"sourceCreated\":true,\"taskPoolCreated\":true,"
        L"\"fenceReady\":true,\"displayTaskSubmitAttempts\":1,"
        L"\"displayTaskSuccesses\":1,\"displayTaskFailures\":0,"
        L"\"lastSubmitTick\":" + std::to_wstring(statusTick) + L"},"
        L"\"frameSources\":["
        L"{\"driverRuntimeMapped\":true,\"driverRuntimeValid\":true,\"driverRuntimeVersion\":2,\"driverRuntimeUpdatedTick\":" + std::to_wstring(runtimeTick) + L"},"
        L"{\"driverRuntimeMapped\":true,\"driverRuntimeValid\":true,\"driverRuntimeVersion\":2,\"driverRuntimeUpdatedTick\":" + std::to_wstring(runtimeTick) + L"},"
        L"{\"driverRuntimeMapped\":true,\"driverRuntimeValid\":true,\"driverRuntimeVersion\":2,\"driverRuntimeUpdatedTick\":" + std::to_wstring(runtimeTick) + L"}]}";
    MonitorSplitter::Control::WriteTextFile(MonitorSplitter::Control::HostStatusPath(), healthyStatus);
    std::wstring crossSessionHealthReason;
    const bool crossSessionHealthOk = AnyDirectHostStatusHealthy(&crossSessionHealthReason);

    const auto startupStatus = [statusTick](const wchar_t* mode) {
        return std::wstring(L"{\"running\":true,\"pid\":123,\"mode\":\"") +
            mode +
            L"\",\"updatedTick\":" +
            std::to_wstring(statusTick) +
            L"}";
    };
    MonitorSplitter::Control::WriteTextFile(MonitorSplitter::Control::HostStatusPath(), startupStatus(L"starting"));
    std::wstring startingReason;
    const bool startingHealthOk = AnyDirectHostStatusHealthy(&startingReason);
    std::wstring startingProgressReason;
    const bool startingProgressOk = AnyDirectHostStartupInProgress(&startingProgressReason);

    MonitorSplitter::Control::WriteTextFile(MonitorSplitter::Control::HostStatusPath(), startupStatus(L"recovering"));
    std::wstring recoveringReason;
    const bool recoveringHealthOk = AnyDirectHostStatusHealthy(&recoveringReason);
    std::wstring recoveringProgressReason;
    const bool recoveringProgressOk = AnyDirectHostStartupInProgress(&recoveringProgressReason);

    const std::wstring stuckStatus =
        std::wstring(
        L"{\"running\":true,\"pid\":123,\"mode\":\"recovering\","
        L"\"updatedTick\":") + std::to_wstring(statusTick) +
        L",\"direct\":{\"sourceCreated\":false,"
        L"\"modeApplyResult\":\"0x00000000\",\"acquireEmptyResult\":\"0x00000000\","
        L"\"lastScanoutCreateResult\":\"0x887a0025\","
        L"\"scanoutCreateAttempts\":1,\"directSetupRetryAttempts\":25}}";
    MonitorSplitter::Control::WriteTextFile(MonitorSplitter::Control::HostStatusPath(), stuckStatus);
    std::wstring stuckDetectReason;
    const bool stuckDetected = MonitorSplitter::Control::HostStatusLikelyStuckInModeChange(stuckStatus, &stuckDetectReason);
    std::wstring stuckProgressReason;
    const bool stuckProgressOk = AnyDirectHostStartupInProgress(&stuckProgressReason);
    const std::wstring stuckApplyStatus =
        std::wstring(
        L"{\"running\":true,\"pid\":123,\"mode\":\"recovering\","
        L"\"updatedTick\":") + std::to_wstring(statusTick) +
        L",\"direct\":{\"sourceCreated\":false,"
        L"\"modeApplyResult\":\"0x887a0025\",\"acquireEmptyResult\":\"0x00000000\","
        L"\"lastScanoutCreateResult\":\"0x00000000\","
        L"\"scanoutCreateAttempts\":0,\"directSetupRetryAttempts\":25}}";
    const bool stuckApplyDetected = MonitorSplitter::Control::HostStatusLikelyStuckInModeChange(stuckApplyStatus);

    DeleteFileW(MonitorSplitter::Control::HostStatusPath().c_str());
    RemoveDirectoryW(selfTestConfigDir.c_str());
    if (previousConfigDirLength != 0 && previousConfigDirLength < ARRAYSIZE(previousConfigDir))
    {
        SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", previousConfigDir);
    }
    else
    {
        SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", nullptr);
    }
    if (!crossSessionHealthOk)
    {
        std::wcerr << L"service selftest failed: cross-session host health should use host-status.json: "
                   << crossSessionHealthReason << L"\n";
        return 1;
    }
    if (startingHealthOk || !startingProgressOk || recoveringHealthOk || !recoveringProgressOk)
    {
        std::wcerr << L"service selftest failed: fresh starting/recovering status must be progress but not healthy: "
                   << startingReason << L" "
                   << startingProgressReason << L" "
                   << recoveringReason << L" "
                   << recoveringProgressReason << L"\n";
        return 1;
    }
    if (!stuckDetected || !stuckApplyDetected || stuckProgressOk)
    {
        std::wcerr << L"service selftest failed: stuck-mode-change status must be detected and not count as startup progress: "
                   << stuckDetectReason << L" "
                   << stuckProgressReason << L"\n";
        return 1;
    }

    std::wcout << L"{\"ok\":true,\"deferredRecoverySelftest\":true,\"agentExitStopSelftest\":true,\"restartThrottleSelftest\":true,\"startupStabilizationSelftest\":true,\"separateAgentWakeSelftest\":true,\"crossSessionHostHealthSelftest\":true,\"startupProgressSelftest\":true,\"stuckModeChangeSelftest\":true}\n";
    return 0;
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(MonitorSplitter::Control::kServiceName, ServiceHandler, nullptr);
    if (g_statusHandle == nullptr)
    {
        return;
    }

    SetServiceStatusValue(SERVICE_START_PENDING, NO_ERROR, 30000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_externalWakeEvent = CreateEventW(ServiceWakeSecurity(), TRUE, FALSE, MonitorSplitter::Control::kServiceWakeEventName);
    if (g_stopEvent == nullptr || g_wakeEvent == nullptr || g_externalWakeEvent == nullptr)
    {
        const DWORD error = GetLastError();
        CloseWorkerEventHandles();
        SetServiceStatusValue(SERVICE_STOPPED, error);
        return;
    }

    AppendLog(std::wstring(L"service started build ") + MonitorSplitter::kBuildTagWide);
    WriteRuntimeStatus(L"service", L"running");
    SetServiceStatusValue(SERVICE_RUNNING);
    ServiceSupervisorLoop();
    WriteRuntimeStatus(L"service", L"stopped");
    AppendLog(L"service stopped");

    CloseWorkerEventHandles();
    SetServiceStatusValue(SERVICE_STOPPED);
}
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc > 1)
    {
        const std::wstring argument = MonitorSplitter::Control::ToLower(argv[1]);
        if (argument == L"--version")
        {
            return PrintVersionJson();
        }
        if (argument == L"--agent")
        {
            bool forceFirstRecovery = false;
            for (int i = 2; i < argc; ++i)
            {
                if (MonitorSplitter::Control::ToLower(argv[i]) == L"--force-first-recovery")
                {
                    forceFirstRecovery = true;
                }
            }
            DWORD sessionId = 0;
            ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
            const auto configDir = MonitorSplitter::Control::ProgramDataConfigDirectory();
            MonitorSplitter::Control::EnsureDirectory(configDir);
            SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", configDir.c_str());
            g_stopEvent = CreateEventW(ServiceWakeSecurity(), TRUE, FALSE, AgentStopEventName(sessionId).c_str());
            g_wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_externalWakeEvent = CreateEventW(ServiceWakeSecurity(), TRUE, FALSE, MonitorSplitter::Control::kAgentWakeEventName);
            if (g_stopEvent == nullptr || g_wakeEvent == nullptr || g_externalWakeEvent == nullptr)
            {
                const DWORD error = GetLastError();
                AppendLog(L"session agent event setup failed in session " +
                    std::to_wstring(sessionId) +
                    L": " +
                    std::to_wstring(error));
                CloseWorkerEventHandles();
                return static_cast<int>(error);
            }
            AppendLog(
                std::wstring(L"session agent started in session ") +
                std::to_wstring(sessionId) +
                L" build " +
                MonitorSplitter::kBuildTagWide +
                (forceFirstRecovery ? L" with immediate first recovery" : L""));
            WriteRuntimeStatus(L"agent", L"running");
            AgentWorkerLoop(forceFirstRecovery);
            WriteRuntimeStatus(L"agent", L"stopped");
            AppendLog(L"session agent stopped in session " + std::to_wstring(sessionId));
            CloseWorkerEventHandles();
            return 0;
        }
        if (argument == L"--once")
        {
            const auto configDir = MonitorSplitter::Control::ProgramDataConfigDirectory();
            MonitorSplitter::Control::EnsureDirectory(configDir);
            SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", configDir.c_str());
            if (MonitorSplitter::Control::DesiredEnabled())
            {
                MonitorSplitter::Control::DirectStack stack;
                EnsureSplitter(stack, true);
            }
            else
            {
                MonitorSplitter::Control::DirectStack stack;
                std::wstring stopReason;
                stack.Stop(10000, stopReason);
            }
            return 0;
        }
        if (argument == L"--console")
        {
            g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_externalWakeEvent = CreateEventW(ServiceWakeSecurity(), TRUE, FALSE, MonitorSplitter::Control::kServiceWakeEventName);
            if (g_stopEvent == nullptr || g_wakeEvent == nullptr || g_externalWakeEvent == nullptr)
            {
                const DWORD error = GetLastError();
                CloseWorkerEventHandles();
                return static_cast<int>(error);
            }
            AgentWorkerLoop();
            CloseWorkerEventHandles();
            return 0;
        }
        if (argument == L"--selftest")
        {
            return ServiceSelfTest();
        }
    }

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { const_cast<LPWSTR>(MonitorSplitter::Control::kServiceName), ServiceMain },
        { nullptr, nullptr },
    };

    if (!StartServiceCtrlDispatcherW(serviceTable))
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        {
            const auto configDir = MonitorSplitter::Control::ProgramDataConfigDirectory();
            MonitorSplitter::Control::EnsureDirectory(configDir);
            SetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", configDir.c_str());
            MonitorSplitter::Control::DirectStack stack;
            EnsureSplitter(stack, true);
            return 0;
        }
        return static_cast<int>(error);
    }

    return 0;
}
