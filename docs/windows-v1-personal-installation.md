# Installed verification and deletion safety

Installation is per-user at `%LOCALAPPDATA%\Programs\GlassWall`. The independent engine owns playback, tray and library; the on-demand WinUI manager closes without sending Stop or Shutdown. Maintenance gracefully closes only processes from its installation directory. A development engine occupying the shared mutex must be exited through its own tray. No force-kill or resident launcher is used.

The installer contains both applications, a short-lived maintenance helper, optional diagnostics probe, embedded icons, title-bar PNG, WinUI resources and matching app-local Release CRTs. Reinstall/update preserves library IDs, names, selection, video paths and the startup preference. Uninstall removes installed program files, shortcuts, Installed Apps registration and the GlassWall Run value, while preserving user data and preferences.

Before removing a source checkout, verify:

1. Task Manager Details shows one engine running from the installed application folder.
2. Start Menu shortcut target and icon use the installed `GlassWall.exe`.
3. The `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value GlassWall targets the installed `GlassWall.Engine.exe` with `--startup`.
4. The original videos referenced by `%LOCALAPPDATA%\GlassWall\library.json` exist outside the checkout. Resolve any references inside it before deletion.
5. The library, cache and settings exist outside the checkout.
6. Reinstall using the final installer, then open the existing library from Start.
7. Confirm the GitHub source upload and Release download, including installer SHA-256. Keep a backup installer and any local verification records wanted for future reference.

Only the development checkout and its generated build/cache folders can then be removed. Keep the installed program folder, `%LOCALAPPDATA%\GlassWall`, the current user's preferences, and every original wallpaper video. Never delete a wallpaper merely because it is listed as unavailable; check its original location first.

## Manual Windows acceptance

Check the exact final installer and installed files, recording hashes, Windows build, GPU/driver and display settings. Source checks, clean Release compilation, dependency checks, installation/IPC observations and manual acceptance are separate evidence categories.

- Confirm icons visually in Start, Installed Apps, manager title bar/taskbar, tray and installer. Desktop shortcut is optional; verify it if selected.
- Open from Start repeatedly, close the manager normally and confirm wallpaper/tray continue with only one engine resident.
- Disable startup and sign out/in: no engine should start automatically. Enable it and sign out/in: the wallpaper should restore without manager, console or picker.
- Observe normal app/desktop transitions, automatic suspension/resume, manual pause and several loops.
- Compare settled playing and suspended CPU/RAM/GPU using the same video and measurement method. Prior user-reported baseline: 134–137 MB engine RAM, roughly 2% playing CPU and 0.1% suspended CPU. No matched measurement is implied by compilation or brief IPC snapshots.
- Qualify hardware decoding with Windows diagnostics. Device-creation flags are insufficient evidence.

The source uses private WorkerW/Progman Explorer topology through `src/desktop-host`. It validates shell ownership, parent/icon topology and display coverage and fails safely; these assumptions remain Windows-version-sensitive. No foreground manipulation or artificial test windows should be used to infer acceptance.
