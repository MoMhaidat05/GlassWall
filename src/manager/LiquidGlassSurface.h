#pragma once
#include <memory>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.ViewManagement.h>

namespace glasswall::manager
{
    // A bounded optical approximation for one capsule. No backdrop sampling,
    // shaders, frame callbacks, blur, media objects or perpetual animations.
    class LiquidGlassSurface : public std::enable_shared_from_this<LiquidGlassSurface>
    {
    public:
        static std::shared_ptr<LiquidGlassSurface> Attach(
            winrt::Microsoft::UI::Xaml::FrameworkElement const& host,
            winrt::Microsoft::UI::Xaml::Controls::Button const& button);
        ~LiquidGlassSurface();
        void Visible(bool value);
        void Motion(bool enabled) { m_motion = enabled; Stop(); }
    private:
        void Initialize();
        void Resize();
        void RefreshPalette();
        void Feedback(float scale);
        void Stop();
        winrt::Microsoft::UI::Xaml::FrameworkElement m_host{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Button m_button{ nullptr };
        winrt::Microsoft::UI::Composition::ShapeVisual m_visual{ nullptr };
        winrt::Microsoft::UI::Composition::CompositionRoundedRectangleGeometry m_outer{ nullptr }, m_inner{ nullptr };
        winrt::Microsoft::UI::Composition::CompositionSpriteShape m_body{ nullptr }, m_rim{ nullptr }, m_reflection{ nullptr };
        winrt::Windows::UI::ViewManagement::UISettings m_settings;
        winrt::Windows::UI::ViewManagement::AccessibilitySettings m_accessibility;
        winrt::event_token m_size{}, m_theme{}, m_enter{}, m_leave{}, m_press{}, m_release{}, m_capture{}, m_effects{}, m_contrast{}, m_colors{}, m_animations{}, m_keyDown{}, m_keyUp{};
        int64_t m_enabledToken{};
        bool m_visible{ true }, m_motion{ true }, m_simple{}, m_initialized{};
    };
}
