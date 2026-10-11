#include "WallpaperRuntime.h"
#include <algorithm>
#include <array>
#include <string_view>

namespace glasswall::engine
{
    using ipc::PlaybackState;
    using ipc::Command;
    namespace
    {
        bool DeviceError(HRESULT result)
        {
            return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
                result == DXGI_ERROR_DEVICE_HUNG;
        }
    }
    WallpaperRuntime::WallpaperRuntime(HINSTANCE instance, HANDLE stop) :
        m_instance(instance), m_stop(stop),
        m_commands(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        m_timer(CreateWaitableTimerW(nullptr, FALSE, nullptr))
    {
        if (!m_commands || !m_timer) throw platform::Error("Create runtime events", GetLastError());
        platform::Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"CoInitializeEx(COINIT_MULTITHREADED)");
        m_com = true;
        m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        m_status.processId = GetCurrentProcessId();
        m_publishedLibrary.processId = GetCurrentProcessId();
        Publish();
    }
    WallpaperRuntime::~WallpaperRuntime()
    {
        m_background.Reset();
        if (m_control && IsWindow(m_control)) SetWindowLongPtrW(m_control, GWLP_USERDATA, 0);
        CleanupPlayback(true);
        if (m_mf) MFShutdown();
        if (m_readerLibrary) FreeLibrary(m_readerLibrary);
        if (m_mfLibrary) FreeLibrary(m_mfLibrary);
        if (m_com) CoUninitialize();
    }
    ipc::PlaybackResponse WallpaperRuntime::Snapshot() const
    {
        std::scoped_lock lock(m_mutex);
        return m_published;
    }
    std::wstring WallpaperRuntime::LibrarySnapshot() const
    {
        library::Snapshot snapshot;
        { std::scoped_lock lock(m_mutex); snapshot = m_publishedLibrary; }
        return library::EncodeSnapshot(snapshot); // serialize/check availability only on demand
    }
    void WallpaperRuntime::RefreshLibrary()
    {
        std::scoped_lock lock(m_mutex);
        if (m_library)
        {
            m_publishedLibrary.catalog = m_library->Data();
            m_publishedLibrary.warning = m_library->Warning();
        }
        m_publishedLibrary.processId = GetCurrentProcessId();
    }
    ipc::PlaybackResponse WallpaperRuntime::Submit(Command command, std::wstring path)
    {
        std::scoped_lock lock(m_mutex);
        auto reply = m_published;
        if (command == Command::GetPlaybackState) return reply;
        if ((m_outstanding && command != Command::StopWallpaper) || m_jobs.size() >= 8 ||
            WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0)
        {
            reply.result = ipc::PlaybackResult::Busy;
            reply.error = HRESULT_FROM_WIN32(ERROR_BUSY);
            return reply;
        }
        auto id = m_nextId++;
        m_jobs.push_back({ command, std::move(path), id });
        m_outstanding = id;
        reply.requestId = id;
        SetEvent(m_commands.get());
        return reply;
    }
    void WallpaperRuntime::SubmitTray(Command command)
    {
        // Owner-thread tray controls remain responsive during candidate validation.
        // They have no controller request ID and cannot overwrite its completion.
        std::scoped_lock lock(m_mutex);
        if (m_jobs.size() < 8) { m_jobs.push_back({ command, {}, 0 }); SetEvent(m_commands.get()); }
    }
    void WallpaperRuntime::Publish()
    {
        m_status.flags = (m_candidate ? 1u : 0u) | (m_frame ? 2u : 0u);
        m_status.loopPhase = m_loop.phase;
        m_status.pauseReasons = m_pause.reasons;
        m_status.playRequested = m_wantPlay ? 1u : 0u;
        m_status.pendingReads = (m_current && m_current->ReadDeadline() ? 1u : 0u) |
            (m_candidate && m_candidate->ReadDeadline() ? 2u : 0u);
        m_background.UpdateTray(m_status.state, m_pause.Has(core::PauseReason::User),
            m_current != nullptr || !m_path.empty());
        std::scoped_lock lock(m_mutex);
        m_published = m_status;
        if (m_current)
        {
            if (m_publishedLibrary.activeId != m_activeId) m_publishedLibrary.activeId = m_activeId;
            if (m_publishedLibrary.activePath != m_path) m_publishedLibrary.activePath = m_path;
        }
        else { m_publishedLibrary.activeId.clear(); m_publishedLibrary.activePath.clear(); }
    }
    void WallpaperRuntime::Complete(std::uint64_t id, HRESULT result)
    {
        if (id)
        {
            m_status.completedRequestId = id;
            m_status.operationError = result;
            std::scoped_lock lock(m_mutex);
            if (m_outstanding == id) m_outstanding = 0;
        }
        Publish();
    }
    void WallpaperRuntime::PrepareGraphics()
    {
        if (!m_mf)
        {
            m_loadStage = L"MediaFoundation.Preflight";
            platform::Log(L"engine", L"stage=MediaFoundation.Preflight; loading system MF libraries.");
            if (!m_mfLibrary) m_mfLibrary = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!m_readerLibrary) m_readerLibrary = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!m_mfLibrary || !m_readerLibrary)
                throw platform::Failure(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND), L"Media Foundation missing; Windows N requires Media Feature Pack");
            m_loadStage = L"MediaFoundation.MFStartup";
            platform::Check(MFStartup(MF_VERSION, MFSTARTUP_FULL), L"MFStartup(MF_VERSION, MFSTARTUP_FULL)");
            m_mf = true;
            platform::Log(L"engine", L"stage=MediaFoundation.MFStartup; HRESULT=0x00000000");
        }
        if (!m_host.IsValid())
        {
            m_renderer.Reset(); m_host.Reset();
            m_loadStage = L"DesktopHost.Create";
            m_host.Create(m_instance, m_control);
            ++m_status.hostsCreated;
        }
        if (!m_renderer.Device())
        {
            m_loadStage = L"Renderer.Create";
            m_renderer.Create(m_host.Window());
            ++m_status.devicesCreated;
            m_status.hardware = ipc::HardwarePath::DxvaConfigured;
        }
    }
    void WallpaperRuntime::BeginLoad(std::wstring const& value, std::uint64_t id, LoadPurpose purpose, std::wstring selectedId)
    {
        bool recovery = purpose == LoadPurpose::Recovery;
        if (m_candidate) throw platform::Failure(HRESULT_FROM_WIN32(ERROR_BUSY), L"Another video is loading");
        m_loadStage = L"VideoReader.ValidateLocalPath";
        platform::Log(L"engine", L"stage=BeginLoad; request=" + std::to_wstring(id) +
            L" recovery=" + std::to_wstring(recovery) + L" path=" + value);
        auto path = media::ValidateLocalPath(value); // invalid input never stops the current session
        m_beforeImport = m_status.state;
        if (!m_current) { m_status.state = recovery ? PlaybackState::Recovering : PlaybackState::Loading; Publish(); }
        PrepareGraphics();
        auto candidate = std::make_unique<media::VideoReader>();
        m_loadStage = L"VideoReader.Open";
        candidate->Open(path, m_renderer.Device(), recovery ? m_savedPosition : 0);
        ++m_status.readersCreated;
        m_candidate = std::move(candidate);
        m_candidatePath = std::move(path);
        m_candidateRecovery = recovery;
        m_loadPurpose = purpose; m_candidateId = std::move(selectedId);
        m_loadId = id;
        m_loadDeadline = GetTickCount64() + 5000;
        if (!m_current) m_status.state = recovery ? PlaybackState::Recovering : PlaybackState::Loading;
        m_status.hardware = ipc::HardwarePath::GpuSamplesRequired;
        m_loadStage = L"VideoReader.WaitFirstSample";
        Publish();
    }
    void WallpaperRuntime::CleanupPlayback(bool clearPath)
    {
        CancelWaitableTimer(m_timer.get()); m_timerArmed = false;
        m_candidate.reset(); m_current.reset(); m_frame.reset();
        m_candidateId.clear();
        m_renderer.Reset(); m_host.Reset();
        m_shown = false; m_latencyReady = false; m_eof = false;
        m_loop.Reset(); m_loopDeadline = 0; m_lastFrameEnd = 0;
        m_status.hardware = ipc::HardwarePath::Uninitialized;
        m_loadDeadline = 0;
        m_presentDeadline = 0;
        if (clearPath)
        {
            m_path.clear(); m_candidatePath.clear(); m_savedPosition = 0;
            m_activeId.clear();
            m_recoveryAt = 0; m_recoveryAttempts = 0; m_wantPlay = false;
            m_status.duration100ns = 0;
            m_status.position100ns = 0;
            m_pause.Set(core::PauseReason::User, false);
            m_pause.Set(core::PauseReason::OutputBlocked, false);
            m_pause.Set(core::PauseReason::Recovery, false);
        }
    }
    void WallpaperRuntime::ProcessStartupRestore()
    {
        if (!m_startupRestorePending) return;
        if (!m_startupRestoreAt) return; // late Explorer recovery is event driven
        auto now = GetTickCount64();
        if (now >= m_startupRestoreUntil)
        {
            m_startupRestoreAt = 0;
            platform::Log(L"engine", L"Explorer startup wait expired; tray remains available. Open the library to retry restore.");
            return;
        }
        if (now < m_startupRestoreAt) return;
        m_startupRestoreAt = now + 1000; // bounded sign-in wait, never a steady-state timer
        if (!desktop::DesktopHost::IsShellReady()) return;
        std::scoped_lock lock(m_mutex);
        // User commands take precedence over a delayed automatic restore.
        if (m_outstanding || !m_jobs.empty())
        { m_startupRestorePending = false; m_startupRestoreAt = 0; return; }
        m_startupRestoreRequest = m_nextId++;
        m_jobs.push_back({Command::RestoreWallpaper, {}, m_startupRestoreRequest});
        m_outstanding = m_startupRestoreRequest;
        m_startupRestorePending = false; m_startupRestoreAt = 0;
        SetEvent(m_commands.get());
    }
    void WallpaperRuntime::ProcessJobs()
    {
        // Work is bounded so commands do not starve sample/message processing.
        for (UINT count = 0; count < 8; ++count)
        {
            Job job;
            {
                std::scoped_lock lock(m_mutex);
                if (m_jobs.empty()) { ResetEvent(m_commands.get()); break; }
                job = std::move(m_jobs.front()); m_jobs.pop_front();
            }
            m_loadStage = L"Command.Dispatch";
            if (!job.id || job.id != m_startupRestoreRequest)
            { m_startupRestorePending = false; m_startupRestoreAt = 0; }
            platform::Log(L"engine", L"stage=ProcessJobs; request=" + std::to_wstring(job.id) +
                L" command=" + std::to_wstring(static_cast<std::uint32_t>(job.command)));
            try
            {
                switch (job.command)
                {
                case Command::ImportWallpaper:
                {
                    if (!m_library) throw platform::Failure(m_publishedLibrary.storageError, L"Library storage is unavailable");
                    auto path = media::ValidateLocalPath(job.path);
                    if (library::FindPath(m_library->Data(), path)) break;
                    BeginLoad(path, job.id, LoadPurpose::Import);
                    continue;
                }
                case Command::ApplyWallpaper:
                case Command::RestoreWallpaper:
                {
                    if (!m_library) throw platform::Failure(m_publishedLibrary.storageError, L"Library storage is unavailable");
                    auto id = job.command == Command::RestoreWallpaper ? m_library->Data().lastSelectedId : job.path;
                    if (id.empty()) break; // first launch has no remembered selection
                    auto entry = library::Find(m_library->Data(), id);
                    if (job.command == Command::RestoreWallpaper && m_current && m_activeId == id)
                    { media::ValidateLocalPath(entry.path); break; }
                    BeginLoad(entry.path, job.id, LoadPurpose::Apply, id);
                    continue;
                }
                case Command::RelinkWallpaper:
                {
                    if (!m_library) throw platform::Failure(m_publishedLibrary.storageError, L"Library storage is unavailable");
                    auto [id, path] = library::DecodeRelink(job.path);
                    library::Find(m_library->Data(), id);
                    path = media::ValidateLocalPath(path);
                    if (auto duplicate = library::FindPath(m_library->Data(), path); duplicate && duplicate->id != id)
                        throw platform::Failure(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), L"This source already belongs to another wallpaper");
                    BeginLoad(path, job.id, LoadPurpose::Relink, id);
                    continue;
                }
                case Command::RenameWallpaper:
                {
                    if (!m_library) throw platform::Failure(m_publishedLibrary.storageError, L"Library storage is unavailable");
                    auto [id, name] = library::DecodeRename(job.path);
                    m_library->Rename(id, name); RefreshLibrary(); break;
                }
                case Command::RemoveWallpaper:
                    if (!m_library) throw platform::Failure(m_publishedLibrary.storageError, L"Library storage is unavailable");
                    m_library->Remove(job.path);
                    if (m_activeId == job.path) m_activeId.clear(); // playback remains; reference alone is removed
                    RefreshLibrary(); break;
                case Command::LoadWallpaper:
                    BeginLoad(job.path, job.id);
                    continue; // completion is the first validated GPU sample, not file-open alone
                case Command::Play:
                case Command::Resume:
                    if (!m_current)
                    {
                        if (!m_path.empty() && (m_status.state == PlaybackState::HostUnavailable || m_status.state == PlaybackState::Recovering))
                        {
                            m_wantPlay = true; m_recoveryAttempts = 0;
                            m_pause.Set(core::PauseReason::User, false);
                            m_pause.Set(core::PauseReason::OutputBlocked, false);
                            m_recoveryAt = GetTickCount64() + 1;
                            m_status.state = PlaybackState::Recovering;
                            break;
                        }
                        throw platform::Failure(MF_E_NOT_INITIALIZED, L"Load a video and wait for Ready before Play/Resume");
                    }
                    m_wantPlay = true;
                    m_pause.Set(core::PauseReason::User, false);
                    m_pause.Set(core::PauseReason::OutputBlocked, false);
                    ApplyPausePolicy();
                    break;
                case Command::Pause:
                    if (!m_current && m_path.empty()) throw platform::Failure(MF_E_NOT_INITIALIZED, L"No loaded wallpaper to pause");
                    m_pause.Set(core::PauseReason::User, true);
                    ApplyPausePolicy();
                    break;
                case Command::StopWallpaper:
                    if (m_loadId) Complete(m_loadId, HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
                    m_loadId = 0;
                    CleanupPlayback(true);
                    m_status.state = PlaybackState::Empty;
                    m_status.mediaError = S_OK;
                    break;
                default: throw platform::Failure(E_INVALIDARG, L"Unknown queued playback command");
                }
                Complete(job.id, S_OK);
            }
            catch (platform::Failure const& failure)
            {
                // Shell topology may still be settling even after Progman exists.
                // Retry only the automatic startup request and only host discovery.
                if (job.id == m_startupRestoreRequest &&
                    std::wstring_view(m_loadStage) == L"DesktopHost.Create")
                { m_startupRestorePending = true; m_startupRestoreAt = GetTickCount64() + 1000; }
                LogFailure(failure.code, failure.operation, m_loadStage, job.id);
                if (!m_current && !m_candidate && m_status.state == PlaybackState::Loading &&
                    ((job.command >= Command::ImportWallpaper && job.command <= Command::RestoreWallpaper) || job.command == Command::RelinkWallpaper))
                { CleanupPlayback(false); m_status.state = m_beforeImport; }
                if (!m_current && !m_candidate && job.command <= Command::GetPlaybackState)
                {
                    CleanupPlayback(false);
                    m_status.state = PlaybackState::Error; m_status.mediaError = failure.code;
                }
                Complete(job.id, failure.code);
            }
            catch (platform::Error const& error)
            {
                if (job.id == m_startupRestoreRequest &&
                    std::wstring_view(m_loadStage) == L"DesktopHost.Create")
                { m_startupRestorePending = true; m_startupRestoreAt = GetTickCount64() + 1000; }
                auto result = HRESULT_FROM_WIN32(error.code());
                auto message = std::string(error.what());
                LogFailure(result, std::wstring(message.begin(), message.end()), m_loadStage, job.id);
                if (!m_current && !m_candidate && m_status.state == PlaybackState::Loading &&
                    ((job.command >= Command::ImportWallpaper && job.command <= Command::RestoreWallpaper) || job.command == Command::RelinkWallpaper))
                { CleanupPlayback(false); m_status.state = m_beforeImport; }
                if (!m_current && !m_candidate && job.command <= Command::GetPlaybackState)
                {
                    CleanupPlayback(false);
                    m_status.state = PlaybackState::Error; m_status.mediaError = result;
                }
                Complete(job.id, result);
            }
            catch (std::exception const& error)
            {
                auto message = std::string(error.what());
                LogFailure(E_FAIL, std::wstring(message.begin(), message.end()), m_loadStage, job.id);
                if (!m_current && !m_candidate && m_status.state == PlaybackState::Loading &&
                    ((job.command >= Command::ImportWallpaper && job.command <= Command::RestoreWallpaper) || job.command == Command::RelinkWallpaper))
                { CleanupPlayback(false); m_status.state = m_beforeImport; }
                if (!m_current && job.command <= Command::GetPlaybackState)
                { CleanupPlayback(false); m_status.state = PlaybackState::Error; m_status.mediaError = E_FAIL; }
                Complete(job.id, E_FAIL);
            }
        }
    }
    void WallpaperRuntime::LogFailure(HRESULT error, std::wstring const& operation, wchar_t const* stage, std::uint64_t request) const
    {
        platform::Log(L"engine", L"PlaybackFailure stage=" + std::wstring(stage) + L" operation=" + operation +
            L" HRESULT=" + platform::HexResult(error) + L" request=" + std::to_wstring(request) +
            L" state=" + std::to_wstring(static_cast<std::uint32_t>(m_status.state)) +
            L" hardwarePath=" + std::to_wstring(static_cast<std::uint32_t>(m_status.hardware)) +
            L" currentReader=" + std::to_wstring(m_current != nullptr) + L" candidateReader=" + std::to_wstring(m_candidate != nullptr) +
            L" decodedFrames=" + std::to_wstring(m_status.decodedFrames) + L" presentedFrames=" + std::to_wstring(m_status.presentedFrames) +
            L" hostsCreated=" + std::to_wstring(m_status.hostsCreated) + L" devicesCreated=" + std::to_wstring(m_status.devicesCreated) +
            L" readersCreated=" + std::to_wstring(m_status.readersCreated) + L" deviceRemovedReason=" + platform::HexResult(m_renderer.RemovedReason()));
    }
    void WallpaperRuntime::FailCurrent(HRESULT error, std::wstring const& description)
    {
        LogFailure(error, description, L"Playback.DecodeOrPresent");
        if (DeviceError(error) || FAILED(m_renderer.RemovedReason()))
        {
            BeginRecovery(true, FAILED(m_renderer.RemovedReason()) ? m_renderer.RemovedReason() : error);
            return;
        }
        CleanupPlayback(false);
        m_wantPlay = false;
        m_status.state = PlaybackState::Error; m_status.mediaError = error;
        Publish();
    }
    void WallpaperRuntime::BeginRecovery(bool device, HRESULT reason)
    {
        if (m_path.empty())
        {
            if (m_loadId) Complete(m_loadId, HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
            m_loadId = 0;
            CleanupPlayback(false); m_status.state = PlaybackState::Error;
            m_status.mediaError = reason; Publish(); return;
        }
        if (device) { ++m_status.deviceRecoveries; m_status.deviceRemovedReason = reason; }
        else ++m_status.hostRecoveries;
        platform::Log(L"engine", std::wstring(device ? L"Device" : L"Desktop host") +
            L" lost; suspend and recreate owned resources. " + platform::HexResult(reason));
        if (m_loadId) Complete(m_loadId, HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
        m_loadId = 0;
        CleanupPlayback(false);
        m_pause.Set(core::PauseReason::Recovery, true);
        m_status.state = PlaybackState::Recovering;
        m_status.mediaError = reason;
        m_recoveryAttempts = 0; m_recoveryAt = GetTickCount64() + 500;
        Publish();
    }
    void WallpaperRuntime::ProcessFrames()
    {
        ApplyPausePolicy();
        auto recoveryAllowed = (m_pause.reasons & ~static_cast<std::uint32_t>(core::PauseReason::Recovery)) == 0;
        if (m_recoveryAt && !m_candidate && recoveryAllowed && GetTickCount64() >= m_recoveryAt)
        {
            m_recoveryAt = 0;
            try { BeginLoad(m_path, 0, LoadPurpose::Recovery); }
            catch (platform::Failure const& error)
            {
                LogFailure(error.code, error.operation, m_loadStage);
                ++m_recoveryAttempts;
                if (m_recoveryAttempts < 3) m_recoveryAt = GetTickCount64() + 1500;
                else { m_status.state = PlaybackState::HostUnavailable; m_status.mediaError = error.code; }
                m_renderer.Reset(); m_host.Reset();
            }
            catch (...) { m_status.state = PlaybackState::Error; m_status.mediaError = E_FAIL; }
        }
        if (m_candidate)
        {
            auto incoming = m_candidate->Take();
            HRESULT failure = S_OK;
            if (incoming)
            {
                try
                {
                    platform::Check(incoming->status, L"VideoReader::Take: first decoded sample/scan-format validation");
                    if (!incoming->sample || (incoming->flags & MF_SOURCE_READERF_ERROR))
                        throw platform::Failure(MF_E_INVALIDMEDIATYPE, L"No initial playable GPU frame");
                    m_renderer.ValidateSample(incoming->sample.Get());
                    bool relinkingActive = m_loadPurpose == LoadPurpose::Relink && m_activeId == m_candidateId;
                    if (m_loadPurpose == LoadPurpose::Relink)
                    {
                        // Commit only after the existing supported-media / GPU-sample checks.
                        // A save failure discards the candidate and retains the prior reader.
                        m_library->Relink(m_candidateId, m_candidatePath, m_candidate->Metadata());
                        RefreshLibrary();
                    }
                    if (m_loadPurpose == LoadPurpose::Import || (m_loadPurpose == LoadPurpose::Relink && !relinkingActive))
                    {
                        if (m_loadPurpose == LoadPurpose::Import)
                        { m_library->Import(m_candidatePath, m_candidate->Metadata()); RefreshLibrary(); }
                        m_candidate.reset(); m_candidatePath.clear(); m_loadDeadline = 0;
                        if (!m_current) { CleanupPlayback(false); m_status.state = m_beforeImport; }
                        else m_status.hardware = ipc::HardwarePath::GpuSampleObserved;
                        Complete(m_loadId, S_OK); m_loadId = 0;
                        return; // validation sample is never displayed or retained by the library
                    }
                    if (m_loadPurpose == LoadPurpose::Apply)
                    {
                        // The GPU sample is valid. Commit the remembered choice before
                        // promotion; failed persistence leaves the prior reader active.
                        m_library->Select(m_candidateId); RefreshLibrary();
                    }
                    m_renderer.SetFullRange(m_candidate->FullRange());
                    if (m_current) m_current.reset();
                    if (m_loadPurpose == LoadPurpose::Apply) m_activeId = std::move(m_candidateId);
                    else if (!m_candidateRecovery && !relinkingActive) m_activeId.clear();
                    m_frame = std::move(incoming);
                    m_current = std::move(m_candidate);
                    m_path = std::move(m_candidatePath);
                    m_savedPosition = m_frame->timestamp;
                    m_status.position100ns = m_savedPosition;
                    m_clockReset = true; m_eof = false; m_emptyReads = 0;
                    m_loop.Reset(); m_loopDeadline = 0; m_lastFrameEnd = 0;
                    if (!m_candidateRecovery) m_initialTimestamp = m_frame->timestamp;
                    m_status.duration100ns = m_current->Duration100ns();
                    if (m_frame->flags & MF_SOURCE_READERF_ENDOFSTREAM) RecordEof(*m_frame);
                    m_status.state = PlaybackState::Ready;
                    if (!m_candidateRecovery && !relinkingActive) m_wantPlay = m_loadPurpose == LoadPurpose::Apply;
                    if (!m_candidateRecovery) { m_recoveryAt = 0; m_recoveryAttempts = 0; }
                    m_pause.Set(core::PauseReason::Recovery, false);
                    ApplyPausePolicy();
                    m_status.hardware = ipc::HardwarePath::GpuSampleObserved;
                    m_status.mediaError = S_OK;
                    ++m_status.decodedFrames;
                    Complete(m_loadId, S_OK); m_loadId = 0; m_loadDeadline = 0;
                    platform::Log(L"engine", L"Wallpaper loaded: first GPU NV12 sample accepted; actual hardware decode is not inferred from this alone.");
                }
                catch (platform::Failure const& error)
                {
                    failure = error.code;
                    LogFailure(error.code, error.operation, L"Load.ValidateFirstGpuSample", m_loadId);
                }
                catch (platform::Error const& error)
                { failure = HRESULT_FROM_WIN32(error.code()); LogFailure(failure, L"Library persistence", L"Load.LibraryCommit", m_loadId); }
                catch (...)
                { failure = E_FAIL; LogFailure(failure, L"Library commit", L"Load.LibraryCommit", m_loadId); }
            }
            else if (GetTickCount64() >= m_loadDeadline)
            {
                failure = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                LogFailure(failure, L"First decoded GPU sample deadline exceeded", L"Load.WaitFirstSample", m_loadId);
            }
            if (FAILED(failure))
            {
                bool recovering = m_candidateRecovery;
                m_candidate.reset(); m_candidateId.clear(); m_candidatePath.clear(); m_loadDeadline = 0;
                if (m_current) m_status.hardware = ipc::HardwarePath::GpuSampleObserved;
                Complete(m_loadId, failure); m_loadId = 0;
                if (!m_current)
                {
                    m_renderer.Reset(); m_host.Reset();
                    m_status.state = recovering ? PlaybackState::HostUnavailable :
                        (m_loadPurpose == LoadPurpose::Raw ? PlaybackState::Error : m_beforeImport);
                    m_status.mediaError = failure;
                    if (recovering && ++m_recoveryAttempts < 3)
                    {
                        m_status.state = PlaybackState::Recovering;
                        m_recoveryAt = GetTickCount64() + 1500;
                    }
                }
            }
        }
        if (!m_current) { Publish(); return; }
        if (!m_host.IsValid())
        {
            BeginRecovery(false, HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)); return;
        }
        if (m_current->Seeking())
        {
            auto result = m_current->CompleteSeekToStart();
            if (!result) { Publish(); return; }
            m_status.lastSeekError = *result;
            m_loop.SeekFinished(SUCCEEDED(*result));
            if (FAILED(*result)) { FailCurrent(*result, L"Loop flush/seek did not complete"); return; }
            ++m_status.loops;
            m_eof = false; m_clockReset = true; m_emptyReads = 0; m_lastFrameEnd = 0;
            m_loopDeadline = 0;
            platform::Log(L"engine", L"stage=Loop.SeekSucceeded; seekAttempts=" + std::to_wstring(m_status.seekAttempts) +
                L" successfulSeeks=" + std::to_wstring(m_status.loops) + L"; awaiting decoded and presented rewind frame.");
        }
        if (m_status.state == PlaybackState::Playing && m_loop.phase != core::LoopPhase::Idle)
        {
            if (!m_loopDeadline) m_loopDeadline = GetTickCount64() + 5000;
            if (GetTickCount64() >= m_loopDeadline)
            { FailCurrent(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Loop seek succeeded but no rewound frame was presented within five seconds"); return; }
        }
        if (m_current->ReadDeadline() && GetTickCount64() >= m_current->ReadDeadline())
        { FailCurrent(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Decoder did not complete the outstanding sample in five seconds"); return; }
        if (m_presentDeadline && GetTickCount64() >= m_presentDeadline)
        {
            m_pause.Set(core::PauseReason::OutputBlocked, true);
            ApplyPausePolicy();
            platform::Log(L"engine", L"DXGI latency event did not signal in five seconds; paused without a render loop. Resume explicitly.");
        }
        // Always drain notifications, including errors that arrive while a
        // retained frame is paused. Otherwise a signaled mailbox could spin.
        {
            auto incoming = m_current->Take();
            if (incoming)
            {
                if (FAILED(incoming->status) || (incoming->flags & MF_SOURCE_READERF_ERROR))
                { FailCurrent(FAILED(incoming->status) ? incoming->status : E_FAIL, L"VideoReader::Take: decoding or sample scan-format validation failed"); return; }
                if (incoming->flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED))
                { FailCurrent(MF_E_INVALIDMEDIATYPE, L"Mid-stream format changes are not supported"); return; }
                if (incoming->flags & MF_SOURCE_READERF_ENDOFSTREAM) RecordEof(*incoming);
                if (incoming->sample)
                {
                    if (m_frame)
                    { FailCurrent(E_UNEXPECTED, L"Decoded frame exceeded the bounded mailbox"); return; }
                    auto phase = m_loop.phase;
                    if (!m_loop.Decoded(incoming->timestamp, m_initialTimestamp, m_current->FramePeriod100ns()))
                    { FailCurrent(MF_E_INVALIDREQUEST, L"Post-seek sample did not rewind to the initial video timestamp"); return; }
                    if (phase == core::LoopPhase::AwaitingDecode)
                    {
                        m_status.loopDecodedTimestamp100ns = incoming->timestamp;
                        platform::Log(L"engine", L"stage=Loop.Decoded; timestamp100ns=" + std::to_wstring(incoming->timestamp) +
                            L" initialTimestamp100ns=" + std::to_wstring(m_initialTimestamp) + L"; presentation still required.");
                    }
                    m_frame = std::move(incoming);
                    ++m_status.decodedFrames; m_emptyReads = 0;
                }
                else if (m_eof && m_loop.phase != core::LoopPhase::Idle)
                { FailCurrent(MF_E_INVALIDMEDIATYPE, L"EOF returned after loop seek without a decoded frame; not retrying an empty loop"); return; }
                else if (!m_eof && ++m_emptyReads > 32)
                { FailCurrent(MF_E_INVALIDMEDIATYPE, L"Too many stream ticks without video"); return; }
            }
        }
        if (m_status.state == PlaybackState::Playing)
        {
            if (!m_frame)
            {
                try
                {
                    if (m_eof)
                    {
                        if (platform::Clock100ns() >= m_lastFrameEnd) StartLoop();
                    }
                    else m_current->Request(); // one request; no reads during asynchronous flush
                }
                catch (platform::Failure const& error)
                {
                    if (m_loop.phase == core::LoopPhase::Flushing) m_status.lastSeekError = error.code;
                    FailCurrent(error.code, error.operation);
                }
            }
            else
            {
                if (m_clockReset)
                {
                    m_clockBase = platform::Clock100ns() - m_frame->timestamp;
                    m_clockReset = false;
                }
                auto remaining = m_clockBase + m_frame->timestamp - platform::Clock100ns();
                if (remaining <= 0 && !m_latencyReady)
                    m_latencyReady = WaitForSingleObject(m_renderer.LatencyEvent(), 0) == WAIT_OBJECT_0;
                if (remaining <= 0 && m_latencyReady)
                {
                    try
                    {
                        m_renderer.ValidateSample(m_frame->sample.Get());
                        if (!m_shown) { m_host.Show(); m_shown = true; }
                        ApplyPausePolicy();
                        if (m_status.state != PlaybackState::Playing) { Publish(); return; }
                        auto presented = m_renderer.Present(m_frame->sample.Get());
                        m_latencyReady = false;
                        m_presentDeadline = 0;
                        if (presented == DXGI_STATUS_OCCLUDED)
                        {
                            m_pause.Set(core::PauseReason::OutputBlocked, true);
                            ApplyPausePolicy();
                            platform::Log(L"engine", L"Desktop output occluded: presentation stopped; explicit Resume clears OutputBlocked.");
                        }
                        else platform::Check(presented, L"IDXGISwapChain2::Present(1, 0)");
                        if (presented == S_OK)
                        {
                            ++m_status.presentedFrames;
                            m_savedPosition = m_frame->timestamp;
                            m_status.position100ns = m_savedPosition;
                            m_lastFrameEnd = m_clockBase + m_frame->timestamp + m_current->FramePeriod100ns();
                            if (m_loop.Presented())
                            {
                                ++m_status.loopCompletions;
                                m_status.loopPresentedTimestamp100ns = m_frame->timestamp;
                                m_loopDeadline = 0;
                                platform::Log(L"engine", L"stage=Loop.Completed; completions=" + std::to_wstring(m_status.loopCompletions) +
                                    L" decodedTimestamp100ns=" + std::to_wstring(m_status.loopDecodedTimestamp100ns) +
                                    L" presentedTimestamp100ns=" + std::to_wstring(m_frame->timestamp) +
                                    L" decodedFrames=" + std::to_wstring(m_status.decodedFrames) +
                                    L" presentedFrames=" + std::to_wstring(m_status.presentedFrames) +
                                    L" hostsCreated=" + std::to_wstring(m_status.hostsCreated) + L" devicesCreated=" +
                                    std::to_wstring(m_status.devicesCreated) + L" readersCreated=" + std::to_wstring(m_status.readersCreated));
                            }
                        }
                        m_frame.reset();
                        ApplyPausePolicy();
                        if (m_status.state == PlaybackState::Playing)
                        {
                            if (!m_eof) m_current->Request();
                        }
                    }
                    catch (platform::Failure const& error) { FailCurrent(error.code, error.operation); }
                }
            }
        }
        Publish();
    }
    void WallpaperRuntime::RecordEof(media::VideoFrame const& frame)
    {
        m_eof = true;
        ++m_status.eofEvents;
        m_status.lastEofTimestamp100ns = frame.timestamp;
        platform::Log(L"engine", L"stage=Loop.EOF; eofEvents=" + std::to_wstring(m_status.eofEvents) +
            L" callbackTimestamp100ns=" + std::to_wstring(frame.timestamp) + L" lastPresentedTimestamp100ns=" +
            std::to_wstring(m_savedPosition) + L" flags=" + std::to_wstring(frame.flags) +
            L" sample=" + std::to_wstring(frame.sample != nullptr) + L" state=" +
            std::to_wstring(static_cast<std::uint32_t>(m_status.state)));
    }
    void WallpaperRuntime::StartLoop()
    {
        if (!m_loop.Begin()) throw platform::Failure(MF_E_INVALIDREQUEST, L"Loop restart already in progress");
        ++m_status.seekAttempts;
        m_status.lastSeekError = E_PENDING;
        platform::Log(L"engine", L"stage=Loop.Begin; attempt=" + std::to_wstring(m_status.seekAttempts) +
            L" lastPresentedTimestamp100ns=" + std::to_wstring(m_savedPosition));
        m_current->SeekToStart();
    }
    DWORD WallpaperRuntime::WaitTimeout() const
    {
        auto now = GetTickCount64();
        ULONGLONG deadline{};
        if (m_loadDeadline) deadline = m_loadDeadline;
        if (m_startupRestorePending && m_startupRestoreAt &&
            (!deadline || m_startupRestoreAt < deadline)) deadline = m_startupRestoreAt;
        auto recoveryAllowed = (m_pause.reasons & ~static_cast<std::uint32_t>(core::PauseReason::Recovery)) == 0;
        if (m_recoveryAt && !m_candidate && recoveryAllowed && (!deadline || m_recoveryAt < deadline)) deadline = m_recoveryAt;
        auto readerDeadline = m_current ? m_current->ReadDeadline() : 0;
        if (readerDeadline && (!deadline || readerDeadline < deadline)) deadline = readerDeadline;
        if (m_presentDeadline && (!deadline || m_presentDeadline < deadline)) deadline = m_presentDeadline;
        auto seekDeadline = m_current ? m_current->SeekDeadline() : 0;
        if (seekDeadline && (!deadline || seekDeadline < deadline)) deadline = seekDeadline;
        if (m_status.state == PlaybackState::Playing && m_loopDeadline && (!deadline || m_loopDeadline < deadline)) deadline = m_loopDeadline;
        if (m_status.state == PlaybackState::Playing && m_eof && !m_frame && m_loop.phase == core::LoopPhase::Idle)
        {
            auto remaining = m_lastFrameEnd - platform::Clock100ns();
            auto boundary = now + (remaining > 0 ? static_cast<ULONGLONG>((remaining + 9999) / 10000) : 0);
            if (!deadline || boundary < deadline) deadline = boundary;
        }
        if (deadline) return deadline <= now ? 0 : static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 5000));
        return INFINITE;
    }
    void WallpaperRuntime::ProcessMessages(bool& quit, int& code)
    {
        MSG message{};
        UINT count{};
        while (count++ < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) { quit = true; code = static_cast<int>(message.wParam); return; }
            if (message.hwnd == m_control &&
                ((message.message == desktop::HostChangedMessage &&
                    reinterpret_cast<HWND>(message.wParam) == m_host.Window() &&
                    !m_host.GeometryMatches(message.lParam)) ||
                 (message.message == desktop::HostLostMessage &&
                    reinterpret_cast<HWND>(message.wParam) == m_host.Window())))
            {
                if (m_current || m_candidate || (!m_path.empty() && m_status.state == PlaybackState::HostUnavailable))
                    BeginRecovery(false, HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
            }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    bool WallpaperRuntime::HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        // Broadcasts can be sent synchronously, bypassing PeekMessage. Defer
        // resource destruction until the owner returns to its normal loop.
        if ((m_taskbarCreated && message == m_taskbarCreated) || message == WM_DISPLAYCHANGE)
            m_hostInvalidated = true;
        if (m_taskbarCreated && message == m_taskbarCreated && m_startupRestorePending)
        {
            m_startupRestoreAt = GetTickCount64();
            m_startupRestoreUntil = m_startupRestoreAt + 60000;
        }
        return m_background.HandleMessage(message, wParam, lParam);
    }
    void WallpaperRuntime::ApplyPausePolicy()
    {
        auto sleep = m_background.TakeSleepDuration();
        if (sleep)
        {
            if (m_status.state == PlaybackState::Playing)
            {
                if (!m_clockReset) m_clockBase += static_cast<LONGLONG>(sleep) * 10000;
                if (m_lastFrameEnd) m_lastFrameEnd += static_cast<LONGLONG>(sleep) * 10000;
                if (m_presentDeadline) m_presentDeadline += sleep;
                if (m_loopDeadline) m_loopDeadline += sleep;
            }
            if (m_loadDeadline) m_loadDeadline += sleep;
            if (m_recoveryAt) m_recoveryAt += sleep;
            if (m_current) m_current->ExtendDeadlines(sleep);
            if (m_candidate) m_candidate->ExtendDeadlines(sleep);
        }
        // Environmental reasons never clear the independent user/recovery bits.
        constexpr auto environmental = static_cast<std::uint32_t>(core::PauseReason::DesktopInactive) |
            static_cast<std::uint32_t>(core::PauseReason::SessionLocked) |
            static_cast<std::uint32_t>(core::PauseReason::DisplayOff) |
            static_cast<std::uint32_t>(core::PauseReason::SystemSuspended) |
            static_cast<std::uint32_t>(core::PauseReason::SessionDisconnected);
        auto before = m_pause.reasons;
        m_pause.reasons = (m_pause.reasons & ~environmental) | m_background.Reasons();
        if ((before & static_cast<std::uint32_t>(core::PauseReason::DesktopInactive)) &&
            !m_pause.Has(core::PauseReason::DesktopInactive))
            m_pause.Set(core::PauseReason::OutputBlocked, false); // one retry on desktop return, never a polling loop
        if (before != m_pause.reasons)
            platform::Log(L"engine", L"Pause reasons=" + std::to_wstring(m_pause.reasons));
        if (!m_current) return;
        if (!m_wantPlay && !m_pause.Has(core::PauseReason::User)) return;
        auto playing = m_pause.CanPlay(m_wantPlay);
        if (!playing && m_status.state != PlaybackState::Paused)
        {
            if (m_status.state == PlaybackState::Playing) ++m_status.pauseTransitions;
            m_pausedAt = platform::Clock100ns();
            m_status.state = PlaybackState::Paused;
            m_presentDeadline = m_loopDeadline = 0;
            CancelWaitableTimer(m_timer.get()); m_timerArmed = false;
            platform::Log(L"engine", L"Playback suspended; reasons=" + std::to_wstring(m_pause.reasons));
        }
        else if (playing && m_status.state != PlaybackState::Playing)
        {
            if (m_status.state == PlaybackState::Paused && !m_clockReset)
            {
                auto paused = platform::Clock100ns() - m_pausedAt;
                m_clockBase += paused;
                if (m_lastFrameEnd) m_lastFrameEnd += paused;
            }
            else m_clockReset = true;
            m_status.state = PlaybackState::Playing;
            ++m_status.resumeTransitions;
            platform::Log(L"engine", L"Playback resumed; pause reasons cleared.");
        }
    }
    int WallpaperRuntime::Run(HWND control)
    {
        m_control = control;
        SetWindowLongPtrW(control, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_background.Start(control);
        ApplyPausePolicy();
        try
        {
            m_library = std::make_unique<library::Store>();
            RefreshLibrary();
            if (!m_library->Data().lastSelectedId.empty())
            {
                m_startupRestorePending = true;
                m_startupRestoreAt = GetTickCount64();
                m_startupRestoreUntil = m_startupRestoreAt + 60000;
            }
        }
        catch (platform::Failure const& failure)
        {
            std::scoped_lock lock(m_mutex);
            m_publishedLibrary.storageError = failure.code;
            m_publishedLibrary.warning = failure.operation;
        }
        catch (platform::Error const& error)
        {
            std::scoped_lock lock(m_mutex);
            m_publishedLibrary.storageError = HRESULT_FROM_WIN32(error.code());
            m_publishedLibrary.warning = platform::ErrorText(error.code());
        }
        catch (...)
        {
            std::scoped_lock lock(m_mutex);
            m_publishedLibrary.storageError = E_FAIL;
            m_publishedLibrary.warning = L"Library storage could not be opened; raw playback and tray remain available.";
        }
        RefreshLibrary();
        { std::scoped_lock lock(m_mutex); m_publishedLibrary.ready = true; }
        bool quit{}; int code{};
        while (!quit && WaitForSingleObject(m_stop, 0) == WAIT_TIMEOUT)
        {
            ProcessMessages(quit, code);
            if (quit) break;
            if (m_hostInvalidated)
            {
                m_hostInvalidated = false;
                if (m_current || m_candidate || (!m_path.empty() && m_status.state == PlaybackState::HostUnavailable))
                    BeginRecovery(false, HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
            }
            ApplyPausePolicy();
            ProcessStartupRestore();
            ProcessJobs();
            ApplyPausePolicy();
            ProcessFrames();
            std::array<HANDLE, 6> events{};
            DWORD count{};
            events[count++] = m_stop; events[count++] = m_commands.get();
            if (m_current) events[count++] = m_current->Event();
            if (m_candidate) events[count++] = m_candidate->Event();
            if (m_status.state == PlaybackState::Playing && m_frame)
            {
                auto remaining = m_clockBase + m_frame->timestamp - platform::Clock100ns();
                if (remaining > 0)
                {
                    LARGE_INTEGER due{}; due.QuadPart = -remaining;
                    if (!SetWaitableTimer(m_timer.get(), &due, 0, nullptr, nullptr, FALSE))
                        throw platform::Error("Set frame deadline timer", GetLastError());
                    m_timerArmed = true; events[count++] = m_timer.get();
                }
                else
                {
                    CancelWaitableTimer(m_timer.get()); m_timerArmed = false;
                    if (!m_presentDeadline) m_presentDeadline = GetTickCount64() + 5000;
                    events[count++] = m_renderer.LatencyEvent();
                }
            }
            else if (m_timerArmed) { CancelWaitableTimer(m_timer.get()); m_timerArmed = false; }
            auto result = MsgWaitForMultipleObjectsEx(count, events.data(), WaitTimeout(),
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_FAILED) throw platform::Error("Runtime event wait", GetLastError());
            // Wait consumes an auto-reset latency event; remember it for the next Step.
            if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count &&
                events[result - WAIT_OBJECT_0] == m_renderer.LatencyEvent())
                m_latencyReady = true;
        }
        CleanupPlayback(true);
        m_status.state = PlaybackState::Empty;
        Publish();
        m_background.Reset();
        SetWindowLongPtrW(control, GWLP_USERDATA, 0);
        return code;
    }
}
