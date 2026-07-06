#pragma once

#include <windows.h>
#include <devpropdef.h>

#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace MonitorSplitter
{
static constexpr DWORD kMaxMonitors = 8;
static constexpr DWORD kDefaultHostWidth = 5120;
static constexpr DWORD kDefaultHeight = 1440;
static constexpr DWORD kDefaultRefresh = 120;

static const DEVPROPKEY kLayoutPropertyKey =
{
    { 0xd0f8b716, 0xa18d, 0x4b32, { 0x9d, 0x44, 0x70, 0xb0, 0x9f, 0x9a, 0x58, 0x21 } },
    2
};

static const DEVPROPKEY kRenderAdapterLuidPropertyKey =
{
    { 0xd0f8b716, 0xa18d, 0x4b32, { 0x9d, 0x44, 0x70, 0xb0, 0x9f, 0x9a, 0x58, 0x21 } },
    3
};

static const DEVPROPKEY kEdidNameBasePropertyKey =
{
    { 0xd0f8b716, 0xa18d, 0x4b32, { 0x9d, 0x44, 0x70, 0xb0, 0x9f, 0x9a, 0x58, 0x21 } },
    4
};

struct MonitorMode
{
    DWORD Width = 0;
    DWORD Height = 0;
    DWORD Refresh = 0;
};

struct Layout
{
    DWORD HostWidth = kDefaultHostWidth;
    DWORD Height = kDefaultHeight;
    DWORD Refresh = kDefaultRefresh;
    std::vector<MonitorMode> Monitors;
};

inline Layout DefaultLayout()
{
    Layout layout;
    layout.Monitors =
    {
        { 1707, kDefaultHeight, kDefaultRefresh },
        { 1706, kDefaultHeight, kDefaultRefresh },
        { 1707, kDefaultHeight, kDefaultRefresh },
    };
    return layout;
}

inline std::wstring Trim(const std::wstring& value)
{
    size_t first = 0;
    while (first < value.size() && iswspace(value[first]))
    {
        first++;
    }

    size_t last = value.size();
    while (last > first && iswspace(value[last - 1]))
    {
        last--;
    }

    return value.substr(first, last - first);
}

inline bool ParseUInt(const std::wstring& text, DWORD& value)
{
    const auto trimmed = Trim(text);
    if (trimmed.empty())
    {
        return false;
    }

    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || *end != L'\0' || parsed > 0xFFFFFFFFul)
    {
        return false;
    }

    value = static_cast<DWORD>(parsed);
    return true;
}

inline bool ParseHeader(const std::wstring& header, DWORD& hostWidth, DWORD& height, DWORD& refresh)
{
    const auto x = header.find(L'x');
    const auto at = header.find(L'@');
    if (x == std::wstring::npos || at == std::wstring::npos || x > at)
    {
        return false;
    }

    return ParseUInt(header.substr(0, x), hostWidth) &&
           ParseUInt(header.substr(x + 1, at - x - 1), height) &&
           ParseUInt(header.substr(at + 1), refresh);
}

inline bool ParseWidths(const std::wstring& widthsText, DWORD height, DWORD refresh, std::vector<MonitorMode>& monitors)
{
    monitors.clear();
    size_t start = 0;
    for (;;)
    {
        const size_t comma = widthsText.find(L',', start);
        const std::wstring token = widthsText.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);

        DWORD width = 0;
        if (!ParseUInt(token, width))
        {
            return false;
        }

        monitors.push_back({ width, height, refresh });
        if (comma == std::wstring::npos)
        {
            break;
        }
        start = comma + 1;
    }

    return true;
}

inline DWORD SumWidths(const Layout& layout)
{
    DWORD sum = 0;
    for (const auto& monitor : layout.Monitors)
    {
        sum += monitor.Width;
    }
    return sum;
}

inline bool ValidateLayout(const Layout& layout, std::wstring* error)
{
    if (layout.Monitors.empty() || layout.Monitors.size() > kMaxMonitors)
    {
        if (error != nullptr)
        {
            *error = L"layout must contain 1 to 8 monitors";
        }
        return false;
    }

    if (layout.Height < 480 || layout.Height > 4320)
    {
        if (error != nullptr)
        {
            *error = L"height must be between 480 and 4320";
        }
        return false;
    }

    if (layout.Refresh < 30 || layout.Refresh > 240)
    {
        if (error != nullptr)
        {
            *error = L"refresh must be between 30 and 240";
        }
        return false;
    }

    DWORD sum = 0;
    for (const auto& monitor : layout.Monitors)
    {
        if (monitor.Width < 320 || monitor.Width > 8192)
        {
            if (error != nullptr)
            {
                *error = L"each monitor width must be between 320 and 8192";
            }
            return false;
        }
        if (monitor.Height != layout.Height || monitor.Refresh != layout.Refresh)
        {
            if (error != nullptr)
            {
                *error = L"all monitor modes must share the layout height and refresh";
            }
            return false;
        }
        sum += monitor.Width;
    }

    if (layout.HostWidth != sum)
    {
        if (error != nullptr)
        {
            *error = L"host width must equal the sum of split widths";
        }
        return false;
    }

    return true;
}

inline bool ParseLayoutSpec(const std::wstring& spec, Layout& layout, std::wstring* error)
{
    const auto trimmed = Trim(spec);
    if (trimmed.empty() || trimmed == L"default" || trimmed == L"thirds")
    {
        layout = DefaultLayout();
        return true;
    }

    DWORD hostWidth = 0;
    DWORD height = kDefaultHeight;
    DWORD refresh = kDefaultRefresh;
    std::wstring widthsText = trimmed;

    const size_t separator = trimmed.find(L':');
    if (separator != std::wstring::npos)
    {
        if (!ParseHeader(trimmed.substr(0, separator), hostWidth, height, refresh))
        {
            if (error != nullptr)
            {
                *error = L"layout header must be hostWidthxheight@refresh";
            }
            return false;
        }
        widthsText = trimmed.substr(separator + 1);
    }

    std::vector<MonitorMode> monitors;
    if (!ParseWidths(widthsText, height, refresh, monitors))
    {
        if (error != nullptr)
        {
            *error = L"split widths must be a comma-separated list of positive integers";
        }
        return false;
    }

    layout.HostWidth = hostWidth == 0 ? 0 : hostWidth;
    layout.Height = height;
    layout.Refresh = refresh;
    layout.Monitors = std::move(monitors);
    if (layout.HostWidth == 0)
    {
        layout.HostWidth = SumWidths(layout);
    }

    return ValidateLayout(layout, error);
}

inline std::wstring SerializeLayout(const Layout& layout)
{
    std::wstring text = std::to_wstring(layout.HostWidth);
    text += L"x";
    text += std::to_wstring(layout.Height);
    text += L"@";
    text += std::to_wstring(layout.Refresh);
    text += L":";

    for (size_t index = 0; index < layout.Monitors.size(); index++)
    {
        if (index != 0)
        {
            text += L",";
        }
        text += std::to_wstring(layout.Monitors[index].Width);
    }

    return text;
}

inline std::wstring MonitorName(size_t count, size_t index)
{
    if (count == 1)
    {
        return L"MonitorSplitter";
    }
    if (count == 2)
    {
        return index == 0 ? L"MonitorSplitter Left" : L"MonitorSplitter Right";
    }
    if (count == 3)
    {
        if (index == 0)
        {
            return L"MonitorSplitter Left";
        }
        if (index == 1)
        {
            return L"MonitorSplitter Center";
        }
        return L"MonitorSplitter Right";
    }

    return L"MonitorSplitter " + std::to_wstring(index + 1);
}

inline std::wstring SharedFrameName(size_t index)
{
    return L"MonitorSplitter.Frame." + std::to_wstring(index);
}

static constexpr DWORD kSharedFrameStatusMagic = 0x4D535346; // MSSF
static constexpr DWORD kSharedFrameStatusVersion = 4;
static constexpr DWORD kRuntimeStringChars = 128;

struct SharedFrameStatus
{
    DWORD Magic = kSharedFrameStatusMagic;
    DWORD Version = kSharedFrameStatusVersion;
    DWORD ConnectorIndex = 0;
    DWORD ProducerAdapterLuidLow = 0;
    LONG ProducerAdapterLuidHigh = 0;
    DWORD LastSourceWidth = 0;
    DWORD LastSourceHeight = 0;
    DWORD LastSharedWidth = 0;
    DWORD LastSharedHeight = 0;
    DWORD LastSharedFormat = 0;
    DWORD ProducerProcessId = 0;
    ULONGLONG SharedHandleValue = 0;
    HRESULT LastCreateResult = S_OK;
    HRESULT LastPublishResult = S_OK;
    HRESULT LastAcquireResult = S_OK;
    ULONGLONG PublishAttempts = 0;
    ULONGLONG PublishedFrames = 0;
    ULONGLONG PublishFailures = 0;
    ULONGLONG LastPublishedTick = 0;
};

static constexpr DWORD kDriverRuntimeStatusMagic = 0x4D535256; // MSRV
static constexpr DWORD kDriverRuntimeStatusVersion = 2;

struct DriverRuntimeStatus
{
    DWORD Magic = kDriverRuntimeStatusMagic;
    DWORD Version = kDriverRuntimeStatusVersion;
    DWORD ConnectorIndex = 0;
    DWORD ProducerProcessId = 0;
    DWORD SharedFrameStatusVersion = kSharedFrameStatusVersion;
    ULONGLONG UpdatedTick = 0;
    ULONGLONG Heartbeats = 0;
    ULONGLONG PendingFrames = 0;
    ULONGLONG AcquiredFrames = 0;
    ULONGLONG FinishedFrames = 0;
    ULONGLONG FailedAcquires = 0;
    ULONGLONG Exits = 0;
    HRESULT LastAcquireResult = S_OK;
    HRESULT LastFinishedResult = S_OK;
    DWORD LastWaitResult = WAIT_TIMEOUT;
    wchar_t ProductVersion[kRuntimeStringChars] = {};
    wchar_t BuildTag[kRuntimeStringChars] = {};
};

inline std::wstring SharedFrameStatusName(size_t index)
{
    return L"Global\\MonitorSplitter.FrameStatus." + std::to_wstring(index);
}

inline std::wstring DriverRuntimeStatusName(size_t index)
{
    return L"Global\\MonitorSplitter.DriverRuntime." + std::to_wstring(index);
}

inline std::wstring SharedFrameTextureName(size_t index)
{
    return L"Global\\MonitorSplitter.FrameTexture." + std::to_wstring(index);
}
}
