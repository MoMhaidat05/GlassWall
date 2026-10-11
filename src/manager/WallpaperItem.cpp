#include "pch.h"
#include "WallpaperItem.h"
#include "WallpaperItem.g.cpp"

namespace winrt::GlassWall::implementation
{
    void WallpaperItem::Initialize(::glasswall::library::Entry const& entry, bool available)
    {
        m_entry = entry;
        m_available = available;
        auto details = !available ? L"Source unavailable" : std::to_wstring(entry.video.width) + L" × " + std::to_wstring(entry.video.height);
        if (available && entry.video.duration100ns > 0)
        {
            auto seconds = entry.video.duration100ns / 10000000;
            wchar_t duration[32]{};
            swprintf_s(duration, L" · %lld:%02lld", seconds / 60, seconds % 60);
            details += duration;
        }
        m_details = hstring{ details };
        if (!available) m_previewStatus = L"Source unavailable";
    }
    Microsoft::UI::Xaml::Visibility WallpaperItem::ActiveVisibility() const
    { return m_active ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
    void WallpaperItem::Active(bool value)
    {
        if (m_active == value) return;
        m_active = value;
        m_changed(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"ActiveVisibility" });
    }
    void WallpaperItem::Thumbnail(Microsoft::UI::Xaml::Media::ImageSource const& value)
    {
        m_thumbnail = value;
        m_changed(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"Thumbnail" });
        m_changed(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"PlaceholderVisibility" });
    }
    Microsoft::UI::Xaml::Visibility WallpaperItem::SelectedVisibility() const
    { return m_selected ? Microsoft::UI::Xaml::Visibility::Visible : Microsoft::UI::Xaml::Visibility::Collapsed; }
    void WallpaperItem::Selected(bool value)
    {
        if (m_selected == value) return;
        m_selected = value;
        m_changed(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"SelectedVisibility" });
    }
    Microsoft::UI::Xaml::Visibility WallpaperItem::PlaceholderVisibility() const
    { return m_thumbnail ? Microsoft::UI::Xaml::Visibility::Collapsed : Microsoft::UI::Xaml::Visibility::Visible; }
    void WallpaperItem::PreviewStatus(std::wstring const& value)
    {
        m_previewStatus = hstring{ value };
        m_changed(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"PreviewStatus" });
    }
    void WallpaperItem::CardWidth(double value)
    {
        if (m_cardWidth == value) return;
        m_cardWidth = value;
        m_changed(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"CardWidth" });
        m_changed(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"PreviewHeight" });
    }
}
