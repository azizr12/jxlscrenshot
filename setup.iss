#define MyAppName "jxlshot"
#define MyAppVersion "2.1.3"
#define MyAppPublisher "azizr12"
#define MyAppURL "https://github.com/azizr12/jxlshot"
#define MyAppExeName "jxlshot_tray.exe"

[Setup]
; NOTE: The value of AppId uniquely identifies this application. 
; Do not use the same AppId value in installers for other applications.
AppId={{8F5E4B3A-9C2D-4E1F-8A7B-6C5D4E3F2A1B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=Output
OutputBaseFilename=jxlshot-setup-x64
Compression=lzma
SolidCompression=yes
WizardStyle=modern
; Changed to 'admin' to ensure installation into Program Files is permitted
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; Removed 'Flags: unchecked' so the startup option is enabled by default
Name: "startup"; Description: "Launch on Windows Startup"; GroupDescription: "Additional options:"

[Files]
Source: "dist\jxlshot.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\jxlshot_tray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "dist\README.md"; DestDir: "{app}"; Flags: ignoreversion
; Note: ADD-to-START-UP.bat is omitted here as the [Registry] section handles this natively and more reliably.

[Icons]
Name: "{group}\{#MyAppName} Tray"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{#MyAppName} CLI"; Filename: "{app}\jxlshot.exe"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Natively adds the application to the current user's startup registry
; This replicates the exact behavior of your batch file without requiring external script execution
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; ValueData: "{app}\{#MyAppExeName}"; Flags: uninsdeletevalue; Tasks: startup

[Run]
; Launch the application immediately after installation completes
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
