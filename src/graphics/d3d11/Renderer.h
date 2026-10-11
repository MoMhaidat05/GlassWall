#pragma once
#include "../../platform/win32/HResult.h"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <mfobjects.h>
#include <mferror.h>
#include <wrl/client.h>

namespace glasswall::graphics
{
    class Renderer
    {
    public:
        ~Renderer() { Reset(); }
        void Create(HWND window);
        void Reset() noexcept;
        void ValidateSample(IMFSample* sample) const;
        void SetFullRange(bool fullRange);
        HRESULT Present(IMFSample* sample);
        HANDLE LatencyEvent() const noexcept { return m_latency.get(); }
        ID3D11Device* Device() const noexcept { return m_device.Get(); }
        HRESULT RemovedReason() const noexcept { return m_device ? m_device->GetDeviceRemovedReason() : S_OK; }
    private:
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
        Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
        Microsoft::WRL::ComPtr<IDXGISwapChain2> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enumerator;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;
        Microsoft::WRL::ComPtr<IMFSample> m_inFlight;
        platform::Handle m_latency;
        UINT m_width{}, m_height{}, m_frame{};
    };
}
