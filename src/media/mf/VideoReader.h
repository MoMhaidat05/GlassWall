#pragma once
#include "../../platform/win32/HResult.h"
#include "MediaPath.h"
#include "../../library/Library.h"
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <optional>

namespace glasswall::media
{
    struct VideoFrame
    {
        HRESULT status{ S_OK };
        DWORD flags{};
        LONGLONG timestamp{};
        Microsoft::WRL::ComPtr<IMFSample> sample;
    };
    struct ReaderMailbox
    {
        ReaderMailbox();
        std::mutex mutex;
        platform::Handle event, flushed;
        VideoFrame frame;
        bool active{ true }, ready{}, pending{}, flushing{};
        ULONGLONG readStarted{};
    };
    class VideoReader
    {
    public:
        VideoReader() = default;
        ~VideoReader() { Close(); }
        void Open(std::wstring const& path, ID3D11Device* device, LONGLONG startAt = 0);
        bool Request();
        void SeekToStart();
        std::optional<HRESULT> CompleteSeekToStart();
        ULONGLONG SeekDeadline() const noexcept { return m_seekDeadline; }
        bool Seeking() const noexcept { return m_seeking; }
        std::optional<VideoFrame> Take();
        HANDLE Event() const noexcept { return m_mailbox ? m_mailbox->event.get() : nullptr; }
        void Close() noexcept;
        ULONGLONG ReadDeadline() const;
        void ExtendDeadlines(ULONGLONG milliseconds);
        bool FullRange() const noexcept { return m_fullRange; }
        LONGLONG Duration100ns() const noexcept { return m_duration; }
        LONGLONG FramePeriod100ns() const noexcept { return m_framePeriod; }
        library::VideoMetadata Metadata() const noexcept { return m_metadata; }
    private:
        Microsoft::WRL::ComPtr<IMFSourceReader> m_reader;
        Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_manager;
        std::shared_ptr<ReaderMailbox> m_mailbox;
        bool m_fullRange{};
        UINT32 m_sourceInterlace{}, m_decodedInterlace{};
        bool m_interlaceLogged{};
        bool m_seeking{};
        ULONGLONG m_seekDeadline{};
        LONGLONG m_duration{}, m_framePeriod{};
        library::VideoMetadata m_metadata;
    };
}
