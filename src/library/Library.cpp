#include "Library.h"
#include "../media/mf/MediaPath.h"
#include "../platform/win32/HResult.h"
#include <ShlObj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <set>

namespace glasswall::library
{
    using namespace winrt::Windows::Data::Json;
    namespace
    {
        void Require(bool condition)
        {
            if (!condition) throw platform::Failure(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"Invalid library metadata");
        }
        bool Text(std::wstring const& value, std::size_t limit, bool empty = false)
        {
            return (empty || !value.empty()) && value.size() <= limit &&
                std::none_of(value.begin(), value.end(), [](wchar_t c) { return c < 32; });
        }
        std::wstring String(JsonObject const& object, wchar_t const* key)
        {
            auto value = object.GetNamedString(key);
            return { value.c_str(), value.size() };
        }
        std::uint32_t Number(JsonObject const& object, wchar_t const* key)
        {
            auto value = object.GetNamedNumber(key);
            Require(std::isfinite(value) && value >= 0 && value <= UINT32_MAX && value == std::floor(value));
            return static_cast<std::uint32_t>(value);
        }
        bool Id(std::wstring const& id)
        {
            if (id.size() != 36) return false;
            for (std::size_t i = 0; i < id.size(); ++i)
            {
                if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != L'-') return false; }
                else if (!((id[i] >= L'0' && id[i] <= L'9') || (id[i] >= L'a' && id[i] <= L'f'))) return false;
            }
            return true;
        }
        std::wstring NewId()
        {
            GUID guid{};
            platform::Check(CoCreateGuid(&guid), L"CoCreateGuid");
            wchar_t text[40]{};
            StringFromGUID2(guid, text, 40);
            std::wstring result(text + 1, 36);
            std::transform(result.begin(), result.end(), result.begin(), towlower);
            return result;
        }
        std::wstring Now()
        {
            SYSTEMTIME time{}; GetSystemTime(&time);
            wchar_t text[32]{};
            swprintf_s(text, L"%04u-%02u-%02uT%02u:%02u:%02uZ", time.wYear, time.wMonth,
                time.wDay, time.wHour, time.wMinute, time.wSecond);
            return text;
        }
        bool Date(std::wstring const& value)
        {
            if (value.size() != 20 || value[4] != L'-' || value[7] != L'-' || value[10] != L'T' ||
                value[13] != L':' || value[16] != L':' || value[19] != L'Z') return false;
            for (std::size_t i = 0; i < value.size(); ++i)
                if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19 &&
                    (value[i] < L'0' || value[i] > L'9')) return false;
            return true;
        }
        void ValidateEntry(Entry const& entry)
        {
            std::filesystem::path path(entry.path);
            Require(Id(entry.id) && Text(entry.displayName, 128) && Text(entry.path, 4095) &&
                path.is_absolute() && entry.path.rfind(L"\\\\", 0) != 0 && path.root_name().wstring().size() == 2 &&
                path.lexically_normal().wstring() == entry.path && Date(entry.addedUtc) &&
                Text(entry.thumbnail, 4095, true) && entry.video.duration100ns >= 0 &&
                ((entry.video.width == 0) == (entry.video.height == 0)) &&
                ((entry.video.frameRateNumerator == 0) == (entry.video.frameRateDenominator == 0)));
        }
        void Set(JsonObject const& object, wchar_t const* key, std::wstring const& value)
        { object.SetNamedValue(key, JsonValue::CreateStringValue(value)); }
        void Set(JsonObject const& object, wchar_t const* key, std::uint32_t value)
        { object.SetNamedValue(key, JsonValue::CreateNumberValue(value)); }
        JsonObject EntryJson(Entry const& entry)
        {
            JsonObject object, video;
            Set(object, L"id", entry.id); Set(object, L"displayName", entry.displayName);
            Set(object, L"path", entry.path); Set(object, L"addedUtc", entry.addedUtc);
            Set(object, L"thumbnail", entry.thumbnail);
            Set(video, L"width", entry.video.width); Set(video, L"height", entry.video.height);
            Set(video, L"frameRateNumerator", entry.video.frameRateNumerator);
            Set(video, L"frameRateDenominator", entry.video.frameRateDenominator);
            // A decimal string preserves all 64 bits through Windows.Data.Json's double representation.
            Set(video, L"duration100ns", std::to_wstring(entry.video.duration100ns));
            object.SetNamedValue(L"video", video);
            return object;
        }
        Entry ReadEntry(JsonObject const& object)
        {
            Entry entry;
            entry.id = String(object, L"id"); entry.displayName = String(object, L"displayName");
            entry.path = String(object, L"path"); entry.addedUtc = String(object, L"addedUtc");
            if (object.HasKey(L"thumbnail")) entry.thumbnail = String(object, L"thumbnail");
            auto video = object.GetNamedObject(L"video");
            entry.video.width = Number(video, L"width"); entry.video.height = Number(video, L"height");
            entry.video.frameRateNumerator = Number(video, L"frameRateNumerator");
            entry.video.frameRateDenominator = Number(video, L"frameRateDenominator");
            auto duration = String(video, L"duration100ns");
            Require(!duration.empty() && duration.size() <= 19 &&
                std::all_of(duration.begin(), duration.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }));
            try { entry.video.duration100ns = std::stoll(duration); } catch (...) { Require(false); }
            ValidateEntry(entry);
            return entry;
        }
        std::wstring Read(std::filesystem::path const& path)
        {
            platform::Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file) throw platform::Error("Open library metadata", GetLastError());
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.get(), &size)) throw platform::Error("Library size", GetLastError());
            Require(size.QuadPart > 0 && size.QuadPart <= MaxJsonBytes);
            std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
            DWORD read{};
            if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
                throw platform::Error("Read library", GetLastError());
            Require(read == bytes.size());
            if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
            int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            Require(count > 0);
            std::wstring result(static_cast<std::size_t>(count), L'\0');
            Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), result.data(), count) == count);
            return result;
        }
        bool Exists(std::filesystem::path const& path)
        {
            auto attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) return true;
            auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
            throw platform::Error("Inspect library metadata", error);
        }
        void PreserveCorrupt(std::filesystem::path const& path)
        {
            auto destination = path.wstring() + L".corrupt-" + NewId();
            if (!MoveFileExW(path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
                throw platform::Error("Preserve corrupt library", GetLastError());
        }
    }
    bool SamePath(std::wstring const& left, std::wstring const& right) noexcept
    {
        return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
            static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }
    bool Available(Entry const& entry) noexcept
    {
        auto attributes = GetFileAttributesW(entry.path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    Entry const& Find(Catalog const& catalog, std::wstring const& id)
    {
        auto found = std::find_if(catalog.entries.begin(), catalog.entries.end(), [&](Entry const& entry) { return entry.id == id; });
        if (found == catalog.entries.end()) throw platform::Failure(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), L"Wallpaper ID is not in the library");
        return *found;
    }
    Entry const* FindPath(Catalog const& catalog, std::wstring const& path) noexcept
    {
        auto found = std::find_if(catalog.entries.begin(), catalog.entries.end(), [&](Entry const& entry) { return SamePath(entry.path, path); });
        return found == catalog.entries.end() ? nullptr : &*found;
    }
    std::wstring Encode(Catalog const& catalog)
    {
        JsonObject object; JsonArray entries;
        Set(object, L"schemaVersion", 1u); Set(object, L"lastSelectedId", catalog.lastSelectedId);
        for (auto const& entry : catalog.entries) entries.Append(EntryJson(entry));
        object.SetNamedValue(L"entries", entries);
        return std::wstring(object.Stringify());
    }
    Catalog Decode(std::wstring const& json)
    {
        try
        {
            auto object = JsonObject::Parse(json);
            auto version = Number(object, L"schemaVersion");
            if (version > 1) throw platform::Failure(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH), L"Library schema is newer; metadata was left untouched");
            Require(version == 1);
            Catalog result; result.lastSelectedId = String(object, L"lastSelectedId");
            auto entries = object.GetNamedArray(L"entries"); Require(entries.Size() <= MaxEntries);
            std::set<std::wstring> ids;
            for (auto const& value : entries)
            {
                auto entry = ReadEntry(value.GetObject());
                Require(ids.insert(entry.id).second && !FindPath(result, entry.path));
                result.entries.push_back(std::move(entry));
            }
            Require(result.lastSelectedId.empty() || ids.contains(result.lastSelectedId));
            return result;
        }
        catch (winrt::hresult_error const&) { Require(false); }
        throw platform::Failure(E_UNEXPECTED, L"Unreachable library parser state");
    }
    std::wstring EncodeEntry(Entry const& entry)
    {
        auto object = EntryJson(entry);
        object.SetNamedValue(L"available", JsonValue::CreateBooleanValue(Available(entry)));
        return std::wstring(object.Stringify());
    }
    std::wstring EncodeSnapshot(Snapshot const& snapshot)
    {
        auto object = JsonObject::Parse(Encode(snapshot.catalog));
        auto entries = object.GetNamedArray(L"entries");
        for (std::uint32_t i = 0; i < entries.Size(); ++i)
            entries.GetObjectAt(i).SetNamedValue(L"available", JsonValue::CreateBooleanValue(Available(snapshot.catalog.entries[i])));
        Set(object, L"activeId", snapshot.activeId); Set(object, L"activePath", snapshot.activePath);
        Set(object, L"warning", snapshot.warning); Set(object, L"processId", snapshot.processId);
        object.SetNamedValue(L"ready", JsonValue::CreateBooleanValue(snapshot.ready));
        object.SetNamedValue(L"storageError", JsonValue::CreateNumberValue(snapshot.storageError));
        return std::wstring(object.Stringify());
    }
    Snapshot DecodeSnapshot(std::wstring const& json)
    {
        Snapshot result; result.catalog = Decode(json);
        auto object = JsonObject::Parse(json);
        result.activeId = String(object, L"activeId"); result.activePath = String(object, L"activePath");
        result.warning = String(object, L"warning"); result.processId = Number(object, L"processId");
        result.ready = object.GetNamedBoolean(L"ready");
        auto error = object.GetNamedNumber(L"storageError");
        Require(std::isfinite(error) && error >= INT32_MIN && error <= INT32_MAX && error == std::floor(error));
        result.storageError = static_cast<HRESULT>(error);
        return result;
    }
    std::wstring RenamePayload(std::wstring const& id, std::wstring const& name)
    { JsonObject object; Set(object, L"id", id); Set(object, L"name", name); return std::wstring(object.Stringify()); }
    std::pair<std::wstring, std::wstring> DecodeRename(std::wstring const& json)
    { auto object = JsonObject::Parse(json); return { String(object, L"id"), String(object, L"name") }; }
    std::wstring RelinkPayload(std::wstring const& id, std::wstring const& path)
    { JsonObject object; Set(object, L"id", id); Set(object, L"path", path); return std::wstring(object.Stringify()); }
    std::pair<std::wstring, std::wstring> DecodeRelink(std::wstring const& json)
    { auto object = JsonObject::Parse(json); return { String(object, L"id"), String(object, L"path") }; }

    std::filesystem::path Store::DefaultDirectory()
    {
        PWSTR folder{};
        platform::Check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder), L"LocalAppData directory");
        std::filesystem::path path;
        try { path = std::filesystem::path(folder) / L"GlassWall"; } catch (...) { CoTaskMemFree(folder); throw; }
        CoTaskMemFree(folder); return path;
    }
    Store::Store(std::filesystem::path directory) : m_directory(std::move(directory))
    {
        std::filesystem::create_directories(m_directory);
        auto lockPath = m_directory / L"library.lock";
        m_lock.reset(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!m_lock) throw platform::Error("Library already owned or inaccessible", GetLastError());
        Load();
    }
    void Store::Load()
    {
        auto primary = m_directory / L"library.json";
        auto backup = m_directory / L"library.json.bak";
        if (Exists(primary))
        {
            try { m_data = Decode(Read(primary)); return; }
            catch (platform::Failure const& failure)
            {
                if (failure.code != HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) throw;
                PreserveCorrupt(primary);
                m_warning = L"Corrupt library metadata was preserved as library.json.corrupt-*; ";
            }
        }
        if (Exists(backup))
        {
            try { m_data = Decode(Read(backup)); }
            catch (platform::Failure const& failure)
            {
                if (failure.code != HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) throw;
                m_warning += L"backup metadata is invalid and was retained; an empty library is available.";
                return;
            }
            Save(m_data);
            m_warning += L"recovered the previous valid backup (the newest change may be absent).";
        }
        else if (!m_warning.empty()) m_warning += L"no valid backup; an empty library is available.";
    }
    void Store::Save(Catalog const& data)
    {
        auto bytes = winrt::to_string(Encode(data));
        if (bytes.size() > MaxJsonBytes) throw platform::Failure(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), L"Library metadata limit exceeded");
        auto primary = m_directory / L"library.json";
        auto backup = m_directory / L"library.json.bak";
        auto temporary = m_directory / (L"library.tmp-" + NewId());
        platform::Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!file) throw platform::Error("Create atomic library save", GetLastError());
        try
        {
            DWORD written{};
            if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
                throw platform::Error("Write library", GetLastError());
            if (written != bytes.size()) throw platform::Error("Short library write", ERROR_WRITE_FAULT);
            if (!FlushFileBuffers(file.get())) throw platform::Error("Flush library", GetLastError());
            file.reset();
            if (Exists(primary))
            {
                if (!ReplaceFileW(primary.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr))
                    throw platform::Error("Replace library atomically", GetLastError());
            }
            else if (!MoveFileExW(temporary.c_str(), primary.c_str(), MOVEFILE_WRITE_THROUGH))
                throw platform::Error("Commit initial library", GetLastError());
        }
        catch (...) { file.reset(); DeleteFileW(temporary.c_str()); throw; }
    }
    Entry Store::Import(std::wstring const& validatedPath, VideoMetadata const& metadata)
    {
        auto path = media::ValidateLocalPath(validatedPath);
        if (auto existing = FindPath(m_data, path)) return *existing;
        if (m_data.entries.size() >= MaxEntries) throw platform::Failure(HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES), L"Library entry limit reached");
        Entry entry{ NewId(), std::filesystem::path(path).stem().wstring(), path, Now(), {}, metadata };
        if (entry.displayName.size() > 128) entry.displayName.resize(128);
        if (!entry.displayName.empty() && entry.displayName.back() >= 0xD800 && entry.displayName.back() <= 0xDBFF)
            entry.displayName.pop_back(); // never split a UTF-16 surrogate pair at the display-name limit
        ValidateEntry(entry);
        auto next = m_data; next.entries.push_back(entry);
        Save(next); m_data = std::move(next); return entry;
    }
    void Store::Rename(std::wstring const& id, std::wstring const& name)
    {
        if (!Text(name, 128) || std::all_of(name.begin(), name.end(), iswspace))
            throw platform::Failure(E_INVALIDARG, L"Use a nonempty display name of at most 128 characters");
        Find(m_data, id);
        auto next = m_data;
        for (auto& entry : next.entries) if (entry.id == id) entry.displayName = name;
        Save(next); m_data = std::move(next);
    }
    void Store::Relink(std::wstring const& id, std::wstring const& validatedPath, VideoMetadata const& metadata)
    {
        Find(m_data, id);
        auto path = media::ValidateLocalPath(validatedPath);
        if (auto duplicate = FindPath(m_data, path); duplicate && duplicate->id != id)
            throw platform::Failure(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), L"This source already belongs to another wallpaper");
        auto next = m_data;
        for (auto& entry : next.entries) if (entry.id == id)
        {
            entry.path = path; entry.video = metadata; entry.thumbnail.clear();
            ValidateEntry(entry);
        }
        Save(next); m_data = std::move(next);
    }
    void Store::Remove(std::wstring const& id)
    {
        Find(m_data, id); auto next = m_data;
        std::erase_if(next.entries, [&](Entry const& entry) { return entry.id == id; });
        if (next.lastSelectedId == id) next.lastSelectedId.clear();
        Save(next); m_data = std::move(next); // never deletes or copies the source video
    }
    void Store::Select(std::wstring const& id)
    {
        Find(m_data, id);
        if (m_data.lastSelectedId == id) return;
        auto next = m_data; next.lastSelectedId = id;
        Save(next); m_data = std::move(next);
    }
}
