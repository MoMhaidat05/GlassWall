#pragma once
#include "WallpaperItem.g.h"
#include "library/Library.h"

namespace winrt::GlassWall::implementation
{
    struct WallpaperItem : WallpaperItemT<WallpaperItem>
    {
        WallpaperItem() = default;
        void Initialize(::glasswall::library::Entry const& entry, bool available);
        hstring Id() const { return hstring{ m_entry.id }; }
        hstring Name() const { return hstring{ m_entry.displayName }; }
        hstring Path() const { return hstring{ m_entry.path }; }
        hstring Details() const { return m_details; }
        Microsoft::UI::Xaml::Visibility ActiveVisibility() const;
        void Active(bool value);
        Microsoft::UI::Xaml::Visibility SelectedVisibility() const;
        void Selected(bool value);
        Microsoft::UI::Xaml::Visibility PlaceholderVisibility() const;
        hstring PreviewStatus() const { return m_previewStatus; }
        void PreviewStatus(std::wstring const& value);
        double CardWidth() const { return m_cardWidth; }
        double PreviewHeight() const { return m_cardWidth * 9. / 16.; }
        void CardWidth(double value);
        Microsoft::UI::Xaml::Media::ImageSource Thumbnail() const { return m_thumbnail; }
        void Thumbnail(Microsoft::UI::Xaml::Media::ImageSource const& value);
        event_token PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) { return m_changed.add(handler); }
        void PropertyChanged(event_token const& token) noexcept { m_changed.remove(token); }
        ::glasswall::library::Entry const& Entry() const { return m_entry; }
        bool Available() const { return m_available; }
    private:
        ::glasswall::library::Entry m_entry;
        hstring m_details, m_previewStatus{ L"Preparing preview…" };
        bool m_active{}, m_selected{}, m_available{};
        double m_cardWidth{ 248. };
        Microsoft::UI::Xaml::Media::ImageSource m_thumbnail{ nullptr };
        event<Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_changed;
    };
}
namespace winrt::GlassWall::factory_implementation
{
    struct WallpaperItem : WallpaperItemT<WallpaperItem, implementation::WallpaperItem> {};
}
