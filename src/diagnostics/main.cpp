#include "../ipc/Client.h"
#include "../platform/win32/HResult.h"
#include "InterlaceTests.h"
#include "LoopTests.h"
#include "PauseTests.h"
#include "LibraryTests.h"
#include "../application/LibraryService.h"
#include <winrt/Windows.Data.Json.h>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace
{
    void PrintJson(std::wstring const& json)
    {
        auto text = json + L"\n";
        DWORD mode{}, written{}; auto output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetConsoleMode(output, &mode))
        {
            if (!WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
                throw glasswall::platform::Error("Write library JSON", GetLastError());
        }
        else
        {
            auto utf8 = winrt::to_string(text);
            if (!WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr))
                throw glasswall::platform::Error("Write UTF-8 library JSON", GetLastError());
        }
    }
    int LibraryCommand(int argc, wchar_t** argv)
    {
        using namespace glasswall;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        std::wstring_view command(argv[1]);
        if (command == L"--library-tests")
        {
            if (argc != 3) return 2;
            return diagnostics::RunLibraryTests(argv[2], std::wcout) ? 0 : 1;
        }
        bool noArgument = command == L"--library-list" || command == L"--library-restore";
        bool oneArgument = command == L"--library-import" || command == L"--library-get" ||
            command == L"--library-apply" || command == L"--library-remove";
        bool twoArguments = command == L"--library-rename" || command == L"--library-relink";
        if ((noArgument && argc != 2) || (oneArgument && argc != 3) ||
            (twoArguments && argc != 4) || (!noArgument && !oneArgument && !twoArguments)) return 2;
        application::LibraryService service;
        if (command == L"--library-import" || command == L"--library-get")
        {
            auto entry = command == L"--library-import" ? service.Import(argv[2]) : service.Metadata(argv[2]);
            auto object = winrt::Windows::Data::Json::JsonObject::Parse(library::EncodeEntry(entry));
            object.SetNamedValue(L"processId", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(service.ProcessId()));
            PrintJson(std::wstring(object.Stringify())); return 0;
        }
        if (command == L"--library-apply") service.Apply(argv[2]);
        else if (command == L"--library-remove") service.Remove(argv[2]);
        else if (command == L"--library-rename") service.Rename(argv[2], argv[3]);
        else if (command == L"--library-relink") service.Relink(argv[2], argv[3]);
        else if (command == L"--library-restore") service.Restore();
        PrintJson(library::EncodeSnapshot(service.List())); return 0;
    }
    wchar_t const* StateName(glasswall::ipc::PlaybackState state)
    {
        using S = glasswall::ipc::PlaybackState;
        switch (state)
        {
        case S::Empty: return L"Empty"; case S::Loading: return L"Loading";
        case S::Ready: return L"Ready"; case S::Playing: return L"Playing";
        case S::Paused: return L"Paused"; case S::Recovering: return L"Recovering";
        case S::HostUnavailable: return L"HostUnavailable"; case S::Error: return L"Error";
        }
        return L"Unknown";
    }
    void PrintPlayback(glasswall::ipc::PlaybackResponse const& value)
    {
        // Numeric counters/errors and fixed enum labels; no unescaped user strings.
        std::wcout << L"{\"processId\":" << value.processId
            << L",\"state\":\"" << StateName(value.state)
            << L"\",\"result\":" << static_cast<std::uint32_t>(value.result)
            << L",\"error\":" << value.error
            << L",\"requestId\":" << value.requestId
            << L",\"completedRequestId\":" << value.completedRequestId
            << L",\"operationError\":" << value.operationError
            << L",\"operationErrorHex\":\"" << glasswall::platform::HexResult(value.operationError) << L"\""
            << L",\"mediaError\":" << value.mediaError
            << L",\"mediaErrorHex\":\"" << glasswall::platform::HexResult(value.mediaError) << L"\""
            << L",\"deviceRemovedReason\":" << value.deviceRemovedReason
            << L",\"deviceRemovedReasonHex\":\"" << glasswall::platform::HexResult(value.deviceRemovedReason) << L"\""
            << L",\"hardwarePath\":" << static_cast<std::uint32_t>(value.hardware)
            << L",\"hardwareDecodeVerified\":false"
            << L",\"flags\":" << value.flags
            << L",\"pauseReasons\":" << value.pauseReasons
            << L",\"playRequested\":" << value.playRequested
            << L",\"pendingReads\":" << value.pendingReads
            << L",\"pauseTransitions\":" << value.pauseTransitions
            << L",\"resumeTransitions\":" << value.resumeTransitions
            << L",\"decodedFrames\":" << value.decodedFrames
            << L",\"presentedFrames\":" << value.presentedFrames
            << L",\"loopSeeks\":" << value.loops
            << L",\"eofEvents\":" << value.eofEvents
            << L",\"seekAttempts\":" << value.seekAttempts
            << L",\"loopCompletions\":" << value.loopCompletions
            << L",\"loopPhase\":" << static_cast<std::uint32_t>(value.loopPhase)
            << L",\"lastSeekError\":" << value.lastSeekError
            << L",\"lastSeekErrorHex\":\"" << glasswall::platform::HexResult(value.lastSeekError) << L"\""
            << L",\"lastEofTimestamp100ns\":" << value.lastEofTimestamp100ns
            << L",\"loopDecodedTimestamp100ns\":" << value.loopDecodedTimestamp100ns
            << L",\"loopPresentedTimestamp100ns\":" << value.loopPresentedTimestamp100ns
            << L",\"duration100ns\":" << value.duration100ns
            << L",\"hostRecoveries\":" << value.hostRecoveries
            << L",\"deviceRecoveries\":" << value.deviceRecoveries
            << L",\"hostsCreated\":" << value.hostsCreated
            << L",\"devicesCreated\":" << value.devicesCreated
            << L",\"readersCreated\":" << value.readersCreated
            << L",\"position100ns\":" << value.position100ns << L"}\n";
    }
}

int wmain(int argc, wchar_t** argv)
{
    using namespace glasswall;
    if (!platform::IsSupportedWindows()) return ERROR_OLD_WIN_VERSION;
    if (argc < 2 || argc > 4)
    {
        std::wcerr << L"Usage: GlassWall.Probe.exe --start | --status | --shutdown | --protocol-tests | --interlace-tests | --loop-tests | --invalid-version\n"
            << L"       --load <local.mp4> | --play | --pause | --resume | --stop-wallpaper | --playback-state | --pause-tests\n";
        std::wcerr << L"       --library-import <file> | --library-list | --library-get <id> | --library-apply <id>\n"
            << L"       --library-rename <id> <name> | --library-remove <id> | --library-restore | --library-tests <fixture-directory>\n";
        return 2;
    }
    try
    {
        std::wstring_view command(argv[1]);
        if (command.starts_with(L"--library-")) return LibraryCommand(argc, argv);
        auto playbackCommand = ipc::Command::Status;
        if (command == L"--load") playbackCommand = ipc::Command::LoadWallpaper;
        else if (command == L"--play") playbackCommand = ipc::Command::Play;
        else if (command == L"--pause") playbackCommand = ipc::Command::Pause;
        else if (command == L"--resume") playbackCommand = ipc::Command::Resume;
        else if (command == L"--stop-wallpaper") playbackCommand = ipc::Command::StopWallpaper;
        else if (command == L"--playback-state") playbackCommand = ipc::Command::GetPlaybackState;
        if (ipc::IsPlaybackCommand(playbackCommand))
        {
            if ((playbackCommand == ipc::Command::LoadWallpaper ? argc != 3 : argc != 2)) return 2;
            auto path = argc == 3 ? std::filesystem::absolute(argv[2]).wstring() : std::wstring{};
            auto reply = ipc::Playback(playbackCommand, path);
            if (!reply)
            {
                if (reply.operation) std::cerr << reply.operation << ": ";
                std::wcerr << platform::ErrorText(reply.error) << L"\n"; return 3;
            }
            PrintPlayback(reply.response);
            return reply.response.result == ipc::PlaybackResult::Accepted ? 0 : 1;
        }
        if (argc != 2) return 2;
        if (command == L"--interlace-tests") return diagnostics::RunInterlaceTests(std::wcout) ? 0 : 1;
        if (command == L"--loop-tests") return diagnostics::RunLoopTests(std::wcout) ? 0 : 1;
        if (command == L"--pause-tests") return diagnostics::RunPauseTests(std::wcout) ? 0 : 1;
        if (command == L"--protocol-tests")
        {
            ipc::Request good;
            auto badVersion = good; ++badVersion.version;
            auto badMagic = good; badMagic.magic = 0;
            auto badCommand = good; badCommand.command = static_cast<ipc::Command>(99);
            auto badReserved = good; badReserved.reserved = 1;
            if (!ipc::IsValid(good) || ipc::IsValid(badVersion) || ipc::IsValid(badMagic) ||
                ipc::IsValid(badCommand) || ipc::IsValid(badReserved)) return 1;
            ipc::Response response;
            response.processId = 1;
            if (!ipc::IsValid(response)) return 1;
            response.state = static_cast<ipc::EngineState>(99);
            if (ipc::IsValid(response)) return 1;
            ipc::PlaybackRequest playback;
            ipc::PlaybackResponse playbackState;
            playbackState.processId = 1;
            if (!ipc::IsValid(playbackState)) return 1;
            playbackState.pauseReasons = core::AllPauseReasons + 1;
            if (ipc::IsValid(playbackState)) return 1;
            playbackState.pauseReasons = 0;
            playbackState.playRequested = 2;
            if (ipc::IsValid(playbackState)) return 1;
            playbackState.playRequested = 0;
            playbackState.loopPhase = static_cast<core::LoopPhase>(99);
            if (ipc::IsValid(playbackState)) return 1;
            if (!ipc::IsValid(playback, sizeof(playback))) return 1;
            playback.command = ipc::Command::LoadWallpaper;
            if (ipc::IsValid(playback, sizeof(playback))) return 1;
            playback.pathBytes = 4;
            if (!ipc::IsValid(playback, sizeof(playback) + 4) ||
                ipc::IsValid(playback, sizeof(playback) + 3)) return 1;
            playback.pathBytes = static_cast<std::uint32_t>(ipc::MaxPathCharacters * 2 + 2);
            if (ipc::IsValid(playback, sizeof(playback) + playback.pathBytes)) return 1;
            std::wcout << L"Protocol validation checks passed. No Windows playback behavior was tested.\n";
            return 0;
        }
        ipc::Reply reply;
        bool expectRejected{};
        if (command == L"--start")
        {
            auto existing = ipc::Query();
            if (existing) reply = existing;
            else
            {
                auto error = ipc::StartEngine();
                if (error) { std::wcerr << platform::ErrorText(error) << L"\n"; return 3; }
                reply = ipc::WaitForEngine();
            }
        }
        else if (command == L"--status") reply = ipc::Query();
        else if (command == L"--shutdown") reply = ipc::Shutdown();
        else if (command == L"--invalid-version")
        {
            ipc::Request request; ++request.version;
            reply = ipc::Exchange(request); expectRejected = true;
        }
        else return 2;
        std::wcout << ipc::Describe(reply) << L"\n";
        if (!reply) return 3;
        if (expectRejected) return reply.response.result == ipc::Result::InvalidRequest ? 0 : 1;
        return reply.response.result == ipc::Result::Success ? 0 : 1;
    }
    catch (platform::Failure const& error)
    { std::wcerr << error.operation << L": " << platform::HexResult(error.code) << L"\n"; return 1; }
    catch (winrt::hresult_error const& error)
    { std::wcerr << error.message().c_str() << L"\n"; return 1; }
    catch (std::exception const& error) { std::cerr << error.what() << "\n"; return 1; }
}
