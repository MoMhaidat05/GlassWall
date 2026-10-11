#ifndef PayloadDir
  #error PayloadDir is required; use tools/windows/package.ps1
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
[Setup]
AppId={{2C7A4F1C-B0C8-40A7-9C6A-3D93C5A290E1}
AppName=GlassWall
AppVersion={#AppVersion}
AppPublisher=GlassWall (Personal use)
DefaultDirName={localappdata}\Programs\GlassWall
DefaultGroupName=GlassWall
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
OutputDir={#OutputDir}
OutputBaseFilename=GlassWall-Setup-{#AppVersion}-x64
SetupIconFile=..\assets\GlassWall.ico
UninstallDisplayIcon={app}\GlassWall.ico
UninstallDisplayName=GlassWall
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
SetupMutex=Local\GlassWall.Personal.Setup
Uninstallable=yes
CreateUninstallRegKey=yes
VersionInfoDescription=GlassWall Personal Use Installer
VersionInfoVersion={#AppVersion}
UsePreviousAppDir=yes

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
; Payload is curated from a clean Release build, including every runtime resource.
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadDir}\GlassWall.Setup.exe"; Flags: dontcopy

[Icons]
Name: "{userprograms}\GlassWall"; Filename: "{app}\GlassWall.exe"; WorkingDir: "{app}"; IconFilename: "{app}\GlassWall.ico"; AppUserModelID: "GlassWall.Personal"
Name: "{userdesktop}\GlassWall"; Filename: "{app}\GlassWall.exe"; WorkingDir: "{app}"; IconFilename: "{app}\GlassWall.ico"; Tasks: desktopicon; AppUserModelID: "GlassWall.Personal"

[Registry]
; Preserve the preference key. Remove only this application's Run value on uninstall.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "GlassWall"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\GlassWall.Engine.exe"; Parameters: "--startup"; WorkingDir: "{app}"; Flags: nowait runhidden
Filename: "{app}\GlassWall.exe"; WorkingDir: "{app}"; Description: "Open GlassWall"; Flags: nowait postinstall skipifsilent unchecked

[Code]
function Maintenance(const Arguments: String): Boolean;
var ExitCode: Integer; Helper: String;
begin
  Helper := ExpandConstant('{app}\GlassWall.Setup.exe');
  if not FileExists(Helper) then begin Result := True; exit; end;
  Result := Exec(Helper, Arguments, ExpandConstant('{app}'), SW_HIDE,
    ewWaitUntilTerminated, ExitCode);
  if Result then Result := ExitCode = 0;
  if not Result then Log('GlassWall maintenance failed: ' + Arguments + ', exit=' + IntToStr(ExitCode));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var ExitCode: Integer; Success: Boolean;
begin
  Result := '';
  ExtractTemporaryFile('GlassWall.Setup.exe');
  Success := Exec(ExpandConstant('{tmp}\GlassWall.Setup.exe'),
    '--shutdown-directory "' + ExpandConstant('{app}') + '"', '', SW_HIDE,
    ewWaitUntilTerminated, ExitCode);
  if Success then Success := ExitCode = 0;
  if not Success then
    Result := 'Close GlassWall and exit its engine using the tray, then retry. An engine from a different folder must also be exited. No user data has been removed.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    if not Maintenance('--configure-startup') then
      RaiseException('GlassWall files are installed, but startup registration failed. See %LOCALAPPDATA%\GlassWall\Logs\setup.log.');
end;

function InitializeUninstall: Boolean;
begin
  Result := Maintenance('--shutdown');
  if not Result then
    MsgBox('Close GlassWall and exit its engine using the tray, then retry uninstall. Your library and original wallpapers are preserved.', mbError, MB_OK);
end;

// No UninstallDelete for the library, preferences, thumbnails, logs, or videos.
