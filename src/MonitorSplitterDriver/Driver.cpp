/*++

Copyright (c) Microsoft Corporation

Abstract:

    This module contains MonitorSplitter's UMDF indirect display driver implementation.

    MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

    User Mode, UMDF

--*/

#include "Driver.h"

#include <sddl.h>
#include <strsafe.h>

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

static constexpr wchar_t kDriverRuntimeLogPath[] = L"C:\\Windows\\Temp\\MonitorSplitterDriver.log";

static void DriverLogLine(const char* Message)
{
    char line[512] = {};
    HRESULT formatResult = StringCchPrintfA(
        line,
        ARRAYSIZE(line),
        "[%llu pid=%lu tid=%lu] %s\n",
        GetTickCount64(),
        GetCurrentProcessId(),
        GetCurrentThreadId(),
        Message);
    if (FAILED(formatResult))
    {
        return;
    }

    OutputDebugStringA(line);

    size_t length = 0;
    if (FAILED(StringCchLengthA(line, ARRAYSIZE(line), &length)) || length == 0)
    {
        return;
    }

    HANDLE file = CreateFileW(
        kDriverRuntimeLogPath,
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
    WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    CloseHandle(file);
}

static void DriverLogStatus(const char* Message, NTSTATUS Status)
{
    char line[512] = {};
    HRESULT formatResult = StringCchPrintfA(
        line,
        ARRAYSIZE(line),
        "%s status=0x%08lX",
        Message,
        static_cast<unsigned long>(Status));
    if (SUCCEEDED(formatResult))
    {
        DriverLogLine(line);
    }
}

static void DriverLogLuid(const char* Message, LUID Value)
{
    char line[512] = {};
    HRESULT formatResult = StringCchPrintfA(
        line,
        ARRAYSIZE(line),
        "%s luid=%lu:%ld",
        Message,
        static_cast<unsigned long>(Value.LowPart),
        Value.HighPart);
    if (SUCCEEDED(formatResult))
    {
        DriverLogLine(line);
    }
}

#pragma region SampleMonitors

static const GUID s_MonitorSplitterContainerIds[MonitorSplitter::kMaxMonitors] =
{
    { 0x82df50e0, 0xc371, 0x4d44, { 0x9f, 0x79, 0x28, 0x6d, 0x34, 0x90, 0x00, 0x01 } },
    { 0x82df50e0, 0xc371, 0x4d44, { 0x9f, 0x79, 0x28, 0x6d, 0x34, 0x90, 0x00, 0x02 } },
    { 0x82df50e0, 0xc371, 0x4d44, { 0x9f, 0x79, 0x28, 0x6d, 0x34, 0x90, 0x00, 0x03 } },
    { 0x82df50e0, 0xc371, 0x4d44, { 0x9f, 0x79, 0x28, 0x6d, 0x34, 0x90, 0x00, 0x04 } },
    { 0x82df50e0, 0xc371, 0x4d44, { 0x9f, 0x79, 0x28, 0x6d, 0x34, 0x90, 0x00, 0x05 } },
    { 0x82df50e0, 0xc371, 0x4d44, { 0x9f, 0x79, 0x28, 0x6d, 0x34, 0x90, 0x00, 0x06 } },
    { 0x82df50e0, 0xc371, 0x4d44, { 0x9f, 0x79, 0x28, 0x6d, 0x34, 0x90, 0x00, 0x07 } },
    { 0x82df50e0, 0xc371, 0x4d44, { 0x9f, 0x79, 0x28, 0x6d, 0x34, 0x90, 0x00, 0x08 } },
};

static DWORD GetModeCount(const MonitorSplitter::MonitorMode& Mode)
{
    return Mode.Refresh == 60 ? 1 : 2;
}

static MonitorSplitter::MonitorMode GetModeByIndex(const MonitorSplitter::MonitorMode& Mode, DWORD ModeIndex)
{
    if (ModeIndex == 0)
    {
        return Mode;
    }

    return { Mode.Width, Mode.Height, 60 };
}

static NTSTATUS QueryLayout(WDFDEVICE Device, MonitorSplitter::Layout& Layout)
{
    WDF_DEVICE_PROPERTY_DATA propertyData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &MonitorSplitter::kLayoutPropertyKey);

    ULONG requiredSize = 0;
    DEVPROPTYPE propertyType = DEVPROP_TYPE_EMPTY;
    NTSTATUS status = WdfDeviceQueryPropertyEx(Device, &propertyData, 0, nullptr, &requiredSize, &propertyType);
    if (requiredSize == 0)
    {
        DriverLogStatus("QueryLayout missing layout property", status);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    std::vector<wchar_t> buffer((requiredSize / sizeof(wchar_t)) + 1, L'\0');
    status = WdfDeviceQueryPropertyEx(
        Device,
        &propertyData,
        static_cast<ULONG>(buffer.size() * sizeof(wchar_t)),
        buffer.data(),
        &requiredSize,
        &propertyType);

    if (!NT_SUCCESS(status))
    {
        DriverLogStatus("QueryLayout WdfDeviceQueryPropertyEx", status);
        return status;
    }

    if (propertyType != DEVPROP_TYPE_STRING)
    {
        DriverLogStatus("QueryLayout invalid property type", static_cast<NTSTATUS>(propertyType));
        return STATUS_INVALID_PARAMETER;
    }

    std::wstring error;
    if (!MonitorSplitter::ParseLayoutSpec(buffer.data(), Layout, &error))
    {
        DriverLogLine("QueryLayout parse failed");
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static bool IsZeroLuid(LUID Value)
{
    return Value.LowPart == 0 && Value.HighPart == 0;
}

static NTSTATUS QueryPreferredRenderAdapterLuid(WDFDEVICE Device, LUID& PreferredRenderAdapter, bool& HasPreferredRenderAdapter)
{
    PreferredRenderAdapter = {};
    HasPreferredRenderAdapter = false;

    WDF_DEVICE_PROPERTY_DATA propertyData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &MonitorSplitter::kRenderAdapterLuidPropertyKey);

    ULONG requiredSize = 0;
    DEVPROPTYPE propertyType = DEVPROP_TYPE_EMPTY;
    NTSTATUS status = WdfDeviceQueryPropertyEx(Device, &propertyData, 0, nullptr, &requiredSize, &propertyType);
    if (requiredSize == 0)
    {
        DriverLogStatus("QueryPreferredRenderAdapter missing property", status);
        return STATUS_SUCCESS;
    }

    if (requiredSize != sizeof(LUID))
    {
        DriverLogStatus("QueryPreferredRenderAdapter invalid size", static_cast<NTSTATUS>(requiredSize));
        return STATUS_INVALID_PARAMETER;
    }

    LUID value = {};
    status = WdfDeviceQueryPropertyEx(
        Device,
        &propertyData,
        sizeof(value),
        &value,
        &requiredSize,
        &propertyType);
    if (!NT_SUCCESS(status))
    {
        DriverLogStatus("QueryPreferredRenderAdapter WdfDeviceQueryPropertyEx", status);
        return status;
    }

    if (propertyType != DEVPROP_TYPE_BINARY)
    {
        DriverLogStatus("QueryPreferredRenderAdapter invalid property type", static_cast<NTSTATUS>(propertyType));
        return STATUS_INVALID_PARAMETER;
    }

    if (IsZeroLuid(value))
    {
        DriverLogLine("QueryPreferredRenderAdapter zero LUID ignored");
        return STATUS_SUCCESS;
    }

    PreferredRenderAdapter = value;
    HasPreferredRenderAdapter = true;
    DriverLogLuid("QueryPreferredRenderAdapter resolved", PreferredRenderAdapter);
    return STATUS_SUCCESS;
}

static NTSTATUS QueryEdidNameBase(WDFDEVICE Device, std::string& EdidNameBase)
{
    EdidNameBase.clear();

    WDF_DEVICE_PROPERTY_DATA propertyData;
    WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &MonitorSplitter::kEdidNameBasePropertyKey);

    ULONG requiredSize = 0;
    DEVPROPTYPE propertyType = DEVPROP_TYPE_EMPTY;
    NTSTATUS status = WdfDeviceQueryPropertyEx(Device, &propertyData, 0, nullptr, &requiredSize, &propertyType);
    if (requiredSize == 0)
    {
        DriverLogStatus("QueryEdidNameBase missing property", status);
        return STATUS_SUCCESS;
    }

    std::vector<wchar_t> buffer((requiredSize / sizeof(wchar_t)) + 1, L'\0');
    status = WdfDeviceQueryPropertyEx(
        Device,
        &propertyData,
        static_cast<ULONG>(buffer.size() * sizeof(wchar_t)),
        buffer.data(),
        &requiredSize,
        &propertyType);
    if (!NT_SUCCESS(status))
    {
        DriverLogStatus("QueryEdidNameBase WdfDeviceQueryPropertyEx", status);
        return status;
    }

    if (propertyType != DEVPROP_TYPE_STRING)
    {
        DriverLogStatus("QueryEdidNameBase invalid property type", static_cast<NTSTATUS>(propertyType));
        return STATUS_INVALID_PARAMETER;
    }

    EdidNameBase = MonitorSplitter::SanitizeEdidNameBase(std::wstring(buffer.data()));
    return STATUS_SUCCESS;
}

static NTSTATUS ValidateSyntheticEdids(const MonitorSplitter::Layout& Layout, const std::string& EdidNameBase)
{
    for (size_t index = 0; index < Layout.Monitors.size(); index++)
    {
        std::array<BYTE, MonitorSplitter::kSyntheticEdidSize> edid = {};
        if (!MonitorSplitter::BuildSyntheticEdid(Layout.Monitors[index], Layout.Monitors.size(), index, edid, EdidNameBase) ||
            !MonitorSplitter::IsSyntheticEdidChecksumValid(edid))
        {
            DriverLogStatus("ValidateSyntheticEdids failed index", static_cast<NTSTATUS>(index));
            return STATUS_INVALID_PARAMETER;
        }
    }

    return STATUS_SUCCESS;
}

class LocalSecurityDescriptor
{
public:
    explicit LocalSecurityDescriptor(PCWSTR Sddl)
    {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                Sddl,
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

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD MonitorSplitterDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY MonitorSplitterDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED MonitorSplitterAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES MonitorSplitterAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION MonitorSplitterParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES MonitorSplitterMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES MonitorSplitterMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN MonitorSplitterMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN MonitorSplitterMonitorUnassignSwapChain;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext = nullptr;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext = nullptr;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

static IndirectDeviceContext* GetDeviceContextOrNull(WDFOBJECT Object)
{
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
    return pContext != nullptr ? pContext->pContext : nullptr;
}

static IndirectMonitorContext* GetMonitorContextOrNull(WDFOBJECT Object)
{
    auto* pContext = WdfObjectGet_IndirectMonitorContextWrapper(Object);
    return pContext != nullptr ? pContext->pContext : nullptr;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT  pDriverObject,
    PUNICODE_STRING pRegistryPath
)
{
    DriverLogLine("DriverEntry enter");

    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config,
        MonitorSplitterDeviceAdd
    );

    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    DriverLogStatus("DriverEntry WdfDriverCreate", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return Status;
}

_Use_decl_annotations_
NTSTATUS MonitorSplitterDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    DriverLogLine("DeviceAdd enter");

    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    // Register for power callbacks - in this sample only power-on is needed
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = MonitorSplitterDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    // If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
    // redirects IoDeviceControl requests to an internal queue. This sample does not need this.
    // IddConfig.EvtIddCxDeviceIoControl = MonitorSplitterIoDeviceControl;

    IddConfig.EvtIddCxAdapterInitFinished = MonitorSplitterAdapterInitFinished;

    IddConfig.EvtIddCxParseMonitorDescription = MonitorSplitterParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = MonitorSplitterMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = MonitorSplitterMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = MonitorSplitterAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = MonitorSplitterMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = MonitorSplitterMonitorUnassignSwapChain;

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    DriverLogStatus("DeviceAdd IddCxDeviceInitConfig", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    DriverLogStatus("DeviceAdd WdfDeviceCreate", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    DriverLogStatus("DeviceAdd IddCxDeviceInitialize", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    MonitorSplitter::Layout layout;
    Status = QueryLayout(Device, layout);
    DriverLogStatus("DeviceAdd QueryLayout", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    std::string edidNameBase;
    Status = QueryEdidNameBase(Device, edidNameBase);
    DriverLogStatus("DeviceAdd QueryEdidNameBase", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = ValidateSyntheticEdids(layout, edidNameBase);
    DriverLogStatus("DeviceAdd ValidateSyntheticEdids", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    LUID preferredRenderAdapter = {};
    bool hasPreferredRenderAdapter = false;
    Status = QueryPreferredRenderAdapterLuid(Device, preferredRenderAdapter, hasPreferredRenderAdapter);
    DriverLogStatus("DeviceAdd QueryPreferredRenderAdapterLuid", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Create a new device context object and attach it to the WDF device object
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    if (pContext == nullptr)
    {
        DriverLogLine("DeviceAdd missing device context wrapper");
        return STATUS_INVALID_DEVICE_STATE;
    }

    try
    {
        pContext->pContext = new IndirectDeviceContext(
            Device,
            std::move(layout),
            std::move(edidNameBase),
            preferredRenderAdapter,
            hasPreferredRenderAdapter);
    }
    catch (...)
    {
        DriverLogLine("DeviceAdd context allocation failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DriverLogLine("DeviceAdd complete");

    return Status;
}

_Use_decl_annotations_
NTSTATUS MonitorSplitterDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    DriverLogLine("DeviceD0Entry enter");

    // This function is called by WDF to start the device in the fully-on power state.

    auto* pContext = GetDeviceContextOrNull(Device);
    if (pContext == nullptr)
    {
        DriverLogLine("DeviceD0Entry missing device context");
        return STATUS_INVALID_DEVICE_STATE;
    }

    const NTSTATUS status = pContext->InitAdapter();
    DriverLogStatus("DeviceD0Entry InitAdapter", status);

    return status;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{

}

Direct3DDevice::Direct3DDevice()
{
    AdapterLuid = LUID{};
}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if (FAILED(hr))
    {
        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        return hr;
    }

    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent, UINT ConnectorIndex)
    : m_hSwapChain(hSwapChain),
      m_Device(Device),
      m_hAvailableBufferEvent(NewFrameEvent),
      m_ConnectorIndex(ConnectorIndex)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    if (!m_hTerminateEvent.Get())
    {
        return;
    }

    // Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Alert the swap-chain processing thread to terminate
    if (m_hTerminateEvent.Get())
    {
        SetEvent(m_hTerminateEvent.Get());
    }

    if (m_hThread.Get())
    {
        // Wait for the thread to terminate
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }

    ResetSharedFrameTexture();

    if (m_SharedFrameStatus != nullptr)
    {
        UnmapViewOfFile(m_SharedFrameStatus);
        m_SharedFrameStatus = nullptr;
    }
    if (m_SharedFrameStatusMapping != nullptr)
    {
        CloseHandle(m_SharedFrameStatusMapping);
        m_SharedFrameStatusMapping = nullptr;
    }
    if (m_DriverRuntimeStatus != nullptr)
    {
        UnmapViewOfFile(m_DriverRuntimeStatus);
        m_DriverRuntimeStatus = nullptr;
    }
    if (m_DriverRuntimeStatusMapping != nullptr)
    {
        CloseHandle(m_DriverRuntimeStatusMapping);
        m_DriverRuntimeStatusMapping = nullptr;
    }
}

bool SwapChainProcessor::IsRunning() const
{
    return m_hThread.Get() != nullptr;
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
    // prioritize this thread for improved throughput in high CPU-load scenarios.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    RunCore();

    // Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
    // provide a new swap-chain if necessary.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;

    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::EnsureSharedFrameStatus()
{
    UpdateDriverRuntimeStatus();

    if (m_SharedFrameStatus != nullptr)
    {
        return;
    }

    LocalSecurityDescriptor SecurityDescriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)");
    if (FAILED(SecurityDescriptor.Status()))
    {
        return;
    }

    m_SharedFrameStatusMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        SecurityDescriptor.Attributes(),
        PAGE_READWRITE,
        0,
        sizeof(MonitorSplitter::SharedFrameStatus),
        MonitorSplitter::SharedFrameStatusName(m_ConnectorIndex).c_str());
    if (m_SharedFrameStatusMapping == nullptr)
    {
        return;
    }

    m_SharedFrameStatus = static_cast<MonitorSplitter::SharedFrameStatus*>(MapViewOfFile(
        m_SharedFrameStatusMapping,
        FILE_MAP_WRITE,
        0,
        0,
        sizeof(MonitorSplitter::SharedFrameStatus)));
    if (m_SharedFrameStatus == nullptr)
    {
        CloseHandle(m_SharedFrameStatusMapping);
        m_SharedFrameStatusMapping = nullptr;
        return;
    }

    *m_SharedFrameStatus = {};
    m_SharedFrameStatus->Magic = MonitorSplitter::kSharedFrameStatusMagic;
    m_SharedFrameStatus->Version = MonitorSplitter::kSharedFrameStatusVersion;
    m_SharedFrameStatus->ConnectorIndex = m_ConnectorIndex;
    m_SharedFrameStatus->ProducerAdapterLuidLow = m_Device->AdapterLuid.LowPart;
    m_SharedFrameStatus->ProducerAdapterLuidHigh = m_Device->AdapterLuid.HighPart;
    m_SharedFrameStatus->ProducerProcessId = GetCurrentProcessId();
    m_SharedFrameStatus->SharedHandleValue = 0;
}

void SwapChainProcessor::EnsureDriverRuntimeStatus()
{
    if (m_DriverRuntimeStatus != nullptr)
    {
        return;
    }

    LocalSecurityDescriptor SecurityDescriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)");
    if (FAILED(SecurityDescriptor.Status()))
    {
        return;
    }

    m_DriverRuntimeStatusMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        SecurityDescriptor.Attributes(),
        PAGE_READWRITE,
        0,
        sizeof(MonitorSplitter::DriverRuntimeStatus),
        MonitorSplitter::DriverRuntimeStatusName(m_ConnectorIndex).c_str());
    if (m_DriverRuntimeStatusMapping == nullptr)
    {
        return;
    }

    m_DriverRuntimeStatus = static_cast<MonitorSplitter::DriverRuntimeStatus*>(MapViewOfFile(
        m_DriverRuntimeStatusMapping,
        FILE_MAP_WRITE,
        0,
        0,
        sizeof(MonitorSplitter::DriverRuntimeStatus)));
    if (m_DriverRuntimeStatus == nullptr)
    {
        CloseHandle(m_DriverRuntimeStatusMapping);
        m_DriverRuntimeStatusMapping = nullptr;
        return;
    }

    *m_DriverRuntimeStatus = {};
}

void SwapChainProcessor::UpdateDriverRuntimeStatus()
{
    EnsureDriverRuntimeStatus();
    if (m_DriverRuntimeStatus == nullptr)
    {
        return;
    }

    m_DriverRuntimeStatus->Magic = MonitorSplitter::kDriverRuntimeStatusMagic;
    m_DriverRuntimeStatus->Version = MonitorSplitter::kDriverRuntimeStatusVersion;
    m_DriverRuntimeStatus->ConnectorIndex = m_ConnectorIndex;
    m_DriverRuntimeStatus->ProducerProcessId = GetCurrentProcessId();
    m_DriverRuntimeStatus->SharedFrameStatusVersion = MonitorSplitter::kSharedFrameStatusVersion;
    m_DriverRuntimeStatus->UpdatedTick = GetTickCount64();
    m_DriverRuntimeStatus->Heartbeats++;
    StringCchCopyW(
        m_DriverRuntimeStatus->ProductVersion,
        ARRAYSIZE(m_DriverRuntimeStatus->ProductVersion),
        MonitorSplitter::kProductVersionWide);
    StringCchCopyW(
        m_DriverRuntimeStatus->BuildTag,
        ARRAYSIZE(m_DriverRuntimeStatus->BuildTag),
        MonitorSplitter::kBuildTagWide);
}

void SwapChainProcessor::UpdateSharedFrameStatus(
    const D3D11_TEXTURE2D_DESC* SourceDesc,
    HRESULT CreateResult,
    HRESULT PublishResult,
    HRESULT AcquireResult,
    bool Published)
{
    EnsureSharedFrameStatus();
    UpdateDriverRuntimeStatus();
    if (m_SharedFrameStatus == nullptr)
    {
        return;
    }

    m_SharedFrameStatus->Magic = MonitorSplitter::kSharedFrameStatusMagic;
    m_SharedFrameStatus->Version = MonitorSplitter::kSharedFrameStatusVersion;
    m_SharedFrameStatus->ConnectorIndex = m_ConnectorIndex;
    m_SharedFrameStatus->ProducerAdapterLuidLow = m_Device->AdapterLuid.LowPart;
    m_SharedFrameStatus->ProducerAdapterLuidHigh = m_Device->AdapterLuid.HighPart;
    m_SharedFrameStatus->ProducerProcessId = GetCurrentProcessId();
    m_SharedFrameStatus->SharedHandleValue = reinterpret_cast<ULONGLONG>(m_SharedFrameHandle);
    if (SourceDesc != nullptr)
    {
        m_SharedFrameStatus->LastSourceWidth = SourceDesc->Width;
        m_SharedFrameStatus->LastSourceHeight = SourceDesc->Height;
    }
    if (m_HasSharedFrameDesc)
    {
        m_SharedFrameStatus->LastSharedWidth = m_SharedFrameDesc.Width;
        m_SharedFrameStatus->LastSharedHeight = m_SharedFrameDesc.Height;
        m_SharedFrameStatus->LastSharedFormat = static_cast<DWORD>(m_SharedFrameDesc.Format);
    }
    m_SharedFrameStatus->LastCreateResult = CreateResult;
    m_SharedFrameStatus->LastPublishResult = PublishResult;
    m_SharedFrameStatus->LastAcquireResult = AcquireResult;
    m_SharedFrameStatus->PublishAttempts++;
    if (Published)
    {
        m_SharedFrameStatus->PublishedFrames++;
        m_SharedFrameStatus->LastPublishedTick = GetTickCount64();
    }
    else
    {
        m_SharedFrameStatus->PublishFailures++;
    }
}

void SwapChainProcessor::ResetSharedFrameTexture()
{
    if (m_SharedFrameHandle != nullptr)
    {
        CloseHandle(m_SharedFrameHandle);
        m_SharedFrameHandle = nullptr;
    }
    m_SharedFrameMutex.Reset();
    m_SharedFrameTexture.Reset();
    m_SharedFrameDesc = {};
    m_HasSharedFrameDesc = false;
}

HRESULT SwapChainProcessor::EnsureSharedFrameTexture(const D3D11_TEXTURE2D_DESC& SourceDesc)
{
    EnsureSharedFrameStatus();

    if (m_SharedFrameDisabled)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }

    if (m_SharedFrameTexture != nullptr &&
        m_HasSharedFrameDesc &&
        m_SharedFrameDesc.Width == SourceDesc.Width &&
        m_SharedFrameDesc.Height == SourceDesc.Height &&
        m_SharedFrameDesc.Format == SourceDesc.Format)
    {
        return S_OK;
    }

    ResetSharedFrameTexture();

    D3D11_TEXTURE2D_DESC SharedDesc = SourceDesc;
    SharedDesc.MipLevels = 1;
    SharedDesc.ArraySize = 1;
    SharedDesc.SampleDesc.Count = 1;
    SharedDesc.SampleDesc.Quality = 0;
    SharedDesc.Usage = D3D11_USAGE_DEFAULT;
    SharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    SharedDesc.CPUAccessFlags = 0;
    SharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ComPtr<ID3D11Texture2D> SharedFrameTexture;
    HRESULT hr = m_Device->Device->CreateTexture2D(&SharedDesc, nullptr, &SharedFrameTexture);
    if (FAILED(hr))
    {
        m_SharedFrameDisabled = true;
        return hr;
    }

    ComPtr<IDXGIResource1> SharedResource;
    hr = SharedFrameTexture.As(&SharedResource);
    if (FAILED(hr))
    {
        m_SharedFrameDisabled = true;
        return hr;
    }

    ComPtr<IDXGIKeyedMutex> SharedFrameMutex;
    hr = SharedFrameTexture.As(&SharedFrameMutex);
    if (FAILED(hr))
    {
        m_SharedFrameDisabled = true;
        return hr;
    }

    LocalSecurityDescriptor SecurityDescriptor(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;AU)");
    hr = SecurityDescriptor.Status();
    if (FAILED(hr))
    {
        m_SharedFrameDisabled = true;
        return hr;
    }

    HANDLE SharedHandle = nullptr;
    const std::wstring SharedName = MonitorSplitter::SharedFrameTextureName(m_ConnectorIndex);
    hr = SharedResource->CreateSharedHandle(
        SecurityDescriptor.Attributes(),
        DXGI_SHARED_RESOURCE_READ,
        SharedName.c_str(),
        &SharedHandle);
    if (FAILED(hr))
    {
        ResetSharedFrameTexture();
        if (hr != DXGI_ERROR_NAME_ALREADY_EXISTS)
        {
            m_SharedFrameDisabled = true;
        }
        return hr;
    }

    m_SharedFrameHandle = SharedHandle;
    m_SharedFrameTexture = SharedFrameTexture;
    m_SharedFrameMutex = SharedFrameMutex;
    m_SharedFrameDesc = SharedDesc;
    m_HasSharedFrameDesc = true;
    return S_OK;
}

HRESULT SwapChainProcessor::PublishFrame(ID3D11Texture2D* SourceTexture)
{
    if (SourceTexture == nullptr)
    {
        UpdateSharedFrameStatus(nullptr, E_POINTER, E_POINTER, S_OK, false);
        return E_POINTER;
    }

    D3D11_TEXTURE2D_DESC SourceDesc = {};
    SourceTexture->GetDesc(&SourceDesc);

    HRESULT createHr = EnsureSharedFrameTexture(SourceDesc);
    if (FAILED(createHr))
    {
        UpdateSharedFrameStatus(&SourceDesc, createHr, createHr, S_OK, false);
        return createHr;
    }

    if (m_SharedFrameMutex == nullptr)
    {
        UpdateSharedFrameStatus(&SourceDesc, createHr, E_FAIL, S_OK, false);
        return E_FAIL;
    }

    HRESULT acquireHr = m_SharedFrameMutex->AcquireSync(0, 2);
    if (FAILED(acquireHr))
    {
        UpdateSharedFrameStatus(&SourceDesc, createHr, acquireHr, acquireHr, false);
        return acquireHr;
    }

    m_Device->DeviceContext->CopyResource(m_SharedFrameTexture.Get(), SourceTexture);
    const HRESULT releaseHr = m_SharedFrameMutex->ReleaseSync(0);
    UpdateSharedFrameStatus(&SourceDesc, createHr, releaseHr, acquireHr, SUCCEEDED(releaseHr));
    return releaseHr;
}

void SwapChainProcessor::RunCore()
{
    auto updateRuntime = [this](HRESULT acquireResult, DWORD waitResult = WAIT_TIMEOUT, HRESULT finishedResult = S_OK) {
        UpdateDriverRuntimeStatus();
        if (m_DriverRuntimeStatus != nullptr)
        {
            m_DriverRuntimeStatus->LastAcquireResult = acquireResult;
            m_DriverRuntimeStatus->LastWaitResult = waitResult;
            m_DriverRuntimeStatus->LastFinishedResult = finishedResult;
        }
    };

    // Get the DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        updateRuntime(hr);
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        updateRuntime(hr);
        return;
    }
    updateRuntime(S_OK);

    // Acquire and release buffers in a loop
    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        // Ask for the next buffer from the producer
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        // AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
        if (hr == E_PENDING)
        {
            // We must wait for a new buffer
            HANDLE WaitHandles [] =
            {
                m_hAvailableBufferEvent,
                m_hTerminateEvent.Get()
            };
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            updateRuntime(hr, WaitResult);
            if (m_DriverRuntimeStatus != nullptr)
            {
                m_DriverRuntimeStatus->PendingFrames++;
            }
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
            {
                // We have a new buffer, so try the AcquireBuffer again
                continue;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1)
            {
                // We need to terminate
                break;
            }
            else
            {
                // The wait was cancelled or something unexpected happened
                hr = HRESULT_FROM_WIN32(WaitResult);
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
            updateRuntime(hr);
            if (m_DriverRuntimeStatus != nullptr)
            {
                m_DriverRuntimeStatus->AcquiredFrames++;
            }

            // We have new frame to process, the surface has a reference on it that the driver has to release
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

            // We have finished processing this frame hence we release the reference on it.
            // If the driver forgets to release the reference to the surface, it will be leaked which results in the
            // surfaces being left around after swapchain is destroyed.
            // NOTE: Although in this sample we release reference to the surface here; the driver still
            // owns the Buffer.MetaData.pSurface surface until IddCxSwapChainReleaseAndAcquireBuffer returns
            // S_OK and gives us a new frame, a driver may want to use the surface in future to re-encode the desktop 
            // for better quality if there is no new frame for a while
            ComPtr<ID3D11Texture2D> AcquiredTexture;
            if (SUCCEEDED(AcquiredBuffer.As(&AcquiredTexture)))
            {
                // PublishFrame performs the minimal GPU-side copy into the shared texture consumed by the direct host.
                PublishFrame(AcquiredTexture.Get());
            }
            AcquiredBuffer.Reset();
            
            // Indicate to OS that we have finished inital processing of the frame, it is a hint that
            // OS could start preparing another frame
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            updateRuntime(S_OK, WAIT_OBJECT_0, hr);
            if (m_DriverRuntimeStatus != nullptr)
            {
                m_DriverRuntimeStatus->LastFinishedResult = hr;
                if (SUCCEEDED(hr))
                {
                    m_DriverRuntimeStatus->FinishedFrames++;
                }
            }
            if (FAILED(hr))
            {
                break;
            }

            // There is no asynchronous encode/send pipeline in MonitorSplitter; the shared texture publication above is
            // complete before the frame is released back to IddCx.
        }
        else
        {
            updateRuntime(hr);
            if (m_DriverRuntimeStatus != nullptr)
            {
                m_DriverRuntimeStatus->FailedAcquires++;
            }
            // The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
            break;
        }
    }
    UpdateDriverRuntimeStatus();
    if (m_DriverRuntimeStatus != nullptr)
    {
        m_DriverRuntimeStatus->Exits++;
        m_DriverRuntimeStatus->LastAcquireResult = hr;
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice, MonitorSplitter::Layout Layout, std::string EdidNameBase, LUID PreferredRenderAdapter, bool HasPreferredRenderAdapter) :
    m_WdfDevice(WdfDevice),
    m_Layout(std::move(Layout)),
    m_EdidNameBase(std::move(EdidNameBase)),
    m_PreferredRenderAdapter(PreferredRenderAdapter),
    m_HasPreferredRenderAdapter(HasPreferredRenderAdapter)
{
    DriverLogLine("IndirectDeviceContext construct");

    m_Adapter = {};
    m_EdidBlocks.resize(m_Layout.Monitors.size());
    m_EdidAvailable.resize(m_Layout.Monitors.size(), false);
    m_MonitorObjects.resize(m_Layout.Monitors.size(), nullptr);
    for (size_t index = 0; index < m_Layout.Monitors.size(); index++)
    {
        m_EdidAvailable[index] = MonitorSplitter::BuildSyntheticEdid(
            m_Layout.Monitors[index],
            m_Layout.Monitors.size(),
            index,
            m_EdidBlocks[index],
            m_EdidNameBase);
    }
    DriverLogStatus("IndirectDeviceContext monitor count", static_cast<NTSTATUS>(m_Layout.Monitors.size()));
}

IndirectDeviceContext::~IndirectDeviceContext()
{
    DepartMonitors();
}

NTSTATUS IndirectDeviceContext::InitAdapter()
{
    DriverLogLine("InitAdapter enter");

    if (m_AdapterInitialized)
    {
        DriverLogLine("InitAdapter already initialized");
        return STATUS_SUCCESS;
    }

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    // Declare basic feature support for the adapter (required)
    AdapterCaps.MaxMonitorsSupported = static_cast<UINT>(m_Layout.Monitors.size());
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // Declare your device strings for telemetry (required)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"MonitorSplitter Virtual Adapter";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"MonitorSplitter";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"MonitorSplitter Split Display";

    // Declare your hardware and firmware versions (required)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    // Initialize a WDF context that can store a pointer to the device context object
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    // Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);
    DriverLogStatus("InitAdapter IddCxAdapterInitAsync", Status);

    if (NT_SUCCESS(Status))
    {
        // Store a reference to the WDF adapter handle
        m_Adapter = AdapterInitOut.AdapterObject;
        m_AdapterInitialized = true;

        if (m_HasPreferredRenderAdapter)
        {
            IDARG_IN_ADAPTERSETRENDERADAPTER renderAdapter = {};
            renderAdapter.PreferredRenderAdapter = m_PreferredRenderAdapter;
            DriverLogLuid("InitAdapter IddCxAdapterSetRenderAdapter", m_PreferredRenderAdapter);
            IddCxAdapterSetRenderAdapter(m_Adapter, &renderAdapter);
        }
        else
        {
            DriverLogLine("InitAdapter no preferred render adapter");
        }

        // Store the device context object into the WDF object context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        if (pContext == nullptr)
        {
            DriverLogLine("InitAdapter missing adapter context wrapper");
            return STATUS_INVALID_DEVICE_STATE;
        }
        pContext->pContext = this;
    }

    return Status;
}

NTSTATUS IndirectDeviceContext::PlugMonitors()
{
    for (DWORD i = 0; i < GetMonitorCount(); i++)
    {
        const NTSTATUS status = FinishInit(i);
        if (!NT_SUCCESS(status))
        {
            DriverLogStatus("PlugMonitors failed", status);
            DepartMonitors();
            return status;
        }
    }

    return STATUS_SUCCESS;
}

void IndirectDeviceContext::DepartMonitors()
{
    for (size_t index = m_MonitorObjects.size(); index > 0; index--)
    {
        IDDCX_MONITOR monitor = m_MonitorObjects[index - 1];
        if (monitor == nullptr)
        {
            continue;
        }

        const NTSTATUS departureStatus = IddCxMonitorDeparture(monitor);
        DriverLogStatus("DepartMonitors IddCxMonitorDeparture", departureStatus);
        WdfObjectDelete((WDFOBJECT)monitor);
        m_MonitorObjects[index - 1] = nullptr;
    }
}

NTSTATUS IndirectDeviceContext::FinishInit(UINT ConnectorIndex)
{
    DriverLogStatus("FinishInit connector", static_cast<NTSTATUS>(ConnectorIndex));

    if (ConnectorIndex >= m_Layout.Monitors.size() ||
        ConnectorIndex >= m_EdidAvailable.size() ||
        !m_EdidAvailable[ConnectorIndex])
    {
        DriverLogLine("FinishInit invalid connector or EDID");
        return STATUS_INVALID_PARAMETER;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        auto* pContext = WdfObjectGet_IndirectMonitorContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    // Virtual split monitors are all reported immediately when the indirect adapter starts.
    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = ConnectorIndex;

    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    DriverLogLine("FinishInit EDID available");
    MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(m_EdidBlocks[ConnectorIndex].size());
    MonitorInfo.MonitorDescription.pData = m_EdidBlocks[ConnectorIndex].data();

    MonitorInfo.MonitorContainerId = s_MonitorSplitterContainerIds[ConnectorIndex % MonitorSplitter::kMaxMonitors];

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    // Create a monitor object with the specified monitor descriptor
    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    DriverLogStatus("FinishInit IddCxMonitorCreate", Status);
    if (NT_SUCCESS(Status))
    {
        // Create a new monitor context object and attach it to the Idd monitor object
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
        if (pMonitorContextWrapper == nullptr)
        {
            DriverLogLine("FinishInit missing monitor context wrapper");
            WdfObjectDelete((WDFOBJECT)MonitorCreateOut.MonitorObject);
            return STATUS_INVALID_DEVICE_STATE;
        }

        try
        {
            pMonitorContextWrapper->pContext = new IndirectMonitorContext(
                MonitorCreateOut.MonitorObject,
                ConnectorIndex,
                m_Layout.Monitors[ConnectorIndex]);
        }
        catch (...)
        {
            DriverLogLine("FinishInit monitor context allocation failed");
            WdfObjectDelete((WDFOBJECT)MonitorCreateOut.MonitorObject);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        // Tell the OS that the monitor has been plugged in
        IDARG_OUT_MONITORARRIVAL ArrivalOut;
        Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
        DriverLogStatus("FinishInit IddCxMonitorArrival", Status);
        if (NT_SUCCESS(Status))
        {
            m_MonitorObjects[ConnectorIndex] = MonitorCreateOut.MonitorObject;
        }
        else
        {
            WdfObjectDelete((WDFOBJECT)MonitorCreateOut.MonitorObject);
        }
    }

    return Status;
}

size_t IndirectDeviceContext::GetMonitorCount() const
{
    return m_Layout.Monitors.size();
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, _In_ UINT ConnectorIndex, MonitorSplitter::MonitorMode Mode) :
    m_Monitor(Monitor),
    m_ConnectorIndex(ConnectorIndex),
    m_Mode(Mode)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

UINT IndirectMonitorContext::GetConnectorIndex() const
{
    return m_ConnectorIndex;
}

MonitorSplitter::MonitorMode IndirectMonitorContext::GetMode() const
{
    return m_Mode;
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    shared_ptr<Direct3DDevice> Device;
    try
    {
        Device = make_shared<Direct3DDevice>(RenderAdapter);
    }
    catch (...)
    {
        DriverLogLine("AssignSwapChain device allocation failed");
        WdfObjectDelete(SwapChain);
        return;
    }

    if (FAILED(Device->Init()))
    {
        // It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
        // swap-chain and try again.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        unique_ptr<SwapChainProcessor> Processor;
        try
        {
            Processor.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent, m_ConnectorIndex));
        }
        catch (...)
        {
            DriverLogLine("AssignSwapChain processor allocation failed");
            WdfObjectDelete(SwapChain);
            return;
        }

        if (Processor == nullptr || !Processor->IsRunning())
        {
            DriverLogLine("AssignSwapChain processor did not start");
            WdfObjectDelete(SwapChain);
            return;
        }

        m_ProcessingThread = std::move(Processor);
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    // Stop processing the last swap-chain
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS MonitorSplitterAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    // This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
    // to report attached monitors.

    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        auto* pDeviceContext = GetDeviceContextOrNull(AdapterObject);
        if (pDeviceContext == nullptr)
        {
            DriverLogLine("AdapterInitFinished missing device context");
            return STATUS_INVALID_DEVICE_STATE;
        }
        return pDeviceContext->PlugMonitors();
    }

    return pInArgs->AdapterInitStatus;
}

_Use_decl_annotations_
NTSTATUS MonitorSplitterAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // MonitorSplitter exposes one generated mode per split. IddCx handles swap-chain assignment for active paths, and
    // inactive paths arrive through MonitorUnassignSwapChain, so there is no extra hardware mode programming here.

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorSplitterParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    MonitorSplitter::MonitorMode Mode;
    if (!MonitorSplitter::DecodeSyntheticEdidMode(
            static_cast<const BYTE*>(pInArgs->MonitorDescription.pData),
            pInArgs->MonitorDescription.DataSize,
            Mode))
    {
        return STATUS_INVALID_PARAMETER;
    }

    const DWORD ModeCount = GetModeCount(Mode);
    pOutArgs->MonitorModeBufferOutputCount = ModeCount;

    if (pInArgs->MonitorModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->MonitorModeBufferInputCount < ModeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (DWORD ModeIndex = 0; ModeIndex < ModeCount; ModeIndex++)
    {
        const auto ModeByIndex = GetModeByIndex(Mode, ModeIndex);
        pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
            ModeByIndex.Width,
            ModeByIndex.Height,
            ModeByIndex.Refresh,
            IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
        );
    }

    pOutArgs->PreferredMonitorModeIdx = 0;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorSplitterMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    auto* pMonitorContext = GetMonitorContextOrNull(MonitorObject);
    if (pMonitorContext == nullptr)
    {
        DriverLogLine("MonitorGetDefaultModes missing monitor context");
        return STATUS_INVALID_DEVICE_STATE;
    }

    const auto Monitor = pMonitorContext->GetMode();
    const DWORD ModeCount = GetModeCount(Monitor);

    pOutArgs->DefaultMonitorModeBufferOutputCount = ModeCount;

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->DefaultMonitorModeBufferInputCount < ModeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (DWORD ModeIndex = 0; ModeIndex < ModeCount; ModeIndex++)
    {
        const auto Mode = GetModeByIndex(Monitor, ModeIndex);
        pInArgs->pDefaultMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
            Mode.Width,
            Mode.Height,
            Mode.Refresh,
            IDDCX_MONITOR_MODE_ORIGIN_DRIVER
        );
    }

    pOutArgs->PreferredMonitorModeIdx = 0;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorSplitterMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    auto* pMonitorContext = GetMonitorContextOrNull(MonitorObject);
    if (pMonitorContext == nullptr)
    {
        DriverLogLine("MonitorQueryModes missing monitor context");
        return STATUS_INVALID_DEVICE_STATE;
    }

    const auto Monitor = pMonitorContext->GetMode();
    const DWORD ModeCount = GetModeCount(Monitor);

    pOutArgs->TargetModeBufferOutputCount = ModeCount;

    if (pInArgs->TargetModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->TargetModeBufferInputCount < ModeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (DWORD ModeIndex = 0; ModeIndex < ModeCount; ModeIndex++)
    {
        const auto Mode = GetModeByIndex(Monitor, ModeIndex);
        pInArgs->pTargetModes[ModeIndex] = CreateIddCxTargetMode(
            Mode.Width,
            Mode.Height,
            Mode.Refresh
        );
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorSplitterMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContext = GetMonitorContextOrNull(MonitorObject);
    if (pMonitorContext == nullptr)
    {
        DriverLogLine("MonitorAssignSwapChain missing monitor context");
        WdfObjectDelete(pInArgs->hSwapChain);
        return STATUS_SUCCESS;
    }

    pMonitorContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS MonitorSplitterMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContext = GetMonitorContextOrNull(MonitorObject);
    if (pMonitorContext == nullptr)
    {
        DriverLogLine("MonitorUnassignSwapChain missing monitor context");
        return STATUS_INVALID_DEVICE_STATE;
    }

    pMonitorContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion


