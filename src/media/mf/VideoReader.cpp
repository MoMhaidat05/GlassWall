#include "VideoReader.h"
#include "InterlacePolicy.h"
#include <mftransform.h>
#include <propvarutil.h>
#include <filesystem>
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <wrl/implements.h>

using Microsoft::WRL::ComPtr;
namespace glasswall::media
{
    namespace
    {
        // SDK stream-selector enums use signed values; DWORD preserves their sentinel bit patterns.
        constexpr DWORD AllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
        constexpr DWORD FirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        constexpr DWORD MediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

        interlace::Flag ReadSampleFlag(IMFSample* sample, REFGUID key)
        {
            interlace::Flag flag;
            flag.status = sample->GetUINT32(key, &flag.value);
            return flag;
        }
        std::wstring FlagText(wchar_t const* name, interlace::Flag const& flag)
        {
            return std::wstring(name) + L"=" + (flag.Present() ? std::to_wstring(flag.value) : L"unset/unreadable") +
                L"[" + platform::HexResult(flag.status) + L"]";
        }

        // Documented GUID, intentionally not an SDK-defined symbol:
        // https://learn.microsoft.com/windows/win32/medfound/mf-source-reader-passthrough-mode
        constexpr GUID SourceReaderPassthrough =
            { 0x043ff126, 0xfe2c, 0x4708, { 0xa0,0x9b,0xda,0x2a,0xb4,0x35,0xce,0xd9 } };
        class Callback final : public Microsoft::WRL::RuntimeClass<
            Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFSourceReaderCallback>
        {
        public:
            explicit Callback(std::shared_ptr<ReaderMailbox> mailbox) : m_mailbox(std::move(mailbox)) {}
            HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT status, DWORD stream, DWORD flags,
                LONGLONG timestamp, IMFSample* sample) override
            {
                std::scoped_lock lock(m_mailbox->mutex);
                if (!m_mailbox->active || m_mailbox->flushing) return S_OK;
                if (FAILED(status) || (flags & MF_SOURCE_READERF_ERROR))
                    platform::Log(L"engine", L"stage=VideoReader.Callback; function=IMFSourceReaderCallback::OnReadSample HRESULT=" +
                        platform::HexResult(status) + L" stream=" + std::to_wstring(stream) +
                        L" flags=" + std::to_wstring(flags) + L" timestamp100ns=" + std::to_wstring(timestamp));
                m_mailbox->pending = false;
                m_mailbox->frame.status = status;
                m_mailbox->frame.flags = flags;
                m_mailbox->frame.timestamp = timestamp;
                m_mailbox->frame.sample = sample;
                m_mailbox->ready = true;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                    platform::Log(L"engine", L"stage=VideoReader.EOF; flags=" + std::to_wstring(flags) +
                        L" timestamp100ns=" + std::to_wstring(timestamp) + L" sample=" + std::to_wstring(sample != nullptr));
                SetEvent(m_mailbox->event.get());
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE OnFlush(DWORD) override
            {
                std::scoped_lock lock(m_mailbox->mutex);
                m_mailbox->pending = false; m_mailbox->ready = false; m_mailbox->frame = {};
                SetEvent(m_mailbox->flushed.get());
                SetEvent(m_mailbox->event.get());
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent* event) override
            {
                HRESULT status{};
                if (event && SUCCEEDED(event->GetStatus(&status)) && FAILED(status))
                {
                    std::scoped_lock lock(m_mailbox->mutex);
                    if (m_mailbox->active)
                    {
                        platform::Log(L"engine", L"stage=VideoReader.Callback; function=IMFSourceReaderCallback::OnEvent/IMFMediaEvent::GetStatus HRESULT=" +
                            platform::HexResult(status));
                        m_mailbox->frame.status = status;
                        m_mailbox->ready = true;
                        SetEvent(m_mailbox->event.get());
                    }
                }
                return S_OK;
            }
        private:
            std::shared_ptr<ReaderMailbox> m_mailbox; // no reader/runtime cycle or raw owner pointer
        };
    }

    ReaderMailbox::ReaderMailbox() :
        event(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        flushed(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        if (!event || !flushed) throw platform::Error("Create reader events", GetLastError());
    }

    void VideoReader::Open(std::wstring const& path, ID3D11Device* device, LONGLONG startAt)
    {
        Close();
        platform::Log(L"engine", L"stage=VideoReader.Open; local source=" + path +
            L" start100ns=" + std::to_wstring(startAt) + L" DXVA enabled; GPU NV12 required; audio disabled.");
        m_mailbox = std::make_shared<ReaderMailbox>();
        auto callback = Microsoft::WRL::Make<Callback>(m_mailbox);
        if (!callback) throw platform::Failure(E_OUTOFMEMORY, L"Create source reader callback");
        UINT token{};
        platform::Check(MFCreateDXGIDeviceManager(&token, m_manager.GetAddressOf()), L"MFCreateDXGIDeviceManager");
        platform::Check(m_manager->ResetDevice(device, token), L"IMFDXGIDeviceManager::ResetDevice");
        ComPtr<IMFAttributes> attributes;
        platform::Check(MFCreateAttributes(attributes.GetAddressOf(), 8), L"MFCreateAttributes");
        platform::Check(attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get()), L"IMFAttributes::SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK)");
        platform::Check(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m_manager.Get()), L"IMFAttributes::SetUnknown(MF_SOURCE_READER_D3D_MANAGER)");
        platform::Check(attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE), L"IMFAttributes::SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE)");
        platform::Check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE), L"IMFAttributes::SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE)");
        platform::Check(attributes->SetUINT32(SourceReaderPassthrough, TRUE), L"IMFAttributes::SetUINT32(SourceReaderPassthrough, TRUE)");
        platform::Check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE), L"IMFAttributes::SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE)");
        platform::Check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE), L"IMFAttributes::SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE)");
        platform::Check(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), m_reader.GetAddressOf()),
            L"MFCreateSourceReaderFromURL: open local MP4");
        PROPVARIANT characteristics{};
        auto sourceResult = m_reader->GetPresentationAttribute(MediaSource,
            MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &characteristics);
        UINT32 sourceFlags{};
        if (SUCCEEDED(sourceResult))
        {
            if (characteristics.vt == VT_UI4) sourceFlags = characteristics.ulVal;
            else sourceResult = MF_E_INVALIDTYPE;
        }
        PropVariantClear(&characteristics);
        platform::Check(sourceResult, L"IMFSourceReader::GetPresentationAttribute(MEDIASOURCE_CHARACTERISTICS)");
        bool seekable = (sourceFlags & MFMEDIASOURCE_CAN_SEEK) != 0;
        PROPVARIANT duration{};
        auto durationResult = m_reader->GetPresentationAttribute(MediaSource, MF_PD_DURATION, &duration);
        if (SUCCEEDED(durationResult) && duration.vt == VT_UI8 && duration.uhVal.QuadPart <= static_cast<ULONGLONG>(INT64_MAX))
            m_duration = static_cast<LONGLONG>(duration.uhVal.QuadPart);
        PropVariantClear(&duration);
        platform::Log(L"engine", L"stage=VideoReader.Source; characteristics=" + std::to_wstring(sourceFlags) +
            L" seekable=" + std::to_wstring(seekable) + L" duration100ns=" + std::to_wstring(m_duration) +
            L" durationHRESULT=" + platform::HexResult(durationResult));
        if (!seekable) throw platform::Failure(MF_E_INVALIDREQUEST, L"Local video source is not seekable; automatic looping requires seeking");
        platform::Check(m_reader->SetStreamSelection(AllStreams, FALSE), L"IMFSourceReader::SetStreamSelection(ALL_STREAMS, FALSE)");
        platform::Check(m_reader->SetStreamSelection(FirstVideoStream, TRUE), L"IMFSourceReader::SetStreamSelection(FIRST_VIDEO_STREAM, TRUE)");
        ComPtr<IMFMediaType> native;
        platform::Check(m_reader->GetNativeMediaType(FirstVideoStream, 0, native.GetAddressOf()),
            L"IMFSourceReader::GetNativeMediaType(FIRST_VIDEO_STREAM)");
        GUID subtype{};
        platform::Check(native->GetGUID(MF_MT_SUBTYPE, &subtype), L"IMFMediaType::GetGUID(MF_MT_SUBTYPE)");
        wchar_t subtypeText[40]{};
        StringFromGUID2(subtype, subtypeText, 40);
        platform::Log(L"engine", L"stage=VideoReader.NativeFormat; subtype=" + std::wstring(subtypeText));
        if (subtype != MFVideoFormat_H264)
            throw platform::Failure(MF_E_INVALIDMEDIATYPE, L"Only H.264/AVC MP4 video is supported");
        UINT32 width{}, height{}, numerator{}, denominator{};
        platform::Check(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height), L"MFGetAttributeSize(MF_MT_FRAME_SIZE)");
        platform::Check(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &numerator, &denominator), L"MFGetAttributeRatio(MF_MT_FRAME_RATE)");
        platform::Log(L"engine", L"stage=VideoReader.NativeFormat; dimensions=" + std::to_wstring(width) + L"x" +
            std::to_wstring(height) + L" frameRate=" + std::to_wstring(numerator) + L"/" + std::to_wstring(denominator) +
            L" h264Profile=" + std::to_wstring(MFGetAttributeUINT32(native.Get(), MF_MT_MPEG2_PROFILE, 0)) +
            L" h264Level=" + std::to_wstring(MFGetAttributeUINT32(native.Get(), MF_MT_MPEG2_LEVEL, 0)));
        if (width != 1920 || height != 1080 || !denominator ||
            !((numerator == 30 && denominator == 1) || (numerator == 30000 && denominator == 1001)))
            throw platform::Failure(MF_E_INVALIDMEDIATYPE, L"Initial format is 1920x1080 at 30 or 29.97 fps");
        m_framePeriod = (10000000LL * denominator) / numerator;
        m_metadata = { width, height, numerator, denominator, m_duration };
        auto sourceInterlace = MFGetAttributeUINT32(native.Get(), MF_MT_INTERLACE_MODE, MFVideoInterlace_Unknown);
        auto rotation = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_ROTATION, 0);
        auto transfer = MFGetAttributeUINT32(native.Get(), MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        auto primaries = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        auto matrix = MFGetAttributeUINT32(native.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        auto range = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        platform::Log(L"engine", L"stage=VideoReader.ColorFormat; interlace=" + std::to_wstring(sourceInterlace) +
            L" rotation=" + std::to_wstring(rotation) + L" transfer=" + std::to_wstring(transfer) +
            L" primaries=" + std::to_wstring(primaries) + L" matrix=" + std::to_wstring(matrix) + L" nominalRange=" + std::to_wstring(range));
        // This is the compressed H.264 declaration, not the decoded frame's scan format.
        // H.264 commonly declares Mixed; the elementary stream/sample flags decide each frame.
        if (!interlace::SupportedDeclaration(sourceInterlace))
            throw platform::Failure(MF_E_INVALIDMEDIATYPE, L"Compressed H.264 declares unsupported/unknown interlacing; expected Progressive or Mixed");
        m_sourceInterlace = sourceInterlace;
        platform::Log(L"engine", L"stage=VideoReader.CompressedInterlace; acceptedDeclaration=" + std::to_wstring(sourceInterlace) +
            L"; decoded samples must independently prove progressive full frames.");
        if (rotation != 0 || transfer != MFVideoTransFunc_709 ||
            primaries != MFVideoPrimaries_BT709 || matrix != MFVideoTransferMatrix_BT709 ||
            (range != MFNominalRange_Unknown && range != MFNominalRange_16_235 && range != MFNominalRange_0_255))
            throw platform::Failure(MF_E_INVALIDMEDIATYPE, L"Only unrotated SDR BT.709 video with supported nominal range is supported");
        m_fullRange = range == MFNominalRange_0_255;
        ComPtr<IMFMediaType> output;
        platform::Check(MFCreateMediaType(output.GetAddressOf()), L"MFCreateMediaType");
        platform::Check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE, Video)");
        platform::Check(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), L"IMFMediaType::SetGUID(MF_MT_SUBTYPE, NV12)");
        platform::Check(m_reader->SetCurrentMediaType(FirstVideoStream, nullptr, output.Get()),
            L"IMFSourceReader::SetCurrentMediaType(NV12): required H.264/DXVA path unavailable or incompatible");
        ComPtr<IMFMediaType> negotiated;
        platform::Check(m_reader->GetCurrentMediaType(FirstVideoStream, negotiated.GetAddressOf()),
            L"IMFSourceReader::GetCurrentMediaType: inspect negotiated NV12 output");
        GUID negotiatedSubtype{};
        platform::Check(negotiated->GetGUID(MF_MT_SUBTYPE, &negotiatedSubtype), L"IMFMediaType::GetGUID(negotiated MF_MT_SUBTYPE)");
        if (negotiatedSubtype != MFVideoFormat_NV12)
            throw platform::Failure(MF_E_UNSUPPORTED_D3D_TYPE, L"VideoReader::Open: decoder did not negotiate required NV12 output");
        auto decodedInterlaceResult = negotiated->GetUINT32(MF_MT_INTERLACE_MODE, &m_decodedInterlace);
        if (decodedInterlaceResult == MF_E_ATTRIBUTENOTFOUND) m_decodedInterlace = MFVideoInterlace_Unknown;
        else
            platform::Check(decodedInterlaceResult, L"IMFMediaType::GetUINT32(decoded MF_MT_INTERLACE_MODE)");
        platform::Log(L"engine", L"stage=VideoReader.DecodedInterlace; negotiatedMode=" + std::to_wstring(m_decodedInterlace) +
            L" attributeHRESULT=" + platform::HexResult(decodedInterlaceResult) + L"; no interlace metadata rewritten.");
        if (!interlace::SupportedDeclaration(m_decodedInterlace) && m_decodedInterlace != MFVideoInterlace_Unknown)
            throw platform::Failure(MF_E_INVALIDMEDIATYPE, L"Negotiated NV12 output declares unsupported interlaced/single-field format");
        ComPtr<IMFSourceReaderEx> extended;
        platform::Check(m_reader.As(&extended), L"IMFSourceReader::QueryInterface(IMFSourceReaderEx)");
        GUID category{};
        ComPtr<IMFTransform> decoder;
        platform::Check(extended->GetTransformForStream(FirstVideoStream, 0,
            &category, decoder.GetAddressOf()), L"IMFSourceReaderEx::GetTransformForStream(FIRST_VIDEO_STREAM, 0)");
        if (category != MFT_CATEGORY_VIDEO_DECODER)
            throw platform::Failure(MF_E_TOPO_CODEC_NOT_FOUND, L"Expected H.264 decoder at transform zero");
        ComPtr<IMFAttributes> decoderAttributes;
        platform::Check(decoder->GetAttributes(decoderAttributes.GetAddressOf()),
            L"IMFTransform::GetAttributes: decoder attributes required to establish D3D11 awareness");
        UINT32 aware{};
        auto awarenessResult = decoderAttributes->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        platform::Log(L"engine", L"stage=VideoReader.DecoderAttributes; MF_SA_D3D11_AWARE=" + std::to_wstring(aware) +
            L" attributeHRESULT=" + platform::HexResult(awarenessResult) + L" actualHardwareDecodeVerified=false");
        if (FAILED(awarenessResult) || !aware)
            throw platform::Failure(MF_E_UNSUPPORTED_D3D_TYPE, L"Selected decoder is not D3D11-aware; no CPU fallback");
        if (startAt > 0)
        {
            PROPVARIANT position{};
            position.vt = VT_I8; position.hVal.QuadPart = startAt;
            platform::Check(m_reader->SetCurrentPosition(GUID_NULL, position), L"IMFSourceReader::SetCurrentPosition: restore");
        }
        platform::Log(L"engine", L"H.264 Source Reader configured for DXVA and GPU NV12. Audio deselected. Actual video-decode activity still requires Windows tracing.");
        Request();
    }

    bool VideoReader::Request()
    {
        if (!m_reader || !m_mailbox) throw platform::Failure(MF_E_NOT_INITIALIZED, L"Reader not loaded");
        if (m_seeking) return false;
        {
            std::scoped_lock lock(m_mailbox->mutex);
            if (!m_mailbox->active) throw platform::Failure(MF_E_NOT_INITIALIZED, L"Reader is closing");
            if (m_mailbox->pending || m_mailbox->ready || m_mailbox->flushing) return false;
            m_mailbox->pending = true;
            m_mailbox->readStarted = GetTickCount64();
        }
        auto result = m_reader->ReadSample(FirstVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(result))
        {
            std::scoped_lock lock(m_mailbox->mutex);
            m_mailbox->pending = false;
            throw platform::Failure(result, L"IMFSourceReader::ReadSample: asynchronous video decode request");
        }
        return true;
    }
    ULONGLONG VideoReader::ReadDeadline() const
    {
        if (!m_mailbox) return 0;
        std::scoped_lock lock(m_mailbox->mutex);
        return m_mailbox->pending ? m_mailbox->readStarted + 5000 : 0;
    }
    void VideoReader::ExtendDeadlines(ULONGLONG milliseconds)
    {
        if (m_seekDeadline) m_seekDeadline += milliseconds;
        if (!m_mailbox) return;
        std::scoped_lock lock(m_mailbox->mutex);
        if (m_mailbox->pending) m_mailbox->readStarted += milliseconds;
    }
    std::optional<VideoFrame> VideoReader::Take()
    {
        if (!m_mailbox) return {};
        std::scoped_lock lock(m_mailbox->mutex);
        if (!m_mailbox->ready) return {};
        auto frame = std::move(m_mailbox->frame);
        m_mailbox->frame = {};
        m_mailbox->ready = false;
        ResetEvent(m_mailbox->event.get());
        if (SUCCEEDED(frame.status) && frame.sample && !(frame.flags & MF_SOURCE_READERF_ERROR))
        {
            // A decoder may finalize its output type with the first sample.
            // Refresh that type rather than treating compressed metadata or a
            // provisional decoded declaration as proof of this frame's layout.
            bool initialTypeChange = !m_interlaceLogged && (frame.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) &&
                !(frame.flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED);
            if (initialTypeChange)
            {
                ComPtr<IMFMediaType> currentType;
                wchar_t const* operation = L"IMFSourceReader::GetCurrentMediaType";
                auto result = m_reader->GetCurrentMediaType(FirstVideoStream, currentType.GetAddressOf());
                GUID subtype{};
                HRESULT interlaceAttributeResult = E_UNEXPECTED; // not queried yet
                if (SUCCEEDED(result))
                {
                    operation = L"IMFMediaType::GetGUID(MF_MT_SUBTYPE)";
                    result = currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
                }
                if (SUCCEEDED(result) && subtype != MFVideoFormat_NV12)
                {
                    operation = L"Validate initial decoded NV12 subtype";
                    result = MF_E_UNSUPPORTED_D3D_TYPE;
                }
                if (SUCCEEDED(result))
                {
                    operation = L"IMFMediaType::GetUINT32(MF_MT_INTERLACE_MODE)";
                    m_decodedInterlace = MFVideoInterlace_Unknown;
                    interlaceAttributeResult = currentType->GetUINT32(MF_MT_INTERLACE_MODE, &m_decodedInterlace);
                    result = interlaceAttributeResult;
                    if (result == MF_E_ATTRIBUTENOTFOUND)
                    {
                        m_decodedInterlace = MFVideoInterlace_Unknown;
                        result = S_OK; // only this optional attribute may be absent
                    }
                }
                platform::Log(L"engine", L"stage=VideoReader.FirstDecodedType; function=" + std::wstring(operation) +
                    L" decodedMode=" + std::to_wstring(m_decodedInterlace) + L" HRESULT=" + platform::HexResult(result) +
                    L" interlaceAttributeHRESULT=" + platform::HexResult(interlaceAttributeResult));
                if (FAILED(result))
                {
                    frame.status = result;
                    frame.sample.Reset();
                    return frame;
                }
            }
            interlace::SampleFlags flags{
                ReadSampleFlag(frame.sample.Get(), MFSampleExtension_Interlaced),
                ReadSampleFlag(frame.sample.Get(), MFSampleExtension_SingleField),
                ReadSampleFlag(frame.sample.Get(), MFSampleExtension_RepeatFirstField),
                ReadSampleFlag(frame.sample.Get(), MFSampleExtension_BottomFieldFirst)
            };
            bool formatChanged = (frame.flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) != 0 ||
                (m_interlaceLogged && (frame.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0);
            auto decision = interlace::Classify(m_decodedInterlace, flags, formatChanged);
            if (!m_interlaceLogged || decision != interlace::Decision::Progressive)
            {
                platform::Log(L"engine", L"stage=VideoReader.SampleInterlace; function=VideoReader::Take decision=" +
                    std::wstring(interlace::Name(decision)) + L" compressedMode=" + std::to_wstring(m_sourceInterlace) +
                    L" decodedMode=" + std::to_wstring(m_decodedInterlace) + L" timestamp100ns=" + std::to_wstring(frame.timestamp) +
                    L" readerFlags=" + std::to_wstring(frame.flags) + L" " + FlagText(L"Interlaced", flags.interlaced) +
                    L" " + FlagText(L"SingleField", flags.singleField) + L" " + FlagText(L"RepeatFirstField", flags.repeatFirstField) +
                    L" " + FlagText(L"BottomFieldFirst", flags.bottomFieldFirst));
                m_interlaceLogged = true;
            }
            if (decision != interlace::Decision::Progressive)
            {
                // Return a normal mailbox failure; do not throw outside the
                // runtime's frame error handling or submit this sample to D3D.
                frame.status = MF_E_INVALIDMEDIATYPE;
                frame.sample.Reset();
            }
            else if (initialTypeChange)
                frame.flags &= ~static_cast<DWORD>(MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED);
        }
        return frame;
    }
    void VideoReader::SeekToStart()
    {
        if (!m_reader || !m_mailbox || m_seeking)
            throw platform::Failure(MF_E_INVALIDREQUEST, L"Begin loop flush: reader unavailable or seek already pending");
        {
            std::scoped_lock lock(m_mailbox->mutex);
            m_mailbox->flushing = true;
            ResetEvent(m_mailbox->flushed.get()); ResetEvent(m_mailbox->event.get());
        }
        m_seeking = true; m_seekDeadline = GetTickCount64() + 2000;
        auto result = m_reader->Flush(FirstVideoStream);
        platform::Log(L"engine", L"stage=VideoReader.LoopFlush; function=IMFSourceReader::Flush HRESULT=" + platform::HexResult(result));
        platform::Check(result, L"IMFSourceReader::Flush: prepare loop seek");
    }
    std::optional<HRESULT> VideoReader::CompleteSeekToStart()
    {
        if (!m_seeking) return {};
        bool completed{};
        {
            std::scoped_lock lock(m_mailbox->mutex);
            auto wait = WaitForSingleObject(m_mailbox->flushed.get(), 0);
            if (wait == WAIT_FAILED)
            {
                auto result = HRESULT_FROM_WIN32(GetLastError());
                m_seeking = false; m_seekDeadline = 0;
                return result;
            }
            if (m_mailbox->ready && FAILED(m_mailbox->frame.status))
            {
                auto result = m_mailbox->frame.status;
                m_seeking = false; m_seekDeadline = 0;
                return result;
            }
            completed = wait == WAIT_OBJECT_0;
            if (!completed) ResetEvent(m_mailbox->event.get());
        }
        if (!completed)
        {
            if (GetTickCount64() < m_seekDeadline) return {};
            m_seeking = false; m_seekDeadline = 0;
            auto result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            platform::Log(L"engine", L"stage=VideoReader.LoopFlush; OnFlush deadline expired HRESULT=" + platform::HexResult(result));
            return result;
        }
        {
            std::scoped_lock lock(m_mailbox->mutex);
            m_mailbox->frame = {}; m_mailbox->pending = m_mailbox->ready = false;
            ResetEvent(m_mailbox->event.get());
        }
        PROPVARIANT position{};
        position.vt = VT_I8; position.hVal.QuadPart = 0;
        auto result = m_reader->SetCurrentPosition(GUID_NULL, position);
        platform::Log(L"engine", L"stage=VideoReader.LoopSeek; function=IMFSourceReader::SetCurrentPosition position100ns=0 HRESULT=" +
            platform::HexResult(result));
        {
            std::scoped_lock lock(m_mailbox->mutex);
            m_mailbox->flushing = false;
        }
        m_seeking = false; m_seekDeadline = 0;
        if (SUCCEEDED(result)) m_interlaceLogged = false;
        return result;
    }
    void VideoReader::Close() noexcept
    {
        bool flushPending{};
        if (m_mailbox)
        {
            std::scoped_lock lock(m_mailbox->mutex);
            m_mailbox->active = false;
            m_mailbox->frame.sample.Reset();
            flushPending = m_mailbox->flushing;
        }
        if (m_reader && m_mailbox)
        {
            HRESULT flush = S_OK;
            if (!flushPending)
            {
                ResetEvent(m_mailbox->flushed.get());
                flush = m_reader->Flush(FirstVideoStream);
            }
            if (SUCCEEDED(flush) && WaitForSingleObject(m_mailbox->flushed.get(), 2000) != WAIT_OBJECT_0)
                platform::Log(L"engine", L"Source Reader flush did not complete within 2 seconds; callback mailbox remains independently owned.");
        }
        m_reader.Reset(); m_manager.Reset(); m_mailbox.reset();
        m_sourceInterlace = m_decodedInterlace = MFVideoInterlace_Unknown;
        m_interlaceLogged = false;
        m_seeking = false; m_seekDeadline = 0; m_duration = m_framePeriod = 0;
        m_metadata = {};
    }
}
