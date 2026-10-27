#define MyAppName "jxlshot"
#define MyAppVersion "2.3.9"
#define MyAppPublisher "azizr12"
#define MyAppURL "https://github.com/azizr12/jxlscrenshot"
#define MyAppExeName "jxlshot_tray.exe"

[Setup]
AppId={{8F5E4B3A-9C2D-4E1F-8A7B-6C5D4E3F2A1B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={localappdata}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=Output
OutputBaseFilename=jxlshot-setup-x64

; Compression Settings
; lzma2 is more modern than lzma. SolidCompression=no prevents 
; heuristic flags on large compressed blocks.
Compression=lzma2
SolidCompression=no

; Adding version info helps AV vendors identify the file as a known application
; rather than an anonymous binary.
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=jxlshot Screenshot Utility
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoVersion={#MyAppVersion}

WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; Make startup optional by default to reduce "persistence" flags
Name: "startup"; Description: "Launch on Windows Startup"; GroupDescription: "Additional options:"; Flags: unchecked

[Files]
Source: "dist\jxlshot_tray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\*.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName} Tray"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Only add to startup if the user checked the box
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; ValueData: "{app}\{#MyAppExeName}"; Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent