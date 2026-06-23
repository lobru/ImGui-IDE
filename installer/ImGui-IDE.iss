; ImGui-IDE — Inno Setup installer with Windows Explorer integration.
; Build with:  iscc installer\ImGui-IDE.iss   (or installer\build-installer.ps1)
; Packages the release exe (renamed to ImGui-IDE.exe) + the languages/ folder,
; adds Start-menu/desktop shortcuts, and registers "Open with ImGui-IDE" on files,
; folders, and folder backgrounds. main() already opens a passed path (file -> doc,
; folder -> project), so the context-menu verbs work out of the box.

#define MyAppName "ImGui-IDE"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "Logan Brunet"
#define MyAppExeName "ImGui-IDE.exe"
; Source paths are relative to this .iss (installer/). Override BuildDir on the
; ISCC command line with /DBuildDir=... if your build tree differs.
#ifndef BuildDir
  #define BuildDir "..\example\out\build\x64-Release"
#endif

[Setup]
AppId={{8F2A6C40-1B7E-4D3A-9C21-5E0A7B9F3D11}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\example\app.ico
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir=output
OutputBaseFilename=ImGui-IDE-Setup-{#MyAppVersion}
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"
Name: "filecontext";  Description: "Add ""Open with ImGui-IDE"" to the file right-click menu";   GroupDescription: "Explorer integration:"
Name: "dircontext";   Description: "Add ""Open with ImGui-IDE"" to the folder right-click menu"; GroupDescription: "Explorer integration:"

[Files]
Source: "{#BuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
; Source the runtime languages from the canonical example/languages/ folder so a
; clean checkout always packages them (independent of the build's POST_BUILD copy).
Source: "..\example\languages\*"; DestDir: "{app}\languages"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\example\app.ico";      DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}";            Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app.ico"
Name: "{group}\Uninstall {#MyAppName}";  Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}";      Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app.ico"; Tasks: desktopicon

[Registry]
; "Open with ImGui-IDE" on ANY file  (%1 = the clicked file)
Root: HKA; Subkey: "Software\Classes\*\shell\OpenWithImGuiIDE";          ValueType: string; ValueName: ""; ValueData: "Open with {#MyAppName}"; Tasks: filecontext; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\*\shell\OpenWithImGuiIDE";          ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#MyAppExeName},0"; Tasks: filecontext
Root: HKA; Subkey: "Software\Classes\*\shell\OpenWithImGuiIDE\command";  ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: filecontext
; "Open with ImGui-IDE" on a FOLDER  (%1 = the clicked folder -> project root)
Root: HKA; Subkey: "Software\Classes\Directory\shell\OpenWithImGuiIDE";         ValueType: string; ValueName: ""; ValueData: "Open with {#MyAppName}"; Tasks: dircontext; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Directory\shell\OpenWithImGuiIDE";         ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#MyAppExeName},0"; Tasks: dircontext
Root: HKA; Subkey: "Software\Classes\Directory\shell\OpenWithImGuiIDE\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: dircontext
; "Open with ImGui-IDE" on a folder BACKGROUND  (%V = the current folder)
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\OpenWithImGuiIDE";         ValueType: string; ValueName: ""; ValueData: "Open with {#MyAppName}"; Tasks: dircontext; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\OpenWithImGuiIDE";         ValueType: string; ValueName: "Icon"; ValueData: "{app}\{#MyAppExeName},0"; Tasks: dircontext
Root: HKA; Subkey: "Software\Classes\Directory\Background\shell\OpenWithImGuiIDE\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%V"""; Tasks: dircontext

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
