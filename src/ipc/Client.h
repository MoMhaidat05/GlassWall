#pragma once
#include "../core/Protocol.h"
#include "../core/PlaybackProtocol.h"
#include <Windows.h>
#include <string>

namespace glasswall::ipc
{
    struct Reply
    {
        DWORD error{};
        Response response{};
        explicit operator bool() const noexcept { return error == ERROR_SUCCESS; }
    };
    Reply Exchange(Request const& request);
    inline Reply Query() { return Exchange(Request{}); }
    inline Reply Shutdown()
    {
        Request request;
        request.command = Command::Shutdown;
        return Exchange(request);
    }
    DWORD StartEngine();
    Reply WaitForEngine(DWORD timeoutMs = 4000);
    std::wstring Describe(Reply const& reply);
    struct PlaybackReply
    {
        DWORD error{};
        PlaybackResponse response{};
        char const* operation{}; // local diagnostic only; never part of wire layout
        explicit operator bool() const noexcept { return error == ERROR_SUCCESS; }
    };
    PlaybackReply Playback(Command command, std::wstring const& path = {}, DWORD expectedProcessId = 0);
    struct LibraryReply
    {
        DWORD error{}, processId{};
        std::wstring json;
        char const* operation{};
        explicit operator bool() const noexcept { return error == ERROR_SUCCESS; }
    };
    LibraryReply QueryLibrary(DWORD expectedProcessId);
}
