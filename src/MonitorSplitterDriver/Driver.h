#pragma once

#define NOMINMAX
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "Trace.h"
#include "..\MonitorSplitterCommon\BuildInfo.h"
#include "..\MonitorSplitterCommon\Layout.h"
#include "..\MonitorSplitterCommon\SyntheticEdid.h"

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace Microsoft
{
    namespace IndirectDisp
    {
        struct MonitorSplitterMode
        {
            DWORD Width;
            DWORD Height;
            DWORD VSync;
        };

        /// <summary>
        /// Manages the creation and lifetime of a Direct3D render device.
        /// </summary>
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            Direct3DDevice();
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent, UINT ConnectorIndex);
            ~SwapChainProcessor();
            bool IsRunning() const;

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();
            HRESULT PublishFrame(ID3D11Texture2D* SourceTexture);
            HRESULT EnsureSharedFrameTexture(const D3D11_TEXTURE2D_DESC& SourceDesc);
            void ResetSharedFrameTexture();
            void EnsureDriverRuntimeStatus();
            void UpdateDriverRuntimeStatus();
            void EnsureSharedFrameStatus();
            void UpdateSharedFrameStatus(const D3D11_TEXTURE2D_DESC* SourceDesc, HRESULT CreateResult, HRESULT PublishResult, HRESULT AcquireResult, bool Published);

            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            UINT m_ConnectorIndex;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> m_SharedFrameTexture;
            Microsoft::WRL::ComPtr<IDXGIKeyedMutex> m_SharedFrameMutex;
            HANDLE m_SharedFrameHandle = nullptr;
            D3D11_TEXTURE2D_DESC m_SharedFrameDesc = {};
            bool m_HasSharedFrameDesc = false;
            bool m_SharedFrameDisabled = false;
            HANDLE m_DriverRuntimeStatusMapping = nullptr;
            MonitorSplitter::DriverRuntimeStatus* m_DriverRuntimeStatus = nullptr;
            HANDLE m_SharedFrameStatusMapping = nullptr;
            MonitorSplitter::SharedFrameStatus* m_SharedFrameStatus = nullptr;
        };

        /// <summary>
        /// Provides a sample implementation of an indirect display driver.
        /// </summary>
        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice, MonitorSplitter::Layout Layout, std::string EdidNameBase, LUID PreferredRenderAdapter, bool HasPreferredRenderAdapter);
            virtual ~IndirectDeviceContext();

            NTSTATUS InitAdapter();
            NTSTATUS PlugMonitors();
            NTSTATUS FinishInit(UINT ConnectorIndex);
            void DepartMonitors();
            size_t GetMonitorCount() const;

        protected:
            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
            MonitorSplitter::Layout m_Layout;
            std::string m_EdidNameBase;
            std::vector<std::array<BYTE, MonitorSplitter::kSyntheticEdidSize>> m_EdidBlocks;
            std::vector<bool> m_EdidAvailable;
            std::vector<IDDCX_MONITOR> m_MonitorObjects;
            LUID m_PreferredRenderAdapter = {};
            bool m_HasPreferredRenderAdapter = false;
            bool m_AdapterInitialized = false;
        };

        class IndirectMonitorContext
        {
        public:
            IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, _In_ UINT ConnectorIndex, MonitorSplitter::MonitorMode Mode);
            virtual ~IndirectMonitorContext();

            UINT GetConnectorIndex() const;
            MonitorSplitter::MonitorMode GetMode() const;
            void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain();

        private:
            IDDCX_MONITOR m_Monitor;
            UINT m_ConnectorIndex;
            MonitorSplitter::MonitorMode m_Mode;
            std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
        } ;
    }
}

