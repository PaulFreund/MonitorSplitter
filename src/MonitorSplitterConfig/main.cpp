#include <windows.h>
#include <shellapi.h>

#include <string>
#include <utility>
#include <vector>

#include "..\MonitorSplitterCommon\BuildInfo.h"
#include "..\MonitorSplitterControl\Control.h"

namespace
{
namespace Control = MonitorSplitter::Control;

constexpr wchar_t kWindowClassName[] = L"MonitorSplitterConfigWindow";
constexpr wchar_t kAppTitle[] = L"MonitorSplitter";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MonitorSplitter.Config";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowMessage = WM_APP + 2;
constexpr wchar_t kDefaultManualText[] =
    L"1. Select the physical ultrawide above.\r\n"
    L"2. Configure split widths and click Apply Target + Layout.\r\n"
    L"3. In Windows Display settings, select the original physical monitor and set Multiple displays to Remove display from desktop.\r\n"
    L"4. Click Enable. Disable stops split output. Use Open Display Settings when you want to return the physical panel to Windows.";
constexpr wchar_t kPanelRestoreManualText[] =
    L"Disable requested.\r\n"
    L"1. Click Open Display Settings if the physical panel should be returned to Windows.\r\n"
    L"2. Select the original physical monitor.\r\n"
    L"3. In Multiple displays, turn Remove display from desktop off so Windows owns the physical panel again.\r\n"
    L"4. Leave MonitorSplitter disabled until the physical panel shows the normal Windows desktop.";

enum ControlId
{
    IdState = 100,
    IdMonitor = 101,
    IdHostWidth = 102,
    IdHeight = 103,
    IdRefresh = 104,
    IdSplits = 105,
    IdRefreshButton = 106,
    IdApplyButton = 107,
    IdEnableButton = 108,
    IdDisableButton = 109,
    IdOpenSettingsButton = 110,
    IdLog = 112,
};

HWND g_window = nullptr;
HWND g_stateLabel = nullptr;
HWND g_monitorCombo = nullptr;
HWND g_hostWidthEdit = nullptr;
HWND g_heightEdit = nullptr;
HWND g_refreshEdit = nullptr;
HWND g_splitsEdit = nullptr;
HWND g_manualEdit = nullptr;
HWND g_logEdit = nullptr;
HFONT g_font = nullptr;
NOTIFYICONDATAW g_tray = {};
UINT g_taskbarCreatedMessage = 0;
bool g_exiting = false;
std::vector<Control::MonitorCandidate> g_candidates;

void SetText(HWND control, const std::wstring& value)
{
    SetWindowTextW(control, value.c_str());
}

void SetManualText(const wchar_t* value)
{
    if (g_manualEdit != nullptr)
    {
        SetWindowTextW(g_manualEdit, value);
    }
}

std::wstring GetText(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void AppendLog(const std::wstring& text)
{
    const std::wstring oldText = GetText(g_logEdit);
    std::wstring next = oldText;
    if (!next.empty())
    {
        next += L"\r\n";
    }
    next += text;
    SetText(g_logEdit, next);
    SendMessageW(g_logEdit, EM_SETSEL, static_cast<WPARAM>(next.size()), static_cast<LPARAM>(next.size()));
    SendMessageW(g_logEdit, EM_SCROLLCARET, 0, 0);
}

void WriteConfigStatus(const wchar_t* state)
{
    Control::EnsureDirectory(Control::ProgramDataConfigDirectory());

    DWORD sessionId = 0xFFFFFFFF;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);

    std::wstring status;
    status += L"{\"running\":";
    status += (_wcsicmp(state, L"stopped") == 0) ? L"false" : L"true";
    status += L",\"role\":\"config\"";
    status += L",\"state\":\"";
    status += Control::JsonEscape(state);
    status += L"\",\"pid\":";
    status += std::to_wstring(GetCurrentProcessId());
    status += L",\"sessionId\":";
    status += std::to_wstring(sessionId);
    status += L",\"updatedTick\":";
    status += std::to_wstring(GetTickCount());
    status += L",\"component\":{\"name\":\"MonitorSplitterConfig\",\"productVersion\":\"";
    status += Control::JsonEscape(MonitorSplitter::kProductVersionWide);
    status += L"\",\"buildTag\":\"";
    status += Control::JsonEscape(MonitorSplitter::kBuildTagWide);
    status += L"\"}}";
    Control::WriteTextFile(Control::ConfigStatusPath(), status);
}

void SetTrayTooltip(const std::wstring& tooltip)
{
    wcsncpy_s(g_tray.szTip, tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void ApplyFont(HWND control)
{
    if (g_font != nullptr)
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    }
}

HWND CreateChild(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id)
{
    HWND control = CreateWindowExW(
        0,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        g_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),
        nullptr);
    ApplyFont(control);
    return control;
}

void ParseLayoutIntoFields(const MonitorSplitter::Layout& layout)
{
    SetText(g_hostWidthEdit, std::to_wstring(layout.HostWidth));
    SetText(g_heightEdit, std::to_wstring(layout.Height));
    SetText(g_refreshEdit, std::to_wstring(layout.Refresh));

    std::wstring splits;
    for (size_t index = 0; index < layout.Monitors.size(); index++)
    {
        if (index != 0)
        {
            splits += L",";
        }
        splits += std::to_wstring(layout.Monitors[index].Width);
    }
    SetText(g_splitsEdit, splits);
}

std::wstring SavedTargetLabel(const std::wstring& selector)
{
    for (const auto& line : Control::SplitSelectorLines(selector))
    {
        LUID adapterId = {};
        UINT32 targetId = 0;
        if (!Control::TryParseAdapterIdentityLine(line, adapterId, &targetId))
        {
            return line;
        }
    }
    return L"saved target";
}

void PopulateMonitorCombo(const MonitorSplitter::Layout& layout)
{
    SendMessageW(g_monitorCombo, CB_RESETCONTENT, 0, 0);
    g_candidates = Control::EnumerateMonitorCandidates();

    const auto savedTarget = Control::LoadHostTarget();
    int selectedIndex = -1;
    bool savedTargetMatchedActiveMonitor = false;
    for (size_t index = 0; index < g_candidates.size(); index++)
    {
        const auto displayName = g_candidates[index].DisplayName();
        SendMessageW(g_monitorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(displayName.c_str()));
        if (!savedTarget.empty() && Control::SavedPhysicalPanelSelectorMatchesCandidate(savedTarget, g_candidates[index]))
        {
            selectedIndex = static_cast<int>(index);
            savedTargetMatchedActiveMonitor = true;
        }
    }

    if (!MonitorSplitter::Trim(savedTarget).empty() && !savedTargetMatchedActiveMonitor)
    {
        Control::MonitorCandidate savedCandidate;
        savedCandidate.FriendlyName = L"Saved off-desktop panel";
        savedCandidate.Device = SavedTargetLabel(savedTarget);
        savedCandidate.SelectorOverride = savedTarget;
        savedCandidate.Width = static_cast<int>(layout.HostWidth);
        savedCandidate.Height = static_cast<int>(layout.Height);
        Control::TryParseSelectorAdapterIdentity(savedTarget, savedCandidate.TargetAdapterId, &savedCandidate.TargetId);
        if (savedCandidate.TargetAdapterId.LowPart != 0 || savedCandidate.TargetAdapterId.HighPart != 0)
        {
            savedCandidate.AdapterIdentity = Control::AdapterIdentity(savedCandidate.TargetAdapterId, savedCandidate.TargetId);
        }

        const auto displayName = savedCandidate.DisplayName();
        selectedIndex = static_cast<int>(g_candidates.size());
        SendMessageW(g_monitorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(displayName.c_str()));
        g_candidates.push_back(std::move(savedCandidate));
    }

    if (!g_candidates.empty())
    {
        SendMessageW(g_monitorCombo, CB_SETCURSEL, selectedIndex < 0 ? 0 : selectedIndex, 0);
    }
}

void ApplySelectedMonitorDefaults(bool forceSplits)
{
    const LRESULT selected = SendMessageW(g_monitorCombo, CB_GETCURSEL, 0, 0);
    if (selected < 0 || static_cast<size_t>(selected) >= g_candidates.size())
    {
        return;
    }

    const auto& candidate = g_candidates[static_cast<size_t>(selected)];
    if (candidate.Width > 0)
    {
        SetText(g_hostWidthEdit, std::to_wstring(candidate.Width));
    }
    if (candidate.Height > 0)
    {
        SetText(g_heightEdit, std::to_wstring(candidate.Height));
    }
    if (MonitorSplitter::Trim(GetText(g_refreshEdit)).empty())
    {
        SetText(g_refreshEdit, L"120");
    }
    if (forceSplits || MonitorSplitter::Trim(GetText(g_splitsEdit)).empty())
    {
        SetText(g_splitsEdit, Control::DefaultSplitsForWidth(candidate.Width));
    }
}

std::wstring ComposeLayoutSpec()
{
    const auto width = MonitorSplitter::Trim(GetText(g_hostWidthEdit));
    const auto height = MonitorSplitter::Trim(GetText(g_heightEdit));
    const auto refresh = MonitorSplitter::Trim(GetText(g_refreshEdit));
    const auto splits = MonitorSplitter::Trim(GetText(g_splitsEdit));
    if (width.empty() || height.empty() || refresh.empty() || splits.empty())
    {
        return {};
    }
    return width + L"x" + height + L"@" + refresh + L":" + splits;
}

std::wstring PanelStateText()
{
    return Control::HostPanelStateDescription();
}

void RefreshState(bool verbose)
{
    WriteConfigStatus(L"running");

    const auto desired = Control::DesiredEnabled();
    const auto hostStatus = Control::ReadTextFile(Control::HostStatusPath());
    const auto service = Control::ServiceStateText();
    const auto panel = PanelStateText();
    std::wstring healthReason;

    std::wstring state;
    if (!desired)
    {
        state = L"Disabled; " + service + L"; " + panel + L".";
        SetTrayTooltip(L"MonitorSplitter: disabled");
    }
    else if (Control::HostStatusHealthy(hostStatus, &healthReason))
    {
        state = L"OK; direct scanout healthy; " + service + L"; " + panel + L".";
        SetTrayTooltip(L"MonitorSplitter: OK");
    }
    else
    {
        state = L"Needs attention; " +
            (healthReason.empty() ? L"direct scanout is not healthy" : healthReason) +
            L"; " + service + L"; " + panel + L".";
        SetTrayTooltip(L"MonitorSplitter: needs attention");
    }

    SetText(g_stateLabel, state);
    if (verbose)
    {
        AppendLog(L"Config directory: " + Control::ConfigDirectory());
        AppendLog(L"Panel: " + panel);
        AppendLog(L"Service: " + service);
        if (!healthReason.empty())
        {
            AppendLog(L"Health: " + healthReason);
        }
        AppendLog(L"Host status: " + MonitorSplitter::Trim(hostStatus));
    }
}

void RefreshAll(bool verbose)
{
    MonitorSplitter::Layout layout;
    Control::LoadLayout(layout);
    ParseLayoutIntoFields(layout);
    PopulateMonitorCombo(layout);
    ApplySelectedMonitorDefaults(false);
    RefreshState(verbose);
}

void OpenDisplaySettings();

bool ApplyConfiguration(bool* restartRequested = nullptr)
{
    if (restartRequested != nullptr)
    {
        *restartRequested = false;
    }

    SetManualText(kDefaultManualText);

    const LRESULT selected = SendMessageW(g_monitorCombo, CB_GETCURSEL, 0, 0);
    if (selected < 0 || static_cast<size_t>(selected) >= g_candidates.size())
    {
        MessageBoxW(g_window, L"Select a physical monitor first.", kAppTitle, MB_ICONWARNING);
        return false;
    }

    const std::wstring spec = ComposeLayoutSpec();
    MonitorSplitter::Layout layout;
    std::wstring error;
    if (!MonitorSplitter::ParseLayoutSpec(spec, layout, &error))
    {
        MessageBoxW(g_window, (L"Invalid layout: " + error).c_str(), kAppTitle, MB_ICONWARNING);
        return false;
    }

    const auto& candidate = g_candidates[static_cast<size_t>(selected)];
    const auto previousLayout = MonitorSplitter::Trim(Control::ReadTextFile(Control::LayoutPath()));
    const auto previousTarget = MonitorSplitter::Trim(Control::LoadHostTarget());
    const auto previousEdidNameBase = MonitorSplitter::Trim(Control::LoadEdidNameBase());

    Control::PreparedConfiguration prepared;
    if (!Control::PrepareConfigurationForCandidate(layout, candidate, prepared, error))
    {
        MessageBoxW(g_window, (L"Could not write MonitorSplitter configuration: " + error).c_str(), kAppTitle, MB_ICONERROR);
        return false;
    }

    AppendLog(L"Saved layout " + prepared.LayoutSpec);
    AppendLog(L"Saved target " + candidate.DisplayName());
    if (!prepared.EdidNameBase.empty())
    {
        AppendLog(L"Saved EDID name base " + prepared.EdidNameBase);
    }
    const bool changed =
        !EqualsIgnoreCase(previousLayout, prepared.LayoutSpec) ||
        !EqualsIgnoreCase(previousTarget, prepared.TargetSelector) ||
        !EqualsIgnoreCase(previousEdidNameBase, prepared.EdidNameBase);
    if (changed)
    {
        const bool requested = Control::RequestServiceRestart(true);
        if (restartRequested != nullptr)
        {
            *restartRequested = requested;
        }
    }
    else
    {
        Control::SignalStackWake();
    }
    RefreshState(false);
    return true;
}

void SetDesiredState(bool enabled)
{
    bool configurationRestartRequested = false;
    if (enabled)
    {
        if (!ApplyConfiguration(&configurationRestartRequested))
        {
            AppendLog(L"Enable cancelled because target/layout could not be applied.");
            RefreshState(false);
            return;
        }
        if (const auto blockReason = Control::HostPanelEnableBlockReason(); !blockReason.empty())
        {
            MessageBoxW(
                g_window,
                (blockReason + L".\r\n\r\nUse Open Display Settings, select the original physical monitor, and set Multiple displays to \"Remove display from desktop\" first.").c_str(),
                kAppTitle,
                MB_ICONWARNING);
            AppendLog(L"Enable blocked: " + blockReason);
            RefreshState(false);
            return;
        }
    }

    if (!enabled)
    {
        const int response = MessageBoxW(
            g_window,
            L"MonitorSplitter will stop split output. Use Open Display Settings afterwards if you want Windows to own the physical panel again.",
            kAppTitle,
            MB_OKCANCEL | MB_ICONWARNING);
        if (response != IDOK)
        {
            AppendLog(L"Disable cancelled.");
            return;
        }
    }

    if (!Control::SaveDesiredEnabled(enabled))
    {
        MessageBoxW(g_window, L"Could not write service desired state. Reinstall the service to repair ProgramData permissions.", kAppTitle, MB_ICONERROR);
        return;
    }
    if (enabled)
    {
        SetManualText(kDefaultManualText);
        Control::StartServiceIfNeeded();
        if (configurationRestartRequested)
        {
            AppendLog(L"Requested enable; configuration change already requested recovery.");
        }
        else
        {
            std::wstring recoveryReason;
            if (Control::CurrentHostStatusNeedsRecovery(&recoveryReason))
            {
                if (Control::RequestServiceRestart())
                {
                    AppendLog(L"Requested enable and recovery: " + recoveryReason);
                }
                else
                {
                    MessageBoxW(g_window, L"Could not write service recovery request.", kAppTitle, MB_ICONERROR);
                    AppendLog(L"Enable recovery request failed.");
                }
            }
            else
            {
                Control::SignalStackWake();
                AppendLog(L"Requested enable; splitter already healthy.");
            }
        }
    }
    else
    {
        Control::SignalStackWake();
        AppendLog(L"Requested disable.");
    }
    if (!enabled)
    {
        SetManualText(kPanelRestoreManualText);
        AppendLog(L"Disable requested. Use Open Display Settings to return the physical panel to Windows if needed.");
    }
    RefreshState(false);
}

void OpenDisplaySettings()
{
    ShellExecuteW(g_window, L"open", L"ms-settings:display-advanced", nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowMainWindow()
{
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
    RefreshAll(false);
}

void ShowTrayMenu()
{
    POINT point = {};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Open MonitorSplitter");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2, L"Enable");
    AppendMenuW(menu, MF_STRING, 3, L"Disable");
    AppendMenuW(menu, MF_STRING, 4, L"Refresh State");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"Exit Tray App");
    SetForegroundWindow(g_window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y, 0, g_window, nullptr);
    DestroyMenu(menu);

    switch (command)
    {
    case 1:
        ShowMainWindow();
        break;
    case 2:
        SetDesiredState(true);
        break;
    case 3:
        SetDesiredState(false);
        break;
    case 4:
        RefreshState(true);
        break;
    case 5:
        g_exiting = true;
        DestroyWindow(g_window);
        break;
    default:
        break;
    }
}

void AddTrayIcon()
{
    ZeroMemory(&g_tray, sizeof(g_tray));
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = g_window;
    g_tray.uID = kTrayIconId;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"MonitorSplitter");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_tray);
}

void CreateUi(HWND hwnd)
{
    g_window = hwnd;
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
    {
        g_font = CreateFontIndirectW(&metrics.lfMessageFont);
    }

    CreateChild(L"STATIC", L"State", 0, 16, 16, 80, 20, -1);
    g_stateLabel = CreateChild(L"STATIC", L"Checking...", SS_LEFT, 108, 16, 620, 44, IdState);

    CreateChild(L"STATIC", L"Monitor", 0, 16, 72, 80, 20, -1);
    g_monitorCombo = CreateChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 108, 68, 510, 240, IdMonitor);
    CreateChild(L"BUTTON", L"Refresh", BS_PUSHBUTTON, 632, 67, 96, 28, IdRefreshButton);

    CreateChild(L"STATIC", L"Host width", 0, 16, 114, 80, 20, -1);
    g_hostWidthEdit = CreateChild(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, 108, 110, 110, 24, IdHostWidth);
    CreateChild(L"STATIC", L"Height", 0, 236, 114, 56, 20, -1);
    g_heightEdit = CreateChild(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 296, 110, 110, 24, IdHeight);
    CreateChild(L"STATIC", L"Refresh", 0, 424, 114, 56, 20, -1);
    g_refreshEdit = CreateChild(L"EDIT", L"120", WS_BORDER | ES_AUTOHSCROLL, 484, 110, 70, 24, IdRefresh);
    CreateChild(L"STATIC", L"Hz", 0, 560, 114, 24, 20, -1);

    CreateChild(L"STATIC", L"Split widths", 0, 16, 150, 86, 20, -1);
    g_splitsEdit = CreateChild(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 108, 146, 300, 24, IdSplits);
    CreateChild(L"STATIC", L"Example: 1280,2560,1280", 0, 424, 150, 220, 20, -1);

    CreateChild(L"BUTTON", L"Apply Target + Layout", BS_PUSHBUTTON, 108, 186, 150, 30, IdApplyButton);
    CreateChild(L"BUTTON", L"Enable", BS_PUSHBUTTON, 270, 186, 90, 30, IdEnableButton);
    CreateChild(L"BUTTON", L"Disable", BS_PUSHBUTTON, 372, 186, 90, 30, IdDisableButton);
    CreateChild(L"BUTTON", L"Open Display Settings", BS_PUSHBUTTON, 474, 186, 154, 30, IdOpenSettingsButton);

    CreateChild(L"STATIC", L"Manual", 0, 16, 234, 80, 20, -1);
    g_manualEdit = CreateChild(
        L"EDIT",
        kDefaultManualText,
        WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        108,
        232,
        620,
        110,
        -1);

    CreateChild(L"STATIC", L"Log", 0, 16, 362, 80, 20, -1);
    g_logEdit = CreateChild(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 108, 360, 620, 140, IdLog);

    SetTimer(hwnd, 1, 5000, nullptr);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == g_taskbarCreatedMessage)
    {
        AddTrayIcon();
        RefreshState(false);
        return 0;
    }

    switch (message)
    {
    case WM_CREATE:
        CreateUi(hwnd);
        AddTrayIcon();
        RefreshAll(false);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IdMonitor:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                ApplySelectedMonitorDefaults(false);
            }
            break;
        case IdRefreshButton:
            RefreshAll(true);
            break;
        case IdApplyButton:
            ApplyConfiguration();
            break;
        case IdEnableButton:
            SetDesiredState(true);
            break;
        case IdDisableButton:
            SetDesiredState(false);
            break;
        case IdOpenSettingsButton:
            OpenDisplaySettings();
            break;
        default:
            break;
        }
        return 0;
    case WM_TIMER:
        RefreshState(false);
        return 0;
    case WM_DISPLAYCHANGE:
        RefreshState(false);
        return 0;
    case WM_DEVICECHANGE:
        RefreshState(false);
        return 0;
    case WM_POWERBROADCAST:
        RefreshState(false);
        return TRUE;
    case kShowMessage:
        ShowMainWindow();
        return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK)
        {
            ShowMainWindow();
        }
        else if (LOWORD(lParam) == WM_RBUTTONUP)
        {
            ShowTrayMenu();
        }
        return 0;
    case WM_CLOSE:
        if (g_exiting)
        {
            DestroyWindow(hwnd);
        }
        else
        {
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    case WM_DESTROY:
        WriteConfigStatus(L"stopped");
        RemoveTrayIcon();
        if (g_font != nullptr)
        {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(kWindowClassName, nullptr);
        if (existing != nullptr)
        {
            PostMessageW(existing, kShowMessage, 0, 0);
        }
        return 0;
    }

    const std::wstring args = Control::ToLower(commandLine == nullptr ? L"" : commandLine);
    const bool startHidden = args.find(L"--tray") != std::wstring::npos;
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        kAppTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        760,
        560,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd == nullptr)
    {
        return 1;
    }

    if (!startHidden)
    {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (mutex != nullptr)
    {
        CloseHandle(mutex);
    }
    return static_cast<int>(message.wParam);
}
