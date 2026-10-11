#include "Renderer.h"
#include <d3d10.h>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace glasswall::graphics
{
    namespace
    {
        struct TextureFrame { ComPtr<ID3D11Texture2D> texture; UINT subresource{}; };
        TextureFrame GetTexture(IMFSample* sample, ID3D11Device* expected)
        {
            if (!sample) throw platform::Failure(E_POINTER, L"Missing decoded video sample");
            DWORD count{};
            platform::Check(sample->GetBufferCount(&count), L"IMFSample::GetBufferCount");
            if (count != 1) throw platform::Failure(MF_E_UNSUPPORTED_D3D_TYPE, L"Expected one GPU-backed video buffer");
            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFDXGIBuffer> dxgi;
            TextureFrame result;
            platform::Check(sample->GetBufferByIndex(0, buffer.GetAddressOf()), L"IMFSample::GetBufferByIndex");
            if (FAILED(buffer.As(&dxgi)))
                throw platform::Failure(MF_E_UNSUPPORTED_D3D_TYPE, L"Decoder returned CPU memory; software frame upload is disabled");
            platform::Check(dxgi->GetResource(IID_PPV_ARGS(result.texture.GetAddressOf())), L"IMFDXGIBuffer::GetResource(ID3D11Texture2D)");
            platform::Check(dxgi->GetSubresourceIndex(&result.subresource), L"IMFDXGIBuffer::GetSubresourceIndex");
            ComPtr<ID3D11Device> owner;
            result.texture->GetDevice(owner.GetAddressOf());
            if (owner.Get() != expected) throw platform::Failure(E_INVALIDARG, L"Cross-device decoded texture is unsupported");
            D3D11_TEXTURE2D_DESC description{};
            result.texture->GetDesc(&description);
            if (description.Format != DXGI_FORMAT_NV12 || description.Width < 1920 ||
                description.Height < 1080 || !description.MipLevels ||
                result.subresource >= description.MipLevels * description.ArraySize)
            {
                platform::Log(L"engine", L"stage=Renderer.ValidateSample; textureFormat=" + std::to_wstring(description.Format) +
                    L" dimensions=" + std::to_wstring(description.Width) + L"x" + std::to_wstring(description.Height) +
                    L" mipLevels=" + std::to_wstring(description.MipLevels) + L" arraySize=" + std::to_wstring(description.ArraySize) +
                    L" subresource=" + std::to_wstring(result.subresource) + L" bindFlags=" + std::to_wstring(description.BindFlags));
                throw platform::Failure(MF_E_UNSUPPORTED_D3D_TYPE, L"Unexpected GPU video texture layout");
            }
            return result;
        }
    }

    void Renderer::Create(HWND window)
    {
        Reset();
        platform::Log(L"engine", L"stage=Renderer.Create; hardware D3D11, NV12 input, BGRA output, progressive 1920x1080.");
        D3D_FEATURE_LEVEL feature{};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        platform::Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, m_device.GetAddressOf(), &feature,
            m_context.GetAddressOf()), L"D3D11CreateDevice: hardware video device required");
        platform::Check(m_device.As(&m_videoDevice), L"ID3D11Device::QueryInterface(ID3D11VideoDevice)");
        platform::Check(m_context.As(&m_videoContext), L"ID3D11DeviceContext::QueryInterface(ID3D11VideoContext)");
        ComPtr<ID3D10Multithread> multithread;
        platform::Check(m_device.As(&multithread), L"ID3D11Device::QueryInterface(ID3D10Multithread)");
        multithread->SetMultithreadProtected(TRUE);

        bool h264{};
        auto profileCount = m_videoDevice->GetVideoDecoderProfileCount();
        platform::Log(L"engine", L"stage=Renderer.DecodeCapabilities; featureLevel=" + std::to_wstring(feature) +
            L" decoderProfileCount=" + std::to_wstring(profileCount));
        for (UINT index = 0; index < profileCount; ++index)
        {
            GUID profile{};
            BOOL supported{};
            platform::Check(m_videoDevice->GetVideoDecoderProfile(index, &profile),
                L"ID3D11VideoDevice::GetVideoDecoderProfile");
            if (profile == D3D11_DECODER_PROFILE_H264_VLD_NOFGT)
            {
                platform::Check(m_videoDevice->CheckVideoDecoderFormat(&profile, DXGI_FORMAT_NV12, &supported),
                    L"ID3D11VideoDevice::CheckVideoDecoderFormat(H264, NV12)");
                if (supported) h264 = true;
            }
        }
        if (!h264) throw platform::Failure(MF_E_UNSUPPORTED_D3D_TYPE, L"GPU exposes no required H.264/NV12 decode profile");
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        platform::Check(m_device.As(&dxgiDevice), L"ID3D11Device::QueryInterface(IDXGIDevice)");
        platform::Check(dxgiDevice->GetAdapter(adapter.GetAddressOf()), L"IDXGIDevice::GetAdapter");
        DXGI_ADAPTER_DESC adapterInfo{};
        platform::Check(adapter->GetDesc(&adapterInfo), L"IDXGIAdapter::GetDesc");
        platform::Log(L"engine", L"stage=Renderer.Adapter; name=" + std::wstring(adapterInfo.Description) +
            L" vendorId=" + std::to_wstring(adapterInfo.VendorId) + L" deviceId=" + std::to_wstring(adapterInfo.DeviceId));
        platform::Check(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())), L"IDXGIAdapter::GetParent(IDXGIFactory2)");
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> swapChain;
        platform::Check(factory->CreateSwapChainForHwnd(m_device.Get(), window, &description,
            nullptr, nullptr, swapChain.GetAddressOf()), L"IDXGIFactory2::CreateSwapChainForHwnd(desktop child)");
        auto association = factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(association))
            platform::Log(L"engine", L"DXGI window association rejected the desktop child: " + platform::HexResult(association) +
                L". The owned child cannot receive focus and no fullscreen-state API is used.");
        platform::Check(swapChain.As(&m_swapChain), L"IDXGISwapChain1::QueryInterface(IDXGISwapChain2)");
        platform::Check(m_swapChain->SetMaximumFrameLatency(1), L"IDXGISwapChain2::SetMaximumFrameLatency(1)");
        m_latency.reset(m_swapChain->GetFrameLatencyWaitableObject());
        if (!m_latency) throw platform::Failure(E_FAIL, L"DXGI latency event unavailable");
        ComPtr<ID3D11Texture2D> backBuffer;
        platform::Check(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())), L"IDXGISwapChain2::GetBuffer: inspect output");
        D3D11_TEXTURE2D_DESC output{};
        backBuffer->GetDesc(&output);
        m_width = output.Width; m_height = output.Height;
        if (!m_width || !m_height) throw platform::Failure(E_FAIL, L"Empty desktop output");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = { 30, 1 };
        content.InputWidth = 1920; content.InputHeight = 1080;
        content.OutputFrameRate = { 30, 1 };
        content.OutputWidth = m_width; content.OutputHeight = m_height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        platform::Check(m_videoDevice->CreateVideoProcessorEnumerator(&content, m_enumerator.GetAddressOf()),
            L"ID3D11VideoDevice::CreateVideoProcessorEnumerator(progressive)");
        UINT inputSupport{}, outputSupport{};
        platform::Check(m_enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &inputSupport), L"ID3D11VideoProcessorEnumerator::CheckVideoProcessorFormat(NV12)");
        platform::Check(m_enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &outputSupport), L"ID3D11VideoProcessorEnumerator::CheckVideoProcessorFormat(BGRA)");
        if (!(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
            !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            throw platform::Failure(MF_E_UNSUPPORTED_D3D_TYPE, L"GPU cannot convert NV12 to desktop BGRA");
        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        platform::Check(m_enumerator->GetVideoProcessorCaps(&caps), L"ID3D11VideoProcessorEnumerator::GetVideoProcessorCaps");
        platform::Log(L"engine", L"stage=Renderer.ProcessorCapabilities; modes=" + std::to_wstring(caps.RateConversionCapsCount) +
            L" maxInputStreams=" + std::to_wstring(caps.MaxInputStreams) + L" maxStreamStates=" +
            std::to_wstring(caps.MaxStreamStates) + L" output=" + std::to_wstring(m_width) + L"x" + std::to_wstring(m_height));
        if (!caps.RateConversionCapsCount || !caps.MaxInputStreams || !caps.MaxStreamStates)
            throw platform::Failure(MF_E_UNSUPPORTED_D3D_TYPE, L"Renderer::Create: GPU exposes no usable single-stream video processor");
        UINT selected = caps.RateConversionCapsCount;
        std::uint64_t fewestReferences = UINT64_MAX;
        for (UINT index = 0; index < caps.RateConversionCapsCount; ++index)
        {
            D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rate{};
            platform::Check(m_enumerator->GetVideoProcessorRateConversionCaps(index, &rate),
                L"ID3D11VideoProcessorEnumerator::GetVideoProcessorRateConversionCaps");
            platform::Log(L"engine", L"stage=Renderer.ProcessorMode; index=" + std::to_wstring(index) +
                L" optimalPastFrames=" + std::to_wstring(rate.PastFrames) + L" optimalFutureFrames=" +
                std::to_wstring(rate.FutureFrames) + L" processorCaps=" + std::to_wstring(rate.ProcessorCaps));
            // These counts describe optimal temporal/deinterlacing processing,
            // not mandatory history for our progressive color conversion.
            // Prefer the simplest mode; never allocate reference-frame queues.
            auto references = static_cast<std::uint64_t>(rate.PastFrames) + rate.FutureFrames;
            if (references < fewestReferences) { selected = index; fewestReferences = references; }
        }
        platform::Log(L"engine", L"stage=Renderer.CreateVideoProcessor; selectedMode=" + std::to_wstring(selected) +
            L" suppliedPastFrames=0 suppliedFutureFrames=0; progressive conversion, normal output rate, auto processing disabled.");
        platform::Check(m_videoDevice->CreateVideoProcessor(m_enumerator.Get(), selected,
            m_processor.GetAddressOf()), L"ID3D11VideoDevice::CreateVideoProcessor");
        m_videoContext->VideoProcessorSetStreamFrameFormat(m_processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_videoContext->VideoProcessorSetStreamOutputRate(m_processor.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
        m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_processor.Get(), 0, FALSE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
        inputColor.YCbCr_Matrix = 1; // BT.709, nominal limited range
        inputColor.Nominal_Range = 1;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
        outputColor.RGB_Range = 0; // full-range SDR RGB
        m_videoContext->VideoProcessorSetStreamColorSpace(m_processor.Get(), 0, &inputColor);
        m_videoContext->VideoProcessorSetOutputColorSpace(m_processor.Get(), &outputColor);
        platform::Log(L"engine", L"D3D11 H.264-capable adapter: " + std::wstring(adapterInfo.Description) +
            L"; two swap buffers, maximum frame latency one.");
    }

    void Renderer::ValidateSample(IMFSample* sample) const { GetTexture(sample, m_device.Get()); }
    void Renderer::SetFullRange(bool fullRange)
    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{};
        color.YCbCr_Matrix = 1;
        color.Nominal_Range = fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 :
            D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        m_videoContext->VideoProcessorSetStreamColorSpace(m_processor.Get(), 0, &color);
    }

    HRESULT Renderer::Present(IMFSample* sample)
    {
        // The caller has consumed DXGI's latency signal before each blit. Keep
        // the previous allocator sample alive until that GPU work is complete.
        m_inFlight.Reset();
        auto frame = GetTexture(sample, m_device.Get());
        D3D11_TEXTURE2D_DESC texture{};
        frame.texture->GetDesc(&texture);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription{};
        inputDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDescription.Texture2D.MipSlice = frame.subresource % texture.MipLevels;
        inputDescription.Texture2D.ArraySlice = frame.subresource / texture.MipLevels;
        ComPtr<ID3D11VideoProcessorInputView> input;
        platform::Check(m_videoDevice->CreateVideoProcessorInputView(frame.texture.Get(), m_enumerator.Get(),
            &inputDescription, input.GetAddressOf()), L"ID3D11VideoDevice::CreateVideoProcessorInputView(NV12)");
        ComPtr<ID3D11Texture2D> backBuffer;
        platform::Check(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())), L"IDXGISwapChain2::GetBuffer: current output");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDescription{};
        outputDescription.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> output;
        platform::Check(m_videoDevice->CreateVideoProcessorOutputView(backBuffer.Get(), m_enumerator.Get(),
            &outputDescription, output.GetAddressOf()), L"ID3D11VideoDevice::CreateVideoProcessorOutputView(BGRA)");
        RECT destination{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
        RECT source{ 0, 0, 1920, 1080 };
        // Center-crop to fill the primary desktop without CPU pixel conversion.
        if (static_cast<std::uint64_t>(m_width) * 1080 > static_cast<std::uint64_t>(m_height) * 1920)
        {
            LONG height = std::max<LONG>(2, static_cast<LONG>((static_cast<std::uint64_t>(1920) * m_height) / m_width) & ~1L);
            source.top = ((1080 - height) / 2) & ~1L; source.bottom = source.top + height;
        }
        else
        {
            LONG width = std::max<LONG>(2, static_cast<LONG>((static_cast<std::uint64_t>(1080) * m_width) / m_height) & ~1L);
            source.left = ((1920 - width) / 2) & ~1L; source.right = source.left + width;
        }
        m_videoContext->VideoProcessorSetOutputTargetRect(m_processor.Get(), TRUE, &destination);
        m_videoContext->VideoProcessorSetStreamSourceRect(m_processor.Get(), 0, TRUE, &source);
        m_videoContext->VideoProcessorSetStreamDestRect(m_processor.Get(), 0, TRUE, &destination);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE; stream.pInputSurface = input.Get();
        stream.InputFrameOrField = m_frame;
        stream.PastFrames = 0; stream.FutureFrames = 0;
        platform::Check(m_videoContext->VideoProcessorBlt(m_processor.Get(), output.Get(), m_frame++, 1, &stream),
            L"ID3D11VideoContext::VideoProcessorBlt(progressive, zero references)");
        m_inFlight = sample;
        return m_swapChain->Present(1, 0);
    }
    void Renderer::Reset() noexcept
    {
        m_latency.reset();
        if (m_context) { m_context->ClearState(); m_context->Flush(); }
        m_inFlight.Reset();
        m_processor.Reset(); m_enumerator.Reset(); m_swapChain.Reset();
        m_videoContext.Reset(); m_videoDevice.Reset(); m_context.Reset(); m_device.Reset();
        m_width = m_height = m_frame = 0;
    }
}
