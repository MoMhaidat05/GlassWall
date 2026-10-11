#ifndef GLASSWALL_THUMBNAIL_CHECK
#include "pch.h"
#endif
#include "ThumbnailCache.h"
#include <ShlObj.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <algorithm>
#include <mutex>
#include <functional>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Graphics.Imaging.h>

namespace glasswall::manager
{
    namespace fs = std::filesystem;
    using namespace winrt;
    using namespace Windows::Storage;
    using namespace Windows::Storage::Streams;
    using namespace Windows::Storage::FileProperties;
    using namespace Windows::Graphics::Imaging;
    namespace
    {
        std::mutex cacheMutex;
        constexpr uint32_t MaxEdge = 320;
        constexpr uint64_t MaxEncodedBytes = 256 * 1024;
        constexpr size_t MaxCachedFiles = 256;
        constexpr DWORD VideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        constexpr DWORD AllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
        void Check(HRESULT hr, wchar_t const* operation)
        { if (FAILED(hr)) throw hresult_error(hr, operation); }
        std::wstring FailureText()
        {
            try { throw; }
            catch (hresult_error const& error)
            {
                wchar_t code[16]{}; swprintf_s(code, L"0x%08X", static_cast<unsigned>(error.code()));
                return std::wstring(error.message()) + L" (" + code + L")";
            }
            catch (std::exception const& error) { return std::wstring(to_hstring(error.what())); }
            catch (...) { return L"Unknown thumbnail error"; }
        }
        fs::path CacheDirectory()
        {
            PWSTR raw{};
            Check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw), L"Locate thumbnail cache");
            fs::path folder = fs::path(raw) / L"GlassWall" / L"Thumbnails";
            CoTaskMemFree(raw);
            return folder;
        }
        void Trim(fs::path const& folder)
        {
            std::vector<fs::directory_entry> files;
            for (auto const& file : fs::directory_iterator(folder))
                if (file.is_regular_file() && file.path().extension() == L".jpg") files.push_back(file);
            std::sort(files.begin(), files.end(), [](auto const& a, auto const& b) { return a.last_write_time() > b.last_write_time(); });
            for (size_t i = MaxCachedFiles; i < files.size(); ++i)
            { std::error_code ignored; fs::remove(files[i].path(), ignored); }
        }
        InMemoryRandomAccessStream Encode(uint32_t w, uint32_t h, array_view<uint8_t const> pixels)
        {
            InMemoryRandomAccessStream encoded;
            auto encoder = BitmapEncoder::CreateAsync(BitmapEncoder::JpegEncoderId(), encoded).get();
            encoder.SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore, w, h, 96, 96, pixels);
            encoder.FlushAsync().get();
            if (!encoded.Size() || encoded.Size() > MaxEncodedBytes)
                throw hresult_error(E_FAIL, L"Encoded preview exceeds the 256 KiB limit");
            encoded.Seek(0);
            return encoded;
        }
        InMemoryRandomAccessStream ShellFrame(std::wstring const& path)
        {
            auto file = StorageFile::GetFileFromPathAsync(path).get();
            auto thumbnail = file.GetThumbnailAsync(ThumbnailMode::VideosView, MaxEdge, ThumbnailOptions::ResizeThumbnail).get();
            if (!thumbnail) throw hresult_error(E_FAIL, L"Shell returned no thumbnail");
            if (thumbnail.Type() != ThumbnailType::Image) throw hresult_error(E_FAIL, L"Shell returned a file icon instead of a video frame");
            if (thumbnail.Size() > 8 * 1024 * 1024) throw hresult_error(E_FAIL, L"Shell thumbnail exceeds the input limit");
            auto decoder = BitmapDecoder::CreateAsync(thumbnail).get();
            auto w = decoder.PixelWidth(), h = decoder.PixelHeight();
            if (!w || !h || w > 4096 || h > 4096) throw hresult_error(E_FAIL, L"Shell thumbnail dimensions exceed the input limit");
            auto ratio = std::min(1.0, static_cast<double>(MaxEdge) / std::max(w, h));
            w = std::max(1u, static_cast<uint32_t>(w * ratio));
            h = std::max(1u, static_cast<uint32_t>(h * ratio));
            BitmapTransform transform;
            transform.ScaledWidth(w); transform.ScaledHeight(h);
            auto pixels = decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore, transform,
                ExifOrientationMode::RespectExifOrientation, ColorManagementMode::DoNotColorManage).get().DetachPixelData();
            return Encode(w, h, pixels);
        }
        struct MediaSession
        {
            MediaSession() { Check(MFStartup(MF_VERSION, MFSTARTUP_LITE), L"Start still-frame Media Foundation session"); }
            ~MediaSession() { MFShutdown(); }
        };
        InMemoryRandomAccessStream MediaFrame(std::wstring const& path)
        {
            // Separate manager-owned reader. No engine decoder, DXGI manager, D3D device,
            // audio decoding, clock, presentation or live preview. Destroyed after one still.
            MediaSession session;
            com_ptr<IMFAttributes> attributes;
            Check(MFCreateAttributes(attributes.put(), 2), L"Create still-reader attributes");
            Check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE), L"Enable bounded still scaling");
            Check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE), L"Configure still reader");
            com_ptr<IMFSourceReader> reader;
            Check(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put()), L"Open video for still extraction");
            Check(reader->SetStreamSelection(AllStreams, FALSE), L"Disable other thumbnail streams");
            Check(reader->SetStreamSelection(VideoStream, TRUE), L"Select thumbnail video stream");
            com_ptr<IMFMediaType> native;
            Check(reader->GetNativeMediaType(VideoStream, 0, native.put()), L"Read thumbnail source type");
            UINT32 w{}, h{};
            Check(MFGetAttributeSize(native.get(), MF_MT_FRAME_SIZE, &w, &h), L"Read thumbnail source dimensions");
            if (!w || !h || w > 4096 || h > 4096 || static_cast<uint64_t>(w) * h > 3840ull * 2160)
                throw hresult_error(E_FAIL, L"Still extraction source exceeds the 4K pixel budget");
            auto ratio = std::min(1.0, static_cast<double>(MaxEdge) / std::max(w, h));
            // Even output dimensions also support the source reader's YUV scaler.
            w = std::max(2u, static_cast<uint32_t>(w * ratio) & ~1u);
            h = std::max(2u, static_cast<uint32_t>(h * ratio) & ~1u);
            com_ptr<IMFMediaType> output;
            Check(MFCreateMediaType(output.put()), L"Create thumbnail output type");
            Check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), L"Set thumbnail major type");
            Check(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), L"Set thumbnail pixel format");
            Check(MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, w, h), L"Set bounded thumbnail dimensions");
            Check(MFSetAttributeRatio(output.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), L"Set thumbnail aspect ratio");
            Check(reader->SetCurrentMediaType(VideoStream, nullptr, output.get()), L"Negotiate scaled still output");
            com_ptr<IMFMediaType> actual;
            Check(reader->GetCurrentMediaType(VideoStream, actual.put()), L"Read negotiated still type");
            Check(MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &w, &h), L"Read negotiated still size");
            if (!w || !h || w > MaxEdge || h > MaxEdge) throw hresult_error(E_FAIL, L"Decoder did not honor thumbnail dimension bounds");
            auto deadline = GetTickCount64() + 4000;
            for (unsigned attempt = 0; attempt < 8 && GetTickCount64() < deadline; ++attempt)
            {
                DWORD flags{}; LONGLONG timestamp{};
                com_ptr<IMFSample> sample;
                Check(reader->ReadSample(VideoStream, 0, nullptr, &flags, &timestamp, sample.put()), L"Decode still frame");
                if (flags & MF_SOURCE_READERF_ERROR)
                    throw hresult_error(E_FAIL, L"Still reader reported a decoder error");
                if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                {
                    // The first read can refine stride/rate metadata while retaining
                    // the requested scaled RGB output. Validate it before copying.
                    actual = nullptr;
                    Check(reader->GetCurrentMediaType(VideoStream, actual.put()), L"Read refined still type");
                    Check(MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &w, &h), L"Read refined still dimensions");
                    GUID subtype{};
                    Check(actual->GetGUID(MF_MT_SUBTYPE, &subtype), L"Read refined still pixel format");
                    if (!w || !h || w > MaxEdge || h > MaxEdge || subtype != MFVideoFormat_RGB32)
                        throw hresult_error(E_FAIL, L"Still reader changed beyond the bounded RGB output");
                }
                if (sample)
                {
                    com_ptr<IMFMediaBuffer> buffer;
                    Check(sample->ConvertToContiguousBuffer(buffer.put()), L"Read still-frame buffer");
                    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
                    auto buffer2d = buffer.try_as<IMF2DBuffer2>();
                    BYTE* top{}; LONG stride{};
                    if (buffer2d)
                    {
                        BYTE* start{}; DWORD length{};
                        Check(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &top, &stride, &start, &length), L"Lock still frame");
                        auto topAddress = static_cast<int64_t>(reinterpret_cast<intptr_t>(top));
                        auto startAddress = static_cast<int64_t>(reinterpret_cast<intptr_t>(start));
                        auto lastRowOffset = static_cast<int64_t>(stride) * (h - 1);
                        auto low = topAddress + std::min<int64_t>(0, lastRowOffset);
                        auto high = topAddress + std::max<int64_t>(0, lastRowOffset) + static_cast<int64_t>(w) * 4;
                        if (static_cast<uint64_t>(std::abs(static_cast<int64_t>(stride))) < w * 4ull || low < startAddress || high > startAddress + length)
                        { buffer2d->Unlock2D(); throw hresult_error(E_FAIL, L"Invalid still-frame stride"); }
                        for (uint32_t row = 0; row < h; ++row)
                            memcpy(pixels.data() + static_cast<size_t>(row) * w * 4, top + static_cast<ptrdiff_t>(row) * stride, w * 4);
                        buffer2d->Unlock2D();
                    }
                    else
                    {
                        UINT32 rawStride{};
                        if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) stride = static_cast<LONG>(rawStride);
                        else Check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride), L"Read still-frame stride");
                        DWORD length{};
                        Check(buffer->Lock(&top, nullptr, &length), L"Lock still-frame bytes");
                        auto rowBytes = static_cast<uint64_t>(std::abs(static_cast<int64_t>(stride)));
                        if (rowBytes < w * 4ull || rowBytes * (h - 1) + w * 4ull > length)
                        { buffer->Unlock(); throw hresult_error(E_FAIL, L"Still-frame buffer is shorter than its dimensions"); }
                        if (stride < 0) top += static_cast<size_t>(h - 1) * rowBytes;
                        for (uint32_t row = 0; row < h; ++row)
                            memcpy(pixels.data() + static_cast<size_t>(row) * w * 4, top + static_cast<ptrdiff_t>(row) * stride, w * 4);
                        buffer->Unlock();
                    }
                    return Encode(w, h, pixels);
                }
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            }
            throw hresult_error(E_FAIL, L"No still frame within the eight-read / four-second cooperative budget");
        }
    }
    void InvalidateThumbnail(std::wstring const& entryId, fs::path const& cacheDirectory)
    {
        std::scoped_lock lock(cacheMutex); // drain an old extraction before deleting its output
        GUID id{};
        auto braced = entryId.size() == 36 ? L"{" + entryId + L"}" : entryId;
        Check(CLSIDFromString(braced.c_str(), &id), L"Validate thumbnail invalidation ID");
        wchar_t safeId[40]{}; StringFromGUID2(id, safeId, 40);
        auto prefix = std::wstring(safeId) + L"-";
        auto folder = cacheDirectory.empty() ? CacheDirectory() : cacheDirectory;
        if (!fs::exists(folder)) return;
        for (auto const& file : fs::directory_iterator(folder))
        {
            auto extension = file.path().extension();
            if (file.is_regular_file() && !file.is_symlink() && file.path().filename().wstring().starts_with(prefix) &&
                (extension == L".jpg" || extension == L".tmp"))
                fs::remove(file.path());
        }
    }
    ThumbnailResult OpenThumbnail(library::Entry const& entry, fs::path const& cacheDirectory, bool shellFirst)
    {
        std::scoped_lock lock(cacheMutex);
        GUID id{};
        // The library stores GUIDs without braces; CLSIDFromString requires the braced form.
        auto braced = entry.id.size() == 36 ? L"{" + entry.id + L"}" : entry.id;
        Check(CLSIDFromString(braced.c_str(), &id), L"Validate thumbnail cache ID");
        wchar_t safeId[40]{}; StringFromGUID2(id, safeId, 40);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(entry.path.c_str(), GetFileExInfoStandard, &info))
            throw hresult_error(HRESULT_FROM_WIN32(GetLastError()), L"Read thumbnail source fingerprint");
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))
            return { nullptr, L"Source is a directory, offline or requires cloud recall" };
        auto folder = cacheDirectory.empty() ? CacheDirectory() : cacheDirectory;
        fs::create_directories(folder);
        auto key = std::wstring(safeId) + L"-" + std::to_wstring(std::hash<std::wstring>{}(entry.path)) + L"-" +
            std::to_wstring(info.nFileSizeHigh) + L"-" + std::to_wstring(info.nFileSizeLow) +
            L"-" + std::to_wstring(info.ftLastWriteTime.dwHighDateTime) + L"-" + std::to_wstring(info.ftLastWriteTime.dwLowDateTime);
        auto cache = folder / (key + L".jpg");
        if (fs::exists(cache))
        {
            try
            {
                if (!fs::file_size(cache) || fs::file_size(cache) > MaxEncodedBytes) throw hresult_error(E_FAIL);
                auto stream = StorageFile::GetFileFromPathAsync(cache.wstring()).get().OpenAsync(FileAccessMode::Read).get();
                auto decoder = BitmapDecoder::CreateAsync(stream).get();
                if (!decoder.PixelWidth() || !decoder.PixelHeight() || decoder.PixelWidth() > MaxEdge || decoder.PixelHeight() > MaxEdge)
                    throw hresult_error(E_FAIL);
                stream.Seek(0);
                return { stream, L"Cached still preview" };
            }
            catch (...) { std::error_code ignored; fs::remove(cache, ignored); }
        }
        InMemoryRandomAccessStream encoded{ nullptr };
        std::wstring diagnostic, shellFailure;
        if (shellFirst)
        {
            try { encoded = ShellFrame(entry.path); diagnostic = L"Shell still preview"; }
            catch (...) { shellFailure = FailureText(); }
        }
        if (!encoded)
        {
            try { encoded = MediaFrame(entry.path); diagnostic = L"Media Foundation still preview"; }
            catch (...) { return { nullptr, L"Shell: " + (shellFirst ? shellFailure : L"skipped by headless checker") + L" · Media Foundation: " + FailureText() }; }
            if (!shellFailure.empty()) diagnostic += L" · Shell fallback reason: " + shellFailure;
        }
        auto temp = folder / (key + L"-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
        try
        {
            auto target = StorageFolder::GetFolderFromPathAsync(folder.wstring()).get().CreateFileAsync(temp.filename().wstring(), CreationCollisionOption::ReplaceExisting).get();
            auto output = target.OpenAsync(FileAccessMode::ReadWrite).get();
            RandomAccessStream::CopyAsync(encoded.GetInputStreamAt(0), output.GetOutputStreamAt(0)).get();
            output.FlushAsync().get(); output.Close();
            if (!MoveFileExW(temp.c_str(), cache.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw_last_error();
            for (auto const& old : fs::directory_iterator(folder))
            {
                if (old.path() != cache && old.path().extension() == L".jpg" && old.path().filename().wstring().starts_with(safeId))
                { std::error_code ignored; fs::remove(old.path(), ignored); }
            }
            Trim(folder);
        }
        catch (...)
        {
            diagnostic += L" · Disk cache unavailable: " + FailureText();
            std::error_code ignored; fs::remove(temp, ignored);
        }
        encoded.Seek(0);
        return { encoded, diagnostic };
    }
}
