#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <sddl.h>
#include <Windows.Devices.Display.Core.Interop.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#pragma comment(lib, "d3dcompiler.lib")

#include <algorithm>
#include <array>
#include <cwctype>
#include <exception>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "..\MonitorSplitterCommon\BuildInfo.h"
#include "..\MonitorSplitterCommon\Layout.h"
#include "..\MonitorSplitterControl\Control.h"

using Microsoft::WRL::ComPtr;
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace d3d11rt = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace display = winrt::Windows::Devices::Display;
namespace displaycore = winrt::Windows::Devices::Display::Core;
namespace metadata = winrt::Windows::Foundation::Metadata;
namespace appcap = winrt::Windows::Security::Authorization::AppCapabilityAccess;

namespace
{
static constexpr ULONGLONG kInitialSharedFrameFreshnessMs = 2000;

struct MonitorInfo
{
    HMONITOR Handle = nullptr;
    RECT Rect = {};
    std::wstring DeviceName;
    std::wstring AdapterString;
    std::wstring AdapterId;
    std::wstring MonitorString;
    std::wstring MonitorId;
    bool IsMonitorSplitter = false;
};

struct AppState;

class FrameSource
{
public:
    virtual ~FrameSource() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool CopyLatestToBackBuffer(ID3D11Texture2D* backBuffer, ID3D11RenderTargetView* targetView) const = 0;
    virtual ULONGLONG FrameGeneration() const = 0;
    virtual void AppendDiagnosticsJson(std::wstringstream& status) const = 0;
    virtual bool IsHealthy() const { return true; }
    virtual bool WantsPollingCopy() const { return false; }
};

class CaptureSource : public FrameSource
{
public:
    CaptureSource(AppState* state, MonitorInfo monitor, int destX);
    ~CaptureSource();

    void Start() override;
    void Stop() override;
    bool CopyLatestToBackBuffer(ID3D11Texture2D* backBuffer, ID3D11RenderTargetView* targetView) const override;
    ULONGLONG FrameGeneration() const override;
    void AppendDiagnosticsJson(std::wstringstream& status) const override;

private:
    void OnFrameArrived(capture::Direct3D11CaptureFramePool const& sender);
    void EnsureLatestTexture(ID3D11Texture2D* sourceTexture);

    AppState* m_State = nullptr;
    MonitorInfo m_Monitor;
    int m_DestX = 0;
    capture::GraphicsCaptureItem m_Item{ nullptr };
    capture::Direct3D11CaptureFramePool m_FramePool{ nullptr };
    capture::GraphicsCaptureSession m_Session{ nullptr };
    winrt::event_token m_FrameArrivedToken{};
    ComPtr<ID3D11Texture2D> m_LatestTexture;
    ULONGLONG m_FrameGeneration = 0;
    mutable UINT64 m_CopyAttempts = 0;
    mutable UINT64 m_CopySuccesses = 0;
    mutable UINT64 m_CopyFailures = 0;
    mutable HRESULT m_LastCopyResult = S_OK;
    mutable ULONGLONG m_LastCopiedGeneration = 0;
};

class SharedFrameSource : public FrameSource
{
public:
    SharedFrameSource(AppState* state, size_t connectorIndex, MonitorSplitter::MonitorMode mode, int destX);
    ~SharedFrameSource() override;

    void Start() override;
    void Stop() override;
    bool CopyLatestToBackBuffer(ID3D11Texture2D* backBuffer, ID3D11RenderTargetView* targetView) const override;
    ULONGLONG FrameGeneration() const override;
    void AppendDiagnosticsJson(std::wstringstream& status) const override;
    bool IsHealthy() const override;
    bool WantsPollingCopy() const override;

private:
    bool IsProducerReady() const;

    AppState* m_State = nullptr;
    size_t m_ConnectorIndex = 0;
    MonitorSplitter::MonitorMode m_Mode = {};
    int m_DestX = 0;
    ComPtr<ID3D11Texture2D> m_Texture;
    ComPtr<IDXGIKeyedMutex> m_Mutex;
    HANDLE m_StatusMapping = nullptr;
    const MonitorSplitter::SharedFrameStatus* m_Status = nullptr;
    HANDLE m_RuntimeStatusMapping = nullptr;
    const MonitorSplitter::DriverRuntimeStatus* m_RuntimeStatus = nullptr;
    ULONGLONG m_SourceStartTick = 0;
    mutable bool m_ObservedFreshPublish = false;
    mutable ULONGLONG m_FirstFreshPublishedTick = 0;
    mutable UINT64 m_CopyAttempts = 0;
    mutable UINT64 m_CopySuccesses = 0;
    mutable UINT64 m_CopyFailures = 0;
    mutable HRESULT m_LastCopyResult = S_OK;
    mutable ULONGLONG m_LastCopiedGeneration = 0;
};

struct CompositionVertex
{
    float Position[2];
    float TexCoord[2];
};

struct AppState
{
    HWND Window = nullptr;
    MonitorSplitter::Layout Layout = MonitorSplitter::DefaultLayout();
    MonitorInfo Host;
    std::vector<MonitorInfo> Sources;
    ComPtr<ID3D11Device> Device;
    ComPtr<ID3D11DeviceContext> Context;
    ComPtr<IDXGISwapChain1> SwapChain;
    ComPtr<ID3D11RenderTargetView> RenderTarget;
    ComPtr<ID3D11VertexShader> CompositionVertexShader;
    ComPtr<ID3D11PixelShader> CompositionPixelShader;
    ComPtr<ID3D11InputLayout> CompositionInputLayout;
    ComPtr<ID3D11SamplerState> CompositionSampler;
    ComPtr<ID3D11RasterizerState> CompositionRasterizer;
    ComPtr<ID3D11BlendState> CompositionBlendState;
    ComPtr<ID3D11DepthStencilState> CompositionDepthStencilState;
    ComPtr<ID3D11Texture2D> ScanoutKickTexture;
    d3d11rt::IDirect3DDevice WinRtDevice{ nullptr };
    std::vector<std::unique_ptr<FrameSource>> FrameSources;
    std::wstring HostTarget;
    bool UsingSharedFrames = false;
    bool DirectMode = false;
    bool DirectTargetAcquired = false;
    std::wstring D3DAdapterDescription;
    LUID D3DAdapterLuid = {};
    UINT D3DAdapterVendorId = 0;
    UINT D3DAdapterDeviceId = 0;
    SIZE_T D3DAdapterDedicatedVideoMemory = 0;
    DWORD LastSharedFrameRetryTick = 0;
    UINT64 SharedFrameStartAttempts = 0;
    HRESULT LastSharedFrameStartResult = S_OK;
    std::wstring LastSharedFrameStartError;
    UINT64 SharedFrameAdapterMismatchCount = 0;
    std::wstring LastSharedFrameAdapterMismatch;
    UINT64 SharedFrameCopyAttempts = 0;
    UINT64 SharedFrameCopySuccesses = 0;
    UINT64 SharedFrameCopyFailures = 0;
    HRESULT LastSharedFrameCopyResult = S_OK;
    bool DirectModeAlreadyActive = false;
    bool DirectModeApplyAttempted = false;
    bool DirectModeApplyFallbackPending = false;
    bool DirectModeApplyFallbackUsed = false;
    bool DirectModeApplyAllowed = true;
    HRESULT DirectReadCurrentResult = S_OK;
    HRESULT DirectAcquireEmptyResult = S_OK;
    HRESULT DirectApplyResult = S_OK;
    HRESULT DirectPostApplyReadResult = S_OK;
    HRESULT LastDirectScanoutCreateResult = S_OK;
    DWORD DirectScanoutCreateAttempts = 0;
    UINT64 DirectSetupRetryAttempts = 0;
    bool DirectReadPathAvailable = false;
    INT32 DirectReadSourceWidth = 0;
    INT32 DirectReadSourceHeight = 0;
    INT32 DirectReadTargetWidth = 0;
    INT32 DirectReadTargetHeight = 0;
    INT32 DirectReadPixelFormat = 0;
    INT32 DirectReadScaling = 0;
    bool DirectReadInterlaced = false;
    HRESULT LastDisplayTaskResult = S_OK;
    INT32 LastDisplayTaskPresentStatus = -1;
    INT32 LastDisplayTaskSourceStatus = -1;
    UINT64 LastDisplayTaskPresentId = 0;
    UINT64 DisplayTaskSubmitAttempts = 0;
    UINT64 DisplayTaskSuccesses = 0;
    UINT64 DisplayTaskFailures = 0;
    UINT64 DirectKeepaliveFrames = 0;
    UINT64 DirectPollingCopyFrames = 0;
    DWORD LastDirectSubmitTick = 0;
    UINT64 DirectInLoopRecoveries = 0;
    HRESULT LastDirectRecoveryResult = S_OK;
    UINT64 DirectWarmupFramesPresented = 0;
    UINT64 ScanoutKickFailures = 0;
    HRESULT LastScanoutKickResult = S_OK;
    HANDLE HostRunningMutex = nullptr;
    HANDLE HostStopEvent = nullptr;
    std::wstring HostMode = L"starting";
    std::wstring HostMessage = L"starting";
    UINT64 PresentedFrames = 0;
    HRESULT LastPresentResult = S_OK;
    DWORD LastHostStatusTick = 0;
    UINT64 InputClickAttempts = 0;
    UINT64 InputClicksForwarded = 0;
    UINT64 InputWheelAttempts = 0;
    UINT64 InputWheelsForwarded = 0;
    UINT64 InputMapFailures = 0;
    UINT64 InputSendFailures = 0;
    DWORD LastInputError = ERROR_SUCCESS;
    displaycore::DisplayManager DirectManager{ nullptr };
    displaycore::DisplayTarget DirectTarget{ nullptr };
    displaycore::DisplayDevice DirectDevice{ nullptr };
    displaycore::DisplaySource DirectSource{ nullptr };
    displaycore::DisplayTaskPool DirectTaskPool{ nullptr };
    displaycore::DisplayFence DirectFence{ nullptr };
    INT32 DirectModeSourceWidth = 0;
    INT32 DirectModeSourceHeight = 0;
    INT32 DirectModeTargetWidth = 0;
    INT32 DirectModeTargetHeight = 0;
    UINT32 DirectModeRefreshNumerator = 0;
    UINT32 DirectModeRefreshDenominator = 0;
    INT32 DirectModePixelFormat = 0;
    bool DirectModeInterlaced = false;
    std::array<displaycore::DisplaySurface, 2> DirectPrimaries{ nullptr, nullptr };
    std::array<displaycore::DisplayScanout, 2> DirectScanouts{ nullptr, nullptr };
    std::array<ComPtr<ID3D11Texture2D>, 2> DirectTextures;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> DirectRenderTargets;
    ComPtr<ID3D11Fence> DirectD3DFence;
    UINT64 DirectFenceValue = 0;
    size_t DirectSurfaceIndex = 0;
    DWORD DirectWarmupFramesRemaining = 0;
    std::array<std::vector<ULONGLONG>, 2> LastDirectSourceGenerations;
    std::array<POINT, 2> LastDirectCursorClient = {};
    std::array<bool, 2> LastDirectCursorVisible{ false, false };
    std::array<bool, 2> DirectPrimaryCleared{ false, false };
    mutable std::mutex D3DLock;
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

std::vector<MonitorInfo> g_monitors;
AppState g_state;
constexpr wchar_t kDeviceRunningMutexName[] = L"Local\\MonitorSplitter.Running";
constexpr wchar_t kHostRunningMutexName[] = L"Local\\MonitorSplitter.HostRunning";
constexpr wchar_t kHostStopEventName[] = L"Local\\MonitorSplitter.HostStop";
constexpr DWORD kDirectKeepaliveIntervalMs = 1000;
constexpr DWORD kDirectPollingCopyIntervalMs = 16;

int Width(const RECT& rect)
{
    return rect.right - rect.left;
}

int Height(const RECT& rect)
{
    return rect.bottom - rect.top;
}

bool SameRect(const RECT& left, const RECT& right)
{
    return left.left == right.left &&
           left.top == right.top &&
           left.right == right.right &&
           left.bottom == right.bottom;
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

HANDLE CreateLocalMutex(PCWSTR name, BOOL initialOwner)
{
    return CreateMutexW(LocalControlObjectSecurity(), initialOwner, name);
}

HANDLE CreateLocalEvent(PCWSTR name, BOOL manualReset, BOOL initialState)
{
    return CreateEventW(LocalControlObjectSecurity(), manualReset, initialState, name);
}

LONG NormalizeAbsoluteMouseCoordinate(LONG value, LONG origin, LONG size)
{
    if (size <= 1)
    {
        return 0;
    }

    const long long normalized = (static_cast<long long>(value - origin) * 65535) / (size - 1);
    return static_cast<LONG>(std::clamp<long long>(normalized, 0, 65535));
}

std::wstring HResultMessage(HRESULT hr)
{
    wchar_t* message = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        hr,
        0,
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    std::wstringstream stream;
    stream << L"0x" << std::hex << static_cast<unsigned long>(hr);
    if (message != nullptr)
    {
        stream << L": " << message;
        LocalFree(message);
    }

    return stream.str();
}

std::wstring HResultCode(HRESULT hr)
{
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"0x%08lx", static_cast<unsigned long>(hr));
    return buffer;
}

std::wstring ToWString(const winrt::hstring& value)
{
    return std::wstring(value.c_str(), value.size());
}

LUID LuidFromDisplayAdapterId(winrt::Windows::Graphics::DisplayAdapterId id)
{
    LUID luid = {};
    luid.LowPart = id.LowPart;
    luid.HighPart = id.HighPart;
    return luid;
}

bool SameLuid(const LUID& left, const LUID& right)
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

std::wstring FormatLuid(const LUID& luid)
{
    std::wstringstream stream;
    stream << luid.HighPart << L":" << static_cast<unsigned long>(luid.LowPart);
    return stream.str();
}

std::wstring HexValue(UINT_PTR value)
{
    std::wstringstream stream;
    stream << L"0x" << std::hex << value;
    return stream.str();
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

std::wstring BoundedWideString(const wchar_t* value, size_t capacity)
{
    if (value == nullptr || capacity == 0)
    {
        return {};
    }

    size_t length = 0;
    while (length < capacity && value[length] != L'\0')
    {
        length++;
    }
    return std::wstring(value, length);
}

void AppendComponentJson(std::wstringstream& status, const wchar_t* componentName)
{
    status << L"{\"name\":\"" << JsonEscape(componentName) << L"\"";
    status << L",\"productVersion\":\"" << JsonEscape(MonitorSplitter::kProductVersionWide) << L"\"";
    status << L",\"buildTag\":\"" << JsonEscape(MonitorSplitter::kBuildTagWide) << L"\"";
    status << L",\"pid\":" << GetCurrentProcessId();
    status << L"}";
}

bool ReadDriverRuntimeStatus(size_t connectorIndex, MonitorSplitter::DriverRuntimeStatus& runtime)
{
    HANDLE mapping = OpenFileMappingW(
        FILE_MAP_READ,
        FALSE,
        MonitorSplitter::DriverRuntimeStatusName(connectorIndex).c_str());
    if (mapping == nullptr)
    {
        return false;
    }

    const auto view = static_cast<const MonitorSplitter::DriverRuntimeStatus*>(MapViewOfFile(
        mapping,
        FILE_MAP_READ,
        0,
        0,
        sizeof(MonitorSplitter::DriverRuntimeStatus)));
    if (view == nullptr)
    {
        CloseHandle(mapping);
        return false;
    }

    runtime = *view;
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return true;
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
    const auto text = ToUtf8(message + L"\n");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == text.size();
}

std::wstring ReadTextFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    ReadFile(file, &buffer[0], size, &read, nullptr);
    CloseHandle(file);
    if (read == 0)
    {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, buffer.data(), static_cast<int>(read), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buffer.data(), static_cast<int>(read), &result[0], required);
    return result;
}

std::wstring GetConfigDirectory()
{
    return MonitorSplitter::Control::ConfigDirectory();
}

std::wstring GetLayoutPath()
{
    return GetConfigDirectory() + L"\\layout.txt";
}

std::wstring GetActiveLayoutPath()
{
    return GetConfigDirectory() + L"\\active-layout.txt";
}

std::wstring GetHostStatusPath()
{
    return GetConfigDirectory() + L"\\host-status.json";
}

std::wstring GetHostTargetPath()
{
    return GetConfigDirectory() + L"\\host-target.txt";
}

std::wstring GetDirectTargetPath()
{
    return GetConfigDirectory() + L"\\direct-target.txt";
}

bool IsDeviceHolderRunning()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kDeviceRunningMutexName);
    if (mutex == nullptr)
    {
        return false;
    }

    CloseHandle(mutex);
    return true;
}

MonitorSplitter::Layout ParseLayoutFileOrThrow(const std::wstring& path, const wchar_t* label)
{
    MonitorSplitter::Layout layout;
    std::wstring error;
    const auto spec = MonitorSplitter::Trim(ReadTextFile(path));
    if (spec.empty())
    {
        throw winrt::hresult_error(E_FAIL, (std::wstring(label) + L" layout file is missing or empty: " + path).c_str());
    }

    if (!MonitorSplitter::ParseLayoutSpec(spec, layout, &error))
    {
        throw winrt::hresult_error(E_INVALIDARG, (std::wstring(label) + L" layout file is invalid: " + error).c_str());
    }

    return layout;
}

MonitorSplitter::Layout LoadLayout(bool requireActiveLayout)
{
    if (requireActiveLayout)
    {
        return ParseLayoutFileOrThrow(GetActiveLayoutPath(), L"active");
    }

    if (IsDeviceHolderRunning())
    {
        try
        {
            return ParseLayoutFileOrThrow(GetActiveLayoutPath(), L"active");
        }
        catch (...)
        {
            if (requireActiveLayout)
            {
                throw;
            }
        }
    }

    return ParseLayoutFileOrThrow(GetLayoutPath(), L"saved");
}

std::wstring LoadHostTarget()
{
    return MonitorSplitter::Trim(ReadTextFile(GetHostTargetPath()));
}

std::wstring LoadDirectTarget()
{
    return MonitorSplitter::Trim(ReadTextFile(GetDirectTargetPath()));
}

std::wstring LoadDirectTargetPreference()
{
    std::wstring preference = LoadDirectTarget();
    if (preference.empty())
    {
        preference = LoadHostTarget();
    }

    return preference;
}

void AppendMonitorInfoJson(std::wstringstream& status, const MonitorInfo& monitor)
{
    status << L"{\"device\":\"" << JsonEscape(monitor.DeviceName) << L"\"";
    status << L",\"adapterString\":\"" << JsonEscape(monitor.AdapterString) << L"\"";
    status << L",\"adapterId\":\"" << JsonEscape(monitor.AdapterId) << L"\"";
    status << L",\"monitorString\":\"" << JsonEscape(monitor.MonitorString) << L"\"";
    status << L",\"monitorId\":\"" << JsonEscape(monitor.MonitorId) << L"\"";
    status << L",\"x\":" << monitor.Rect.left;
    status << L",\"y\":" << monitor.Rect.top;
    status << L",\"width\":" << Width(monitor.Rect);
    status << L",\"height\":" << Height(monitor.Rect);
    status << L",\"monitorSplitter\":" << (monitor.IsMonitorSplitter ? L"true" : L"false");
    status << L"}";
}

void AppendWindowStateJson(std::wstringstream& status, const AppState& state)
{
    RECT windowRect = {};
    const bool hasWindow = state.Window != nullptr;
    const bool hasWindowRect = hasWindow && GetWindowRect(state.Window, &windowRect) != FALSE;
    const LONG_PTR exStyle = hasWindow ? GetWindowLongPtrW(state.Window, GWL_EXSTYLE) : 0;
    const bool appWindow = (exStyle & static_cast<LONG_PTR>(WS_EX_APPWINDOW)) != 0;
    const bool noActivate = (exStyle & static_cast<LONG_PTR>(WS_EX_NOACTIVATE)) != 0;
    const bool toolWindow = (exStyle & static_cast<LONG_PTR>(WS_EX_TOOLWINDOW)) != 0;
    const bool topmost = (exStyle & static_cast<LONG_PTR>(WS_EX_TOPMOST)) != 0;

    status << L"{\"hwnd\":\"" << JsonEscape(HexValue(reinterpret_cast<UINT_PTR>(state.Window))) << L"\"";
    status << L",\"visible\":" << (hasWindow && IsWindowVisible(state.Window) ? L"true" : L"false");
    status << L",\"exStyle\":\"" << JsonEscape(HexValue(static_cast<UINT_PTR>(exStyle))) << L"\"";
    status << L",\"topmost\":" << (topmost ? L"true" : L"false");
    status << L",\"toolWindow\":" << (toolWindow ? L"true" : L"false");
    status << L",\"appWindow\":" << (appWindow ? L"true" : L"false");
    status << L",\"noActivate\":" << (noActivate ? L"true" : L"false");
    status << L",\"hostRectMatched\":" << (hasWindowRect && SameRect(windowRect, state.Host.Rect) ? L"true" : L"false");
    status << L",\"x\":" << (hasWindowRect ? windowRect.left : 0);
    status << L",\"y\":" << (hasWindowRect ? windowRect.top : 0);
    status << L",\"width\":" << (hasWindowRect ? Width(windowRect) : 0);
    status << L",\"height\":" << (hasWindowRect ? Height(windowRect) : 0);
    status << L"}";
}

void AppendD3DAdapterJson(std::wstringstream& status, const AppState& state)
{
    status << L"{\"description\":\"" << JsonEscape(state.D3DAdapterDescription) << L"\"";
    status << L",\"luidLow\":" << static_cast<unsigned long>(state.D3DAdapterLuid.LowPart);
    status << L",\"luidHigh\":" << state.D3DAdapterLuid.HighPart;
    status << L",\"vendorId\":" << state.D3DAdapterVendorId;
    status << L",\"deviceId\":" << state.D3DAdapterDeviceId;
    status << L",\"dedicatedVideoMemory\":" << static_cast<unsigned long long>(state.D3DAdapterDedicatedVideoMemory);
    status << L"}";
}

void AppendDirectStatusJson(std::wstringstream& status, const AppState& state)
{
    const auto readySurfaceCount = std::count_if(
        state.DirectTextures.begin(),
        state.DirectTextures.end(),
        [](const auto& texture) { return texture != nullptr; });
    const auto readyScanoutCount = std::count_if(
        state.DirectScanouts.begin(),
        state.DirectScanouts.end(),
        [](const auto& scanout) { return scanout != nullptr; });

    status << L"{\"enabled\":" << (state.DirectMode ? L"true" : L"false");
    status << L",\"targetSelected\":" << (state.DirectTarget != nullptr ? L"true" : L"false");
    status << L",\"targetAcquired\":" << (state.DirectTargetAcquired ? L"true" : L"false");
    status << L",\"deviceCreated\":" << (state.DirectDevice != nullptr ? L"true" : L"false");
    status << L",\"sourceCreated\":" << (state.DirectSource != nullptr ? L"true" : L"false");
    status << L",\"taskPoolCreated\":" << (state.DirectTaskPool != nullptr ? L"true" : L"false");
    status << L",\"fenceReady\":" << (state.DirectFence != nullptr && state.DirectD3DFence != nullptr ? L"true" : L"false");
    status << L",\"modeAlreadyActive\":" << (state.DirectModeAlreadyActive ? L"true" : L"false");
    status << L",\"modeApplyAttempted\":" << (state.DirectModeApplyAttempted ? L"true" : L"false");
    status << L",\"modeApplyFallbackPending\":" << (state.DirectModeApplyFallbackPending ? L"true" : L"false");
    status << L",\"modeApplyFallbackUsed\":" << (state.DirectModeApplyFallbackUsed ? L"true" : L"false");
    status << L",\"readCurrentResult\":\"" << HResultCode(state.DirectReadCurrentResult) << L"\"";
    status << L",\"acquireEmptyResult\":\"" << HResultCode(state.DirectAcquireEmptyResult) << L"\"";
    status << L",\"modeApplyResult\":\"" << HResultCode(state.DirectApplyResult) << L"\"";
    status << L",\"postApplyReadResult\":\"" << HResultCode(state.DirectPostApplyReadResult) << L"\"";
    status << L",\"scanoutCreateAttempts\":" << state.DirectScanoutCreateAttempts;
    status << L",\"directSetupRetryAttempts\":" << state.DirectSetupRetryAttempts;
    status << L",\"lastScanoutCreateResult\":\"" << HResultCode(state.LastDirectScanoutCreateResult) << L"\"";
    status << L",\"readPath\":{";
    status << L"\"available\":" << (state.DirectReadPathAvailable ? L"true" : L"false");
    status << L",\"sourceWidth\":" << state.DirectReadSourceWidth;
    status << L",\"sourceHeight\":" << state.DirectReadSourceHeight;
    status << L",\"targetWidth\":" << state.DirectReadTargetWidth;
    status << L",\"targetHeight\":" << state.DirectReadTargetHeight;
    status << L",\"pixelFormat\":" << state.DirectReadPixelFormat;
    status << L",\"scaling\":" << state.DirectReadScaling;
    status << L",\"interlaced\":" << (state.DirectReadInterlaced ? L"true" : L"false");
    status << L"}";
    status << L",\"surfaceCount\":" << readySurfaceCount;
    status << L",\"scanoutCount\":" << readyScanoutCount;
    status << L",\"surfaceMode\":\"primary-alias\"";
    status << L",\"surfaceIndex\":" << state.DirectSurfaceIndex;
    status << L",\"fenceValue\":" << state.DirectFenceValue;
    status << L",\"displayTaskSubmitAttempts\":" << state.DisplayTaskSubmitAttempts;
    status << L",\"displayTaskSuccesses\":" << state.DisplayTaskSuccesses;
    status << L",\"displayTaskFailures\":" << state.DisplayTaskFailures;
    status << L",\"keepaliveFrames\":" << state.DirectKeepaliveFrames;
    status << L",\"pollingCopyFrames\":" << state.DirectPollingCopyFrames;
    status << L",\"lastSubmitTick\":" << state.LastDirectSubmitTick;
    status << L",\"keepaliveIntervalMs\":" << kDirectKeepaliveIntervalMs;
    status << L",\"inLoopRecoveries\":" << state.DirectInLoopRecoveries;
    status << L",\"lastRecoveryHresult\":\"" << HResultCode(state.LastDirectRecoveryResult) << L"\"";
    status << L",\"warmupFramesPresented\":" << state.DirectWarmupFramesPresented;
    status << L",\"scanoutKickFailures\":" << state.ScanoutKickFailures;
    status << L",\"lastScanoutKickResult\":\"" << HResultCode(state.LastScanoutKickResult) << L"\"";
    const double refreshHz = state.DirectModeRefreshDenominator == 0
        ? 0.0
        : static_cast<double>(state.DirectModeRefreshNumerator) /
            static_cast<double>(state.DirectModeRefreshDenominator);
    status << L",\"appliedMode\":{";
    status << L"\"sourceWidth\":" << state.DirectModeSourceWidth;
    status << L",\"sourceHeight\":" << state.DirectModeSourceHeight;
    status << L",\"targetWidth\":" << state.DirectModeTargetWidth;
    status << L",\"targetHeight\":" << state.DirectModeTargetHeight;
    status << L",\"refreshNumerator\":" << state.DirectModeRefreshNumerator;
    status << L",\"refreshDenominator\":" << state.DirectModeRefreshDenominator;
    status << L",\"refreshHz\":" << refreshHz;
    status << L",\"pixelFormat\":" << state.DirectModePixelFormat;
    status << L",\"interlaced\":" << (state.DirectModeInterlaced ? L"true" : L"false");
    status << L"}";
    status << L"}";
}

void WriteHostStatus(AppState& state, const std::wstring& mode, const std::wstring& message, bool running = true)
{
    state.HostMode = mode;
    state.HostMessage = message;
    state.LastHostStatusTick = GetTickCount();

    const auto directory = GetConfigDirectory();
    CreateDirectoryW(directory.c_str(), nullptr);

    size_t healthyFrameSourceCount = 0;
    size_t publishingFrameSourceCount = 0;
    for (const auto& frameSource : state.FrameSources)
    {
        if (frameSource != nullptr && frameSource->IsHealthy())
        {
            healthyFrameSourceCount++;
        }
        if (frameSource != nullptr && frameSource->FrameGeneration() > 0)
        {
            publishingFrameSourceCount++;
        }
    }

    std::wstringstream status;
    status << L"{\"running\":" << (running ? L"true" : L"false");
    status << L",\"pid\":" << GetCurrentProcessId();
    status << L",\"component\":";
    AppendComponentJson(status, L"MonitorSplitterHost");
    status << L",\"mode\":\"" << JsonEscape(mode) << L"\"";
    status << L",\"layout\":\"" << JsonEscape(MonitorSplitter::SerializeLayout(state.Layout)) << L"\"";
    status << L",\"hostTarget\":\"" << JsonEscape(state.HostTarget) << L"\"";
    status << L",\"hostTargetConfigured\":" << (!state.HostTarget.empty() ? L"true" : L"false");
    status << L",\"hostDevice\":\"" << JsonEscape(state.Host.DeviceName) << L"\"";
    status << L",\"hostX\":" << state.Host.Rect.left;
    status << L",\"hostY\":" << state.Host.Rect.top;
    status << L",\"hostWidth\":" << Width(state.Host.Rect);
    status << L",\"hostHeight\":" << Height(state.Host.Rect);
    status << L",\"sourceCount\":" << state.FrameSources.size();
    status << L",\"selectedSourceCount\":" << state.Sources.size();
    status << L",\"expectedSourceCount\":" << state.Layout.Monitors.size();
    status << L",\"healthyFrameSourceCount\":" << healthyFrameSourceCount;
    status << L",\"publishingFrameSourceCount\":" << publishingFrameSourceCount;
    status << L",\"usingSharedFrames\":" << (state.UsingSharedFrames ? L"true" : L"false");
    status << L",\"sharedFrameStartAttempts\":" << state.SharedFrameStartAttempts;
    status << L",\"lastSharedFrameStartResult\":\"" << HResultCode(state.LastSharedFrameStartResult) << L"\"";
    status << L",\"lastSharedFrameStartError\":\"" << JsonEscape(state.LastSharedFrameStartError) << L"\"";
    status << L",\"sharedFrameAdapterMismatchCount\":" << state.SharedFrameAdapterMismatchCount;
    status << L",\"lastSharedFrameAdapterMismatch\":\"" << JsonEscape(state.LastSharedFrameAdapterMismatch) << L"\"";
    status << L",\"sharedFrameCopyAttempts\":" << state.SharedFrameCopyAttempts;
    status << L",\"sharedFrameCopySuccesses\":" << state.SharedFrameCopySuccesses;
    status << L",\"sharedFrameCopyFailures\":" << state.SharedFrameCopyFailures;
    status << L",\"lastSharedFrameCopyResult\":\"" << HResultCode(state.LastSharedFrameCopyResult) << L"\"";
    status << L",\"lastDisplayTaskResult\":\"" << HResultCode(state.LastDisplayTaskResult) << L"\"";
    status << L",\"lastDisplayTaskPresentStatus\":" << state.LastDisplayTaskPresentStatus;
    status << L",\"lastDisplayTaskSourceStatus\":" << state.LastDisplayTaskSourceStatus;
    status << L",\"lastDisplayTaskPresentId\":" << state.LastDisplayTaskPresentId;
    status << L",\"presentedFrames\":" << state.PresentedFrames;
    status << L",\"lastPresentResult\":\"" << HResultCode(state.LastPresentResult) << L"\"";
    status << L",\"updatedTick\":" << state.LastHostStatusTick;
    status << L",\"inputClickAttempts\":" << state.InputClickAttempts;
    status << L",\"inputClicksForwarded\":" << state.InputClicksForwarded;
    status << L",\"inputWheelAttempts\":" << state.InputWheelAttempts;
    status << L",\"inputWheelsForwarded\":" << state.InputWheelsForwarded;
    status << L",\"inputMapFailures\":" << state.InputMapFailures;
    status << L",\"inputSendFailures\":" << state.InputSendFailures;
    status << L",\"lastInputError\":" << state.LastInputError;
    status << L",\"d3dAdapter\":";
    AppendD3DAdapterJson(status, state);
    status << L",\"direct\":";
    AppendDirectStatusJson(status, state);
    status << L",\"host\":";
    AppendMonitorInfoJson(status, state.Host);
    status << L",\"window\":";
    AppendWindowStateJson(status, state);
    status << L",\"sources\":[";
    for (size_t index = 0; index < state.Sources.size(); index++)
    {
        if (index != 0)
        {
            status << L",";
        }
        AppendMonitorInfoJson(status, state.Sources[index]);
    }
    status << L"]";
    status << L",\"frameSources\":[";
    for (size_t index = 0; index < state.FrameSources.size(); index++)
    {
        if (index != 0)
        {
            status << L",";
        }
        state.FrameSources[index]->AppendDiagnosticsJson(status);
    }
    status << L"]";
    status << L",\"message\":\"" << JsonEscape(message) << L"\"";
    status << L"}";
    WriteTextFile(GetHostStatusPath(), status.str());
}

void MaybeRefreshHostStatus(AppState& state)
{
    const DWORD now = GetTickCount();
    if (state.LastHostStatusTick == 0 || now - state.LastHostStatusTick >= 1000)
    {
        WriteHostStatus(state, state.HostMode, state.HostMessage);
    }
}

void CloseHostRunningMutex(AppState& state)
{
    if (state.HostRunningMutex != nullptr)
    {
        ReleaseMutex(state.HostRunningMutex);
        CloseHandle(state.HostRunningMutex);
        state.HostRunningMutex = nullptr;
    }
}

void CloseHostStopEvent(AppState& state)
{
    if (state.HostStopEvent != nullptr)
    {
        CloseHandle(state.HostStopEvent);
        state.HostStopEvent = nullptr;
    }
}

bool IsHostStopRequested(const AppState& state)
{
    return state.HostStopEvent != nullptr &&
           WaitForSingleObject(state.HostStopEvent, 0) == WAIT_OBJECT_0;
}

std::wstring ToLower(std::wstring value)
{
    for (auto& ch : value)
    {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return value;
}

bool ContainsIgnoreCase(const std::wstring& value, const wchar_t* needle)
{
    return ToLower(value).find(ToLower(needle)) != std::wstring::npos;
}

bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    return ToLower(left) == ToLower(right);
}

bool LooksLikeMonitorSplitterDevice(const std::wstring& deviceName, const std::wstring& deviceString, const std::wstring& deviceId)
{
    return ContainsIgnoreCase(deviceName, L"MonitorSplitter") ||
           ContainsIgnoreCase(deviceString, L"MonitorSplitter") ||
           ContainsIgnoreCase(deviceId, L"MonitorSplitter");
}

template <typename T>
ComPtr<T> GetDxgiInterface(d3d11rt::IDirect3DSurface const& surface)
{
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<T> result;
    winrt::check_hresult(access->GetInterface(__uuidof(T), reinterpret_cast<void**>(result.GetAddressOf())));
    return result;
}

void PopulateDisplayIdentity(MonitorInfo& monitor)
{
    for (DWORD adapterIndex = 0;; adapterIndex++)
    {
        DISPLAY_DEVICEW adapter = {};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0))
        {
            break;
        }

        if (monitor.DeviceName != adapter.DeviceName)
        {
            continue;
        }

        monitor.AdapterString = adapter.DeviceString;
        monitor.AdapterId = adapter.DeviceID;
        monitor.IsMonitorSplitter = monitor.IsMonitorSplitter ||
                                    LooksLikeMonitorSplitterDevice(adapter.DeviceName, adapter.DeviceString, adapter.DeviceID);
        break;
    }

    DISPLAY_DEVICEW child = {};
    child.cb = sizeof(child);
    if (EnumDisplayDevicesW(monitor.DeviceName.c_str(), 0, &child, 0))
    {
        monitor.MonitorString = child.DeviceString;
        monitor.MonitorId = child.DeviceID;
        monitor.IsMonitorSplitter = monitor.IsMonitorSplitter ||
                                    LooksLikeMonitorSplitterDevice(child.DeviceName, child.DeviceString, child.DeviceID);
    }
}

BOOL CALLBACK EnumMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
    {
        return TRUE;
    }

    MonitorInfo monitorInfo;
    monitorInfo.Handle = monitor;
    monitorInfo.Rect = info.rcMonitor;
    monitorInfo.DeviceName = info.szDevice;
    PopulateDisplayIdentity(monitorInfo);
    g_monitors.push_back(monitorInfo);
    return TRUE;
}

bool LooksLikeHost(const MonitorInfo& monitor, const MonitorSplitter::Layout& layout)
{
    return !monitor.IsMonitorSplitter &&
           Width(monitor.Rect) >= static_cast<int>(layout.HostWidth) &&
           Height(monitor.Rect) >= static_cast<int>(layout.Height);
}

bool LooksLikeSplitSource(const MonitorInfo& monitor, const MonitorSplitter::Layout& layout)
{
    const int width = Width(monitor.Rect);
    const int height = Height(monitor.Rect);
    if (height != static_cast<int>(layout.Height))
    {
        return false;
    }

    for (const auto& expected : layout.Monitors)
    {
        if (width == static_cast<int>(expected.Width))
        {
            return true;
        }
    }

    return false;
}

bool PickMonitors(AppState& state)
{
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, 0);
    state.HostTarget = LoadHostTarget();

    auto hostIt = std::find_if(g_monitors.begin(), g_monitors.end(), [&state](const MonitorInfo& monitor) {
        return LooksLikeHost(monitor, state.Layout) &&
               (state.HostTarget.empty() || EqualsIgnoreCase(monitor.DeviceName, state.HostTarget));
    });
    if (hostIt == g_monitors.end())
    {
        return false;
    }

    state.Host = *hostIt;

    std::vector<MonitorInfo> sources;
    std::vector<MonitorInfo> splitterSources;
    for (const auto& monitor : g_monitors)
    {
        if (monitor.Handle != state.Host.Handle && LooksLikeSplitSource(monitor, state.Layout))
        {
            sources.push_back(monitor);
            if (monitor.IsMonitorSplitter)
            {
                splitterSources.push_back(monitor);
            }
        }
    }

    if (splitterSources.size() >= state.Layout.Monitors.size())
    {
        sources = std::move(splitterSources);
    }

    if (sources.size() < state.Layout.Monitors.size())
    {
        return false;
    }

    std::sort(sources.begin(), sources.end(), [](const MonitorInfo& left, const MonitorInfo& right) {
        if (left.Rect.left != right.Rect.left)
        {
            return left.Rect.left < right.Rect.left;
        }
        return left.Rect.top < right.Rect.top;
    });

    state.Sources.clear();
    for (size_t index = 0; index < state.Layout.Monitors.size(); index++)
    {
        state.Sources.push_back(sources[index]);
    }

    return true;
}

bool PickDirectSources(AppState& state)
{
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, 0);
    state.HostTarget = LoadHostTarget();

    state.Host = {};
    state.Host.DeviceName = L"DisplayCore";
    state.Host.MonitorString = L"physical panel removed from desktop";
    state.Host.Rect = {
        0,
        0,
        static_cast<LONG>(state.Layout.HostWidth),
        static_cast<LONG>(state.Layout.Height)
    };

    std::vector<MonitorInfo> sources;
    std::vector<MonitorInfo> splitterSources;
    for (const auto& monitor : g_monitors)
    {
        if (LooksLikeSplitSource(monitor, state.Layout))
        {
            sources.push_back(monitor);
            if (monitor.IsMonitorSplitter)
            {
                splitterSources.push_back(monitor);
            }
        }
    }

    if (splitterSources.size() >= state.Layout.Monitors.size())
    {
        sources = std::move(splitterSources);
    }

    if (sources.size() < state.Layout.Monitors.size())
    {
        return false;
    }

    std::sort(sources.begin(), sources.end(), [](const MonitorInfo& left, const MonitorInfo& right) {
        if (left.Rect.left != right.Rect.left)
        {
            return left.Rect.left < right.Rect.left;
        }
        return left.Rect.top < right.Rect.top;
    });

    state.Sources.clear();
    for (size_t index = 0; index < state.Layout.Monitors.size(); index++)
    {
        state.Sources.push_back(sources[index]);
    }

    return true;
}

void CreateD3DDevice(AppState& state, const LUID* preferredAdapterLuid = nullptr)
{
    static const D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL selectedFeatureLevel = {};
    ComPtr<IDXGIAdapter> preferredAdapter;
    if (preferredAdapterLuid != nullptr)
    {
        ComPtr<IDXGIFactory6> factory;
        winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        winrt::check_hresult(factory->EnumAdapterByLuid(*preferredAdapterLuid, IID_PPV_ARGS(&preferredAdapter)));
    }

    winrt::check_hresult(D3D11CreateDevice(
        preferredAdapter.Get(),
        preferredAdapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &state.Device,
        &selectedFeatureLevel,
        &state.Context));

    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(state.Device.As(&multithread)))
    {
        multithread->SetMultithreadProtected(TRUE);
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(state.Device.As(&dxgiDevice));

    ComPtr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgiDevice->GetAdapter(&adapter));
    DXGI_ADAPTER_DESC adapterDesc = {};
    winrt::check_hresult(adapter->GetDesc(&adapterDesc));
    state.D3DAdapterDescription = adapterDesc.Description;
    state.D3DAdapterLuid = adapterDesc.AdapterLuid;
    state.D3DAdapterVendorId = adapterDesc.VendorId;
    state.D3DAdapterDeviceId = adapterDesc.DeviceId;
    state.D3DAdapterDedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;

    winrt::com_ptr<IInspectable> inspectableDevice;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectableDevice.put()));
    state.WinRtDevice = inspectableDevice.as<d3d11rt::IDirect3DDevice>();
}

void ResetSwapChain(AppState& state)
{
    std::scoped_lock lock(state.D3DLock);
    state.RenderTarget.Reset();
    state.SwapChain.Reset();
}

void CreateSwapChain(AppState& state)
{
    std::scoped_lock lock(state.D3DLock);

    ComPtr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(state.Device.As(&dxgiDevice));

    ComPtr<IDXGIAdapter> adapter;
    winrt::check_hresult(dxgiDevice->GetAdapter(&adapter));

    ComPtr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = Width(state.Host.Rect);
    desc.Height = Height(state.Host.Rect);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SampleDesc.Count = 1;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    winrt::check_hresult(factory->CreateSwapChainForHwnd(
        state.Device.Get(),
        state.Window,
        &desc,
        nullptr,
        nullptr,
        &state.SwapChain));

    factory->MakeWindowAssociation(state.Window, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> backBuffer;
    winrt::check_hresult(state.SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    winrt::check_hresult(state.Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &state.RenderTarget));
}

bool TryStartSharedFrameSources(AppState& state)
{
    std::vector<std::unique_ptr<FrameSource>> frameSources;
    state.SharedFrameStartAttempts++;

    int destX = 0;
    try
    {
        for (size_t index = 0; index < state.Layout.Monitors.size(); index++)
        {
            const auto& mode = state.Layout.Monitors[index];
            auto frameSource = std::make_unique<SharedFrameSource>(&state, index, mode, destX);
            frameSource->Start();
            frameSources.push_back(std::move(frameSource));
            destX += static_cast<int>(mode.Width);
        }
    }
    catch (winrt::hresult_error const& error)
    {
        state.LastSharedFrameStartResult = static_cast<HRESULT>(error.code());
        state.LastSharedFrameStartError = std::wstring(error.message()) + L"\n" + HResultMessage(error.code());
        return false;
    }
    catch (std::exception const&)
    {
        state.LastSharedFrameStartResult = E_FAIL;
        state.LastSharedFrameStartError = L"standard exception while starting shared-frame sources";
        return false;
    }
    catch (...)
    {
        state.LastSharedFrameStartResult = E_FAIL;
        state.LastSharedFrameStartError = L"unknown shared-frame source startup failure";
        return false;
    }

    state.FrameSources = std::move(frameSources);
    state.LastSharedFrameStartResult = S_OK;
    state.LastSharedFrameStartError.clear();
    return true;
}

bool TryPromoteToSharedFrameSources(AppState& state)
{
    if (state.UsingSharedFrames)
    {
        return false;
    }

    const DWORD now = GetTickCount();
    if (state.LastSharedFrameRetryTick != 0 &&
        now - state.LastSharedFrameRetryTick < 1000)
    {
        return false;
    }
    state.LastSharedFrameRetryTick = now;

    if (!TryStartSharedFrameSources(state))
    {
        return false;
    }

    state.UsingSharedFrames = true;
    WriteHostStatus(state, state.DirectMode ? L"direct-shared" : L"shared", L"promoted to driver shared textures");
    return true;
}

bool WaitForSharedFrameSources(AppState& state, DWORD timeoutMs)
{
    const DWORD startTick = GetTickCount();
    DWORD attempt = 0;

    for (;;)
    {
        if (TryStartSharedFrameSources(state))
        {
            state.UsingSharedFrames = true;
            WriteHostStatus(
                state,
                state.DirectMode ? L"direct-shared" : L"shared",
                attempt == 0 ? L"using driver shared textures" : L"using driver shared textures after waiting for producer frames");
            return true;
        }

        if (IsHostStopRequested(state) ||
            GetTickCount() - startTick >= timeoutMs)
        {
            return false;
        }

        attempt++;
        std::wstring message = L"waiting for driver shared textures from the virtual display producer";
        if (!state.LastSharedFrameStartError.empty())
        {
            message += L"\n";
            message += state.LastSharedFrameStartError;
        }
        message += L"\nAttempt ";
        message += std::to_wstring(attempt);
        message += L".";
        WriteHostStatus(state, L"recovering", message);
        Sleep(500);
    }
}

void StartCaptureFrameSources(AppState& state, const wchar_t* message)
{
    if (!capture::GraphicsCaptureSession::IsSupported())
    {
        throw winrt::hresult_error(E_FAIL, L"Windows Graphics Capture is not supported and MonitorSplitter shared frame textures are not available.");
    }

    state.FrameSources.clear();
    int destX = 0;
    for (const auto& source : state.Sources)
    {
        auto frameSource = std::make_unique<CaptureSource>(&state, source, destX);
        frameSource->Start();
        state.FrameSources.push_back(std::move(frameSource));
        destX += Width(source.Rect);
    }

    WriteHostStatus(state, state.DirectMode ? L"direct-capture" : L"capture", message);
}

void StartFrameSources(AppState& state)
{
    state.FrameSources.clear();
    state.UsingSharedFrames = false;
    state.LastSharedFrameRetryTick = 0;
    state.PresentedFrames = 0;
    state.LastPresentResult = S_OK;
    state.LastHostStatusTick = 0;
    state.SharedFrameStartAttempts = 0;
    state.LastSharedFrameStartResult = S_OK;
    state.LastSharedFrameStartError.clear();
    state.SharedFrameCopyAttempts = 0;
    state.SharedFrameCopySuccesses = 0;
    state.SharedFrameCopyFailures = 0;
    state.LastSharedFrameCopyResult = S_OK;

    if (state.DirectMode)
    {
        if (WaitForSharedFrameSources(state, 30 * 1000))
        {
            return;
        }

        std::wstring message = L"driver shared textures are not available.";
        if (!state.LastSharedFrameStartError.empty())
        {
            message += L"\n";
            message += state.LastSharedFrameStartError;
        }
        const HRESULT hr = FAILED(state.LastSharedFrameStartResult) ? state.LastSharedFrameStartResult : E_FAIL;
        throw winrt::hresult_error(hr, message.c_str());
    }

    if (TryStartSharedFrameSources(state))
    {
        state.UsingSharedFrames = true;
        WriteHostStatus(state, state.DirectMode ? L"direct-shared" : L"shared", L"using driver shared textures");
        return;
    }

    StartCaptureFrameSources(state, L"using Windows Graphics Capture fallback");
}

bool FrameSourcesHealthy(const AppState& state)
{
    if (state.FrameSources.empty())
    {
        return false;
    }

    return std::all_of(state.FrameSources.begin(), state.FrameSources.end(), [](const auto& frameSource) {
        return frameSource != nullptr && frameSource->IsHealthy();
    });
}

void DemoteUnhealthySharedFrames(AppState& state)
{
    if (!state.UsingSharedFrames || FrameSourcesHealthy(state))
    {
        return;
    }

    state.FrameSources.clear();
    state.UsingSharedFrames = false;
    state.LastSharedFrameRetryTick = GetTickCount();
    if (state.DirectMode)
    {
        throw winrt::hresult_error(E_FAIL, L"driver shared textures stopped publishing.");
    }
    StartCaptureFrameSources(state, L"driver shared textures stopped publishing; using Windows Graphics Capture fallback");
}

bool TryMapSourcePointToClient(const AppState& state, POINT sourcePoint, POINT& clientPoint)
{
    int destX = 0;
    for (const auto& source : state.Sources)
    {
        if (sourcePoint.x >= source.Rect.left &&
            sourcePoint.x < source.Rect.right &&
            sourcePoint.y >= source.Rect.top &&
            sourcePoint.y < source.Rect.bottom)
        {
            clientPoint.x = destX + (sourcePoint.x - source.Rect.left);
            clientPoint.y = sourcePoint.y - source.Rect.top;
            return true;
        }

        destX += Width(source.Rect);
    }

    return false;
}

bool TryGetCursorClientPoint(const AppState& state, POINT& clientPoint)
{
    CURSORINFO cursorInfo = {};
    cursorInfo.cbSize = sizeof(cursorInfo);
    if (!GetCursorInfo(&cursorInfo) || (cursorInfo.flags & CURSOR_SHOWING) == 0)
    {
        return false;
    }

    return TryMapSourcePointToClient(state, cursorInfo.ptScreenPos, clientPoint);
}

void ClearRenderTargetRect(
    ID3D11DeviceContext1* context,
    ID3D11RenderTargetView* targetView,
    LONG targetWidth,
    LONG targetHeight,
    LONG x,
    LONG y,
    LONG width,
    LONG height,
    const FLOAT color[4])
{
    if (context == nullptr || targetView == nullptr || width <= 0 || height <= 0)
    {
        return;
    }

    D3D11_RECT rect = {};
    rect.left = std::clamp<LONG>(x, 0, targetWidth);
    rect.top = std::clamp<LONG>(y, 0, targetHeight);
    rect.right = std::clamp<LONG>(x + width, 0, targetWidth);
    rect.bottom = std::clamp<LONG>(y + height, 0, targetHeight);
    if (rect.left >= rect.right || rect.top >= rect.bottom)
    {
        return;
    }

    context->ClearView(targetView, color, &rect, 1);
}

void DrawSoftwareCursor(AppState& state, ID3D11RenderTargetView* targetView)
{
    POINT clientPoint = {};
    if (!TryGetCursorClientPoint(state, clientPoint))
    {
        return;
    }

    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(state.Context.As(&context1)))
    {
        return;
    }

    const LONG targetWidth = static_cast<LONG>(state.Layout.HostWidth);
    const LONG targetHeight = static_cast<LONG>(state.Layout.Height);
    static const FLOAT black[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    static const FLOAT white[] = { 1.0f, 1.0f, 1.0f, 1.0f };

    for (LONG row = 0; row < 15; row++)
    {
        const LONG width = std::min<LONG>(row + 2, 11);
        ClearRenderTargetRect(context1.Get(), targetView, targetWidth, targetHeight, clientPoint.x, clientPoint.y + row, width, 1, black);
        if (width > 2)
        {
            ClearRenderTargetRect(context1.Get(), targetView, targetWidth, targetHeight, clientPoint.x + 1, clientPoint.y + row, width - 2, 1, white);
        }
    }
    ClearRenderTargetRect(context1.Get(), targetView, targetWidth, targetHeight, clientPoint.x + 4, clientPoint.y + 12, 8, 3, black);
    ClearRenderTargetRect(context1.Get(), targetView, targetWidth, targetHeight, clientPoint.x + 6, clientPoint.y + 14, 3, 7, black);
    ClearRenderTargetRect(context1.Get(), targetView, targetWidth, targetHeight, clientPoint.x + 7, clientPoint.y + 15, 1, 5, white);
}

HRESULT EnsureCompositionPipeline(AppState& state)
{
    if (state.CompositionVertexShader != nullptr &&
        state.CompositionPixelShader != nullptr &&
        state.CompositionInputLayout != nullptr &&
        state.CompositionSampler != nullptr &&
        state.CompositionRasterizer != nullptr &&
        state.CompositionBlendState != nullptr &&
        state.CompositionDepthStencilState != nullptr)
    {
        return S_OK;
    }

    static const char shaderSource[] = R"(
        struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
        struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
        Texture2D srcTex : register(t0);
        SamplerState srcSampler : register(s0);
        PSIn VSMain(VSIn input)
        {
            PSIn output;
            output.pos = float4(input.pos, 0.0f, 1.0f);
            output.uv = input.uv;
            return output;
        }
        float4 PSMain(PSIn input) : SV_Target
        {
            return srcTex.Sample(srcSampler, input.uv);
        }
    )";

    ComPtr<ID3DBlob> vertexBlob;
    HRESULT hr = D3DCompile(
        shaderSource,
        sizeof(shaderSource) - 1,
        nullptr,
        nullptr,
        nullptr,
        "VSMain",
        "vs_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &vertexBlob,
        nullptr);
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<ID3DBlob> pixelBlob;
    hr = D3DCompile(
        shaderSource,
        sizeof(shaderSource) - 1,
        nullptr,
        nullptr,
        nullptr,
        "PSMain",
        "ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &pixelBlob,
        nullptr);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = state.Device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &state.CompositionVertexShader);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = state.Device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &state.CompositionPixelShader);
    if (FAILED(hr))
    {
        return hr;
    }

    const D3D11_INPUT_ELEMENT_DESC inputElements[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = state.Device->CreateInputLayout(
        inputElements,
        ARRAYSIZE(inputElements),
        vertexBlob->GetBufferPointer(),
        vertexBlob->GetBufferSize(),
        &state.CompositionInputLayout);
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = state.Device->CreateSamplerState(&samplerDesc, &state.CompositionSampler);
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    hr = state.Device->CreateRasterizerState(&rasterizerDesc, &state.CompositionRasterizer);
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = state.Device->CreateBlendState(&blendDesc, &state.CompositionBlendState);
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = FALSE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depthStencilDesc.StencilEnable = FALSE;
    return state.Device->CreateDepthStencilState(&depthStencilDesc, &state.CompositionDepthStencilState);
}

HRESULT DrawTextureToTarget(
    AppState& state,
    ID3D11Texture2D* sourceTexture,
    ID3D11RenderTargetView* targetView,
    LONG targetWidth,
    LONG targetHeight,
    LONG x,
    LONG y,
    LONG width,
    LONG height)
{
    if (sourceTexture == nullptr || targetView == nullptr || width <= 0 || height <= 0)
    {
        return E_INVALIDARG;
    }

    RECT rect = {};
    rect.left = std::clamp<LONG>(x, 0, targetWidth);
    rect.top = std::clamp<LONG>(y, 0, targetHeight);
    rect.right = std::clamp<LONG>(x + width, 0, targetWidth);
    rect.bottom = std::clamp<LONG>(y + height, 0, targetHeight);
    if (rect.left >= rect.right || rect.top >= rect.bottom)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = EnsureCompositionPipeline(state);
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<ID3D11ShaderResourceView> sourceView;
    hr = state.Device->CreateShaderResourceView(sourceTexture, nullptr, &sourceView);
    if (FAILED(hr))
    {
        return hr;
    }

    const float x0 = (static_cast<float>(rect.left) / static_cast<float>(targetWidth)) * 2.0f - 1.0f;
    const float x1 = (static_cast<float>(rect.right) / static_cast<float>(targetWidth)) * 2.0f - 1.0f;
    const float y0 = 1.0f - (static_cast<float>(rect.top) / static_cast<float>(targetHeight)) * 2.0f;
    const float y1 = 1.0f - (static_cast<float>(rect.bottom) / static_cast<float>(targetHeight)) * 2.0f;
    const CompositionVertex vertices[] =
    {
        { { x0, y0 }, { 0.0f, 0.0f } },
        { { x1, y0 }, { 1.0f, 0.0f } },
        { { x0, y1 }, { 0.0f, 1.0f } },
        { { x0, y1 }, { 0.0f, 1.0f } },
        { { x1, y0 }, { 1.0f, 0.0f } },
        { { x1, y1 }, { 1.0f, 1.0f } },
    };

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA bufferData = {};
    bufferData.pSysMem = vertices;

    ComPtr<ID3D11Buffer> vertexBuffer;
    hr = state.Device->CreateBuffer(&bufferDesc, &bufferData, &vertexBuffer);
    if (FAILED(hr))
    {
        return hr;
    }

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(targetWidth);
    viewport.Height = static_cast<float>(targetHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    const UINT stride = sizeof(CompositionVertex);
    const UINT offset = 0;
    const FLOAT blendFactor[4] = {};
    state.Context->OMSetRenderTargets(1, &targetView, nullptr);
    state.Context->OMSetBlendState(state.CompositionBlendState.Get(), blendFactor, 0xffffffff);
    state.Context->OMSetDepthStencilState(state.CompositionDepthStencilState.Get(), 0);
    state.Context->RSSetState(state.CompositionRasterizer.Get());
    state.Context->RSSetViewports(1, &viewport);
    state.Context->IASetInputLayout(state.CompositionInputLayout.Get());
    state.Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    state.Context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
    state.Context->VSSetShader(state.CompositionVertexShader.Get(), nullptr, 0);
    state.Context->PSSetShader(state.CompositionPixelShader.Get(), nullptr, 0);
    state.Context->PSSetSamplers(0, 1, state.CompositionSampler.GetAddressOf());
    state.Context->PSSetShaderResources(0, 1, sourceView.GetAddressOf());
    state.Context->Draw(ARRAYSIZE(vertices), 0);

    ID3D11ShaderResourceView* nullView = nullptr;
    state.Context->PSSetShaderResources(0, 1, &nullView);
    return S_OK;
}

HRESULT EnsureScanoutKickTexture(AppState& state)
{
    if (state.ScanoutKickTexture != nullptr)
    {
        return S_OK;
    }

    const UINT32 blackPixel = 0xff000000u;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = &blackPixel;
    data.SysMemPitch = sizeof(blackPixel);

    return state.Device->CreateTexture2D(&desc, &data, &state.ScanoutKickTexture);
}

bool DrawScanoutKick(AppState& state, ID3D11RenderTargetView* targetView)
{
    // DisplayCore primary-alias scanout can accept tasks but keep showing black unless each composed
    // frame includes a tiny post-copy draw. Keep this invisible and preserve the product command path.
    const LONG targetWidth = static_cast<LONG>(state.Layout.HostWidth);
    const LONG targetHeight = static_cast<LONG>(state.Layout.Height);
    if (targetWidth <= 0 || targetHeight <= 0 || targetView == nullptr)
    {
        state.LastScanoutKickResult = E_INVALIDARG;
        state.ScanoutKickFailures++;
        return false;
    }

    HRESULT hr = EnsureScanoutKickTexture(state);
    if (FAILED(hr))
    {
        state.LastScanoutKickResult = hr;
        state.ScanoutKickFailures++;
        return false;
    }

    hr = DrawTextureToTarget(
        state,
        state.ScanoutKickTexture.Get(),
        targetView,
        targetWidth,
        targetHeight,
        targetWidth - 1,
        targetHeight - 1,
        1,
        1);
    state.LastScanoutKickResult = hr;
    if (FAILED(hr))
    {
        state.ScanoutKickFailures++;
        return false;
    }
    return true;
}

void RenderFrame(AppState& state)
{
    std::scoped_lock lock(state.D3DLock);

    if (state.SwapChain == nullptr || state.RenderTarget == nullptr)
    {
        return;
    }

    if (state.PresentedFrames == 0)
    {
        static const float black[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        state.Context->ClearRenderTargetView(state.RenderTarget.Get(), black);
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(state.SwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
    {
        return;
    }

    for (const auto& frameSource : state.FrameSources)
    {
        frameSource->CopyLatestToBackBuffer(backBuffer.Get(), state.RenderTarget.Get());
    }

    const HRESULT presentResult = state.SwapChain->Present(1, 0);
    state.LastPresentResult = presentResult;
    if (SUCCEEDED(presentResult))
    {
        state.PresentedFrames++;
    }
}

bool TryMapClientPointToSource(const AppState& state, POINT clientPoint, POINT& sourcePoint)
{
    if (clientPoint.x < 0 || clientPoint.y < 0 || clientPoint.y >= static_cast<LONG>(state.Layout.Height))
    {
        return false;
    }

    int destX = 0;
    for (const auto& source : state.Sources)
    {
        const int sourceWidth = Width(source.Rect);
        const int sourceHeight = Height(source.Rect);
        if (clientPoint.x >= destX &&
            clientPoint.x < destX + sourceWidth &&
            clientPoint.y < sourceHeight)
        {
            sourcePoint.x = source.Rect.left + (clientPoint.x - destX);
            sourcePoint.y = source.Rect.top + clientPoint.y;
            return true;
        }

        destX += sourceWidth;
    }

    return false;
}

bool SendAbsoluteMouseMove(POINT point)
{
    const LONG virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = NormalizeAbsoluteMouseCoordinate(point.x, virtualLeft, virtualWidth);
    input.mi.dy = NormalizeAbsoluteMouseCoordinate(point.y, virtualTop, virtualHeight);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    return SendInput(1, &input, sizeof(input)) == 1;
}

bool SendMouseButtonClickAt(POINT point, DWORD downFlag, DWORD upFlag, DWORD mouseData = 0)
{
    if (!SendAbsoluteMouseMove(point))
    {
        return false;
    }

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = downFlag;
    inputs[0].mi.mouseData = mouseData;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = upFlag;
    inputs[1].mi.mouseData = mouseData;

    return SendInput(2, inputs, sizeof(INPUT)) == 2;
}

bool SendMouseWheelAt(POINT point, int delta, bool horizontal)
{
    if (!SendAbsoluteMouseMove(point))
    {
        return false;
    }

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);

    return SendInput(1, &input, sizeof(input)) == 1;
}

bool BridgeClientClick(DWORD downFlag, DWORD upFlag, DWORD mouseData, LPARAM lParam)
{
    g_state.InputClickAttempts++;

    POINT clientPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    POINT sourcePoint = {};
    if (!TryMapClientPointToSource(g_state, clientPoint, sourcePoint))
    {
        g_state.InputMapFailures++;
        return false;
    }

    if (!SendMouseButtonClickAt(sourcePoint, downFlag, upFlag, mouseData))
    {
        g_state.InputSendFailures++;
        g_state.LastInputError = GetLastError();
        return false;
    }

    g_state.InputClicksForwarded++;
    g_state.LastInputError = ERROR_SUCCESS;
    return true;
}

bool BridgeWheel(HWND window, bool horizontal, WPARAM wParam, LPARAM lParam)
{
    g_state.InputWheelAttempts++;

    POINT screenPoint = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    POINT clientPoint = screenPoint;
    ScreenToClient(window, &clientPoint);

    POINT sourcePoint = {};
    if (!TryMapClientPointToSource(g_state, clientPoint, sourcePoint))
    {
        g_state.InputMapFailures++;
        return false;
    }

    if (!SendMouseWheelAt(sourcePoint, GET_WHEEL_DELTA_WPARAM(wParam), horizontal))
    {
        g_state.InputSendFailures++;
        g_state.LastInputError = GetLastError();
        return false;
    }

    g_state.InputWheelsForwarded++;
    g_state.LastInputError = ERROR_SUCCESS;
    return true;
}

void Configure(AppState& state)
{
    state.FrameSources.clear();
    state.UsingSharedFrames = false;
    state.LastSharedFrameRetryTick = 0;
    ResetSwapChain(state);

    if (!PickMonitors(state))
    {
        const auto message = L"Could not find host monitor and virtual monitors for layout " + MonitorSplitter::SerializeLayout(state.Layout);
        throw winrt::hresult_error(
            E_FAIL,
            message.c_str());
    }

    SetWindowPos(
        state.Window,
        HWND_TOPMOST,
        state.Host.Rect.left,
        state.Host.Rect.top,
        Width(state.Host.Rect),
        Height(state.Host.Rect),
        SWP_SHOWWINDOW | SWP_NOACTIVATE);

    CreateSwapChain(state);
    StartFrameSources(state);
}

bool DirectTargetMatchesPreference(const displaycore::DisplayTarget& target, const std::wstring& preference)
{
    if (preference.empty())
    {
        return true;
    }

    const auto adapterId = target.Adapter().Id();
    std::wstringstream adapterKey;
    adapterKey << L"adapter:" << static_cast<unsigned long>(adapterId.LowPart)
               << L":" << adapterId.HighPart
               << L":" << target.AdapterRelativeId();

    std::wstringstream stream(preference);
    std::wstring item;
    while (std::getline(stream, item))
    {
        item = MonitorSplitter::Trim(item);
        if (item.empty())
        {
            continue;
        }
        if (MonitorSplitter::Control::IsSelectorMetadataLine(item))
        {
            continue;
        }

        if (EqualsIgnoreCase(ToWString(target.DeviceInterfacePath()), item) ||
            EqualsIgnoreCase(ToWString(target.StableMonitorId()), item) ||
            EqualsIgnoreCase(adapterKey.str(), item))
        {
            return true;
        }
    }

    return false;
}

displaycore::DisplayTarget FindDirectTarget(AppState& state, const displaycore::DisplayManager& manager)
{
    std::vector<displaycore::DisplayTarget> connectedTargets;
    const auto directTargetPreference = LoadDirectTargetPreference();
    state.HostTarget = directTargetPreference;
    for (const auto& target : manager.GetCurrentTargets())
    {
        if (!target.IsConnected())
        {
            continue;
        }

        connectedTargets.push_back(target);
    }

    auto findMatchingTarget = [&](const std::vector<displaycore::DisplayTarget>& candidates) -> displaycore::DisplayTarget
    {
        for (const auto& candidate : candidates)
        {
            if (DirectTargetMatchesPreference(candidate, directTargetPreference))
            {
                return candidate;
            }
        }

        return nullptr;
    };

    auto countMatchingTargets = [&](const std::vector<displaycore::DisplayTarget>& candidates) -> size_t
    {
        size_t count = 0;
        for (const auto& candidate : candidates)
        {
            if (DirectTargetMatchesPreference(candidate, directTargetPreference))
            {
                count++;
            }
        }

        return count;
    };

    displaycore::DisplayTarget target{ nullptr };
    if (!directTargetPreference.empty())
    {
        const size_t matchCount = countMatchingTargets(connectedTargets);
        if (matchCount > 1)
        {
            throw winrt::hresult_error(
                HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
                L"Multiple connected DisplayCore targets match the saved direct target selector.");
        }
        if (matchCount == 1)
        {
            target = findMatchingTarget(connectedTargets);
        }
    }

    if (directTargetPreference.empty())
    {
        throw winrt::hresult_error(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
            L"No saved direct target selector. Run MonitorSplitterCtl hosttarget <selector> while the panel is native, then MonitorSplitterCtl panel split.");
    }

    if (target == nullptr)
    {
        throw winrt::hresult_error(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
            L"No connected DisplayCore target matches direct-target.txt or host-target.txt.");
    }

    state.Host.DeviceName = ToWString(target.DeviceInterfacePath());
    state.Host.MonitorId = ToWString(target.StableMonitorId());
    std::wstringstream monitorString;
    monitorString << L"DisplayCore target usageKind=" << static_cast<int>(target.UsageKind());
    state.Host.MonitorString = monitorString.str();
    state.Host.AdapterId = ToWString(target.Adapter().DeviceInterfacePath());
    state.Host.IsMonitorSplitter = false;
    return target;
}

double PresentationRateHz(const displaycore::DisplayPresentationRate& rate)
{
    if (rate.VerticalSyncRate.Denominator == 0)
    {
        return 0.0;
    }

    return static_cast<double>(rate.VerticalSyncRate.Numerator) /
           static_cast<double>(rate.VerticalSyncRate.Denominator);
}

displaycore::DisplayModeInfo SelectDirectMode(
    const displaycore::DisplayPath& path,
    const MonitorSplitter::Layout& layout)
{
    displaycore::DisplayModeInfo bestMode{ nullptr };
    double bestDiff = 1000000.0;
    const double desiredRefresh = static_cast<double>(layout.Refresh);

    for (const auto& mode : path.FindModes(displaycore::DisplayModeQueryOptions::None))
    {
        const auto sourceResolution = mode.SourceResolution();
        const auto targetResolution = mode.TargetResolution();
        if (sourceResolution.Width != static_cast<int32_t>(layout.HostWidth) ||
            sourceResolution.Height != static_cast<int32_t>(layout.Height) ||
            targetResolution.Width != static_cast<int32_t>(layout.HostWidth) ||
            targetResolution.Height != static_cast<int32_t>(layout.Height) ||
            mode.IsInterlaced())
        {
            continue;
        }

        const double refresh = PresentationRateHz(mode.PresentationRate());
        const double diff = refresh > desiredRefresh ? refresh - desiredRefresh : desiredRefresh - refresh;
        if (diff < bestDiff)
        {
            bestMode = mode;
            bestDiff = diff;
        }
    }

    if (bestMode == nullptr || bestDiff > 0.5)
    {
        throw winrt::hresult_error(
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            L"DisplayCore target does not expose the configured 1:1 host mode and refresh.");
    }

    return bestMode;
}

void ThrowIfAcquireFailed(const displaycore::DisplayManagerResultWithState& result)
{
    if (result.ErrorCode() != displaycore::DisplayManagerResult::Success)
    {
        const HRESULT hr = FAILED(result.ExtendedErrorCode())
            ? static_cast<HRESULT>(result.ExtendedErrorCode())
            : E_FAIL;
        throw winrt::hresult_error(hr, L"DisplayCore target acquisition failed.");
    }

    winrt::check_hresult(result.ExtendedErrorCode());
}

void ThrowIfApplyFailed(const displaycore::DisplayStateOperationResult& result)
{
    if (result.Status() != displaycore::DisplayStateOperationStatus::Success)
    {
        const HRESULT hr = FAILED(result.ExtendedErrorCode())
            ? static_cast<HRESULT>(result.ExtendedErrorCode())
            : E_FAIL;
        throw winrt::hresult_error(hr, L"DisplayCore mode apply failed.");
    }

    winrt::check_hresult(result.ExtendedErrorCode());
}

bool DirectPathMatchesLayout(
    const displaycore::DisplayPath& path,
    const MonitorSplitter::Layout& layout)
{
    if (path == nullptr ||
        path.SourceResolution() == nullptr ||
        path.TargetResolution() == nullptr)
    {
        return false;
    }

    const auto sourceResolution = path.SourceResolution().Value();
    const auto targetResolution = path.TargetResolution().Value();
    const auto interlaced = path.IsInterlaced();
    return sourceResolution.Width == static_cast<int32_t>(layout.HostWidth) &&
           sourceResolution.Height == static_cast<int32_t>(layout.Height) &&
           targetResolution.Width == static_cast<int32_t>(layout.HostWidth) &&
           targetResolution.Height == static_cast<int32_t>(layout.Height) &&
           (interlaced == nullptr || !interlaced.Value());
}

void CaptureDirectPathDiagnostics(AppState& state, const displaycore::DisplayPath& path)
{
    state.DirectReadPathAvailable = false;
    state.DirectReadSourceWidth = 0;
    state.DirectReadSourceHeight = 0;
    state.DirectReadTargetWidth = 0;
    state.DirectReadTargetHeight = 0;
    state.DirectReadPixelFormat = 0;
    state.DirectReadScaling = 0;
    state.DirectReadInterlaced = false;

    if (path == nullptr ||
        path.SourceResolution() == nullptr ||
        path.TargetResolution() == nullptr)
    {
        return;
    }

    const auto sourceResolution = path.SourceResolution().Value();
    const auto targetResolution = path.TargetResolution().Value();
    state.DirectReadPathAvailable = true;
    state.DirectReadSourceWidth = sourceResolution.Width;
    state.DirectReadSourceHeight = sourceResolution.Height;
    state.DirectReadTargetWidth = targetResolution.Width;
    state.DirectReadTargetHeight = targetResolution.Height;
    state.DirectReadPixelFormat = static_cast<INT32>(path.SourcePixelFormat());
    state.DirectReadScaling = static_cast<INT32>(path.Scaling());
    const auto interlaced = path.IsInterlaced();
    state.DirectReadInterlaced = interlaced != nullptr && interlaced.Value();
}

void ResetDirectOutput(AppState& state)
{
    std::scoped_lock lock(state.D3DLock);
    state.DirectRenderTargets = {};
    state.DirectTextures = {};
    state.DirectD3DFence.Reset();
    state.DirectFence = nullptr;
    state.DirectScanouts = { nullptr, nullptr };
    state.DirectPrimaries = { nullptr, nullptr };
    state.DirectTaskPool = nullptr;
    state.DirectSource = nullptr;
    state.DirectDevice = nullptr;
    state.DirectTarget = nullptr;
    state.DirectTargetAcquired = false;
    state.DirectManager = nullptr;
    state.DirectFenceValue = 0;
    state.DirectSurfaceIndex = 0;
    state.DirectWarmupFramesRemaining = 0;
    state.DirectModeAlreadyActive = false;
    state.DirectModeApplyAttempted = false;
    state.DirectReadCurrentResult = S_OK;
    state.DirectAcquireEmptyResult = S_OK;
    state.DirectApplyResult = S_OK;
    state.LastDirectScanoutCreateResult = S_OK;
    state.DirectScanoutCreateAttempts = 0;
    state.DirectReadPathAvailable = false;
    state.DirectReadSourceWidth = 0;
    state.DirectReadSourceHeight = 0;
    state.DirectReadTargetWidth = 0;
    state.DirectReadTargetHeight = 0;
    state.DirectReadPixelFormat = 0;
    state.DirectReadScaling = 0;
    state.DirectReadInterlaced = false;
    state.LastDisplayTaskResult = S_OK;
    state.LastDisplayTaskPresentStatus = -1;
    state.LastDisplayTaskSourceStatus = -1;
    state.LastDisplayTaskPresentId = 0;
    state.DisplayTaskSubmitAttempts = 0;
    state.DisplayTaskSuccesses = 0;
    state.DisplayTaskFailures = 0;
    state.DirectKeepaliveFrames = 0;
    state.DirectPollingCopyFrames = 0;
    state.LastDirectSubmitTick = 0;
    state.DirectModeSourceWidth = 0;
    state.DirectModeSourceHeight = 0;
    state.DirectModeTargetWidth = 0;
    state.DirectModeTargetHeight = 0;
    state.DirectModeRefreshNumerator = 0;
    state.DirectModeRefreshDenominator = 0;
    state.DirectModePixelFormat = 0;
    state.DirectModeInterlaced = false;
    for (auto& generations : state.LastDirectSourceGenerations)
    {
        generations.clear();
    }
    state.LastDirectCursorClient = {};
    state.LastDirectCursorVisible = { false, false };
    state.DirectPrimaryCleared = { false, false };
    state.DirectWarmupFramesRemaining = 12;
}

void SetupDirectOutput(AppState& state)
{
    if (state.DirectManager == nullptr)
    {
        state.DirectManager = displaycore::DisplayManager::Create(displaycore::DisplayManagerOptions::None);
    }
    if (state.DirectTarget == nullptr)
    {
        state.DirectTarget = FindDirectTarget(state, state.DirectManager);
    }

    auto targetVector = winrt::single_threaded_vector<displaycore::DisplayTarget>();
    targetVector.Append(state.DirectTarget);

    state.DirectTargetAcquired = false;
    state.DirectModeAlreadyActive = false;
    state.DirectModeApplyAttempted = false;
    state.DirectModeApplyFallbackUsed = false;
    state.DirectReadCurrentResult = S_OK;
    state.DirectAcquireEmptyResult = S_OK;
    state.DirectApplyResult = S_OK;
    state.DirectPostApplyReadResult = S_OK;
    state.LastDirectScanoutCreateResult = S_OK;
    state.DirectScanoutCreateAttempts = 0;
    state.DirectReadPathAvailable = false;
    state.DirectReadSourceWidth = 0;
    state.DirectReadSourceHeight = 0;
    state.DirectReadTargetWidth = 0;
    state.DirectReadTargetHeight = 0;
    state.DirectReadPixelFormat = 0;
    state.DirectReadScaling = 0;
    state.DirectReadInterlaced = false;

    // ForceReapply is a one-shot recovery hint from the controlling process.
    // Keeping it set across transient retries repeatedly modesets the desktop
    // and can make every physical display flicker while Windows is settling.
    const bool forceApply = state.DirectModeApplyFallbackPending;
    state.DirectModeApplyFallbackPending = false;

    displaycore::DisplayManagerResultWithState acquireResult{ nullptr };
    displaycore::DisplayState displayState{ nullptr };
    displaycore::DisplayPath path{ nullptr };
    displaycore::DisplayModeInfo selectedMode{ nullptr };
    bool modeAlreadyMatched = false;

    if (!forceApply)
    {
        try
        {
            auto readResult = state.DirectManager.TryAcquireTargetsAndReadCurrentState(targetVector);
            state.DirectReadCurrentResult = static_cast<HRESULT>(readResult.ExtendedErrorCode());
            ThrowIfAcquireFailed(readResult);

            auto readState = readResult.State();
            auto readPath = readState.GetPathForTarget(state.DirectTarget);
            CaptureDirectPathDiagnostics(state, readPath);
            if (DirectPathMatchesLayout(readPath, state.Layout))
            {
                acquireResult = readResult;
                displayState = readState;
                path = readPath;
                selectedMode = SelectDirectMode(path, state.Layout);
                state.DirectTargetAcquired = true;
                state.DirectModeAlreadyActive = true;
                modeAlreadyMatched = true;
            }
        }
        catch (winrt::hresult_error const& error)
        {
            state.DirectReadCurrentResult = static_cast<HRESULT>(error.code());
        }
    }

    if (!modeAlreadyMatched)
    {
        if (!state.DirectModeApplyAllowed)
        {
            throw winrt::hresult_error(
                HRESULT_FROM_WIN32(ERROR_RETRY),
                L"DisplayCore target is not at the configured direct mode yet; retrying without applying display mode.");
        }

        acquireResult = state.DirectManager.TryAcquireTargetsAndCreateEmptyState(targetVector);
        state.DirectAcquireEmptyResult = static_cast<HRESULT>(acquireResult.ExtendedErrorCode());
        ThrowIfAcquireFailed(acquireResult);
        state.DirectTargetAcquired = true;
        displayState = acquireResult.State();

        path = displayState.ConnectTarget(state.DirectTarget);
        path.IsInterlaced(false);
        path.Scaling(displaycore::DisplayPathScaling::Identity);
        path.SourcePixelFormat(directx::DirectXPixelFormat::B8G8R8A8UIntNormalized);
        selectedMode = SelectDirectMode(path, state.Layout);
        path.ApplyPropertiesFromMode(selectedMode);

        state.DirectModeApplyAttempted = true;
        state.DirectModeApplyFallbackUsed = forceApply;
        const auto applyResult = displayState.TryApply(forceApply
            ? displaycore::DisplayStateApplyOptions::ForceReapply
            : displaycore::DisplayStateApplyOptions::None);
        state.DirectApplyResult = static_cast<HRESULT>(applyResult.ExtendedErrorCode());
        ThrowIfApplyFailed(applyResult);

        acquireResult = state.DirectManager.TryAcquireTargetsAndReadCurrentState(targetVector);
        state.DirectPostApplyReadResult = static_cast<HRESULT>(acquireResult.ExtendedErrorCode());
        ThrowIfAcquireFailed(acquireResult);
        displayState = acquireResult.State();
        path = displayState.GetPathForTarget(state.DirectTarget);
        CaptureDirectPathDiagnostics(state, path);
        state.DirectModeAlreadyActive = DirectPathMatchesLayout(path, state.Layout);
    }

    INT32 primaryWidth = static_cast<INT32>(state.Layout.HostWidth);
    INT32 primaryHeight = static_cast<INT32>(state.Layout.Height);
    directx::DirectXPixelFormat primaryPixelFormat = directx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    state.DirectModeSourceWidth = primaryWidth;
    state.DirectModeSourceHeight = primaryHeight;
    state.DirectModeTargetWidth = primaryWidth;
    state.DirectModeTargetHeight = primaryHeight;
    state.DirectModeRefreshNumerator = static_cast<UINT32>(state.Layout.Refresh);
    state.DirectModeRefreshDenominator = 1;
    state.DirectModePixelFormat = static_cast<INT32>(primaryPixelFormat);
    state.DirectModeInterlaced = false;
    const auto selectedSourceResolution = selectedMode.SourceResolution();
    const auto selectedTargetResolution = selectedMode.TargetResolution();
    const auto selectedRate = selectedMode.PresentationRate();
    state.DirectModeSourceWidth = selectedSourceResolution.Width;
    state.DirectModeSourceHeight = selectedSourceResolution.Height;
    state.DirectModeTargetWidth = selectedTargetResolution.Width;
    state.DirectModeTargetHeight = selectedTargetResolution.Height;
    state.DirectModeRefreshNumerator = selectedRate.VerticalSyncRate.Numerator;
    state.DirectModeRefreshDenominator = selectedRate.VerticalSyncRate.Denominator;
    state.DirectModeInterlaced = selectedMode.IsInterlaced();
    if (path != nullptr && path.SourceResolution() != nullptr)
    {
        const auto primaryResolution = path.SourceResolution().Value();
        primaryWidth = primaryResolution.Width;
        primaryHeight = primaryResolution.Height;
        primaryPixelFormat = path.SourcePixelFormat();
        state.DirectModePixelFormat = static_cast<INT32>(primaryPixelFormat);
    }

    path = nullptr;
    displayState = nullptr;
    acquireResult = nullptr;

    state.DirectDevice = state.DirectManager.CreateDisplayDevice(state.DirectTarget.Adapter());

    try
    {
        state.DirectSource = state.DirectDevice.CreateScanoutSource(state.DirectTarget);
        state.LastDirectScanoutCreateResult = S_OK;
    }
    catch (winrt::hresult_error const& error)
    {
        const HRESULT hr = static_cast<HRESULT>(error.code());
        state.LastDirectScanoutCreateResult = hr;
        state.DirectScanoutCreateAttempts++;
        throw;
    }

    state.DirectTaskPool = state.DirectDevice.CreateTaskPool();

    d3d11rt::Direct3DMultisampleDescription multisample = {};
    multisample.Count = 1;

    displaycore::DisplayPrimaryDescription primaryDescription(
        static_cast<uint32_t>(primaryWidth),
        static_cast<uint32_t>(primaryHeight),
        primaryPixelFormat,
        directx::DirectXColorSpace::RgbFullG22NoneP709,
        false,
        multisample);

    auto deviceInterop = state.DirectDevice.as<IDisplayDeviceInterop>();
    ComPtr<ID3D11Device5> d3dDevice5;
    winrt::check_hresult(state.Device.As(&d3dDevice5));

    for (size_t index = 0; index < state.DirectPrimaries.size(); index++)
    {
        state.DirectPrimaries[index] = state.DirectDevice.CreatePrimary(state.DirectTarget, primaryDescription);
        state.DirectScanouts[index] = state.DirectDevice.CreateSimpleScanout(
            state.DirectSource,
            state.DirectPrimaries[index],
            0,
            1);

        auto surfaceInspectable = state.DirectPrimaries[index].as<::IInspectable>();
        HANDLE surfaceHandle = nullptr;
        winrt::check_hresult(deviceInterop->CreateSharedHandle(
            surfaceInspectable.get(),
            nullptr,
            GENERIC_ALL,
            nullptr,
            &surfaceHandle));

        HRESULT openHr = d3dDevice5->OpenSharedResource1(
            surfaceHandle,
            IID_PPV_ARGS(&state.DirectTextures[index]));
        CloseHandle(surfaceHandle);
        winrt::check_hresult(openHr);

        D3D11_TEXTURE2D_DESC textureDesc = {};
        state.DirectTextures[index]->GetDesc(&textureDesc);
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = 0;
        viewDesc.Format = textureDesc.Format;
        winrt::check_hresult(state.Device->CreateRenderTargetView(
            state.DirectTextures[index].Get(),
            &viewDesc,
            &state.DirectRenderTargets[index]));
    }

    winrt::check_hresult(d3dDevice5->CreateFence(
        0,
        D3D11_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&state.DirectD3DFence)));

    HANDLE fenceHandle = nullptr;
    winrt::check_hresult(state.DirectD3DFence->CreateSharedHandle(
        nullptr,
        GENERIC_ALL,
        nullptr,
        &fenceHandle));

    winrt::com_ptr<::IInspectable> displayFenceInspectable;
    HRESULT openFenceHr = deviceInterop->OpenSharedHandle(
        fenceHandle,
        __uuidof(::IInspectable),
        displayFenceInspectable.put_void());
    CloseHandle(fenceHandle);
    winrt::check_hresult(openFenceHr);
    state.DirectFence = displayFenceInspectable.as<displaycore::DisplayFence>();

    state.DirectModeApplyFallbackPending = false;
}

void RenderDirectFrame(AppState& state)
{
    std::scoped_lock lock(state.D3DLock);

    if (state.DirectTaskPool == nullptr ||
        state.DirectFence == nullptr ||
        state.DirectD3DFence == nullptr)
    {
        return;
    }

    const size_t surfaceIndex = state.DirectSurfaceIndex;
    auto& targetTexture = state.DirectTextures[surfaceIndex];
    auto& targetView = state.DirectRenderTargets[surfaceIndex];
    if (targetTexture == nullptr || targetView == nullptr)
    {
        return;
    }

    auto& surfaceGenerations = state.LastDirectSourceGenerations[surfaceIndex];
    if (surfaceGenerations.size() != state.FrameSources.size())
    {
        surfaceGenerations.assign(state.FrameSources.size(), 0);
    }
    const auto previousSurfaceGenerations = surfaceGenerations;
    const bool previousPrimaryCleared = state.DirectPrimaryCleared[surfaceIndex];
    const DWORD previousWarmupFramesRemaining = state.DirectWarmupFramesRemaining;
    const bool previousCursorVisible = state.LastDirectCursorVisible[surfaceIndex];
    const POINT previousCursorClient = state.LastDirectCursorClient[surfaceIndex];

    POINT cursorClient = {};
    const bool cursorVisible = TryGetCursorClientPoint(state, cursorClient);
    const bool cursorChanged =
        cursorVisible != state.LastDirectCursorVisible[surfaceIndex] ||
        (cursorVisible &&
            (cursorClient.x != state.LastDirectCursorClient[surfaceIndex].x ||
             cursorClient.y != state.LastDirectCursorClient[surfaceIndex].y));

    const bool primaryNeedsInitialize = !state.DirectPrimaryCleared[surfaceIndex];
    const bool warmupFrame = state.DirectWarmupFramesRemaining > 0;
    const DWORD now = GetTickCount();
    const bool keepaliveFrame =
        !warmupFrame &&
        !primaryNeedsInitialize &&
        state.PresentedFrames > 0 &&
        state.LastDirectSubmitTick != 0 &&
        static_cast<DWORD>(now - state.LastDirectSubmitTick) >= kDirectKeepaliveIntervalMs;
    bool wantsPollingCopy = false;
    for (const auto& frameSource : state.FrameSources)
    {
        if (frameSource != nullptr && frameSource->WantsPollingCopy())
        {
            wantsPollingCopy = true;
            break;
        }
    }
    const bool pollingCopyFrame =
        !warmupFrame &&
        !primaryNeedsInitialize &&
        wantsPollingCopy &&
        state.PresentedFrames > 0 &&
        state.LastDirectSubmitTick != 0 &&
        static_cast<DWORD>(now - state.LastDirectSubmitTick) >= kDirectPollingCopyIntervalMs;
    bool hasNewFrame = warmupFrame || keepaliveFrame || pollingCopyFrame || state.PresentedFrames == 0 || primaryNeedsInitialize || cursorChanged;
    for (size_t index = 0; index < state.FrameSources.size(); index++)
    {
        const ULONGLONG generation = state.FrameSources[index]->FrameGeneration();
        if (generation != surfaceGenerations[index])
        {
            hasNewFrame = true;
            break;
        }
    }

    if (!hasNewFrame)
    {
        return;
    }

    if (warmupFrame || primaryNeedsInitialize)
    {
        static const float black[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        state.Context->ClearRenderTargetView(targetView.Get(), black);
        state.DirectPrimaryCleared[surfaceIndex] = true;
        if (warmupFrame)
        {
            state.DirectWarmupFramesRemaining--;
        }
    }

    for (size_t index = 0; index < state.FrameSources.size(); index++)
    {
        const ULONGLONG generation = state.FrameSources[index]->FrameGeneration();
        if (warmupFrame || primaryNeedsInitialize || cursorChanged || keepaliveFrame || pollingCopyFrame || generation != surfaceGenerations[index])
        {
            if (state.FrameSources[index]->CopyLatestToBackBuffer(targetTexture.Get(), targetView.Get()))
            {
                surfaceGenerations[index] = state.FrameSources[index]->FrameGeneration();
            }
        }
    }

    DrawSoftwareCursor(state, targetView.Get());
    state.LastDirectCursorVisible[surfaceIndex] = cursorVisible;
    state.LastDirectCursorClient[surfaceIndex] = cursorClient;

    auto rollbackConsumedFrameState = [&]()
    {
        surfaceGenerations = previousSurfaceGenerations;
        state.DirectPrimaryCleared[surfaceIndex] = previousPrimaryCleared;
        state.DirectWarmupFramesRemaining = previousWarmupFramesRemaining;
        state.LastDirectCursorVisible[surfaceIndex] = previousCursorVisible;
        state.LastDirectCursorClient[surfaceIndex] = previousCursorClient;
    };

    if (!DrawScanoutKick(state, targetView.Get()))
    {
        rollbackConsumedFrameState();
        state.LastPresentResult = state.LastScanoutKickResult;
        return;
    }

    ComPtr<ID3D11DeviceContext4> context4;
    winrt::check_hresult(state.Context.As(&context4));
    winrt::check_hresult(context4->Signal(state.DirectD3DFence.Get(), ++state.DirectFenceValue));
    state.Context->Flush();

    auto task = state.DirectTaskPool.CreateTask();
    task.SetScanout(state.DirectScanouts[surfaceIndex]);
    task.SetWait(state.DirectFence, state.DirectFenceValue);

    state.DisplayTaskSubmitAttempts++;
    if (metadata::ApiInformation::IsMethodPresent(L"Windows.Devices.Display.Core.DisplayTaskPool", L"TryExecuteTask"))
    {
        auto taskResult = state.DirectTaskPool.TryExecuteTask(task);
        state.LastDisplayTaskResult = S_OK;
        state.LastDisplayTaskPresentStatus = static_cast<INT32>(taskResult.PresentStatus());
        state.LastDisplayTaskSourceStatus = static_cast<INT32>(taskResult.SourceStatus());
        state.LastDisplayTaskPresentId = taskResult.PresentId();
        if (taskResult.PresentStatus() != displaycore::DisplayPresentStatus::Success)
        {
            rollbackConsumedFrameState();
            state.LastPresentResult = E_FAIL;
            state.DisplayTaskFailures++;
            return;
        }
    }
    else
    {
        state.DirectTaskPool.ExecuteTask(task);
        state.LastDisplayTaskResult = S_OK;
        state.LastDisplayTaskPresentStatus = -1;
        state.LastDisplayTaskSourceStatus = -1;
        state.LastDisplayTaskPresentId = 0;
    }

    state.LastPresentResult = S_OK;
    state.DisplayTaskSuccesses++;
    state.PresentedFrames++;
    if (keepaliveFrame)
    {
        state.DirectKeepaliveFrames++;
    }
    if (pollingCopyFrame)
    {
        state.DirectPollingCopyFrames++;
    }
    if (warmupFrame)
    {
        state.DirectWarmupFramesPresented++;
    }
    state.LastDirectSubmitTick = GetTickCount();
    state.DirectSurfaceIndex = (surfaceIndex + 1) % state.DirectTextures.size();
}

CaptureSource::CaptureSource(AppState* state, MonitorInfo monitor, int destX) :
    m_State(state),
    m_Monitor(std::move(monitor)),
    m_DestX(destX)
{
}

CaptureSource::~CaptureSource()
{
    Stop();
}

bool BorderlessCaptureAccessAllowed()
{
    static bool checked = false;
    static bool allowed = false;
    if (checked)
    {
        return allowed;
    }

    checked = true;
    if (!metadata::ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureAccess"))
    {
        return false;
    }

    try
    {
        allowed =
            capture::GraphicsCaptureAccess::RequestAccessAsync(capture::GraphicsCaptureAccessKind::Borderless).get() ==
            appcap::AppCapabilityAccessStatus::Allowed;
    }
    catch (...)
    {
        allowed = false;
    }

    return allowed;
}

void DisableCaptureBorderIfAvailable(capture::GraphicsCaptureSession const& session)
{
    if (!metadata::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired") ||
        !BorderlessCaptureAccessAllowed())
    {
        return;
    }

    try
    {
        session.IsBorderRequired(false);
    }
    catch (...)
    {
    }
}

void CaptureSource::Start()
{
    auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForMonitor(
        m_Monitor.Handle,
        winrt::guid_of<capture::GraphicsCaptureItem>(),
        winrt::put_abi(m_Item)));

    m_FramePool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
        m_State->WinRtDevice,
        directx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        m_Item.Size());

    m_FrameArrivedToken = m_FramePool.FrameArrived([this](auto const& sender, auto const&) {
        OnFrameArrived(sender);
    });

    m_Session = m_FramePool.CreateCaptureSession(m_Item);
    m_Session.IsCursorCaptureEnabled(m_State->DirectMode);
    DisableCaptureBorderIfAvailable(m_Session);
    m_Session.StartCapture();
}

void CaptureSource::Stop()
{
    if (m_FramePool != nullptr)
    {
        m_FramePool.FrameArrived(m_FrameArrivedToken);
    }
    if (m_Session != nullptr)
    {
        m_Session.Close();
        m_Session = nullptr;
    }
    if (m_FramePool != nullptr)
    {
        m_FramePool.Close();
        m_FramePool = nullptr;
    }
    m_Item = nullptr;
    m_LatestTexture.Reset();
}

void CaptureSource::EnsureLatestTexture(ID3D11Texture2D* sourceTexture)
{
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceTexture->GetDesc(&sourceDesc);

    if (m_LatestTexture != nullptr)
    {
        D3D11_TEXTURE2D_DESC latestDesc = {};
        m_LatestTexture->GetDesc(&latestDesc);
        if (latestDesc.Width == sourceDesc.Width &&
            latestDesc.Height == sourceDesc.Height &&
            latestDesc.Format == sourceDesc.Format)
        {
            return;
        }
    }

    D3D11_TEXTURE2D_DESC latestDesc = sourceDesc;
    latestDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    latestDesc.CPUAccessFlags = 0;
    latestDesc.MiscFlags = 0;
    latestDesc.Usage = D3D11_USAGE_DEFAULT;

    m_LatestTexture.Reset();
    winrt::check_hresult(m_State->Device->CreateTexture2D(&latestDesc, nullptr, &m_LatestTexture));
}

void CaptureSource::OnFrameArrived(capture::Direct3D11CaptureFramePool const& sender)
{
    auto frame = sender.TryGetNextFrame();
    if (frame == nullptr)
    {
        return;
    }

    auto sourceTexture = GetDxgiInterface<ID3D11Texture2D>(frame.Surface());

    std::scoped_lock lock(m_State->D3DLock);
    EnsureLatestTexture(sourceTexture.Get());
    m_State->Context->CopyResource(m_LatestTexture.Get(), sourceTexture.Get());
    m_FrameGeneration++;
}

bool CaptureSource::CopyLatestToBackBuffer(ID3D11Texture2D* backBuffer, ID3D11RenderTargetView* targetView) const
{
    (void)backBuffer;
    m_CopyAttempts++;
    if (m_LatestTexture == nullptr)
    {
        m_CopyFailures++;
        m_LastCopyResult = E_PENDING;
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    m_LatestTexture->GetDesc(&sourceDesc);

    const UINT copyWidth = std::min<UINT>(sourceDesc.Width, static_cast<UINT>(Width(m_Monitor.Rect)));
    const UINT copyHeight = std::min<UINT>(sourceDesc.Height, static_cast<UINT>(Height(m_Monitor.Rect)));
    const HRESULT hr = DrawTextureToTarget(
        *m_State,
        m_LatestTexture.Get(),
        targetView,
        static_cast<LONG>(m_State->Layout.HostWidth),
        static_cast<LONG>(m_State->Layout.Height),
        m_DestX,
        0,
        static_cast<LONG>(copyWidth),
        static_cast<LONG>(copyHeight));
    m_LastCopyResult = hr;
    if (FAILED(hr))
    {
        m_CopyFailures++;
        return false;
    }

    m_CopySuccesses++;
    m_LastCopiedGeneration = m_FrameGeneration;
    return true;
}
ULONGLONG CaptureSource::FrameGeneration() const
{
    return m_FrameGeneration;
}

void CaptureSource::AppendDiagnosticsJson(std::wstringstream& status) const
{
    status << L"{\"kind\":\"capture\"";
    status << L",\"index\":0";
    status << L",\"destX\":" << m_DestX;
    status << L",\"width\":" << Width(m_Monitor.Rect);
    status << L",\"height\":" << Height(m_Monitor.Rect);
    status << L",\"generation\":" << m_FrameGeneration;
    status << L",\"copyAttempts\":" << m_CopyAttempts;
    status << L",\"copySuccesses\":" << m_CopySuccesses;
    status << L",\"copyFailures\":" << m_CopyFailures;
    status << L",\"lastCopyResult\":\"" << HResultCode(m_LastCopyResult) << L"\"";
    status << L",\"lastCopiedGeneration\":" << m_LastCopiedGeneration;
    status << L"}";
}

SharedFrameSource::SharedFrameSource(AppState* state, size_t connectorIndex, MonitorSplitter::MonitorMode mode, int destX) :
    m_State(state),
    m_ConnectorIndex(connectorIndex),
    m_Mode(mode),
    m_DestX(destX)
{
}

SharedFrameSource::~SharedFrameSource()
{
    Stop();
}

HRESULT OpenSharedFrameTextureFromStatus(
    ID3D11Device1* device,
    const MonitorSplitter::SharedFrameStatus& status,
    ID3D11Texture2D** texture)
{
    if (device == nullptr || texture == nullptr)
    {
        return E_POINTER;
    }

    *texture = nullptr;
    if (status.ProducerProcessId == 0 || status.SharedHandleValue == 0)
    {
        return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
    }

    HANDLE sourceProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, status.ProducerProcessId);
    if (sourceProcess == nullptr)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HANDLE localHandle = nullptr;
    const BOOL duplicated = DuplicateHandle(
        sourceProcess,
        reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(status.SharedHandleValue)),
        GetCurrentProcess(),
        &localHandle,
        0,
        FALSE,
        DUPLICATE_SAME_ACCESS);
    CloseHandle(sourceProcess);
    if (!duplicated)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    ComPtr<ID3D11Resource> resource;
    const HRESULT openHr = device->OpenSharedResource1(localHandle, IID_PPV_ARGS(&resource));
    CloseHandle(localHandle);
    if (FAILED(openHr))
    {
        return openHr;
    }

    ComPtr<ID3D11Texture2D> textureResult;
    const HRESULT textureHr = resource.As(&textureResult);
    if (FAILED(textureHr))
    {
        return textureHr;
    }

    *texture = textureResult.Detach();
    return S_OK;
}

HRESULT OpenSharedFrameTextureByName(
    ID3D11Device1* device,
    size_t connectorIndex,
    ID3D11Texture2D** texture)
{
    if (device == nullptr || texture == nullptr)
    {
        return E_POINTER;
    }

    *texture = nullptr;

    ComPtr<ID3D11Resource> resource;
    const HRESULT openHr = device->OpenSharedResourceByName(
        MonitorSplitter::SharedFrameTextureName(connectorIndex).c_str(),
        DXGI_SHARED_RESOURCE_READ,
        IID_PPV_ARGS(&resource));
    if (FAILED(openHr))
    {
        return openHr;
    }

    ComPtr<ID3D11Texture2D> textureResult;
    const HRESULT textureHr = resource.As(&textureResult);
    if (FAILED(textureHr))
    {
        return textureHr;
    }

    *texture = textureResult.Detach();
    return S_OK;
}

void SharedFrameSource::Start()
{
    m_SourceStartTick = GetTickCount64();
    m_ObservedFreshPublish = false;
    m_FirstFreshPublishedTick = 0;

    ComPtr<ID3D11Device1> d3dDevice;
    winrt::check_hresult(m_State->Device.As(&d3dDevice));

    m_StatusMapping = OpenFileMappingW(
        FILE_MAP_READ,
        FALSE,
        MonitorSplitter::SharedFrameStatusName(m_ConnectorIndex).c_str());
    if (m_StatusMapping == nullptr)
    {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Stop();
        throw winrt::hresult_error(hr, L"MonitorSplitter shared frame producer status is not available.");
    }

    m_Status = static_cast<const MonitorSplitter::SharedFrameStatus*>(MapViewOfFile(
        m_StatusMapping,
        FILE_MAP_READ,
        0,
        0,
        sizeof(MonitorSplitter::SharedFrameStatus)));
    if (m_Status == nullptr)
    {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Stop();
        throw winrt::hresult_error(hr, L"MonitorSplitter shared frame producer status could not be mapped.");
    }

    m_RuntimeStatusMapping = OpenFileMappingW(
        FILE_MAP_READ,
        FALSE,
        MonitorSplitter::DriverRuntimeStatusName(m_ConnectorIndex).c_str());
    if (m_RuntimeStatusMapping != nullptr)
    {
        m_RuntimeStatus = static_cast<const MonitorSplitter::DriverRuntimeStatus*>(MapViewOfFile(
            m_RuntimeStatusMapping,
            FILE_MAP_READ,
            0,
            0,
            sizeof(MonitorSplitter::DriverRuntimeStatus)));
        if (m_RuntimeStatus == nullptr)
        {
            CloseHandle(m_RuntimeStatusMapping);
            m_RuntimeStatusMapping = nullptr;
        }
    }

    if (!IsProducerReady())
    {
        Stop();
        throw winrt::hresult_error(E_PENDING, L"MonitorSplitter shared frame producer has not published a valid frame yet.");
    }

    HRESULT openHr = OpenSharedFrameTextureByName(d3dDevice.Get(), m_ConnectorIndex, &m_Texture);
    if (FAILED(openHr))
    {
        openHr = OpenSharedFrameTextureFromStatus(d3dDevice.Get(), *m_Status, &m_Texture);
    }
    winrt::check_hresult(openHr);
    winrt::check_hresult(m_Texture.As(&m_Mutex));

    D3D11_TEXTURE2D_DESC desc = {};
    m_Texture->GetDesc(&desc);
    if (desc.Width < m_Mode.Width ||
        desc.Height < m_Mode.Height ||
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        Stop();
        throw winrt::hresult_error(E_FAIL, L"MonitorSplitter shared frame texture has an incompatible format or size.");
    }
}

void SharedFrameSource::Stop()
{
    if (m_Status != nullptr)
    {
        UnmapViewOfFile(m_Status);
        m_Status = nullptr;
    }
    if (m_StatusMapping != nullptr)
    {
        CloseHandle(m_StatusMapping);
        m_StatusMapping = nullptr;
    }
    if (m_RuntimeStatus != nullptr)
    {
        UnmapViewOfFile(m_RuntimeStatus);
        m_RuntimeStatus = nullptr;
    }
    if (m_RuntimeStatusMapping != nullptr)
    {
        CloseHandle(m_RuntimeStatusMapping);
        m_RuntimeStatusMapping = nullptr;
    }
    m_Mutex.Reset();
    m_Texture.Reset();
}

bool SharedFrameSource::IsProducerReady() const
{
    if (m_Status == nullptr)
    {
        return false;
    }

    const auto status = *m_Status;
    if (status.Magic != MonitorSplitter::kSharedFrameStatusMagic ||
        status.Version != MonitorSplitter::kSharedFrameStatusVersion ||
        status.ConnectorIndex != m_ConnectorIndex ||
        status.PublishedFrames == 0 ||
        FAILED(status.LastCreateResult) ||
        FAILED(status.LastPublishResult) ||
        FAILED(status.LastAcquireResult) ||
        status.LastSharedWidth < m_Mode.Width ||
        status.LastSharedHeight < m_Mode.Height ||
        status.LastSharedFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        return false;
    }

    const LUID producerLuid = { status.ProducerAdapterLuidLow, status.ProducerAdapterLuidHigh };
    if (!SameLuid(producerLuid, m_State->D3DAdapterLuid))
    {
        m_State->SharedFrameAdapterMismatchCount++;
        m_State->LastSharedFrameAdapterMismatch =
            L"connector " + std::to_wstring(m_ConnectorIndex) +
            L" producer adapter " + FormatLuid(producerLuid) +
            L" does not match host adapter " + FormatLuid(m_State->D3DAdapterLuid);
        return false;
    }

    if (!m_ObservedFreshPublish)
    {
        const ULONGLONG now = GetTickCount64();
        if (status.LastPublishedTick == 0 ||
            status.LastPublishedTick > now ||
            now - status.LastPublishedTick > kInitialSharedFrameFreshnessMs ||
            status.LastPublishedTick + kInitialSharedFrameFreshnessMs < m_SourceStartTick)
        {
            return false;
        }
        m_ObservedFreshPublish = true;
        m_FirstFreshPublishedTick = status.LastPublishedTick;
    }

    return true;
}

bool SharedFrameSource::IsHealthy() const
{
    return m_Texture != nullptr && m_Mutex != nullptr && IsProducerReady();
}

bool SharedFrameSource::WantsPollingCopy() const
{
    if (m_Status == nullptr || m_RuntimeStatus == nullptr)
    {
        return false;
    }

    const auto status = *m_Status;
    const auto runtime = *m_RuntimeStatus;
    if (status.Magic != MonitorSplitter::kSharedFrameStatusMagic ||
        status.Version != MonitorSplitter::kSharedFrameStatusVersion ||
        status.ConnectorIndex != m_ConnectorIndex ||
        runtime.Magic != MonitorSplitter::kDriverRuntimeStatusMagic ||
        runtime.Version != MonitorSplitter::kDriverRuntimeStatusVersion ||
        runtime.ConnectorIndex != m_ConnectorIndex ||
        status.LastPublishedTick == 0 ||
        runtime.UpdatedTick == 0)
    {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (status.LastPublishedTick > now || runtime.UpdatedTick > now)
    {
        return false;
    }

    constexpr ULONGLONG kProducerCounterStaleMs = 1000;
    constexpr ULONGLONG kRuntimeHeartbeatFreshMs = 2000;
    return (now - status.LastPublishedTick) > kProducerCounterStaleMs &&
           (now - runtime.UpdatedTick) <= kRuntimeHeartbeatFreshMs;
}

ULONGLONG SharedFrameSource::FrameGeneration() const
{
    if (m_Status == nullptr)
    {
        return 0;
    }

    const auto status = *m_Status;
    if (status.Magic != MonitorSplitter::kSharedFrameStatusMagic ||
        status.Version != MonitorSplitter::kSharedFrameStatusVersion ||
        status.ConnectorIndex != m_ConnectorIndex)
    {
        return 0;
    }

    return status.PublishedFrames;
}

bool SharedFrameSource::CopyLatestToBackBuffer(ID3D11Texture2D* backBuffer, ID3D11RenderTargetView* targetView) const
{
    (void)backBuffer;
    m_CopyAttempts++;
    if (!IsHealthy())
    {
        m_CopyFailures++;
        m_LastCopyResult = E_PENDING;
        return false;
    }

    m_State->SharedFrameCopyAttempts++;
    HRESULT hr = m_Mutex->AcquireSync(0, 4);
    if (FAILED(hr))
    {
        m_State->SharedFrameCopyFailures++;
        m_State->LastSharedFrameCopyResult = hr;
        m_CopyFailures++;
        m_LastCopyResult = hr;
        return false;
    }

    hr = DrawTextureToTarget(
        *m_State,
        m_Texture.Get(),
        targetView,
        static_cast<LONG>(m_State->Layout.HostWidth),
        static_cast<LONG>(m_State->Layout.Height),
        m_DestX,
        0,
        static_cast<LONG>(m_Mode.Width),
        static_cast<LONG>(m_Mode.Height));
    const HRESULT releaseHr = m_Mutex->ReleaseSync(0);
    m_State->LastSharedFrameCopyResult = FAILED(hr) ? hr : releaseHr;
    if (FAILED(hr) || FAILED(releaseHr))
    {
        m_State->SharedFrameCopyFailures++;
        m_CopyFailures++;
        m_LastCopyResult = FAILED(hr) ? hr : releaseHr;
        return false;
    }

    m_State->SharedFrameCopySuccesses++;
    m_CopySuccesses++;
    m_LastCopyResult = S_OK;
    m_LastCopiedGeneration = FrameGeneration();
    return true;
}
void SharedFrameSource::AppendDiagnosticsJson(std::wstringstream& status) const
{
    MonitorSplitter::SharedFrameStatus producer = {};
    const bool producerMapped = m_Status != nullptr;
    if (producerMapped)
    {
        producer = *m_Status;
    }
    MonitorSplitter::DriverRuntimeStatus driverRuntime = {};
    const bool driverRuntimeMapped = ReadDriverRuntimeStatus(m_ConnectorIndex, driverRuntime);
    const bool driverRuntimeValid =
        driverRuntimeMapped &&
        driverRuntime.Magic == MonitorSplitter::kDriverRuntimeStatusMagic &&
        driverRuntime.Version == MonitorSplitter::kDriverRuntimeStatusVersion;

    status << L"{\"kind\":\"shared\"";
    status << L",\"index\":" << m_ConnectorIndex;
    status << L",\"destX\":" << m_DestX;
    status << L",\"width\":" << m_Mode.Width;
    status << L",\"height\":" << m_Mode.Height;
    status << L",\"healthy\":" << (IsHealthy() ? L"true" : L"false");
    status << L",\"generation\":" << FrameGeneration();
    status << L",\"initialFreshPublishObserved\":" << (m_ObservedFreshPublish ? L"true" : L"false");
    status << L",\"pollingCopyActive\":" << (WantsPollingCopy() ? L"true" : L"false");
    status << L",\"sourceStartTick\":" << m_SourceStartTick;
    status << L",\"firstFreshPublishedTick\":" << m_FirstFreshPublishedTick;
    status << L",\"copyAttempts\":" << m_CopyAttempts;
    status << L",\"copySuccesses\":" << m_CopySuccesses;
    status << L",\"copyFailures\":" << m_CopyFailures;
    status << L",\"lastCopyResult\":\"" << HResultCode(m_LastCopyResult) << L"\"";
    status << L",\"lastCopiedGeneration\":" << m_LastCopiedGeneration;
    status << L",\"producerMapped\":" << (producerMapped ? L"true" : L"false");
    status << L",\"producerPid\":" << (producerMapped ? producer.ProducerProcessId : 0);
    status << L",\"producerPublishedFrames\":" << (producerMapped ? producer.PublishedFrames : 0);
    status << L",\"producerPublishAttempts\":" << (producerMapped ? producer.PublishAttempts : 0);
    status << L",\"producerPublishFailures\":" << (producerMapped ? producer.PublishFailures : 0);
    status << L",\"producerLastPublishedTick\":" << (producerMapped ? producer.LastPublishedTick : 0);
    status << L",\"producerLastPublishedAgeMs\":";
    if (producerMapped && producer.LastPublishedTick != 0 && producer.LastPublishedTick <= GetTickCount64())
    {
        status << (GetTickCount64() - producer.LastPublishedTick);
    }
    else
    {
        status << 0;
    }
    status << L",\"producerSourceWidth\":" << (producerMapped ? producer.LastSourceWidth : 0);
    status << L",\"producerSourceHeight\":" << (producerMapped ? producer.LastSourceHeight : 0);
    status << L",\"producerSharedWidth\":" << (producerMapped ? producer.LastSharedWidth : 0);
    status << L",\"producerSharedHeight\":" << (producerMapped ? producer.LastSharedHeight : 0);
    status << L",\"producerSharedFormat\":" << (producerMapped ? producer.LastSharedFormat : 0);
    status << L",\"producerLastCreateResult\":\"" << HResultCode(producerMapped ? producer.LastCreateResult : E_PENDING) << L"\"";
    status << L",\"producerLastPublishResult\":\"" << HResultCode(producerMapped ? producer.LastPublishResult : E_PENDING) << L"\"";
    status << L",\"producerLastAcquireResult\":\"" << HResultCode(producerMapped ? producer.LastAcquireResult : E_PENDING) << L"\"";
    status << L",\"driverRuntimeMapped\":" << (driverRuntimeMapped ? L"true" : L"false");
    status << L",\"driverRuntimeValid\":" << (driverRuntimeValid ? L"true" : L"false");
    status << L",\"driverRuntimeVersion\":" << (driverRuntimeMapped ? driverRuntime.Version : 0);
    status << L",\"driverRuntimePid\":" << (driverRuntimeMapped ? driverRuntime.ProducerProcessId : 0);
    status << L",\"driverRuntimeUpdatedTick\":" << (driverRuntimeMapped ? driverRuntime.UpdatedTick : 0);
    status << L",\"driverRuntimeUpdatedAgeMs\":";
    if (driverRuntimeMapped && driverRuntime.UpdatedTick != 0 && driverRuntime.UpdatedTick <= GetTickCount64())
    {
        status << (GetTickCount64() - driverRuntime.UpdatedTick);
    }
    else
    {
        status << 0;
    }
    status << L",\"driverRuntimeHeartbeats\":" << (driverRuntimeMapped ? driverRuntime.Heartbeats : 0);
    status << L",\"driverRuntimePendingFrames\":" << (driverRuntimeMapped ? driverRuntime.PendingFrames : 0);
    status << L",\"driverRuntimeAcquiredFrames\":" << (driverRuntimeMapped ? driverRuntime.AcquiredFrames : 0);
    status << L",\"driverRuntimeFinishedFrames\":" << (driverRuntimeMapped ? driverRuntime.FinishedFrames : 0);
    status << L",\"driverRuntimeFailedAcquires\":" << (driverRuntimeMapped ? driverRuntime.FailedAcquires : 0);
    status << L",\"driverRuntimeExits\":" << (driverRuntimeMapped ? driverRuntime.Exits : 0);
    status << L",\"driverRuntimeLastAcquireResult\":\"" << HResultCode(driverRuntimeMapped ? driverRuntime.LastAcquireResult : S_OK) << L"\"";
    status << L",\"driverRuntimeLastFinishedResult\":\"" << HResultCode(driverRuntimeMapped ? driverRuntime.LastFinishedResult : S_OK) << L"\"";
    status << L",\"driverRuntimeLastWaitResult\":" << (driverRuntimeMapped ? driverRuntime.LastWaitResult : 0);
    status << L",\"driverSharedFrameStatusVersion\":" << (driverRuntimeMapped ? driverRuntime.SharedFrameStatusVersion : 0);
    status << L",\"driverProductVersion\":\"" << JsonEscape(driverRuntimeMapped ? BoundedWideString(driverRuntime.ProductVersion, ARRAYSIZE(driverRuntime.ProductVersion)) : std::wstring()) << L"\"";
    status << L",\"driverBuildTag\":\"" << JsonEscape(driverRuntimeMapped ? BoundedWideString(driverRuntime.BuildTag, ARRAYSIZE(driverRuntime.BuildTag)) : std::wstring()) << L"\"";
    status << L"}";
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        SetTimer(window, 1, 8, nullptr);
        return 0;
    case WM_TIMER:
        if (IsHostStopRequested(g_state))
        {
            DestroyWindow(window);
            return 0;
        }
        try
        {
            DemoteUnhealthySharedFrames(g_state);
            TryPromoteToSharedFrameSources(g_state);
        }
        catch (winrt::hresult_error const& error)
        {
            const std::wstring statusMessage = std::wstring(error.message()) + L"\n" + HResultMessage(error.code());
            WriteHostStatus(g_state, L"error", statusMessage);
        }
        RenderFrame(g_state);
        MaybeRefreshHostStatus(g_state);
        return 0;
    case WM_DISPLAYCHANGE:
        try
        {
            Configure(g_state);
        }
        catch (...)
        {
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        BridgeClientClick(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0, lParam);
        return 0;
    case WM_LBUTTONUP:
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        BridgeClientClick(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0, lParam);
        return 0;
    case WM_RBUTTONUP:
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        BridgeClientClick(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0, lParam);
        return 0;
    case WM_MBUTTONUP:
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        BridgeClientClick(
            MOUSEEVENTF_XDOWN,
            MOUSEEVENTF_XUP,
            GET_XBUTTON_WPARAM(wParam),
            lParam);
        return TRUE;
    case WM_XBUTTONUP:
        return TRUE;
    case WM_MOUSEWHEEL:
        BridgeWheel(window, false, wParam, lParam);
        return 0;
    case WM_MOUSEHWHEEL:
        BridgeWheel(window, true, wParam, lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_MOVE ||
            (wParam & 0xfff0) == SC_SIZE ||
            (wParam & 0xfff0) == SC_MAXIMIZE)
        {
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        WriteHostStatus(g_state, L"stopped", L"host stopped", false);
        g_state.FrameSources.clear();
        ResetSwapChain(g_state);
        CloseHostRunningMutex(g_state);
        CloseHostStopEvent(g_state);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

bool IsTransientDirectSetupError(HRESULT hr)
{
    return hr == DXGI_ERROR_MODE_CHANGE_IN_PROGRESS ||
           hr == HRESULT_FROM_WIN32(ERROR_RETRY);
}

DWORD DirectSetupRetryBackoffMs(DWORD attempt)
{
    static constexpr DWORD kBackoffMs[] = {
        1000,
        2000,
        5000,
        10000,
        15000,
        30000,
    };
    DWORD index = attempt - 1;
    const DWORD maxIndex = static_cast<DWORD>(ARRAYSIZE(kBackoffMs) - 1);
    if (index > maxIndex)
    {
        index = maxIndex;
    }
    return kBackoffMs[index];
}

bool WaitForHostStopOrTimeout(const AppState& state, DWORD timeoutMs)
{
    if (state.HostStopEvent != nullptr)
    {
        return WaitForSingleObject(state.HostStopEvent, timeoutMs) == WAIT_OBJECT_0;
    }
    Sleep(timeoutMs);
    return false;
}

void SetupDirectOutputWithRetry(AppState& state, DWORD timeoutMs)
{
    const DWORD startTick = GetTickCount();
    DWORD attempt = 0;
    state.DirectSetupRetryAttempts = 0;

    for (;;)
    {
        try
        {
            SetupDirectOutput(state);
            state.DirectSetupRetryAttempts = 0;
            return;
        }
        catch (winrt::hresult_error const& error)
        {
            const HRESULT hr = static_cast<HRESULT>(error.code());
            const DWORD elapsed = GetTickCount() - startTick;
            if (!IsTransientDirectSetupError(hr) ||
                elapsed >= timeoutMs ||
                IsHostStopRequested(state))
            {
                throw;
            }

            attempt++;
            state.DirectSetupRetryAttempts = attempt;
            DWORD delayMs = DirectSetupRetryBackoffMs(attempt);
            const DWORD remainingMs = timeoutMs - elapsed;
            if (delayMs > remainingMs)
            {
                delayMs = remainingMs;
            }
            const std::wstring statusMessage =
                std::wstring(error.message()) +
                L"\n" +
                HResultMessage(hr) +
                L"\nDisplayCore is still completing a mode change. Backing off before retry " +
                std::to_wstring(attempt) +
                L" for " +
                std::to_wstring(delayMs) +
                L" ms without reapplying display mode.";
            WriteHostStatus(state, L"recovering", statusMessage);
            ResetDirectOutput(state);
            if (WaitForHostStopOrTimeout(state, delayMs))
            {
                winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
            }
        }
    }
}

int RunDirectHost()
{
    g_state.DirectMode = true;

    g_state.HostRunningMutex = CreateLocalMutex(kHostRunningMutexName, TRUE);
    if (g_state.HostRunningMutex == nullptr)
    {
        winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(g_state.HostRunningMutex);
        g_state.HostRunningMutex = nullptr;
        return 0;
    }

    g_state.HostStopEvent = CreateLocalEvent(kHostStopEventName, TRUE, FALSE);
    if (g_state.HostStopEvent == nullptr)
    {
        winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
    }
    ResetEvent(g_state.HostStopEvent);

    g_state.Layout = LoadLayout(true);

    g_state.Host = {};
    g_state.Host.DeviceName = L"DisplayCore";
    g_state.Host.MonitorString = L"physical panel removed from desktop";
    g_state.Host.Rect = {
        0,
        0,
        static_cast<LONG>(g_state.Layout.HostWidth),
        static_cast<LONG>(g_state.Layout.Height)
    };

    g_state.DirectManager = displaycore::DisplayManager::Create(displaycore::DisplayManagerOptions::None);
    g_state.DirectTarget = FindDirectTarget(g_state, g_state.DirectManager);
    const LUID targetAdapterLuid = LuidFromDisplayAdapterId(g_state.DirectTarget.Adapter().Id());

    CreateD3DDevice(g_state, &targetAdapterLuid);
    StartFrameSources(g_state);
    WriteHostStatus(g_state, L"starting", L"setting up DisplayCore direct scanout");
    SetupDirectOutputWithRetry(g_state, 90 * 1000);

    WriteHostStatus(
        g_state,
        L"direct-shared",
        L"using DisplayCore direct scanout");

    DWORD recoveryAttempts = 0;
    while (!IsHostStopRequested(g_state))
    {
        try
        {
            DemoteUnhealthySharedFrames(g_state);
            TryPromoteToSharedFrameSources(g_state);
            RenderDirectFrame(g_state);
            MaybeRefreshHostStatus(g_state);
            if (g_state.DirectDevice != nullptr && g_state.DirectSource != nullptr)
            {
                g_state.DirectDevice.WaitForVBlank(g_state.DirectSource);
            }
            else
            {
                Sleep(8);
            }
            recoveryAttempts = 0;
        }
        catch (winrt::hresult_error const& error)
        {
            const HRESULT hr = static_cast<HRESULT>(error.code());
            g_state.DirectInLoopRecoveries++;
            g_state.LastDirectRecoveryResult = hr;
            const std::wstring statusMessage =
                std::wstring(error.message()) +
                L"\n" +
                HResultMessage(hr) +
                L"\nDirect render-loop recovery #" +
                std::to_wstring(g_state.DirectInLoopRecoveries) +
                L".";
            if (!IsHostStopRequested(g_state) && recoveryAttempts < 3)
            {
                recoveryAttempts++;
                WriteHostStatus(g_state, L"recovering", statusMessage);
                try
                {
                    ResetDirectOutput(g_state);
                    if (WaitForHostStopOrTimeout(g_state, 250))
                    {
                        continue;
                    }
                    SetupDirectOutputWithRetry(g_state, 60 * 1000);
                    continue;
                }
                catch (...)
                {
                }
            }

            WriteHostStatus(g_state, L"error", statusMessage, false);
            ResetDirectOutput(g_state);
            throw;
        }
    }

    WriteHostStatus(g_state, L"stopped", L"direct host stopped", false);
    g_state.FrameSources.clear();
    ResetDirectOutput(g_state);
    CloseHostRunningMutex(g_state);
    CloseHostStopEvent(g_state);
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    try
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (commandLine != nullptr && wcsstr(commandLine, L"--force-apply") != nullptr)
        {
            g_state.DirectModeApplyFallbackPending = true;
            g_state.DirectModeApplyAllowed = true;
        }
        if (commandLine != nullptr && wcsstr(commandLine, L"--no-mode-apply") != nullptr)
        {
            g_state.DirectModeApplyAllowed = false;
        }
        return RunDirectHost();
    }
    catch (winrt::hresult_error const& error)
    {
        const std::wstring message = std::wstring(error.message()) + L"\n" + HResultMessage(error.code());
        WriteHostStatus(g_state, L"error", message, false);
        if (g_state.DirectMode)
        {
            ResetDirectOutput(g_state);
        }
        CloseHostRunningMutex(g_state);
        CloseHostStopEvent(g_state);
        return 1;
    }
}
