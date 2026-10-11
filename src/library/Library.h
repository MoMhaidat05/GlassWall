#pragma once
#include "../platform/win32/Support.h"
#include <filesystem>
#include <cstdint>
#include <vector>

namespace glasswall::library
{
    inline constexpr std::size_t MaxEntries = 512;
    inline constexpr std::size_t MaxJsonBytes = 2 * 1024 * 1024;
    struct VideoMetadata
    {
        std::uint32_t width{}, height{}, frameRateNumerator{}, frameRateDenominator{};
        std::int64_t duration100ns{}; // zero means unavailable
    };
    struct Entry
    {
        std::wstring id, displayName, path, addedUtc, thumbnail;
        VideoMetadata video;
    };
    struct Catalog
    {
        std::vector<Entry> entries;
        std::wstring lastSelectedId;
    };
    struct Snapshot
    {
        Catalog catalog;
        std::wstring activeId, activePath, warning;
        HRESULT storageError{};
        DWORD processId{};
        bool ready{};
    };
    bool SamePath(std::wstring const& left, std::wstring const& right) noexcept;
    bool Available(Entry const& entry) noexcept;
    Entry const& Find(Catalog const& catalog, std::wstring const& id);
    Entry const* FindPath(Catalog const& catalog, std::wstring const& path) noexcept;
    std::wstring Encode(Catalog const& catalog);
    Catalog Decode(std::wstring const& json);
    std::wstring EncodeSnapshot(Snapshot const& snapshot);
    Snapshot DecodeSnapshot(std::wstring const& json);
    std::wstring EncodeEntry(Entry const& entry);
    std::wstring RenamePayload(std::wstring const& id, std::wstring const& name);
    std::pair<std::wstring, std::wstring> DecodeRename(std::wstring const& json);
    std::wstring RelinkPayload(std::wstring const& id, std::wstring const& path);
    std::pair<std::wstring, std::wstring> DecodeRelink(std::wstring const& json);

    // Ordinary native types; no XAML, UI lifetime, media decoder or video copies.
    // A lifetime file lock prevents a second session from overwriting stale data.
    class Store
    {
    public:
        explicit Store(std::filesystem::path directory = DefaultDirectory());
        static std::filesystem::path DefaultDirectory();
        Catalog const& Data() const noexcept { return m_data; }
        std::wstring const& Warning() const noexcept { return m_warning; }
        Entry Import(std::wstring const& validatedPath, VideoMetadata const& metadata);
        void Rename(std::wstring const& id, std::wstring const& name);
        void Remove(std::wstring const& id);
        void Relink(std::wstring const& id, std::wstring const& validatedPath, VideoMetadata const& metadata);
        void Select(std::wstring const& id); // called only after engine validation succeeds
    private:
        void Load();
        void Save(Catalog const& data);
        std::filesystem::path m_directory;
        platform::Handle m_lock;
        Catalog m_data;
        std::wstring m_warning;
    };
}
