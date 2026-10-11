#pragma once
#include "../library/Library.h"
#include "../core/LibraryProtocol.h"
#include "../platform/win32/HResult.h"
#include <fstream>
#include <ostream>
#include <algorithm>
#include <cwctype>

namespace glasswall::diagnostics
{
    inline bool RunLibraryTests(std::filesystem::path const& parent, std::wostream& output)
    {
        using namespace library;
        GUID guid{}; platform::Check(CoCreateGuid(&guid), L"Test directory GUID");
        wchar_t suffix[40]{}; StringFromGUID2(guid, suffix, 40);
        auto root = std::filesystem::absolute(parent) / suffix;
        std::filesystem::create_directories(root);
        auto source = root / L"خلفية café 日本.mp4";
        // This fixture tests reference storage only. It is deliberately not playable video.
        { std::ofstream file(source, std::ios::binary); file << "reference-only fixture"; }
        auto check = [](bool value) { if (!value) throw std::runtime_error("Library regression check failed"); };
        auto rejected = [&](auto action) { bool failed{}; try { action(); } catch (...) { failed = true; } check(failed); };
        std::wstring id, added;
        auto catalogDirectory = root / L"catalog";
        auto replacement = root / L"replacement.mp4";
        { std::ofstream file(replacement, std::ios::binary); file << "replacement reference fixture"; }
        {
            Store store(root / L"relink-catalog");
            auto entry = store.Import(source.wstring(), { 1920, 1080, 30, 1, 30000000 });
            store.Rename(entry.id, L"Keep this name"); store.Select(entry.id);
            auto before = Encode(store.Data());
            rejected([&] { store.Relink(entry.id, (root / L"missing.mp4").wstring(), {}); });
            rejected([&] { store.Relink(L"unknown", replacement.wstring(), {}); });
            check(Encode(store.Data()) == before);
            platform::Handle blocker(CreateFileW((root / L"relink-catalog" / L"library.json").c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            check(static_cast<bool>(blocker));
            rejected([&] { store.Relink(entry.id, replacement.wstring(), { 1280, 720, 24, 1, 20000000 }); });
            check(Encode(store.Data()) == before); blocker.reset();
            // Relinking an unavailable source keeps the identity, name and selection.
            std::filesystem::rename(source, root / L"temporarily-moved.mp4");
            store.Relink(entry.id, replacement.wstring(), { 1280, 720, 24, 1, 20000000 });
            auto changed = Find(store.Data(), entry.id);
            check(changed.id == entry.id && changed.displayName == L"Keep this name" && changed.addedUtc == entry.addedUtc &&
                changed.path == replacement.wstring() && changed.video.width == 1280 && changed.video.duration100ns == 20000000 &&
                changed.thumbnail.empty() && store.Data().lastSelectedId == entry.id);
            std::filesystem::rename(root / L"temporarily-moved.mp4", source);
            auto duplicate = store.Import(source.wstring(), {});
            before = Encode(store.Data());
            rejected([&] { store.Relink(entry.id, source.wstring(), {}); });
            check(Encode(store.Data()) == before);
            store.Relink(entry.id, replacement.wstring(), { 640, 360, 25, 1, 10000000 }); // same path metadata refresh
            check(Find(store.Data(), entry.id).video.width == 640);
            auto payload = DecodeRelink(RelinkPayload(entry.id, replacement.wstring()));
            check(payload.first == entry.id && payload.second == replacement.wstring());
            rejected([&] { DecodeRelink(L"{invalid"); });
            rejected([&] { DecodeRelink(L"{\"id\":\"unknown\"}"); });
            store.Remove(duplicate.id);
        }
        {
            Store store(root / L"relink-catalog");
            auto changed = store.Data().entries.at(0);
            check(changed.displayName == L"Keep this name" && changed.path == replacement.wstring() && changed.video.width == 640);
            std::filesystem::rename(replacement, root / L"unavailable-replacement.mp4");
            check(!Available(changed)); store.Remove(changed.id);
            check(store.Data().entries.empty() && std::filesystem::exists(root / L"unavailable-replacement.mp4"));
        }
        {
            Store store(catalogDirectory);
            auto entry = store.Import(source.wstring(), { 1920, 1080, 30000, 1001, 30000000 });
            id = entry.id; added = entry.addedUtc;
            auto variant = source.wstring();
            std::transform(variant.begin(), variant.end(), variant.begin(), towupper);
            check(store.Import(variant, {}).id == id && store.Data().entries.size() == 1);
            check(store.Import((source.parent_path() / L"." / source.filename()).wstring(), {}).id == id);
            store.Rename(id, L"هدوء / café \"日本\""); store.Select(id);
            check(store.Data().lastSelectedId == id);
            rejected([&] { store.Import((root / L"missing.mp4").wstring(), {}); });
            rejected([&] { store.Import(L"relative.mp4", {}); });
            rejected([&] { store.Import(root.wstring(), {}); });
            rejected([&] { store.Rename(id, L"  "); });
            rejected([&] { store.Rename(id, std::wstring(129, L'x')); });
            rejected([&] { store.Remove(L"unknown"); });
            rejected([&] { Store competing(catalogDirectory); });
            auto original = Encode(store.Data());
            platform::Handle blocker(CreateFileW((catalogDirectory / L"library.json").c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            check(static_cast<bool>(blocker));
            rejected([&] { store.Rename(id, L"must not commit"); });
            check(Encode(store.Data()) == original); blocker.reset();
        }
        {
            Store store(catalogDirectory);
            auto entry = Find(store.Data(), id);
            check(entry.addedUtc == added && entry.displayName == L"هدوء / café \"日本\"" &&
                entry.video.frameRateNumerator == 30000 && entry.video.duration100ns == 30000000 &&
                store.Data().lastSelectedId == id);
            Snapshot snapshot{ store.Data(), id, source.wstring(), L"", S_OK, 1, true };
            check(DecodeSnapshot(EncodeSnapshot(snapshot)).activeId == id);
            auto renamedSource = root / L"moved.mp4";
            std::filesystem::rename(source, renamedSource);
            check(!Available(Find(store.Data(), id)) && store.Data().lastSelectedId == id);
            std::filesystem::rename(renamedSource, source);
            store.Rename(id, L"backup name"); store.Rename(id, L"newest name");
        }
        { std::ofstream file(catalogDirectory / L"library.json", std::ios::binary | std::ios::trunc); file << "{invalid"; }
        {
            Store recovered(catalogDirectory);
            check(Find(recovered.Data(), id).displayName == L"backup name" && !recovered.Warning().empty());
            bool preserved{};
            for (auto const& file : std::filesystem::directory_iterator(catalogDirectory))
                if (file.path().filename().wstring().starts_with(L"library.json.corrupt-")) preserved = true;
            check(preserved);
            recovered.Remove(id);
            check(recovered.Data().entries.empty() && recovered.Data().lastSelectedId.empty() && std::filesystem::exists(source));
        }
        auto emptyDirectory = root / L"corrupt-no-backup";
        std::filesystem::create_directories(emptyDirectory);
        { std::ofstream file(emptyDirectory / L"library.json"); file << "[]"; }
        { Store store(emptyDirectory); check(store.Data().entries.empty() && !store.Warning().empty()); store.Import(source.wstring(), {}); }
        auto futureDirectory = root / L"future-schema";
        std::filesystem::create_directories(futureDirectory);
        { std::ofstream file(futureDirectory / L"library.json"); file << "{\"schemaVersion\":99}"; }
        rejected([&] { Store store(futureDirectory); });
        check(std::filesystem::exists(futureDirectory / L"library.json"));
        rejected([&] { Decode(L"{\"schemaVersion\":1,\"entries\":[],\"lastSelectedId\":\"unknown\"}"); });
        ipc::PlaybackRequest request;
        request.command = ipc::Command::ApplyWallpaper;
        check(!ipc::IsValid(request, sizeof(request)));
        request.pathBytes = 74; check(ipc::IsValid(request, sizeof(request) + 74));
        request.command = ipc::Command::RestoreWallpaper; check(!ipc::IsValid(request, sizeof(request) + 74));
        request.pathBytes = 0; check(ipc::IsValid(request, sizeof(request)));
        request.command = ipc::Command::RelinkWallpaper;
        check(!ipc::IsValid(request, sizeof(request)));
        request.pathBytes = 74; check(ipc::IsValid(request, sizeof(request) + 74));
        request.pathBytes = static_cast<std::uint32_t>(ipc::MaxPathCharacters * 2 + 2);
        check(!ipc::IsValid(request, sizeof(request) + request.pathBytes));
        request.command = static_cast<ipc::Command>(16); request.pathBytes = 74;
        check(!ipc::IsValid(request, sizeof(request) + 74));
        ipc::LibraryResponse reply; reply.processId = 1; reply.jsonBytes = 4;
        check(ipc::IsValid(reply, 20)); reply.jsonBytes = 3; check(!ipc::IsValid(reply, 19));
        output << L"Library storage/protocol tests passed. No engine, decoder, desktop or foreground window was started.\n"
            << L"Test fixtures retained at " << root.wstring() << L"\n";
        return true;
    }
}
