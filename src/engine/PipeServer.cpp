#include "PipeServer.h"
#include "../ipc/PipeIo.h"
#include "WallpaperRuntime.h"
#include "../core/LibraryProtocol.h"
#include <winrt/base.h>
#include <vector>
#include <array>
#include <cstring>

namespace glasswall::engine
{
    void Serve(platform::Identity const& identity, HANDLE stop, HWND control, WallpaperRuntime& runtime) noexcept
    {
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
            platform::UserSecurity security(identity.sid);
            // Keep the sole instance alive across requests. Closing it after each
            // reply removes the endpoint before a back-to-back client can reopen it.
            platform::Handle pipe(CreateNamedPipeW(identity.pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1, 4096, static_cast<DWORD>(ipc::MaxPlaybackMessage), ipc::TimeoutMs, security.attributes()));
            if (!pipe) throw platform::Error("CreateNamedPipeW", GetLastError());
            while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT)
            {
                auto connected = ipc::PipeIo(pipe.get(), ipc::Operation::Connect, nullptr, 0, stop, INFINITE);
                if (connected.error)
                {
                    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) break;
                    throw platform::Error("ConnectNamedPipe", connected.error);
                }

                std::array<std::byte, ipc::MaxPlaybackMessage> message{};
                auto read = ipc::PipeIo(pipe.get(), ipc::Operation::Read, message.data(),
                    static_cast<DWORD>(message.size()), stop);
                bool shutdown{};
                std::uint32_t magic{};
                if (read.bytes >= sizeof(magic)) std::memcpy(&magic, message.data(), sizeof(magic));
                bool responseWritten{};
                if (!read.error && magic == ipc::PlaybackMagic && read.bytes >= sizeof(ipc::PlaybackRequest))
                {
                    ipc::PlaybackRequest request;
                    std::memcpy(&request, message.data(), sizeof(request));
                    auto response = runtime.Snapshot();
                    bool valid = ipc::IsValid(request, read.bytes);
                    std::wstring path;
                    if (valid && request.pathBytes)
                    {
                        static_assert(sizeof(wchar_t) == 2);
                        path.resize(request.pathBytes / 2);
                        std::memcpy(path.data(), message.data() + sizeof(request), request.pathBytes);
                        valid = path.back() == L'\0' && path.find(L'\0') == path.size() - 1;
                        if (valid) path.pop_back();
                    }
                    if (valid && request.command == ipc::Command::GetLibrary)
                    {
                        try
                        {
                        auto json = runtime.LibrarySnapshot();
                        ipc::LibraryResponse header;
                        header.processId = GetCurrentProcessId();
                        header.jsonBytes = static_cast<std::uint32_t>((json.size() + 1) * sizeof(wchar_t));
                        std::vector<std::byte> outgoing(sizeof(header) + header.jsonBytes);
                        if (!ipc::IsValid(header, outgoing.size())) throw platform::Error("Library reply exceeds limit", ERROR_FILE_TOO_LARGE);
                        std::memcpy(outgoing.data(), &header, sizeof(header));
                        std::memcpy(outgoing.data() + sizeof(header), json.c_str(), header.jsonBytes);
                        auto written = ipc::PipeIo(pipe.get(), ipc::Operation::Write, outgoing.data(), static_cast<DWORD>(outgoing.size()), stop);
                        responseWritten = !written.error;
                        }
                        catch (...)
                        {
                            // A metadata/query failure must never terminate playback.
                            response.result = ipc::PlaybackResult::InvalidRequest; response.error = E_FAIL;
                            auto written = ipc::PipeIo(pipe.get(), ipc::Operation::Write, &response, sizeof(response), stop);
                            responseWritten = !written.error;
                            platform::Log(L"engine", L"Library snapshot failed; playback remains independent.");
                        }
                    }
                    else
                    {
                        if (valid) response = runtime.Submit(request.command, std::move(path));
                        else { response.result = ipc::PlaybackResult::InvalidRequest; response.error = E_INVALIDARG; }
                        auto written = ipc::PipeIo(pipe.get(), ipc::Operation::Write, &response, sizeof(response), stop);
                        responseWritten = !written.error;
                    }
                }
                else if (!read.error && read.bytes == sizeof(ipc::Request))
                {
                    ipc::Request request;
                    std::memcpy(&request, message.data(), sizeof(request));
                    auto valid = ipc::IsValid(request);
                    shutdown = valid && request.command == ipc::Command::Shutdown;
                    ipc::Response response;
                    response.processId = GetCurrentProcessId();
                    response.result = valid ? ipc::Result::Success : ipc::Result::InvalidRequest;
                    response.state = shutdown ? ipc::EngineState::Stopping : ipc::EngineState::Ready;
                    auto written = ipc::PipeIo(pipe.get(), ipc::Operation::Write, &response, sizeof(response), stop);
                    responseWritten = !written.error;
                }
                if (responseWritten)
                {
                    // Keep the reply alive until the reader acknowledges it; never
                    // block indefinitely in FlushFileBuffers.
                    std::uint32_t acknowledgement{};
                    ipc::PipeIo(pipe.get(), ipc::Operation::Read, &acknowledgement, sizeof(acknowledgement), stop);
                }
                DisconnectNamedPipe(pipe.get());
                if (shutdown)
                {
                    platform::Log(L"engine", L"Explicit shutdown command accepted.");
                    PostMessageW(control, ShutdownMessage, 0, 0);
                    break;
                }
                // Invalid/short/stalled clients are disconnected after a bounded read.
            }
        }
        catch (platform::Error const& error)
        {
            platform::Log(L"engine", platform::ErrorText(error.code()));
            PostMessageW(control, FailureMessage, error.code(), 0);
        }
        catch (...)
        {
            platform::Log(L"engine", L"Unexpected IPC server failure.");
            PostMessageW(control, FailureMessage, ERROR_UNHANDLED_EXCEPTION, 0);
        }
    }
}
