# GlassWall

**GlassWall** is a lightweight animated wallpaper application for Windows 11, built with native C++ and WinUI 3.

It plays local video wallpapers behind desktop icons while keeping the wallpaper engine independent of the management interface. The application prioritizes efficient resource usage, automatic playback suspension, and straightforward wallpaper management.

## Features

- **Native wallpaper engine:** Media Foundation and Direct3D 11 rendering, with hardware-accelerated decoding where supported.
- **Intelligent playback suspension:** Automatically pauses video decoding and rendering when leaving the desktop, then resumes when appropriate.
- **Independent background operation:** The wallpaper engine and system tray remain active after closing the management window.
- **Wallpaper library:** Import, select, rename, relink, and remove wallpapers without modifying or deleting original video files.
- **Thumbnail previews:** Cached video thumbnails with bounded extraction and memory usage.
- **Automatic startup:** Optionally restore the last selected wallpaper when signing in to Windows.
- **Native Windows interface:** WinUI 3 management application with a dark interface and lightweight visual effects.
- **System tray controls:** Manage wallpaper playback without opening the main application.

## Compatibility and Limitations

GlassWall currently supports:

| Component | Support |
|---|---|
| Operating system | Windows 11 x64 |
| Displays | Single display |
| Video container | Local, seekable MP4 |
| Video codec | H.264/AVC |
| Resolution | 1920 × 1080 |
| Frame rate | 29.97 or 30 FPS |
| Color format | SDR BT.709 |
| Rendering | Direct3D 11 / Media Foundation |

Other resolutions, frame rates, codecs, HDR video, and multi-monitor configurations are not currently supported.

Hardware decoding availability depends on the video, GPU, driver, and Windows media components. Desktop integration relies on Windows Explorer behavior and may be affected by Windows updates.

## Installation

Download the latest installer from [GitHub Releases](https://github.com/MoMhaidat05/GlassWall/releases).

1. Download `GlassWall-Setup-1.0.0-x64.exe`.
2. Optionally verify its SHA-256 checksum against the accompanying `.sha256` file.
3. Run the installer.
4. Launch **GlassWall** from the Windows Start menu.

The installer is unsigned, so Windows SmartScreen may display an unrecognized-publisher warning.

GlassWall uses a per-user installation and does not require administrator privileges.

**Installation directory:**

`%LOCALAPPDATA%\Programs\GlassWall`

### Automatic Startup

Automatic startup is enabled on first installation.

Use the **Start GlassWall with Windows** switch in the application's sidebar settings to enable or disable it.

At sign-in, only the wallpaper engine starts. It restores the previously selected wallpaper without opening the management interface.

### Application Data

Wallpaper library information, cached thumbnails, and application data are stored under:

`%LOCALAPPDATA%\GlassWall`

Original video files remain at their existing locations. Moving or deleting a video may require relinking it through the wallpaper library.

Application updates and uninstall preserve the library and user data.

## Building from Source

### Requirements

- Windows 11 x64
- Visual Studio 2026 (18.x) with C++ desktop development and WinUI development components
- Required MSVC toolsets, including v145 for the WinUI manager
- Windows SDK 10.0.26100.0
- NuGet CLI (`nuget.exe`)
- Inno Setup 6 for installer creation

Exact build-tool and dependency versions are recorded in `windows/toolchain-versions.json` and the project's package manifests.

### Build

Clone the repository and open PowerShell in its root directory.

```powershell
.\tools\windows\build.ps1 -Configuration Release
```

To build only the native components:

```powershell
.\tools\windows\build.ps1 -Configuration Release -Scope Native
```

Build outputs are generated under:

`artifacts\bin\x64\Release`

The WinUI management application requires its complete output directory, including resource files and runtime dependencies.

The Visual Studio solution is located at:

`windows\GlassWall.sln`

### Native Tests

The available native tests validate protocol handling, video interlace policy, playback loop state, suspension logic, and library operations.

```powershell
.\artifacts\bin\x64\Release\GlassWall.Probe.exe --protocol-tests
.\artifacts\bin\x64\Release\GlassWall.Probe.exe --interlace-tests
.\artifacts\bin\x64\Release\GlassWall.Probe.exe --loop-tests
.\artifacts\bin\x64\Release\GlassWall.Probe.exe --pause-tests
.\artifacts\bin\x64\Release\GlassWall.Probe.exe --library-tests "$PWD\artifacts\tests\library"
```

These tests do not replace validation of actual wallpaper playback on Windows.

## Building the Installer

Inno Setup 6 is required.

Run:

```powershell
.\tools\windows\package.ps1
```

For custom tool locations:

```powershell
.\tools\windows\package.ps1 -NuGetPath 'D:\Tools\nuget.exe' -ISCCPath 'D:\Tools\InnoSetup\ISCC.exe'
```

The packaging script restores dependencies, builds the required components, validates runtime dependencies, and generates the installer.

Output:

`artifacts\installer\GlassWall-Setup-1.0.0-x64.exe`

An accompanying SHA-256 checksum file is also generated.

Additional installation and verification details are available in [Windows installation documentation](docs/windows-v1-personal-installation.md).

## Architecture

GlassWall separates wallpaper playback from its management interface:

- **GlassWall.Engine.exe:** Native Win32 wallpaper hosting, Media Foundation video processing, Direct3D 11 rendering, automatic suspension, and system tray functionality.
- **GlassWall.exe:** WinUI 3 interface for wallpaper library management, selection, and application settings.
- **Native IPC:** Communication between the management interface and the independent wallpaper engine.
- **Persistent library:** Local metadata storage with atomic updates and cached thumbnails.

The management interface runs on demand. Closing it does not terminate wallpaper playback.

## License

No open-source license has currently been assigned to this repository. Existing source and third-party licensing notices remain applicable.