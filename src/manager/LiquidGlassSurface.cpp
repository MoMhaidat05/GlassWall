#include "pch.h"
#include "LiquidGlassSurface.h"
#include "platform/win32/Support.h"
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Metadata.h>

namespace glasswall::manager
{
    using namespace winrt;
    using namespace Microsoft::UI::Composition;
    using namespace Microsoft::UI::Xaml;
    using namespace Windows::Foundation::Numerics;
    using Windows::UI::Color;

    std::shared_ptr<LiquidGlassSurface> LiquidGlassSurface::Attach(FrameworkElement const& host, Controls::Button const& button)
    {
        auto surface = std::shared_ptr<LiquidGlassSurface>(new LiquidGlassSurface);
        surface->m_host = host;
        surface->m_button = button;
        surface->Initialize();
        return surface;
    }
    void LiquidGlassSurface::Initialize()
    {
        auto compositor = Hosting::ElementCompositionPreview::GetElementVisual(m_host).Compositor();
        m_visual = compositor.CreateShapeVisual();
        m_outer = compositor.CreateRoundedRectangleGeometry();
        m_inner = compositor.CreateRoundedRectangleGeometry();
        m_body = compositor.CreateSpriteShape(m_outer);
        m_body.StrokeThickness(1.2f);
        m_rim = compositor.CreateSpriteShape(m_inner);
        m_rim.StrokeThickness(1.0f);
        m_reflection = compositor.CreateSpriteShape(m_outer);
        m_reflection.StrokeThickness(2.2f);
        m_visual.Shapes().Append(m_body);
        m_visual.Shapes().Append(m_rim);
        m_visual.Shapes().Append(m_reflection);
        Hosting::ElementCompositionPreview::SetElementChildVisual(m_host, m_visual);
        auto weak = weak_from_this();
        m_size = m_host.SizeChanged([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Resize(); });
        m_theme = m_host.ActualThemeChanged([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->RefreshPalette(); });
        m_enter = m_button.PointerEntered([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Feedback(1.015f); });
        m_leave = m_button.PointerExited([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Feedback(1.f); });
        m_press = m_button.PointerPressed([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Feedback(.985f); });
        m_release = m_button.PointerReleased([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Feedback(1.f); });
        m_capture = m_button.PointerCaptureLost([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Stop(); });
        m_keyDown = m_button.KeyDown([weak](auto const&, Input::KeyRoutedEventArgs const& args)
        {
            if (args.Key() == Windows::System::VirtualKey::Space || args.Key() == Windows::System::VirtualKey::Enter)
                if (auto self = weak.lock()) self->Feedback(.985f);
        });
        m_keyUp = m_button.KeyUp([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Feedback(1.f); });
        m_enabledToken = m_button.RegisterPropertyChangedCallback(Controls::Control::IsEnabledProperty(), [weak](auto const&, auto const&)
        {
            if (auto self = weak.lock()) { self->Stop(); self->m_visual.Opacity(self->m_button.IsEnabled() ? 1.f : .35f); }
        });
        // Settings notifications can arrive on a worker; marshal before using XAML/composition.
        auto queue = m_host.DispatcherQueue();
        auto changed = [weak, queue](auto const&, auto const&)
        {
            queue.TryEnqueue([weak] { if (auto self = weak.lock()) self->RefreshPalette(); });
        };
        m_effects = m_settings.AdvancedEffectsEnabledChanged(changed);
        try { m_contrast = m_accessibility.HighContrastChanged(changed); }
        catch (hresult_error const& error)
        {
            // This unpackaged Windows installation exposes the event in metadata
            // but rejects its registration. UISettings color notifications also
            // cover contrast-theme palette changes; keep the same material/fallback.
            if (error.code() != HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && error.code() != CLASS_E_CLASSNOTAVAILABLE) throw;
            m_colors = m_settings.ColorValuesChanged(changed);
            platform::Log(L"manager", L"HighContrastChanged unavailable; using ColorValuesChanged.");
        }
        if (Windows::Foundation::Metadata::ApiInformation::IsEventPresent(L"Windows.UI.ViewManagement.UISettings", L"AnimationsEnabledChanged"))
        {
            m_animations = m_settings.AnimationsEnabledChanged(changed);
        }
        m_initialized = true;
        Resize();
        RefreshPalette();
        m_visual.Opacity(m_button.IsEnabled() ? 1.f : .35f);
    }
    void LiquidGlassSurface::Resize()
    {
        auto w = static_cast<float>(m_host.ActualWidth());
        auto h = static_cast<float>(m_host.ActualHeight());
        if (w < 12 || h < 12) return;
        Stop();
        m_visual.Size({ w, h });
        m_visual.CenterPoint({ w / 2, h / 2, 0 });
        m_outer.Offset({ 2, 2 });
        m_outer.Size({ w - 4, h - 4 });
        m_outer.CornerRadius({ (h - 4) / 2, (h - 4) / 2 });
        m_inner.Offset({ 5, 5 });
        m_inner.Size({ w - 10, h - 10 });
        m_inner.CornerRadius({ (h - 10) / 2, (h - 10) / 2 });
    }
    void LiquidGlassSurface::RefreshPalette()
    {
        Stop();
        m_simple = m_accessibility.HighContrast() || !m_settings.AdvancedEffectsEnabled();
        m_visual.IsVisible(m_visible && !m_simple);
        if (m_simple)
        {
            m_button.Background(Media::SolidColorBrush{ m_settings.GetColorValue(Windows::UI::ViewManagement::UIColorType::Background) });
            m_button.Foreground(Media::SolidColorBrush{ m_settings.GetColorValue(Windows::UI::ViewManagement::UIColorType::Foreground) });
            m_button.BorderBrush(m_button.Foreground());
            return;
        }
        m_button.Background(Media::SolidColorBrush{ Color{ 0, 0, 0, 0 } });
        m_button.BorderBrush(Media::SolidColorBrush{ Color{ 0, 0, 0, 0 } });
        m_button.ClearValue(Controls::Control::ForegroundProperty());
        auto c = m_visual.Compositor();
        bool dark = m_host.ActualTheme() == ElementTheme::Dark;
        auto lens = c.CreateRadialGradientBrush();
        lens.EllipseCenter({ .22f, -.65f });
        lens.EllipseRadius({ 1.0f, 1.65f });
        lens.ColorStops().Append(c.CreateColorGradientStop(0, dark ? Color{255, 122, 125, 125} : Color{255, 252, 252, 249}));
        lens.ColorStops().Append(c.CreateColorGradientStop(.42f, dark ? Color{255, 66, 70, 73} : Color{255, 232, 233, 229}));
        lens.ColorStops().Append(c.CreateColorGradientStop(.78f, dark ? Color{255, 38, 42, 47} : Color{255, 204, 207, 203}));
        lens.ColorStops().Append(c.CreateColorGradientStop(1, dark ? Color{255, 46, 51, 55} : Color{255, 221, 223, 219}));
        m_body.FillBrush(lens);
        auto edge = c.CreateLinearGradientBrush();
        edge.StartPoint({ .1f, 0 }); edge.EndPoint({ .8f, 1 });
        edge.ColorStops().Append(c.CreateColorGradientStop(0, Color{200, 255, 255, 245}));
        edge.ColorStops().Append(c.CreateColorGradientStop(.28f, Color{65, 237, 241, 240}));
        edge.ColorStops().Append(c.CreateColorGradientStop(.62f, Color{180, 14, 18, 22}));
        edge.ColorStops().Append(c.CreateColorGradientStop(.86f, Color{90, 212, 223, 224}));
        edge.ColorStops().Append(c.CreateColorGradientStop(1, Color{165, 245, 247, 237}));
        m_body.StrokeBrush(edge);
        auto inside = c.CreateLinearGradientBrush();
        inside.StartPoint({ .15f, 0 }); inside.EndPoint({ .65f, 1 });
        inside.ColorStops().Append(c.CreateColorGradientStop(0, Color{80, 250, 250, 240}));
        inside.ColorStops().Append(c.CreateColorGradientStop(.35f, Color{0, 255, 255, 255}));
        inside.ColorStops().Append(c.CreateColorGradientStop(.72f, Color{95, 8, 12, 18}));
        inside.ColorStops().Append(c.CreateColorGradientStop(1, Color{50, 226, 231, 227}));
        m_rim.StrokeBrush(inside);
        auto specular = c.CreateLinearGradientBrush();
        specular.StartPoint({ .1f, 0 }); specular.EndPoint({ .35f, 1 });
        specular.ColorStops().Append(c.CreateColorGradientStop(0, Color{125, 255, 255, 249}));
        specular.ColorStops().Append(c.CreateColorGradientStop(.12f, Color{40, 255, 255, 249}));
        specular.ColorStops().Append(c.CreateColorGradientStop(.5f, Color{0, 255, 255, 255}));
        specular.ColorStops().Append(c.CreateColorGradientStop(.85f, Color{0, 255, 255, 255}));
        specular.ColorStops().Append(c.CreateColorGradientStop(1, Color{65, 211, 224, 222}));
        m_reflection.StrokeBrush(specular);
    }
    void LiquidGlassSurface::Feedback(float scale)
    {
        if (!m_visible || !m_motion || m_simple || !m_button.IsEnabled() || !m_settings.AnimationsEnabled()) { Stop(); return; }
        auto animation = m_visual.Compositor().CreateVector3KeyFrameAnimation();
        animation.InsertKeyFrame(1, float3{ scale, scale, 1 });
        animation.Duration(std::chrono::milliseconds(160));
        animation.IterationCount(1);
        m_visual.StartAnimation(L"Scale", animation);
    }
    void LiquidGlassSurface::Stop()
    {
        if (!m_visual) return;
        m_visual.StopAnimation(L"Scale");
        m_visual.Scale({ 1, 1, 1 });
    }
    void LiquidGlassSurface::Visible(bool value)
    {
        m_visible = value;
        Stop();
        m_visual.IsVisible(value && !m_simple);
    }
    LiquidGlassSurface::~LiquidGlassSurface()
    {
        if (!m_initialized) return;
        m_host.SizeChanged(m_size); m_host.ActualThemeChanged(m_theme);
        m_button.PointerEntered(m_enter); m_button.PointerExited(m_leave);
        m_button.PointerPressed(m_press); m_button.PointerReleased(m_release); m_button.PointerCaptureLost(m_capture);
        m_button.KeyDown(m_keyDown); m_button.KeyUp(m_keyUp);
        m_button.UnregisterPropertyChangedCallback(Controls::Control::IsEnabledProperty(), m_enabledToken);
        m_settings.AdvancedEffectsEnabledChanged(m_effects);
        if (m_contrast.value) m_accessibility.HighContrastChanged(m_contrast);
        if (m_colors.value) m_settings.ColorValuesChanged(m_colors);
        if (m_animations.value) m_settings.AnimationsEnabledChanged(m_animations);
        Stop();
        Hosting::ElementCompositionPreview::SetElementChildVisual(m_host, nullptr);
        m_visual.Close();
    }
}
