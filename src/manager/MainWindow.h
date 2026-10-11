#pragma once
#include "MainWindow.g.h"
#include "WallpaperItem.h"
#include "LiquidGlassSurface.h"
#include "ipc/Client.h"
#include <deque>
#include <set>
#include <winrt/Windows.Storage.Streams.h>

namespace winrt::GlassWall::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        void Import_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Apply_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Refresh_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Pause_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Wallpapers_SelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void Wallpapers_ContainerContentChanging(Microsoft::UI::Xaml::Controls::ListViewBase const&, Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const&);
        void LibraryLayout_SizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void Navigation_SelectionChanged(Microsoft::UI::Xaml::Controls::NavigationView const&, Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
        void SidebarToggle_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void StartupSettings_Opened(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&);
        void StartWithWindows_Toggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void Wallpapers_SizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void WallpaperMenu_Click(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void WallpaperMenu_Loaded(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    private:
        enum class Action { Refresh, Import, Apply, Pause, Resume, Rename, Relink, Remove };
        winrt::fire_and_forget RunCommandAsync(Action action, std::wstring value = {}, std::wstring detail = {});
        winrt::fire_and_forget ManageWallpaperAsync(Action action, std::wstring id);
        winrt::fire_and_forget PickImportAsync();
        winrt::fire_and_forget LoadThumbnailAsync(::glasswall::library::Entry entry, uint64_t revision);
        winrt::fire_and_forget CompleteThumbnailAsync(::glasswall::library::Entry entry, uint64_t revision, Windows::Storage::Streams::IRandomAccessStream stream, std::wstring diagnostic);
        void SetBusy(bool value);
        void ShowError(std::wstring const& message);
        void ShowSelection();
        void RequestThumbnail(GlassWall::WallpaperItem const& item);
        void PumpThumbnails();
        void ReleaseImages();
        void UpdateCardWidth(double width);
        void UpdateTitleBar();
        GlassWall::WallpaperItem FindItem(std::wstring const& id);
        void AcceptSnapshot(::glasswall::library::Snapshot const&, std::vector<bool> const&, ::glasswall::ipc::PlaybackResponse const&, std::wstring const& selection, bool rebuild = false);
        Windows::Foundation::Collections::IObservableVector<GlassWall::WallpaperItem> m_items;
        std::shared_ptr<::glasswall::manager::LiquidGlassSurface> m_glass;
        std::deque<std::wstring> m_pendingThumbnails, m_decodedImages;
        std::set<std::wstring> m_queued, m_realized, m_failedThumbnails;
        std::wstring m_selectedId, m_activeId;
        winrt::Windows::UI::ViewManagement::UISettings m_uiSettings;
        winrt::event_token m_titleColors{};
        double m_cardWidth{248.};
        uint64_t m_libraryRevision{};
        bool m_loadingStartupSetting{};
        bool m_closed{}, m_busy{}, m_thumbnailBusy{}, m_visible{ true }, m_userPaused{}, m_hasWallpaper{}, m_storageReady{};
    };
}
namespace winrt::GlassWall::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
