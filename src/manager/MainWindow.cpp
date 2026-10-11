#include "pch.h"
#include "MainWindow.h"
#include "MainWindow.g.cpp"
#include "ThumbnailCache.h"
#include "application/LibraryService.h"
#include "platform/win32/HResult.h"
#include "platform/win32/Autostart.h"
#include <microsoft.ui.xaml.window.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <optional>
#include <cmath>
#include <cwctype>

namespace winrt::GlassWall::implementation
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    using namespace Windows::Foundation;
    namespace native = ::glasswall;
    namespace
    {
        struct WorkerApartment
        {
            WorkerApartment() { check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
            ~WorkerApartment() { CoUninitialize(); }
        };
        std::wstring ErrorMessage()
        {
            try { throw; }
            catch (native::platform::Failure const& error) { return error.operation + L" (" + native::platform::HexResult(error.code) + L")"; }
            catch (native::platform::Error const& error) { return native::platform::ErrorText(error.code()); }
            catch (hresult_error const& error) { return std::wstring(error.message()) + L" (" + native::platform::HexResult(error.code()) + L")"; }
            catch (std::exception const& error) { return std::wstring(to_hstring(error.what())); }
            catch (...) { return L"This operation failed. Refresh to inspect the current wallpaper, then try again."; }
        }
        void PlaybackCommand(native::application::LibraryService& service, native::ipc::Command command)
        {
            auto reply = native::ipc::Playback(command, {}, service.ProcessId());
            if (!reply) throw native::platform::Error("Playback command", reply.error);
            if (reply.response.result != native::ipc::PlaybackResult::Accepted)
                throw native::platform::Failure(FAILED(reply.response.error) ? reply.response.error : HRESULT_FROM_WIN32(ERROR_BUSY), L"Playback command rejected; refresh and retry");
            auto deadline = GetTickCount64() + 15000;
            for (;;)
            {
                auto state = service.State();
                if (state.completedRequestId == reply.response.requestId)
                { native::platform::Check(state.operationError, L"Playback control"); return; }
                if (state.completedRequestId > reply.response.requestId || GetTickCount64() >= deadline)
                    throw native::platform::Failure(HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED), L"Playback control completion is uncertain; refresh current state");
                Sleep(50); // bounded command completion only, never an idle timer
            }
        }
        std::wstring PlaybackText(native::ipc::PlaybackResponse const& state)
        {
            using native::ipc::PlaybackState;
            if (state.state == PlaybackState::Empty) return L"No active wallpaper";
            if (state.state == PlaybackState::Loading) return L"Preparing wallpaper";
            if (state.state == PlaybackState::Recovering) return L"Recovering wallpaper";
            if (state.state == PlaybackState::HostUnavailable) return L"Desktop host unavailable";
            if (state.state == PlaybackState::Error) return L"Wallpaper needs attention — check the tray or refresh";
            if (state.pauseReasons & static_cast<uint32_t>(native::core::PauseReason::User)) return L"Wallpaper manually paused";
            if (state.pauseReasons & static_cast<uint32_t>(native::core::PauseReason::DesktopInactive)) return L"Paused automatically while you’re away from the desktop";
            if (state.pauseReasons) return L"Paused automatically by the current desktop/session state";
            return state.state == PlaybackState::Playing ? L"Wallpaper playing · Automatic loop" : L"Wallpaper ready";
        }
    }

    MainWindow::MainWindow() : m_items(single_threaded_observable_vector<GlassWall::WallpaperItem>())
    {
        InitializeComponent();
        Wallpapers().ItemsSource(m_items);
        Navigation().SelectedItem(LibraryNavigation());
        if (Microsoft::UI::Windowing::AppWindowTitleBar::IsCustomizationSupported())
        {
            ExtendsContentIntoTitleBar(true);
            SetTitleBar(AppTitleBar());
            AppWindow().TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
        }
        else AppTitleBar().Visibility(Visibility::Collapsed);
        AppWindow().Resize({ 1180, 780 });
        auto iconPath = native::platform::ExecutableDirectory() + L"\\GlassWall.ico";
        AppWindow().SetIcon(iconPath.c_str());
        // The taskbar has an independent icon in the self-contained Windowing runtime.
        AppWindow().SetTaskbarIcon(iconPath.c_str());
        // The shell group can use relaunch metadata instead of WM_GETICON.
        // Keep the shortcut/process identity, but explicitly supply the packaged icon.
        try
        {
            HWND window{};
            check_hresult(m_inner.as<::IWindowNative>()->get_WindowHandle(&window));
            com_ptr<IPropertyStore> properties;
            check_hresult(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(properties.put())));
            auto setString = [&](PROPERTYKEY const& key, std::wstring const& text)
            {
                PROPVARIANT value{};
                check_hresult(InitPropVariantFromString(text.c_str(), &value));
                auto result = properties->SetValue(key, value);
                PropVariantClear(&value);
                check_hresult(result);
            };
            setString(PKEY_AppUserModel_RelaunchIconResource, iconPath + L",0");
            setString(PKEY_AppUserModel_ID, L"GlassWall.Personal");
        }
        catch (hresult_error const& error)
        {
            native::platform::Log(L"manager", L"Taskbar icon metadata unavailable: " +
                native::platform::HexResult(error.code()));
        }
        if (auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
        {
            presenter.PreferredMinimumWidth(800);
            presenter.PreferredMinimumHeight(640);
        }
        auto weak = get_weak();
        auto queue = DispatcherQueue();
        m_titleColors = m_uiSettings.ColorValuesChanged([weak, queue](auto const&, auto const&)
        {
            queue.TryEnqueue([weak] { if (auto self = weak.get(); self && !self->m_closed) self->UpdateTitleBar(); });
        });
        AppTitleBar().SizeChanged([weak](auto const&, auto const&)
        { if (auto self = weak.get(); self && !self->m_closed) self->UpdateTitleBar(); });
        Root().Loaded([weak](auto const&, auto const&)
        {
            if (auto self = weak.get(); self && !self->m_closed && !self->m_glass)
            {
                self->m_glass = native::manager::LiquidGlassSurface::Attach(self->GlassHost(), self->ApplyButton());
                self->UpdateTitleBar();
                self->RunCommandAsync(Action::Refresh);
            }
        });
        Activated([weak](auto const&, WindowActivatedEventArgs const& args)
        {
            if (auto self = weak.get(); self && !self->m_closed)
            {
                bool active = args.WindowActivationState() != WindowActivationState::Deactivated;
                if (self->m_glass) self->m_glass->Motion(active);
                self->TitleText().Opacity(active ? 1. : .6);
                if (active && self->m_glass && !self->m_busy) self->RunCommandAsync(Action::Refresh);
            }
        });
        AppWindow().Changed([weak](auto const& window, auto const&)
        {
            if (auto self = weak.get(); self && !self->m_closed)
            {
                auto presenter = window.Presenter().template try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
                bool visible = window.IsVisible() && (!presenter || presenter.State() != Microsoft::UI::Windowing::OverlappedPresenterState::Minimized);
                self->m_visible = visible;
                if (self->m_glass) self->m_glass->Visible(visible);
                if (visible)
                {
                    for (auto const& id : self->m_realized)
                        if (auto item = self->FindItem(id)) self->RequestThumbnail(item);
                    self->PumpThumbnails();
                }
            }
        });
        Closed([weak](auto const&, auto const&)
        {
            if (auto self = weak.get())
            {
                self->m_closed = true;
                self->m_pendingThumbnails.clear(); self->m_queued.clear();
                self->m_uiSettings.ColorValuesChanged(self->m_titleColors);
                self->m_glass.reset();
                self->ReleaseImages();
                self->Wallpapers().ItemsSource(nullptr);
                self->m_items.Clear();
                self->Content(nullptr);
                native::platform::Log(L"manager", L"Manager closed; no engine shutdown/stop command sent.");
                Application::Current().Exit();
            }
        });
        native::platform::Log(L"manager", L"Management window created.");
    }
    void MainWindow::StartupSettings_Opened(IInspectable const&, IInspectable const&)
    {
        m_loadingStartupSetting = true;
        try { StartupToggle().IsOn(native::platform::StartWithWindows()); }
        catch (...) { ShowError(ErrorMessage()); }
        m_loadingStartupSetting = false;
    }
    void MainWindow::StartWithWindows_Toggled(IInspectable const&, RoutedEventArgs const&)
    {
        if (m_loadingStartupSetting) return;
        try { native::platform::SetStartWithWindows(StartupToggle().IsOn()); }
        catch (...)
        {
            auto message = ErrorMessage();
            m_loadingStartupSetting = true;
            StartupToggle().IsOn(!StartupToggle().IsOn());
            m_loadingStartupSetting = false;
            ShowError(message);
        }
    }
    void MainWindow::UpdateTitleBar()
    {
        if (!ExtendsContentIntoTitleBar()) return;
        auto bar = AppWindow().TitleBar();
        auto scale = AppTitleBar().XamlRoot() ? AppTitleBar().XamlRoot().RasterizationScale() : 1.;
        TitleLeftInset().Width({ bar.LeftInset() / scale, GridUnitType::Pixel });
        TitleRightInset().Width({ bar.RightInset() / scale, GridUnitType::Pixel });
        HIGHCONTRASTW contrast{ sizeof(HIGHCONTRASTW) };
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        bool highContrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        auto background = highContrast ? m_uiSettings.GetColorValue(Windows::UI::ViewManagement::UIColorType::Background) : Windows::UI::Color{255, 16, 18, 23};
        auto foreground = highContrast ? m_uiSettings.GetColorValue(Windows::UI::ViewManagement::UIColorType::Foreground) : Windows::UI::Color{255, 240, 240, 237};
        auto hover = highContrast ? m_uiSettings.UIElementColor(Windows::UI::ViewManagement::UIElementType::Highlight) : Windows::UI::Color{255, 48, 51, 59};
        bar.BackgroundColor(background); bar.ForegroundColor(foreground);
        bar.InactiveBackgroundColor(background); bar.InactiveForegroundColor(foreground);
        bar.ButtonBackgroundColor(background); bar.ButtonForegroundColor(foreground);
        bar.ButtonInactiveBackgroundColor(background);
        bar.ButtonInactiveForegroundColor(highContrast ? foreground : Windows::UI::Color{255, 140, 144, 153});
        bar.ButtonHoverBackgroundColor(hover); bar.ButtonPressedBackgroundColor(hover);
        bar.ButtonHoverForegroundColor(highContrast ? m_uiSettings.UIElementColor(Windows::UI::ViewManagement::UIElementType::HighlightText) : foreground);
        bar.ButtonPressedForegroundColor(bar.ButtonHoverForegroundColor());
    }
    void MainWindow::SidebarToggle_Click(IInspectable const&, RoutedEventArgs const&)
    {
        bool expanded = Navigation().PaneDisplayMode() != NavigationViewPaneDisplayMode::Left;
        Navigation().PaneDisplayMode(expanded ? NavigationViewPaneDisplayMode::Left : NavigationViewPaneDisplayMode::LeftCompact);
        Navigation().IsPaneOpen(expanded);
        SidebarToggleLabel().Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
        auto name = expanded ? L"Collapse navigation" : L"Expand navigation";
        ToolTipService::SetToolTip(SidebarToggle(), box_value(name));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(SidebarToggle(), name);
    }
    void MainWindow::Navigation_SelectionChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const&) {}
    void MainWindow::Refresh_Click(IInspectable const&, RoutedEventArgs const&) { RunCommandAsync(Action::Refresh, L"retry-thumbnails"); }
    void MainWindow::Import_Click(IInspectable const&, RoutedEventArgs const&) { PickImportAsync(); }
    void MainWindow::Apply_Click(IInspectable const&, RoutedEventArgs const&) { if (!m_selectedId.empty()) RunCommandAsync(Action::Apply, m_selectedId); }
    void MainWindow::Pause_Click(IInspectable const&, RoutedEventArgs const&) { RunCommandAsync(m_userPaused ? Action::Resume : Action::Pause); }
    void MainWindow::WallpaperMenu_Loaded(IInspectable const& sender, RoutedEventArgs const&)
    {
        auto button = sender.as<Button>();
        auto id = std::wstring(unbox_value<hstring>(button.Tag()));
        if (auto item = FindItem(id)) Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(button,
            hstring{ L"Wallpaper actions for " + std::wstring(item.Name()) });
    }
    void MainWindow::WallpaperMenu_Click(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (m_closed || m_busy || !m_storageReady) return;
        auto button = sender.as<Button>();
        auto id = std::wstring(unbox_value<hstring>(button.Tag()));
        auto item = FindItem(id);
        if (!item) return;
        MenuFlyout menu;
        Style presenterStyle(xaml_typename<MenuFlyoutPresenter>());
        presenterStyle.Setters().Append(Setter(FrameworkElement::RequestedThemeProperty(), box_value(ElementTheme::Dark)));
        menu.MenuFlyoutPresenterStyle(presenterStyle);
        auto weak = get_weak();
        auto add = [&](wchar_t const* label, Action action)
        {
            MenuFlyoutItem command;
            command.Text(label);
            command.Click([weak, id, action](auto const&, auto const&)
            { if (auto self = weak.get()) self->ManageWallpaperAsync(action, id); });
            menu.Items().Append(command);
        };
        add(L"Edit Details", Action::Rename);
        add(L"Change Source File", Action::Relink);
        menu.Items().Append(MenuFlyoutSeparator{});
        add(L"Remove from Library", Action::Remove);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(button, hstring{ L"Wallpaper actions for " + std::wstring(item.Name()) });
        menu.ShowAt(button);
    }
    fire_and_forget MainWindow::ManageWallpaperAsync(Action action, std::wstring id)
    {
        auto lifetime = get_strong();
        if (m_closed || m_busy || !m_storageReady) co_return;
        auto item = FindItem(id);
        if (!item) co_return;
        SetBusy(true);
        std::wstring detail, error;
        bool accepted{};
        try
        {
            if (action == Action::Relink)
            {
                HWND hwnd{}; check_hresult(m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd));
                Windows::Storage::Pickers::FileOpenPicker picker;
                picker.FileTypeFilter().Append(L".mp4");
                check_hresult(picker.as<::IInitializeWithWindow>()->Initialize(hwnd));
                if (auto file = co_await picker.PickSingleFileAsync())
                { detail = file.Path(); accepted = true; }
            }
            else
            {
                ContentDialog dialog;
                dialog.XamlRoot(Root().XamlRoot());
                dialog.RequestedTheme(ElementTheme::Dark);
                dialog.CloseButtonText(L"Cancel");
                dialog.DefaultButton(ContentDialogButton::Close);
                TextBox name;
                if (action == Action::Rename)
                {
                    dialog.Title(box_value(L"Edit Details"));
                    name.Header(box_value(L"Library display name"));
                    name.Text(item.Name()); name.MaxLength(128);
                    name.SelectAll();
                    StackPanel content; content.Spacing(12);
                    TextBlock hint; hint.Text(L"The original video filename stays unchanged."); hint.TextWrapping(TextWrapping::Wrap);
                    content.Children().Append(name); content.Children().Append(hint);
                    dialog.Content(content); dialog.PrimaryButtonText(L"Save");
                    auto initialName = std::wstring(name.Text());
                    dialog.IsPrimaryButtonEnabled(!initialName.empty() && !std::all_of(initialName.begin(), initialName.end(), iswspace));
                    name.TextChanged([weakDialog = make_weak(dialog)](auto const& sender, auto const&)
                    {
                        auto text = std::wstring(sender.template as<TextBox>().Text());
                        if (auto current = weakDialog.get()) current.IsPrimaryButtonEnabled(!text.empty() && !std::all_of(text.begin(), text.end(), iswspace));
                    });
                    dialog.Opened([name](auto const&, auto const&) { name.Focus(FocusState::Programmatic); });
                }
                else
                {
                    dialog.Title(box_value(L"Remove from Library?"));
                    TextBlock content; content.TextWrapping(TextWrapping::Wrap);
                    content.Text(hstring{ L"Remove “" + std::wstring(item.Name()) + L"” from the library? The original video file will remain on disk.\n\n"
                        L"If this wallpaper is active, it will keep playing until you stop it from the tray or apply another wallpaper. Its library association and remembered selection will be removed." });
                    dialog.Content(content); dialog.PrimaryButtonText(L"Remove");
                }
                if (co_await dialog.ShowAsync() == ContentDialogResult::Primary)
                { accepted = true; if (action == Action::Rename) detail = name.Text(); }
            }
        }
        catch (...) { error = ErrorMessage(); }
        if (m_closed) co_return;
        SetBusy(false);
        if (!error.empty()) ShowError(error);
        else if (accepted) RunCommandAsync(action, std::move(id), std::move(detail));
    }
    void MainWindow::SetBusy(bool value)
    {
        m_busy = value;
        ImportButton().IsEnabled(!value); RefreshButton().IsEnabled(!value);
        auto selected = FindItem(m_selectedId);
        ApplyButton().IsEnabled(!value && m_storageReady && selected && get_self<WallpaperItem>(selected)->Available());
        PauseButton().IsEnabled(!value && m_hasWallpaper);
        if (!value) PumpThumbnails();
    }
    void MainWindow::ShowError(std::wstring const& message)
    {
        Notice().Title(L"GlassWall needs your attention");
        Notice().Message(hstring{ message });
        Notice().Severity(InfoBarSeverity::Error); Notice().IsOpen(true);
    }
    fire_and_forget MainWindow::PickImportAsync()
    {
        auto lifetime = get_strong();
        if (m_closed || m_busy) co_return;
        SetBusy(true);
        std::wstring path, error;
        try
        {
            HWND hwnd{}; check_hresult(m_inner.as<::IWindowNative>()->get_WindowHandle(&hwnd));
            Windows::Storage::Pickers::FileOpenPicker picker;
            picker.FileTypeFilter().Append(L".mp4");
            check_hresult(picker.as<::IInitializeWithWindow>()->Initialize(hwnd));
            if (auto file = co_await picker.PickSingleFileAsync()) path = file.Path();
        }
        catch (...) { error = ErrorMessage(); }
        if (m_closed) co_return;
        SetBusy(false);
        if (!error.empty()) ShowError(error);
        else if (!path.empty()) RunCommandAsync(Action::Import, std::move(path));
    }
    fire_and_forget MainWindow::RunCommandAsync(Action action, std::wstring value, std::wstring detail)
    {
        if (m_closed || m_busy) co_return;
        SetBusy(true);
        Notice().IsOpen(false);
        if (action != Action::Refresh) PlaybackStatus().Text(L"Updating your wallpaper…");
        auto weak = get_weak(); auto queue = DispatcherQueue();
        bool retryThumbnails = action == Action::Refresh && value == L"retry-thumbnails";
        std::wstring selection = m_selectedId;
        co_await resume_background();
        std::optional<native::library::Snapshot> snapshot;
        native::ipc::PlaybackResponse state{};
        std::vector<bool> available;
        std::wstring error;
        try
        {
            WorkerApartment apartment;
            native::application::LibraryService service;
            try
            {
                if (action == Action::Import) selection = service.Import(value).id;
                else if (action == Action::Apply) service.Apply(value);
                else if (action == Action::Pause) PlaybackCommand(service, native::ipc::Command::Pause);
                else if (action == Action::Resume) PlaybackCommand(service, native::ipc::Command::Resume);
                else if (action == Action::Rename) service.Rename(value, detail);
                else if (action == Action::Relink)
                {
                    service.Relink(value, detail);
                    try { native::manager::InvalidateThumbnail(value); }
                    catch (...) { error = L"The source was updated, but thumbnail cache cleanup failed: " + ErrorMessage(); }
                }
                else if (action == Action::Remove)
                {
                    service.Remove(value);
                    try { native::manager::InvalidateThumbnail(value); }
                    catch (...) { error = L"The wallpaper was removed, but thumbnail cache cleanup failed: " + ErrorMessage(); }
                }
            }
            catch (...) { error = ErrorMessage(); }
            snapshot = service.List();
            state = service.State();
            for (auto const& entry : snapshot->catalog.entries) available.push_back(native::library::Available(entry));
        }
        catch (...)
        {
            // A snapshot without a completed state/availability read is not a
            // complete UI update (for example, the engine exited during refresh).
            snapshot.reset(); available.clear();
            if (error.empty()) error = ErrorMessage();
        }
        try
        {
            queue.TryEnqueue([weak, retryThumbnails, rebuild = action == Action::Relink || action == Action::Remove, snapshot = std::move(snapshot), available = std::move(available), state, selection = std::move(selection), error = std::move(error)]
            {
                if (auto self = weak.get(); self && !self->m_closed)
                {
                    if (snapshot)
                    {
                        // Explicit Refresh retries failures; activation refreshes state without decoding again.
                        if (retryThumbnails) self->m_failedThumbnails.clear();
                        self->AcceptSnapshot(*snapshot, available, state, selection, rebuild);
                        for (auto const& id : self->m_realized)
                            if (auto item = self->FindItem(id)) self->RequestThumbnail(item);
                    }
                    else
                    {
                        self->m_hasWallpaper = false; self->m_storageReady = false;
                        self->m_activeId.clear();
                        for (auto const& item : self->m_items) get_self<WallpaperItem>(item)->Active(false);
                        self->ShowSelection();
                        self->PlaybackStatus().Text(L"Engine state unavailable — Refresh to reconnect");
                    }
                    self->SetBusy(false);
                    if (!error.empty()) self->ShowError(error);
                }
            });
        }
        catch (...) { /* Dispatcher/process may already have closed. */ }
    }
    GlassWall::WallpaperItem MainWindow::FindItem(std::wstring const& id)
    {
        for (auto const& item : m_items) if (item.Id() == id) return item;
        return nullptr;
    }
    void MainWindow::AcceptSnapshot(native::library::Snapshot const& snapshot, std::vector<bool> const& available,
        native::ipc::PlaybackResponse const& state, std::wstring const& selection, bool rebuild)
    {
        bool same = !rebuild && m_items.Size() == snapshot.catalog.entries.size();
        for (uint32_t i = 0; same && i < m_items.Size(); ++i)
        {
            auto model = get_self<WallpaperItem>(m_items.GetAt(i));
            auto const& entry = snapshot.catalog.entries[i];
            same = model->Entry().id == entry.id && model->Entry().displayName == entry.displayName &&
                model->Entry().path == entry.path && model->Available() == available[i];
        }
        if (!same)
        {
            ++m_libraryRevision;
            ReleaseImages();
            m_realized.clear(); m_pendingThumbnails.clear(); m_queued.clear(); m_failedThumbnails.clear();
            m_items.Clear();
            for (size_t i = 0; i < snapshot.catalog.entries.size(); ++i)
            {
                auto item = make<WallpaperItem>();
                get_self<WallpaperItem>(item)->Initialize(snapshot.catalog.entries[i], available[i]);
                get_self<WallpaperItem>(item)->CardWidth(m_cardWidth);
                m_items.Append(item);
            }
        }
        m_activeId = snapshot.activeId;
        m_storageReady = SUCCEEDED(snapshot.storageError);
        for (auto const& item : m_items) get_self<WallpaperItem>(item)->Active(!m_activeId.empty() && item.Id() == m_activeId);
        m_selectedId = selection;
        auto selected = FindItem(m_selectedId);
        if (!selected)
        {
            m_selectedId = snapshot.activeId.empty() ? snapshot.catalog.lastSelectedId : snapshot.activeId;
            selected = FindItem(m_selectedId);
            if (!selected && m_items.Size()) { selected = m_items.GetAt(0); m_selectedId = selected.Id(); }
            if (!selected) m_selectedId.clear();
        }
        Wallpapers().SelectedItem(selected);
        ShowSelection();
        EmptyLibrary().Visibility(m_items.Size() ? Visibility::Collapsed : Visibility::Visible);
        LibraryCount().Text(hstring{ std::to_wstring(m_items.Size()) + (m_items.Size() == 1 ? L" local wallpaper" : L" local wallpapers") });
        m_userPaused = (state.pauseReasons & static_cast<uint32_t>(native::core::PauseReason::User)) != 0;
        m_hasWallpaper = !snapshot.activePath.empty();
        PauseButton().Content(box_value(m_userPaused ? L"Resume" : L"Pause"));
        SYSTEMTIME now{}; GetLocalTime(&now);
        wchar_t time[16]{}; swprintf_s(time, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
        PlaybackStatus().Text(hstring{ PlaybackText(state) + L" · Checked " + time });
        if (!snapshot.warning.empty())
        {
            Notice().Title(m_storageReady ? L"Library notice" : L"Library storage unavailable");
            Notice().Message(hstring{ snapshot.warning });
            Notice().Severity(m_storageReady ? InfoBarSeverity::Warning : InfoBarSeverity::Error); Notice().IsOpen(true);
        }
    }
    void MainWindow::Wallpapers_SelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (m_closed) return;
        auto selected = Wallpapers().SelectedItem().try_as<GlassWall::WallpaperItem>();
        m_selectedId = selected ? std::wstring(selected.Id()) : std::wstring{};
        ShowSelection();
    }
    void MainWindow::ShowSelection()
    {
        for (auto const& candidate : m_items)
        {
            get_self<WallpaperItem>(candidate)->Selected(candidate.Id() == m_selectedId);
            if (auto container = Wallpapers().ContainerFromItem(candidate).try_as<GridViewItem>())
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(container,
                    hstring{ std::wstring(candidate.Name()) + L", " + std::wstring(candidate.Details()) + (candidate.Id() == m_activeId ? L", active wallpaper" : L"") });
        }
        auto item = FindItem(m_selectedId);
        PosterImage().ImageSource(item ? item.Thumbnail() : nullptr);
        SelectedName().Text(item ? item.Name() : L"Select a wallpaper");
        SelectedDetails().Text(item ? item.Details() : L"Still previews. Effortless looping.");
        SelectedActive().Text(item && item.Id() == m_activeId ? L"Active on your desktop" : item ?
            (get_self<WallpaperItem>(item)->Available() ? L"Selected · Ready to apply" : L"Selected · Source unavailable") : L"");
        SelectedPath().Text(item ? item.Path() : L"Original files stay in place.");
        PreviewDiagnostic().Text(item ? item.PreviewStatus() : L"");
        PosterStatus().Text(item ? item.PreviewStatus() : L"Select a wallpaper");
        PosterStatus().Visibility(item && item.Thumbnail() ? Visibility::Collapsed : Visibility::Visible);
        ApplyButton().IsEnabled(!m_busy && m_storageReady && item && get_self<WallpaperItem>(item)->Available());
        if (item) RequestThumbnail(item);
    }
    void MainWindow::LibraryLayout_SizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    {
        bool compact = args.NewSize().Width < 780;
        DetailColumn().Width({ compact ? 0. : 296., GridUnitType::Pixel });
        DetailRow().Height({ compact ? 1. : 0., compact ? GridUnitType::Auto : GridUnitType::Pixel });
        Grid::SetColumn(DetailsPane(), compact ? 0 : 1);
        Grid::SetRow(DetailsPane(), compact ? 1 : 0);
        PosterHost().Visibility(compact ? Visibility::Collapsed : Visibility::Visible);
        DetailScroll().MaxHeight(compact ? 180. : std::max(100., static_cast<double>(args.NewSize().Height) - 42.));
    }
    void MainWindow::Wallpapers_SizeChanged(IInspectable const&, SizeChangedEventArgs const& args)
    { UpdateCardWidth(args.NewSize().Width); }
    void MainWindow::UpdateCardWidth(double width)
    {
        if (width < 120) return;
        auto columns = std::max(1., std::floor((width - 12.) / 240.));
        m_cardWidth = std::min(360., std::floor((width - 12.) / columns) - 24.);
        for (auto const& item : m_items) get_self<WallpaperItem>(item)->CardWidth(m_cardWidth);
    }
    void MainWindow::Wallpapers_ContainerContentChanging(ListViewBase const&, ContainerContentChangingEventArgs const& args)
    {
        if (m_closed) return;
        auto item = args.Item().try_as<GlassWall::WallpaperItem>();
        if (!item) return;
        auto model = get_self<WallpaperItem>(item);
        model->Active(!m_activeId.empty() && item.Id() == m_activeId);
        model->Selected(item.Id() == m_selectedId);
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(args.ItemContainer(),
            hstring{ std::wstring(item.Name()) + L", " + std::wstring(item.Details()) + (item.Id() == m_activeId ? L", active wallpaper" : L"") });
        auto id = std::wstring(item.Id());
        if (args.InRecycleQueue())
        {
            m_realized.erase(id);
            if (id != m_selectedId) item.Thumbnail(nullptr);
        }
        else { m_realized.insert(id); RequestThumbnail(item); }
    }
    void MainWindow::RequestThumbnail(GlassWall::WallpaperItem const& item)
    {
        auto id = std::wstring(item.Id());
        if (!get_self<WallpaperItem>(item)->Available() || item.Thumbnail() || m_failedThumbnails.contains(id) || m_queued.contains(id) || m_pendingThumbnails.size() >= 32) return;
        m_queued.insert(id); m_pendingThumbnails.push_back(id);
        get_self<WallpaperItem>(item)->PreviewStatus(L"Preparing preview…");
        PumpThumbnails();
    }
    void MainWindow::PumpThumbnails()
    {
        if (m_closed || m_busy || m_thumbnailBusy || !m_visible) return;
        while (!m_pendingThumbnails.empty())
        {
            auto id = std::move(m_pendingThumbnails.front()); m_pendingThumbnails.pop_front();
            auto item = FindItem(id);
            if (!item || (id != m_selectedId && !m_realized.contains(id))) { m_queued.erase(id); continue; }
            m_thumbnailBusy = true;
            LoadThumbnailAsync(get_self<WallpaperItem>(item)->Entry(), m_libraryRevision);
            return;
        }
    }
    fire_and_forget MainWindow::LoadThumbnailAsync(native::library::Entry entry, uint64_t revision)
    {
        auto weak = get_weak(); auto queue = DispatcherQueue();
        co_await resume_background();
        Windows::Storage::Streams::IRandomAccessStream stream{ nullptr };
        std::wstring diagnostic;
        try { WorkerApartment apartment; auto result = native::manager::OpenThumbnail(entry); stream = result.stream; diagnostic = result.diagnostic; }
        catch (...) { diagnostic = ErrorMessage(); }
        native::platform::Log(L"manager", L"Thumbnail " + entry.id + L": " + diagnostic);
        try
        {
            queue.TryEnqueue([weak, entry = std::move(entry), revision, stream, diagnostic = std::move(diagnostic)]
            {
                if (auto self = weak.get(); self && !self->m_closed) self->CompleteThumbnailAsync(entry, revision, stream, diagnostic);
            });
        }
        catch (...) { /* No UI object or process lifetime is retained by this worker. */ }
    }
    fire_and_forget MainWindow::CompleteThumbnailAsync(native::library::Entry entry, uint64_t revision, Windows::Storage::Streams::IRandomAccessStream stream, std::wstring diagnostic)
    {
        auto lifetime = get_strong();
        auto id = entry.id;
        auto item = FindItem(id);
        if (revision != m_libraryRevision) item = nullptr;
        if (item && get_self<WallpaperItem>(item)->Entry().path != entry.path) item = nullptr;
        bool failed = !stream;
        if (stream && m_visible && item && (id == m_selectedId || m_realized.contains(id)))
        {
            try
            {
                Media::Imaging::BitmapImage image;
                image.DecodePixelType(Media::Imaging::DecodePixelType::Physical);
                // Cache generation bounds both dimensions, including portrait videos.
                co_await image.SetSourceAsync(stream);
                if (!m_closed && FindItem(id) == item && (id == m_selectedId || m_realized.contains(id)))
                {
                    item.Thumbnail(image);
                    std::erase(m_decodedImages, id); m_decodedImages.push_back(id);
                    while (m_decodedImages.size() > 24)
                    {
                        auto oldest = std::move(m_decodedImages.front()); m_decodedImages.pop_front();
                        if (oldest == m_selectedId) { m_decodedImages.push_back(oldest); continue; }
                        if (auto old = FindItem(oldest)) old.Thumbnail(nullptr);
                    }
                    get_self<WallpaperItem>(item)->PreviewStatus(diagnostic);
                    if (id == m_selectedId) ShowSelection();
                }
            }
            catch (...) { failed = true; diagnostic = L"Image loading: " + ErrorMessage(); native::platform::Log(L"manager", diagnostic); }
        }
        if (stream) { try { stream.Close(); } catch (...) { /* Best-effort release of a failed/closed stream. */ } }
        if (m_closed) co_return;
        if (failed && item)
        {
            m_failedThumbnails.insert(id);
            get_self<WallpaperItem>(item)->PreviewStatus(L"Preview unavailable · " + diagnostic + L" · Refresh to retry");
            if (id == m_selectedId) ShowSelection();
        }
        if (revision == m_libraryRevision) m_queued.erase(id);
        m_thumbnailBusy = false;
        PumpThumbnails();
    }
    void MainWindow::ReleaseImages()
    {
        PosterImage().ImageSource(nullptr);
        for (auto const& item : m_items) item.Thumbnail(nullptr);
        m_decodedImages.clear();
    }
}
