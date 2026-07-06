#pragma once

#include <windows.h>
#include <winsvc.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <sstream>
#include <string>
#include <vector>

#include "..\MonitorSplitterCommon\Layout.h"
#include "..\MonitorSplitterCommon\SyntheticEdid.h"

namespace MonitorSplitter::Control
{
inline constexpr wchar_t kServiceName[] = L"MonitorSplitterService";
inline constexpr wchar_t kServiceWakeEventName[] = L"Global\\MonitorSplitter.ServiceWake";
inline constexpr wchar_t kAgentWakeEventName[] = L"Global\\MonitorSplitter.AgentWake";
inline constexpr wchar_t kSelectorMetadataPrefix[] = L"msp:";
inline constexpr wchar_t kEdidNameBaseSelectorMetadataPrefix[] = L"msp:edid-name-base=";

struct MonitorSnapshot
{
    HMONITOR Handle = nullptr;
    std::wstring DeviceName;
    RECT MonitorRect = {};
    DWORD Flags = 0;
};

struct AdapterSnapshot
{
    std::wstring DeviceName;
    std::wstring DeviceString;
    std::wstring DeviceId;
};

struct DisplayPathSnapshot
{
    std::wstring SourceName;
    std::wstring TargetFriendlyName;
    std::wstring TargetDevicePath;
    LUID TargetAdapterId = {};
    UINT32 TargetId = 0;
};

struct MonitorCandidate
{
    std::wstring Device;
    std::wstring FriendlyName;
    std::wstring AdapterString;
    std::wstring AdapterId;
    std::wstring DevicePath;
    std::wstring AdapterIdentity;
    std::wstring SelectorOverride;
    LUID TargetAdapterId = {};
    UINT32 TargetId = 0;
    int X = 0;
    int Y = 0;
    int Width = 0;
    int Height = 0;
    bool Primary = false;

    std::wstring SelectorBundle() const
    {
        if (!MonitorSplitter::Trim(SelectorOverride).empty())
        {
            return SelectorOverride;
        }

        std::wstring result;
        const auto append = [&result](const std::wstring& value) {
            const auto trimmed = MonitorSplitter::Trim(value);
            if (trimmed.empty())
            {
                return;
            }
            if (result.find(trimmed) != std::wstring::npos)
            {
                return;
            }
            if (!result.empty())
            {
                result += L"\n";
            }
            result += trimmed;
        };

        append(DevicePath);
        append(AdapterIdentity);
        append(Device);
        append(FriendlyName);
        return result;
    }

    std::wstring DisplayName() const
    {
        std::wstring name = FriendlyName.empty() ? Device : FriendlyName;
        if (!Device.empty() && Device != name)
        {
            name += L" / ";
            name += Device;
        }
        if (Width > 0 && Height > 0)
        {
            name += L" (";
            name += std::to_wstring(Width);
            name += L"x";
            name += std::to_wstring(Height);
            name += L")";
        }
        if (Primary)
        {
            name += L" primary";
        }
        return name;
    }
};

inline std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

inline bool ContainsInsensitive(const std::wstring& text, const std::wstring& needle)
{
    return ToLower(text).find(ToLower(needle)) != std::wstring::npos;
}

inline bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix)
{
    if (suffix.empty() || value.size() < suffix.size())
    {
        return false;
    }

    return _wcsicmp(value.c_str() + value.size() - suffix.size(), suffix.c_str()) == 0;
}

inline bool StartsWithInsensitive(const std::wstring& value, const wchar_t* prefix)
{
    const size_t prefixLength = wcslen(prefix);
    return value.size() >= prefixLength &&
           _wcsnicmp(value.c_str(), prefix, prefixLength) == 0;
}

inline bool IsSelectorMetadataLine(const std::wstring& value)
{
    return StartsWithInsensitive(MonitorSplitter::Trim(value), kSelectorMetadataPrefix);
}

inline bool IsVolatileDisplaySourceNameLine(const std::wstring& value)
{
    const auto trimmed = MonitorSplitter::Trim(value);
    constexpr wchar_t prefix[] = L"\\\\.\\DISPLAY";
    constexpr size_t prefixLength = ARRAYSIZE(prefix) - 1;
    if (trimmed.size() <= prefixLength ||
        _wcsnicmp(trimmed.c_str(), prefix, prefixLength) != 0)
    {
        return false;
    }

    for (size_t index = prefixLength; index < trimmed.size(); index++)
    {
        if (!iswdigit(trimmed[index]))
        {
            return false;
        }
    }
    return true;
}

inline std::string ToUtf8(const std::wstring& value)
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
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
    result.pop_back();
    return result;
}

inline std::wstring FromUtf8(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (required <= 0)
    {
        codePage = CP_ACP;
        required = MultiByteToWideChar(codePage, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (required <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(codePage, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
    return result;
}

inline void AppendUniqueSelectorLine(std::vector<std::wstring>& selectors, const std::wstring& selector)
{
    const auto trimmed = MonitorSplitter::Trim(selector);
    if (trimmed.empty())
    {
        return;
    }

    const auto existing = std::find_if(selectors.begin(), selectors.end(), [&trimmed](const std::wstring& value) {
        return _wcsicmp(value.c_str(), trimmed.c_str()) == 0;
    });
    if (existing == selectors.end())
    {
        selectors.push_back(trimmed);
    }
}

inline std::wstring JoinSelectorLines(const std::vector<std::wstring>& selectors)
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

inline std::wstring ReadEdidNameBaseFromSelectorMetadata(const std::wstring& selector)
{
    std::wstringstream stream(selector);
    std::wstring line;
    while (std::getline(stream, line))
    {
        const auto trimmed = MonitorSplitter::Trim(line);
        if (!StartsWithInsensitive(trimmed, kEdidNameBaseSelectorMetadataPrefix))
        {
            continue;
        }

        const size_t prefixLength = wcslen(kEdidNameBaseSelectorMetadataPrefix);
        const auto value = MonitorSplitter::Trim(trimmed.substr(prefixLength));
        if (!value.empty())
        {
            return value;
        }
    }

    return {};
}

inline std::wstring SelectorWithEdidNameBaseMetadata(const std::wstring& selector, const std::wstring& nameBase)
{
    std::vector<std::wstring> lines;
    std::wstringstream stream(selector);
    std::wstring line;
    while (std::getline(stream, line))
    {
        const auto trimmed = MonitorSplitter::Trim(line);
        if (trimmed.empty() || StartsWithInsensitive(trimmed, kEdidNameBaseSelectorMetadataPrefix))
        {
            continue;
        }
        AppendUniqueSelectorLine(lines, trimmed);
    }

    const auto trimmedNameBase = MonitorSplitter::Trim(nameBase);
    if (!trimmedNameBase.empty())
    {
        AppendUniqueSelectorLine(lines, std::wstring(kEdidNameBaseSelectorMetadataPrefix) + trimmedNameBase);
    }

    return JoinSelectorLines(lines);
}

inline std::wstring ProgramDataConfigDirectory()
{
    wchar_t programData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, ARRAYSIZE(programData));
    std::wstring path = length != 0 && length < ARRAYSIZE(programData)
        ? std::wstring(programData)
        : std::wstring(L"C:\\ProgramData");
    path += L"\\MonitorSplitter";
    return path;
}

inline std::wstring LocalConfigDirectory()
{
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
    if (length == 0 || length >= ARRAYSIZE(localAppData))
    {
        wchar_t tempPath[MAX_PATH] = {};
        const DWORD tempLength = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
        if (tempLength == 0 || tempLength >= ARRAYSIZE(tempPath))
        {
            return L".";
        }

        std::wstring fallback = tempPath;
        fallback += L"MonitorSplitter";
        return fallback;
    }

    std::wstring path = localAppData;
    path += L"\\MonitorSplitter";
    return path;
}

inline std::wstring ConfigDirectory()
{
    wchar_t overridePath[MAX_PATH] = {};
    const DWORD overrideLength = GetEnvironmentVariableW(L"MONITORSPLITTER_CONFIGDIR", overridePath, ARRAYSIZE(overridePath));
    if (overrideLength != 0 && overrideLength < ARRAYSIZE(overridePath))
    {
        return MonitorSplitter::Trim(overridePath);
    }

    const auto machinePath = ProgramDataConfigDirectory();
    const DWORD machineAttributes = GetFileAttributesW(machinePath.c_str());
    if (machineAttributes != INVALID_FILE_ATTRIBUTES && (machineAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return machinePath;
    }

    return LocalConfigDirectory();
}

inline bool MachineConfigExists()
{
    const auto machinePath = ProgramDataConfigDirectory();
    const DWORD attributes = GetFileAttributesW(machinePath.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline std::wstring LayoutPath()
{
    return ConfigDirectory() + L"\\layout.txt";
}

inline std::wstring ActiveLayoutPath()
{
    return ConfigDirectory() + L"\\active-layout.txt";
}

inline std::wstring HostStatusPath()
{
    return ConfigDirectory() + L"\\host-status.json";
}

inline std::wstring ServiceStatusPath()
{
    return ProgramDataConfigDirectory() + L"\\service-status.json";
}

inline std::wstring AgentStatusPath()
{
    return ProgramDataConfigDirectory() + L"\\agent-status.json";
}

inline std::wstring ConfigStatusPath()
{
    return ProgramDataConfigDirectory() + L"\\config-status.json";
}

inline std::wstring HostTargetPath()
{
    return ConfigDirectory() + L"\\host-target.txt";
}

inline std::wstring EdidNameBasePath()
{
    return ConfigDirectory() + L"\\edid-name-base.txt";
}

inline std::wstring DirectTargetPath()
{
    return ConfigDirectory() + L"\\direct-target.txt";
}

inline std::wstring DesiredStatePath()
{
    return ProgramDataConfigDirectory() + L"\\service-enabled.txt";
}

inline std::wstring RestartRequestPath()
{
    return ProgramDataConfigDirectory() + L"\\service-restart.txt";
}

inline bool EnsureDirectory(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr))
    {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

inline std::wstring ReadTextFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    const DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0)
    {
        CloseHandle(file);
        return {};
    }

    std::string buffer(size, '\0');
    DWORD read = 0;
    ReadFile(file, buffer.data(), size, &read, nullptr);
    CloseHandle(file);
    buffer.resize(read);
    return FromUtf8(buffer);
}

inline bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring directory;
    if (slash != std::wstring::npos)
    {
        directory = path.substr(0, slash);
        EnsureDirectory(directory);
    }

    static volatile LONG writeCounter = 0;
    const LONG counter = InterlockedIncrement(&writeCounter);
    const std::wstring tempPath = path +
        L".tmp." +
        std::to_wstring(GetCurrentProcessId()) +
        L"." +
        std::to_wstring(GetTickCount64()) +
        L"." +
        std::to_wstring(counter);

    HANDLE file = CreateFileW(
        tempPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    const auto bytes = ToUtf8(text + L"\n");
    if (bytes.size() > MAXDWORD)
    {
        CloseHandle(file);
        DeleteFileW(tempPath.c_str());
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    const BOOL flushed = ok ? FlushFileBuffers(file) : FALSE;
    CloseHandle(file);
    if (!ok || !flushed || written != bytes.size())
    {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    return true;
}

inline std::wstring JsonEscape(const std::wstring& value)
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

inline std::wstring TargetDevicePathToEdidRegistryPath(std::wstring targetDevicePath)
{
    targetDevicePath = MonitorSplitter::Trim(targetDevicePath);
    constexpr wchar_t prefix[] = L"\\\\?\\";
    constexpr size_t prefixLength = ARRAYSIZE(prefix) - 1;
    if (targetDevicePath.size() > prefixLength &&
        _wcsnicmp(targetDevicePath.c_str(), prefix, prefixLength) == 0)
    {
        targetDevicePath = targetDevicePath.substr(prefixLength);
    }

    const size_t classGuidSeparator = targetDevicePath.find(L"#{");
    if (classGuidSeparator != std::wstring::npos)
    {
        targetDevicePath = targetDevicePath.substr(0, classGuidSeparator);
    }

    for (auto& ch : targetDevicePath)
    {
        if (ch == L'#')
        {
            ch = L'\\';
        }
    }

    if (_wcsnicmp(targetDevicePath.c_str(), L"DISPLAY\\", 8) != 0)
    {
        return {};
    }

    return L"SYSTEM\\CurrentControlSet\\Enum\\" + targetDevicePath + L"\\Device Parameters";
}

inline std::wstring DecodeEdidMonitorName(const std::vector<BYTE>& edid)
{
    if (edid.size() < 128)
    {
        return {};
    }

    const size_t descriptorOffsets[] = { 54, 72, 90, 108 };
    for (const size_t offset : descriptorOffsets)
    {
        if (offset + 18 > edid.size())
        {
            continue;
        }
        if (edid[offset] != 0x00 ||
            edid[offset + 1] != 0x00 ||
            edid[offset + 2] != 0x00 ||
            edid[offset + 3] != 0xFC ||
            edid[offset + 4] != 0x00)
        {
            continue;
        }

        std::wstring name;
        for (size_t index = 0; index < 13; index++)
        {
            const BYTE value = edid[offset + 5 + index];
            if (value == 0x00 || value == 0x0A)
            {
                break;
            }
            if (value >= 0x20 && value <= 0x7E)
            {
                name.push_back(static_cast<wchar_t>(value));
            }
        }
        return MonitorSplitter::Trim(name);
    }

    return {};
}

inline std::wstring ReadPhysicalEdidNameFromTargetDevicePath(const std::wstring& targetDevicePath)
{
    const auto registryPath = TargetDevicePathToEdidRegistryPath(targetDevicePath);
    if (registryPath.empty())
    {
        return {};
    }

    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        registryPath.c_str(),
        L"EDID",
        RRF_RT_REG_BINARY,
        &type,
        nullptr,
        &size);
    if (result != ERROR_SUCCESS || size == 0)
    {
        return {};
    }

    std::vector<BYTE> edid(size);
    result = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        registryPath.c_str(),
        L"EDID",
        RRF_RT_REG_BINARY,
        &type,
        edid.data(),
        &size);
    if (result != ERROR_SUCCESS)
    {
        return {};
    }

    return DecodeEdidMonitorName(edid);
}

inline bool LoadLayout(Layout& layout)
{
    const auto spec = MonitorSplitter::Trim(ReadTextFile(LayoutPath()));
    if (spec.empty())
    {
        layout = DefaultLayout();
        return true;
    }

    std::wstring error;
    return ParseLayoutSpec(spec, layout, &error);
}

inline bool SaveLayout(const Layout& layout)
{
    return WriteTextFile(LayoutPath(), SerializeLayout(layout));
}

inline std::wstring LoadHostTarget()
{
    return ReadTextFile(HostTargetPath());
}

inline bool SaveHostTarget(const std::wstring& selector)
{
    return WriteTextFile(HostTargetPath(), selector);
}

inline std::wstring LoadEdidNameBase()
{
    return MonitorSplitter::Trim(ReadTextFile(EdidNameBasePath()));
}

inline bool SaveEdidNameBase(const std::wstring& nameBase)
{
    return WriteTextFile(EdidNameBasePath(), MonitorSplitter::Trim(nameBase));
}

inline bool EnsureHostTargetEdidNameBaseMetadata(const std::wstring& nameBase)
{
    const auto trimmedNameBase = MonitorSplitter::Trim(nameBase);
    if (trimmedNameBase.empty())
    {
        return false;
    }

    const auto selector = LoadHostTarget();
    if (MonitorSplitter::Trim(selector).empty())
    {
        return false;
    }

    const auto metadataNameBase = ReadEdidNameBaseFromSelectorMetadata(selector);
    if (_wcsicmp(metadataNameBase.c_str(), trimmedNameBase.c_str()) == 0)
    {
        return true;
    }

    const auto repairedSelector = SelectorWithEdidNameBaseMetadata(selector, trimmedNameBase);
    if (MonitorSplitter::Trim(repairedSelector).empty())
    {
        return false;
    }

    return SaveHostTarget(repairedSelector);
}

inline std::wstring ExpectedEdidFriendlyName(size_t count, size_t index, const std::wstring& edidNameBase = {})
{
    return MonitorSplitter::EdidMonitorNameWide(count, index, edidNameBase);
}

inline std::wstring ExpectedEdidIndexSuffix(size_t index)
{
    return MonitorSplitter::EdidIndexSuffixWide(index);
}

inline bool MatchesMonitorSplitterEdidName(
    const std::wstring& friendlyName,
    size_t count,
    size_t index,
    const std::wstring& edidNameBase = {})
{
    const auto trimmed = MonitorSplitter::Trim(friendlyName);
    const auto configuredName = ExpectedEdidFriendlyName(count, index, edidNameBase);
    return (!edidNameBase.empty() && ContainsInsensitive(trimmed, configuredName)) ||
           ContainsInsensitive(trimmed, ExpectedEdidFriendlyName(count, index)) ||
           ContainsInsensitive(trimmed, MonitorSplitter::MonitorName(count, index)) ||
           EndsWithInsensitive(trimmed, ExpectedEdidIndexSuffix(index));
}

inline bool DesiredEnabled()
{
    const auto value = ToLower(MonitorSplitter::Trim(ReadTextFile(DesiredStatePath())));
    return value == L"1" || value == L"true" || value == L"enabled" || value == L"enable" || value == L"on";
}

inline bool SaveDesiredEnabled(bool enabled)
{
    return WriteTextFile(DesiredStatePath(), enabled ? L"1" : L"0");
}

inline bool SignalNamedEvent(const wchar_t* name)
{
    HANDLE eventHandle = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (eventHandle == nullptr)
    {
        return false;
    }
    const BOOL ok = SetEvent(eventHandle);
    CloseHandle(eventHandle);
    return ok != FALSE;
}

inline bool SignalServiceWake()
{
    return SignalNamedEvent(kServiceWakeEventName);
}

inline bool SignalAgentWake()
{
    return SignalNamedEvent(kAgentWakeEventName);
}

inline bool SignalStackWake()
{
    const bool serviceWake = SignalServiceWake();
    const bool agentWake = SignalAgentWake();
    return serviceWake || agentWake;
}

inline bool IsForcedRestartRequest(const std::wstring& token)
{
    const std::wstring trimmed = Trim(token);
    constexpr wchar_t prefix[] = L"force:";
    return trimmed.rfind(prefix, 0) == 0;
}

inline bool RequestServiceRestart(bool forceHostRestart = false)
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    const std::wstring token = std::wstring(forceHostRestart ? L"force:" : L"recover:") +
                               std::to_wstring(GetTickCount64()) +
                               L":" +
                               std::to_wstring(counter.QuadPart);
    const bool saved = WriteTextFile(RestartRequestPath(), token);
    SignalStackWake();
    return saved;
}

inline bool TryReadUnsignedJsonField(const std::wstring& json, const std::wstring& fieldName, unsigned long long& value)
{
    const std::wstring marker = L"\"" + fieldName + L"\":";
    const size_t markerIndex = json.find(marker);
    if (markerIndex == std::wstring::npos)
    {
        return false;
    }

    size_t index = markerIndex + marker.size();
    while (index < json.size() && iswspace(json[index]))
    {
        index++;
    }

    bool foundDigit = false;
    unsigned long long parsed = 0;
    while (index < json.size() && iswdigit(json[index]))
    {
        foundDigit = true;
        parsed = (parsed * 10) + static_cast<unsigned long long>(json[index] - L'0');
        index++;
    }

    if (!foundDigit)
    {
        return false;
    }

    value = parsed;
    return true;
}

inline bool TryReadUnsignedJsonFieldStats(
    const std::wstring& json,
    const std::wstring& fieldName,
    unsigned long long& count,
    unsigned long long& minimum,
    unsigned long long& maximum)
{
    const std::wstring marker = L"\"" + fieldName + L"\":";
    size_t searchIndex = 0;
    count = 0;
    minimum = 0;
    maximum = 0;

    for (;;)
    {
        const size_t markerIndex = json.find(marker, searchIndex);
        if (markerIndex == std::wstring::npos)
        {
            break;
        }

        size_t index = markerIndex + marker.size();
        while (index < json.size() && iswspace(json[index]))
        {
            index++;
        }

        bool foundDigit = false;
        unsigned long long parsed = 0;
        while (index < json.size() && iswdigit(json[index]))
        {
            foundDigit = true;
            parsed = (parsed * 10) + static_cast<unsigned long long>(json[index] - L'0');
            index++;
        }

        if (foundDigit)
        {
            if (count == 0 || parsed < minimum)
            {
                minimum = parsed;
            }
            if (count == 0 || parsed > maximum)
            {
                maximum = parsed;
            }
            count++;
        }

        searchIndex = markerIndex + marker.size();
    }

    return count != 0;
}

inline bool TryReadSignedJsonField(const std::wstring& json, const std::wstring& fieldName, long long& value)
{
    const std::wstring marker = L"\"" + fieldName + L"\":";
    const size_t markerIndex = json.find(marker);
    if (markerIndex == std::wstring::npos)
    {
        return false;
    }

    size_t index = markerIndex + marker.size();
    while (index < json.size() && iswspace(json[index]))
    {
        index++;
    }

    const bool negative = index < json.size() && json[index] == L'-';
    if (negative)
    {
        index++;
    }

    bool foundDigit = false;
    long long parsed = 0;
    while (index < json.size() && iswdigit(json[index]))
    {
        foundDigit = true;
        parsed = (parsed * 10) + static_cast<long long>(json[index] - L'0');
        index++;
    }

    if (!foundDigit)
    {
        return false;
    }

    value = negative ? -parsed : parsed;
    return true;
}

inline bool HostStatusModeIs(const std::wstring& status, const wchar_t* mode)
{
    return status.find(std::wstring(L"\"mode\":\"") + mode + L"\"") != std::wstring::npos;
}

inline bool HostStatusLayoutMatches(const std::wstring& status, const std::wstring& layoutSpec)
{
    return status.find(L"\"layout\":\"" + layoutSpec + L"\"") != std::wstring::npos;
}

inline bool HostStatusDriverRuntimeFresh(
    const std::wstring& status,
    unsigned long long expectedSourceCount,
    std::wstring* reason = nullptr)
{
    const auto fail = [reason](const std::wstring& message) {
        if (reason != nullptr)
        {
            *reason = message;
        }
        return false;
    };

    if (expectedSourceCount == 0)
    {
        return false;
    }

    unsigned long long runtimeTickCount = 0;
    unsigned long long oldestRuntimeTick = 0;
    unsigned long long newestRuntimeTick = 0;
    if (!TryReadUnsignedJsonFieldStats(status, L"driverRuntimeUpdatedTick", runtimeTickCount, oldestRuntimeTick, newestRuntimeTick) ||
        runtimeTickCount < expectedSourceCount)
    {
        return fail(L"not all split frame producer runtimes are reporting heartbeat status");
    }

    const unsigned long long now = GetTickCount64();
    if (oldestRuntimeTick == 0 || oldestRuntimeTick > now)
    {
        return fail(L"split frame producer runtime heartbeat is invalid");
    }

    constexpr unsigned long long kDriverRuntimeStaleMs = 5000;
    const unsigned long long oldestRuntimeAge = now - oldestRuntimeTick;
    if (oldestRuntimeAge > kDriverRuntimeStaleMs)
    {
        return fail(L"split frame producer runtime heartbeat is stale for " + std::to_wstring(oldestRuntimeAge) + L" ms");
    }

    if (reason != nullptr)
    {
        reason->clear();
    }
    return true;
}

inline bool HostStatusFreshAndRunning(const std::wstring& status, std::wstring* reason = nullptr, DWORD expectedPid = 0)
{
    const auto fail = [reason](const wchar_t* message) {
        if (reason != nullptr)
        {
            *reason = message;
        }
        return false;
    };

    if (status.empty())
    {
        return fail(L"host status JSON has not been written yet");
    }
    if (status.find(L"\"running\":true") == std::wstring::npos)
    {
        return fail(L"direct host status is not running");
    }
    if (status.find(L"\"mode\":\"error\"") != std::wstring::npos)
    {
        return fail(L"direct host reported error mode");
    }

    unsigned long long value = 0;
    if (expectedPid != 0)
    {
        if (!TryReadUnsignedJsonField(status, L"pid", value) || value != expectedPid)
        {
            return fail(L"direct host status belongs to a different process");
        }
    }

    if (!TryReadUnsignedJsonField(status, L"updatedTick", value))
    {
        return fail(L"direct host status does not include an update timestamp");
    }
    constexpr DWORD kHostStatusStaleMs = 15000;
    const DWORD updatedTick = static_cast<DWORD>(value);
    if (static_cast<DWORD>(GetTickCount() - updatedTick) > kHostStatusStaleMs)
    {
        return fail(L"direct host status is stale");
    }

    if (reason != nullptr)
    {
        reason->clear();
    }
    return true;
}

inline bool TryReadHostStatusProcessId(const std::wstring& status, DWORD& processId)
{
    unsigned long long value = 0;
    if (!TryReadUnsignedJsonField(status, L"pid", value) ||
        value == 0 ||
        value > MAXDWORD)
    {
        processId = 0;
        return false;
    }

    processId = static_cast<DWORD>(value);
    return true;
}

inline bool IsProcessIdStillRunning(DWORD processId)
{
    if (processId == 0)
    {
        return false;
    }

    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
    {
        return false;
    }

    DWORD exitCode = 0;
    const bool running = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

inline bool HostStatusProcessIsAlive(const std::wstring& status, std::wstring* reason = nullptr)
{
    const auto fail = [reason](const wchar_t* message) {
        if (reason != nullptr)
        {
            *reason = message;
        }
        return false;
    };

    DWORD processId = 0;
    if (!TryReadHostStatusProcessId(status, processId))
    {
        return fail(L"direct host status does not include a valid process id");
    }
    if (!IsProcessIdStillRunning(processId))
    {
        return fail(L"direct host process from status is no longer running");
    }

    if (reason != nullptr)
    {
        reason->clear();
    }
    return true;
}

inline bool HostStatusHealthy(const std::wstring& status, std::wstring* reason = nullptr, DWORD expectedPid = 0)
{
    const auto fail = [reason](const wchar_t* message) {
        if (reason != nullptr)
        {
            *reason = message;
        }
        return false;
    };

    if (!HostStatusFreshAndRunning(status, reason, expectedPid))
    {
        return false;
    }
    if (!HostStatusModeIs(status, L"direct-shared"))
    {
        return fail(L"direct host has not reached direct-shared mode yet");
    }
    if (status.find(L"\"usingSharedFrames\":true") == std::wstring::npos)
    {
        return fail(L"direct host is not using shared frames yet");
    }
    if (status.find(L"\"targetAcquired\":true") == std::wstring::npos ||
        status.find(L"\"deviceCreated\":true") == std::wstring::npos ||
        status.find(L"\"sourceCreated\":true") == std::wstring::npos ||
        status.find(L"\"taskPoolCreated\":true") == std::wstring::npos ||
        status.find(L"\"fenceReady\":true") == std::wstring::npos)
    {
        return fail(L"direct host scanout resources are not all ready");
    }
    if (status.find(L"\"lastPresentResult\":\"0x00000000\"") == std::wstring::npos ||
        status.find(L"\"lastDisplayTaskResult\":\"0x00000000\"") == std::wstring::npos)
    {
        return fail(L"direct host has not reported a successful present yet");
    }

    unsigned long long value = 0;
    unsigned long long sourceCount = 0;
    unsigned long long expectedSourceCount = 0;
    if (!TryReadUnsignedJsonField(status, L"sourceCount", sourceCount) ||
        !TryReadUnsignedJsonField(status, L"expectedSourceCount", expectedSourceCount) ||
        expectedSourceCount == 0 ||
        sourceCount < expectedSourceCount)
    {
        return fail(L"direct host has not mapped all expected split sources");
    }

    unsigned long long healthyFrameSourceCount = 0;
    unsigned long long publishingFrameSourceCount = 0;
    if (!TryReadUnsignedJsonField(status, L"healthyFrameSourceCount", healthyFrameSourceCount) ||
        healthyFrameSourceCount < expectedSourceCount)
    {
        return fail(L"not all split frame sources are healthy");
    }
    if (!TryReadUnsignedJsonField(status, L"publishingFrameSourceCount", publishingFrameSourceCount) ||
        publishingFrameSourceCount < expectedSourceCount)
    {
        return fail(L"not all split frame sources are publishing frames");
    }
    std::wstring runtimeReason;
    if (!HostStatusDriverRuntimeFresh(status, expectedSourceCount, &runtimeReason))
    {
        if (reason != nullptr)
        {
            *reason = runtimeReason.empty()
                ? L"split frame producer runtime heartbeat is not fresh"
                : runtimeReason;
        }
        return false;
    }

    if (!TryReadUnsignedJsonField(status, L"displayTaskSubmitAttempts", value) || value == 0)
    {
        return fail(L"direct host has not submitted a display task yet");
    }
    if (!TryReadUnsignedJsonField(status, L"displayTaskSuccesses", value) || value == 0)
    {
        return fail(L"direct host has not completed a display task yet");
    }
    if (!TryReadUnsignedJsonField(status, L"lastSubmitTick", value))
    {
        return fail(L"direct host status does not include a submit timestamp");
    }
    constexpr DWORD kHostSubmitStaleMs = 5000;
    const DWORD lastSubmitTick = static_cast<DWORD>(value);
    if (static_cast<DWORD>(GetTickCount() - lastSubmitTick) > kHostSubmitStaleMs)
    {
        return fail(L"direct host has not submitted a display task recently");
    }
    if (!TryReadUnsignedJsonField(status, L"displayTaskFailures", value) || value != 0)
    {
        return fail(L"direct host reported display task failures");
    }
    long long taskStatus = 0;
    if (!TryReadSignedJsonField(status, L"lastDisplayTaskPresentStatus", taskStatus) ||
        (taskStatus != 0 && taskStatus != -1))
    {
        return fail(L"direct host reported a nonzero display task present status");
    }
    if (!TryReadSignedJsonField(status, L"lastDisplayTaskSourceStatus", taskStatus) ||
        (taskStatus != 0 && taskStatus != -1))
    {
        return fail(L"direct host reported a nonzero display task source status");
    }
    if (!TryReadUnsignedJsonField(status, L"presentedFrames", value) || value == 0)
    {
        return fail(L"direct host has not presented a frame yet");
    }
    if (reason != nullptr)
    {
        reason->clear();
    }
    return true;
}

inline bool HostStatusLikelyStuckInModeChange(const std::wstring& status, std::wstring* reason = nullptr)
{
    if (!HostStatusModeIs(status, L"recovering") && !HostStatusModeIs(status, L"starting"))
    {
        return false;
    }

    if (status.find(L"\"sourceCreated\":false") == std::wstring::npos)
    {
        return false;
    }
    if (status.find(L"\"lastScanoutCreateResult\":\"0x887a0025\"") == std::wstring::npos &&
        status.find(L"\"modeApplyResult\":\"0x887a0025\"") == std::wstring::npos &&
        status.find(L"\"acquireEmptyResult\":\"0x887a0025\"") == std::wstring::npos)
    {
        return false;
    }

    unsigned long long scanoutAttempts = 0;
    unsigned long long retryAttempts = 0;
    TryReadUnsignedJsonField(status, L"scanoutCreateAttempts", scanoutAttempts);
    TryReadUnsignedJsonField(status, L"directSetupRetryAttempts", retryAttempts);
    if (scanoutAttempts < 20 && retryAttempts < 20)
    {
        return false;
    }

    if (reason != nullptr)
    {
        *reason = L"direct host is stuck waiting for DisplayCore mode change completion";
    }
    return true;
}

inline bool HostStatusInProgressForSupervision(const std::wstring& status, std::wstring* reason = nullptr, DWORD expectedPid = 0)
{
    if (!HostStatusFreshAndRunning(status, reason, expectedPid))
    {
        return false;
    }
    if (HostStatusLikelyStuckInModeChange(status, reason))
    {
        return false;
    }

    if (HostStatusModeIs(status, L"starting") || HostStatusModeIs(status, L"recovering"))
    {
        if (reason != nullptr)
        {
            reason->clear();
        }
        return true;
    }

    if (HostStatusModeIs(status, L"direct-shared"))
    {
        return HostStatusHealthy(status, reason, expectedPid);
    }

    if (reason != nullptr)
    {
        *reason = L"direct host is not in a supervised startup or running mode";
    }
    return false;
}

inline bool HostStatusStartupInProgress(const std::wstring& status, std::wstring* reason = nullptr, DWORD expectedPid = 0)
{
    if (!HostStatusFreshAndRunning(status, reason, expectedPid))
    {
        return false;
    }
    if (HostStatusLikelyStuckInModeChange(status, reason))
    {
        return false;
    }

    if (HostStatusModeIs(status, L"starting") || HostStatusModeIs(status, L"recovering"))
    {
        if (reason != nullptr)
        {
            reason->clear();
        }
        return true;
    }

    if (reason != nullptr)
    {
        *reason = L"direct host is not in startup/recovery mode";
    }
    return false;
}

inline bool StartServiceIfNeeded()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        return false;
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS | SERVICE_START);
    if (service == nullptr)
    {
        CloseServiceHandle(manager);
        return false;
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD needed = 0;
    bool ok = false;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed))
    {
        if (status.dwCurrentState == SERVICE_RUNNING || status.dwCurrentState == SERVICE_START_PENDING)
        {
            ok = true;
        }
        else
        {
            ok = StartServiceW(service, 0, nullptr) != FALSE || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

inline std::wstring ServiceStateText()
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        return L"service manager unavailable";
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr)
    {
        CloseServiceHandle(manager);
        return L"service not installed";
    }

    SERVICE_STATUS_PROCESS status = {};
    DWORD needed = 0;
    std::wstring text = L"service unknown";
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed))
    {
        switch (status.dwCurrentState)
        {
        case SERVICE_RUNNING:
            text = L"service running";
            break;
        case SERVICE_START_PENDING:
            text = L"service starting";
            break;
        case SERVICE_STOPPED:
            text = L"service stopped";
            break;
        default:
            text = L"service state " + std::to_wstring(status.dwCurrentState);
            break;
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return text;
}

inline std::wstring AdapterIdentity(LUID adapterId, UINT32 targetId)
{
    return L"adapter:" + std::to_wstring(static_cast<unsigned long>(adapterId.LowPart)) +
           L":" + std::to_wstring(adapterId.HighPart) +
           L":" + std::to_wstring(targetId);
}

inline BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto* monitors = reinterpret_cast<std::vector<MonitorSnapshot>*>(data);
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info))
    {
        MonitorSnapshot snapshot;
        snapshot.Handle = monitor;
        snapshot.DeviceName = info.szDevice;
        snapshot.MonitorRect = info.rcMonitor;
        snapshot.Flags = info.dwFlags;
        monitors->push_back(snapshot);
    }
    return TRUE;
}

inline std::vector<MonitorSnapshot> EnumerateMonitors()
{
    std::vector<MonitorSnapshot> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

inline std::vector<AdapterSnapshot> EnumerateAdapters()
{
    std::vector<AdapterSnapshot> adapters;
    for (DWORD index = 0;; index++)
    {
        DISPLAY_DEVICEW adapter = {};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0))
        {
            break;
        }

        AdapterSnapshot snapshot;
        snapshot.DeviceName = adapter.DeviceName;
        snapshot.DeviceString = adapter.DeviceString;
        snapshot.DeviceId = adapter.DeviceID;
        adapters.push_back(std::move(snapshot));
    }
    return adapters;
}

inline const AdapterSnapshot* FindAdapter(const std::vector<AdapterSnapshot>& adapters, const std::wstring& deviceName)
{
    for (const auto& adapter : adapters)
    {
        if (_wcsicmp(adapter.DeviceName.c_str(), deviceName.c_str()) == 0)
        {
            return &adapter;
        }
    }
    return nullptr;
}

inline std::vector<DisplayPathSnapshot> QueryDisplayPaths()
{
    std::vector<DisplayPathSnapshot> results;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
    {
        return results;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    LONG query = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (query != ERROR_SUCCESS)
    {
        return results;
    }
    paths.resize(pathCount);

    for (const auto& path : paths)
    {
        DisplayPathSnapshot snapshot;
        snapshot.TargetAdapterId = path.targetInfo.adapterId;
        snapshot.TargetId = path.targetInfo.id;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS)
        {
            snapshot.SourceName = sourceName.viewGdiDeviceName;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS)
        {
            snapshot.TargetFriendlyName = targetName.monitorFriendlyDeviceName;
            snapshot.TargetDevicePath = targetName.monitorDevicePath;
        }

        results.push_back(std::move(snapshot));
    }
    return results;
}

inline const DisplayPathSnapshot* FindDisplayPath(const std::vector<DisplayPathSnapshot>& paths, const std::wstring& sourceName)
{
    for (const auto& path : paths)
    {
        if (_wcsicmp(path.SourceName.c_str(), sourceName.c_str()) == 0)
        {
            return &path;
        }
    }
    return nullptr;
}

inline std::wstring GetSourceName(const DISPLAYCONFIG_PATH_SOURCE_INFO& sourceInfo)
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

inline std::wstring GetTargetFriendlyName(const DISPLAYCONFIG_PATH_TARGET_INFO& targetInfo)
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

inline std::wstring GetTargetDevicePath(const DISPLAYCONFIG_PATH_TARGET_INFO& targetInfo)
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

inline bool QueryDisplayConfigRaw(
    UINT32 flags,
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS || pathCount == 0)
        {
            return false;
        }

        paths.assign(pathCount, {});
        modes.assign(modeCount, {});
        result = QueryDisplayConfig(flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_SUCCESS)
        {
            paths.resize(pathCount);
            modes.resize(modeCount);
            return true;
        }
        if (result != ERROR_INSUFFICIENT_BUFFER)
        {
            return false;
        }
    }
    return false;
}

inline bool QueryActiveDisplayConfigRaw(
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
{
    return QueryDisplayConfigRaw(QDC_ONLY_ACTIVE_PATHS, paths, modes);
}

inline bool IsMonitorSplitterAdapter(const AdapterSnapshot* adapter)
{
    if (adapter == nullptr)
    {
        return false;
    }
    return ContainsInsensitive(adapter->DeviceString, L"MonitorSplitter") ||
           ContainsInsensitive(adapter->DeviceId, L"MonitorSplitterDriver");
}

inline std::vector<std::wstring> SplitSelectorLines(const std::wstring& selector)
{
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= selector.size())
    {
        const size_t end = selector.find_first_of(L"\r\n", start);
        const auto line = MonitorSplitter::Trim(selector.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!line.empty())
        {
            lines.push_back(line);
        }
        if (end == std::wstring::npos)
        {
            break;
        }
        start = end + 1;
    }
    return lines;
}

inline std::wstring StableSelectorForSavedPhysicalPanelMatch(const std::wstring& selector)
{
    std::vector<std::wstring> stableLines;
    for (const auto& line : SplitSelectorLines(selector))
    {
        if (IsSelectorMetadataLine(line) || IsVolatileDisplaySourceNameLine(line))
        {
            continue;
        }
        AppendUniqueSelectorLine(stableLines, line);
    }

    if (stableLines.empty())
    {
        return selector;
    }
    return JoinSelectorLines(stableLines);
}

inline bool SelectorMatchesCandidate(const std::wstring& selector, const MonitorCandidate& candidate)
{
    const auto candidateSelector = SplitSelectorLines(candidate.SelectorOverride);
    for (const auto& line : SplitSelectorLines(selector))
    {
        if (IsSelectorMetadataLine(line))
        {
            continue;
        }

        for (const auto& candidateLine : candidateSelector)
        {
            if (_wcsicmp(line.c_str(), candidateLine.c_str()) == 0)
            {
                return true;
            }
        }

        if (_wcsicmp(line.c_str(), candidate.Device.c_str()) == 0 ||
            _wcsicmp(line.c_str(), candidate.FriendlyName.c_str()) == 0 ||
            _wcsicmp(line.c_str(), candidate.DevicePath.c_str()) == 0 ||
            _wcsicmp(line.c_str(), candidate.AdapterIdentity.c_str()) == 0)
        {
            return true;
        }
    }
    return false;
}

inline bool SavedPhysicalPanelSelectorMatchesCandidate(const std::wstring& selector, const MonitorCandidate& candidate)
{
    return SelectorMatchesCandidate(StableSelectorForSavedPhysicalPanelMatch(selector), candidate);
}

inline bool TryParseAdapterIdentityLine(const std::wstring& value, LUID& adapterId, UINT32* targetId)
{
    const auto trimmed = MonitorSplitter::Trim(value);
    constexpr wchar_t prefix[] = L"adapter:";
    constexpr size_t prefixLength = ARRAYSIZE(prefix) - 1;
    if (trimmed.size() <= prefixLength ||
        _wcsnicmp(trimmed.c_str(), prefix, prefixLength) != 0)
    {
        return false;
    }

    unsigned long lowPart = 0;
    long highPart = 0;
    unsigned int parsedTargetId = 0;
    if (swscanf_s(trimmed.c_str() + prefixLength, L"%lu:%ld:%u", &lowPart, &highPart, &parsedTargetId) != 3)
    {
        return false;
    }

    adapterId.LowPart = static_cast<DWORD>(lowPart);
    adapterId.HighPart = static_cast<LONG>(highPart);
    if (targetId != nullptr)
    {
        *targetId = static_cast<UINT32>(parsedTargetId);
    }
    return true;
}

inline bool TryParseSelectorAdapterIdentity(const std::wstring& selector, LUID& adapterId, UINT32* targetId)
{
    for (const auto& line : SplitSelectorLines(selector))
    {
        if (TryParseAdapterIdentityLine(line, adapterId, targetId))
        {
            return true;
        }
    }
    adapterId = {};
    if (targetId != nullptr)
    {
        *targetId = 0;
    }
    return false;
}

inline bool IsDisplayInterfacePathLine(const std::wstring& value)
{
    const auto trimmed = MonitorSplitter::Trim(value);
    constexpr wchar_t prefix[] = L"\\\\?\\DISPLAY#";
    constexpr size_t prefixLength = ARRAYSIZE(prefix) - 1;
    return trimmed.size() > prefixLength &&
           _wcsnicmp(trimmed.c_str(), prefix, prefixLength) == 0;
}

inline std::wstring ExactPhysicalPanelSelectorBundle(const std::wstring& selector)
{
    std::vector<std::wstring> exactLines;
    std::vector<std::wstring> metadataLines;
    for (const auto& line : SplitSelectorLines(selector))
    {
        if (IsSelectorMetadataLine(line))
        {
            AppendUniqueSelectorLine(metadataLines, line);
            continue;
        }

        LUID adapterId = {};
        UINT32 targetId = 0;
        if (IsDisplayInterfacePathLine(line) || TryParseAdapterIdentityLine(line, adapterId, &targetId))
        {
            AppendUniqueSelectorLine(exactLines, line);
        }
    }

    if (exactLines.empty())
    {
        return selector;
    }

    for (const auto& metadataLine : metadataLines)
    {
        AppendUniqueSelectorLine(exactLines, metadataLine);
    }
    return JoinSelectorLines(exactLines);
}

inline bool RepairHostTargetSelectorForDirectMode()
{
    const auto selector = LoadHostTarget();
    const auto repaired = ExactPhysicalPanelSelectorBundle(selector);
    if (MonitorSplitter::Trim(repaired).empty() ||
        MonitorSplitter::Trim(repaired) == MonitorSplitter::Trim(selector))
    {
        return true;
    }

    return SaveHostTarget(repaired);
}

inline std::vector<MonitorCandidate> EnumerateMonitorCandidates()
{
    const auto monitors = EnumerateMonitors();
    const auto adapters = EnumerateAdapters();
    const auto paths = QueryDisplayPaths();

    std::vector<MonitorCandidate> candidates;
    for (const auto& monitor : monitors)
    {
        const AdapterSnapshot* adapter = FindAdapter(adapters, monitor.DeviceName);
        if (IsMonitorSplitterAdapter(adapter))
        {
            continue;
        }

        MonitorCandidate candidate;
        candidate.Device = monitor.DeviceName;
        candidate.Primary = (monitor.Flags & MONITORINFOF_PRIMARY) != 0;
        candidate.X = monitor.MonitorRect.left;
        candidate.Y = monitor.MonitorRect.top;
        candidate.Width = monitor.MonitorRect.right - monitor.MonitorRect.left;
        candidate.Height = monitor.MonitorRect.bottom - monitor.MonitorRect.top;

        if (adapter != nullptr)
        {
            candidate.AdapterString = adapter->DeviceString;
            candidate.AdapterId = adapter->DeviceId;
        }

        if (const auto* path = FindDisplayPath(paths, monitor.DeviceName))
        {
            candidate.FriendlyName = path->TargetFriendlyName;
            candidate.DevicePath = path->TargetDevicePath;
            candidate.TargetAdapterId = path->TargetAdapterId;
            candidate.TargetId = path->TargetId;
            candidate.AdapterIdentity = AdapterIdentity(path->TargetAdapterId, path->TargetId);
        }

        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

inline bool SelectorMatchesDisplayPath(const std::wstring& selector, const DisplayPathSnapshot& path)
{
    const auto adapterIdentity = AdapterIdentity(path.TargetAdapterId, path.TargetId);
    for (const auto& line : SplitSelectorLines(selector))
    {
        if (IsSelectorMetadataLine(line))
        {
            continue;
        }

        if (_wcsicmp(line.c_str(), path.TargetDevicePath.c_str()) == 0 ||
            _wcsicmp(line.c_str(), path.TargetFriendlyName.c_str()) == 0 ||
            _wcsicmp(line.c_str(), adapterIdentity.c_str()) == 0)
        {
            return true;
        }
    }
    return false;
}

inline bool HostTargetMatchesActiveDesktopMonitor()
{
    const auto selector = LoadHostTarget();
    if (MonitorSplitter::Trim(selector).empty())
    {
        return false;
    }

    for (const auto& candidate : EnumerateMonitorCandidates())
    {
        if (SavedPhysicalPanelSelectorMatchesCandidate(selector, candidate))
        {
            return true;
        }
    }
    return false;
}

inline std::wstring HostPanelEnableBlockReason()
{
    if (MonitorSplitter::Trim(LoadHostTarget()).empty())
    {
        return L"physical panel target is not configured; apply a target and layout before enabling MonitorSplitter";
    }

    if (HostTargetMatchesActiveDesktopMonitor())
    {
        return L"physical panel is still in the Windows desktop; remove it in Display Settings before enabling MonitorSplitter";
    }

    return {};
}

inline std::wstring HostPanelStateDescription()
{
    if (MonitorSplitter::Trim(LoadHostTarget()).empty())
    {
        return L"panel target not configured";
    }

    if (HostTargetMatchesActiveDesktopMonitor())
    {
        return L"physical panel is still in the Windows desktop; remove it in Display Settings before direct mode";
    }

    return L"physical panel is not in the active Windows desktop list";
}

inline std::wstring ResolveEdidNameBaseFromDisplayConfigDatabase(const std::wstring& selector)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!QueryDisplayConfigRaw(QDC_DATABASE_CURRENT, paths, modes))
    {
        return {};
    }

    for (const auto& path : paths)
    {
        DisplayPathSnapshot snapshot;
        snapshot.TargetFriendlyName = GetTargetFriendlyName(path.targetInfo);
        snapshot.TargetDevicePath = GetTargetDevicePath(path.targetInfo);
        snapshot.TargetAdapterId = path.targetInfo.adapterId;
        snapshot.TargetId = path.targetInfo.id;
        if (snapshot.TargetFriendlyName.empty())
        {
            snapshot.TargetFriendlyName = ReadPhysicalEdidNameFromTargetDevicePath(snapshot.TargetDevicePath);
        }
        if (SelectorMatchesDisplayPath(selector, snapshot))
        {
            return MonitorSplitter::Trim(snapshot.TargetFriendlyName);
        }
    }

    return {};
}

inline std::wstring PersistResolvedEdidNameBase(const std::wstring& nameBase)
{
    const auto trimmed = MonitorSplitter::Trim(nameBase);
    if (!trimmed.empty())
    {
        SaveEdidNameBase(trimmed);
        EnsureHostTargetEdidNameBaseMetadata(trimmed);
    }
    return trimmed;
}

inline std::wstring ResolveEdidNameBase(const Layout& layout, bool preferSaved = true)
{
    const auto savedNameBase = MonitorSplitter::Trim(LoadEdidNameBase());
    if (preferSaved && !savedNameBase.empty())
    {
        EnsureHostTargetEdidNameBaseMetadata(savedNameBase);
        return savedNameBase;
    }

    const auto selector = LoadHostTarget();
    if (!MonitorSplitter::Trim(selector).empty())
    {
        const auto metadataName = ReadEdidNameBaseFromSelectorMetadata(selector);
        if (!metadataName.empty())
        {
            return PersistResolvedEdidNameBase(metadataName);
        }

        for (const auto& line : SplitSelectorLines(selector))
        {
            if (IsSelectorMetadataLine(line))
            {
                continue;
            }

            const auto registryName = ReadPhysicalEdidNameFromTargetDevicePath(line);
            if (!registryName.empty())
            {
                return PersistResolvedEdidNameBase(registryName);
            }
        }

        const auto databaseName = ResolveEdidNameBaseFromDisplayConfigDatabase(selector);
        if (!databaseName.empty())
        {
            return PersistResolvedEdidNameBase(databaseName);
        }

        std::wstring matchedName;
        size_t matchCount = 0;
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
            matchedName = candidate.FriendlyName;
        }

        if (matchCount == 1)
        {
            return PersistResolvedEdidNameBase(matchedName);
        }
    }

    return !preferSaved ? savedNameBase : std::wstring();
}

struct PreparedConfiguration
{
    Layout LayoutValue;
    std::wstring LayoutSpec;
    std::wstring TargetSelector;
    std::wstring EdidNameBase;
};

inline std::wstring CandidateEdidNameBase(const MonitorCandidate& candidate)
{
    auto nameBase = MonitorSplitter::Trim(candidate.FriendlyName);
    if (ContainsInsensitive(nameBase, L"Saved off-desktop panel"))
    {
        nameBase.clear();
    }
    if (!nameBase.empty())
    {
        return nameBase;
    }

    const auto selector = candidate.SelectorBundle();
    nameBase = ReadEdidNameBaseFromSelectorMetadata(selector);
    if (!nameBase.empty())
    {
        return nameBase;
    }

    for (const auto& line : SplitSelectorLines(selector))
    {
        if (IsSelectorMetadataLine(line))
        {
            continue;
        }

        nameBase = ReadPhysicalEdidNameFromTargetDevicePath(line);
        if (!nameBase.empty())
        {
            return nameBase;
        }
    }

    return {};
}

inline bool PersistPreparedConfiguration(
    const Layout& layout,
    const std::wstring& selector,
    std::wstring edidNameBase,
    PreparedConfiguration& prepared,
    std::wstring& reason)
{
    const auto exactSelector = ExactPhysicalPanelSelectorBundle(selector);
    if (MonitorSplitter::Trim(exactSelector).empty())
    {
        reason = L"physical panel target selector is missing";
        return false;
    }

    const auto layoutSpec = SerializeLayout(layout);
    auto selectorBundle = SelectorWithEdidNameBaseMetadata(exactSelector, edidNameBase);
    if (MonitorSplitter::Trim(selectorBundle).empty())
    {
        selectorBundle = exactSelector;
    }

    if (!SaveLayout(layout))
    {
        reason = L"could not write layout config: " + LayoutPath();
        return false;
    }
    if (!SaveHostTarget(selectorBundle))
    {
        reason = L"could not write host target config: " + HostTargetPath();
        return false;
    }

    if (MonitorSplitter::Trim(edidNameBase).empty())
    {
        edidNameBase = ResolveEdidNameBase(layout, false);
        const auto repairedSelector = ExactPhysicalPanelSelectorBundle(LoadHostTarget());
        selectorBundle = SelectorWithEdidNameBaseMetadata(repairedSelector, edidNameBase);
        if (MonitorSplitter::Trim(selectorBundle).empty())
        {
            selectorBundle = repairedSelector;
        }
        if (!MonitorSplitter::Trim(selectorBundle).empty() && !SaveHostTarget(selectorBundle))
        {
            reason = L"could not write repaired host target config: " + HostTargetPath();
            return false;
        }
    }

    edidNameBase = MonitorSplitter::Trim(edidNameBase);
    if (!edidNameBase.empty() && !SaveEdidNameBase(edidNameBase))
    {
        reason = L"could not write EDID name base config: " + EdidNameBasePath();
        return false;
    }

    prepared.LayoutValue = layout;
    prepared.LayoutSpec = layoutSpec;
    prepared.TargetSelector = MonitorSplitter::Trim(LoadHostTarget());
    if (prepared.TargetSelector.empty())
    {
        prepared.TargetSelector = selectorBundle;
    }
    prepared.EdidNameBase = edidNameBase;
    reason.clear();
    return true;
}

inline bool PrepareConfigurationForCandidate(
    const Layout& layout,
    const MonitorCandidate& candidate,
    PreparedConfiguration& prepared,
    std::wstring& reason)
{
    const auto selector = candidate.SelectorBundle();
    if (MonitorSplitter::Trim(selector).empty())
    {
        reason = L"selected physical monitor did not provide a usable target selector";
        return false;
    }

    return PersistPreparedConfiguration(
        layout,
        selector,
        CandidateEdidNameBase(candidate),
        prepared,
        reason);
}

inline bool PrepareSavedConfiguration(PreparedConfiguration& prepared, std::wstring& reason)
{
    Layout layout;
    if (!LoadLayout(layout))
    {
        reason = L"layout file is invalid";
        return false;
    }

    const auto selector = LoadHostTarget();
    if (MonitorSplitter::Trim(selector).empty())
    {
        reason = L"physical panel target is not configured; apply a target and layout before enabling MonitorSplitter";
        return false;
    }

    const auto edidNameBase = ResolveEdidNameBase(layout);
    return PersistPreparedConfiguration(
        layout,
        LoadHostTarget(),
        edidNameBase,
        prepared,
        reason);
}

inline std::wstring DefaultSplitsForWidth(int width)
{
    if (width <= 0)
    {
        return L"1280,2560,1280";
    }
    const int left = width / 4;
    const int center = width / 2;
    const int right = width - left - center;
    return std::to_wstring(left) + L"," + std::to_wstring(center) + L"," + std::to_wstring(right);
}
}
