; Script Inno Setup para KaxWav UM2 Pro Studio Suite
; Gera o executavel unico: KaxWav_UM2_Setup.exe

#define MyAppName "KaxWav UM2 ASIO Pro"
#define MyAppVersion "2.0.0"
#define MyAppPublisher "Kaxcav Audio Engineering"
#define MyAppURL "https://github.com/Kaxcav/driver-kaxwav"
#define MyAppExeName "BehringerUM2Panel.exe"
#define DriverCLSID "{77DBBCCD-91C6-4856-A900-FC8D007D161C}"

[Setup]
AppId={{E68A1B52-3F1C-4B7E-97C5-6A93F40E7D2B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\KaxWav\Behringer UM2 ASIO
DisableDirPage=yes
DefaultGroupName=KaxWav
DisableProgramGroupPage=yes
OutputDir=..\dist_release
OutputBaseFilename=KaxWav_UM2_Setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Files]
; Binarios do driver e bridge
Source: "BehringerUM2ASIODriver.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace regserver 64bit
Source: "BehringerUM2Bridge.dll";     DestDir: "{app}"; Flags: ignoreversion restartreplace 64bit
Source: "test_com_reg.exe";           DestDir: "{app}"; Flags: ignoreversion 64bit
Source: "BehringerUM2Panel.exe";      DestDir: "{app}"; Flags: ignoreversion 64bit

[Icons]
Name: "{autoprograms}\KaxWav UM2 Control Panel"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\KaxWav UM2 Control Panel";  Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Registry]
; HKLM ASIO Host Registration (para FL Studio, Ableton, Reaper, Cubase)
Root: HKLM64; Subkey: "SOFTWARE\ASIO\Behringer UM2 ASIO"; ValueType: string; ValueName: "CLSID"; ValueData: "{#DriverCLSID}"; Flags: uninsdeletekey
Root: HKLM64; Subkey: "SOFTWARE\ASIO\Behringer UM2 ASIO"; ValueType: string; ValueName: "Description"; ValueData: "Behringer U-Phoria Low Latency ASIO Driver"; Flags: uninsdeletekey

; HKCU ASIO Host Registration (compatibilidade sem elevacao)
Root: HKCU; Subkey: "Software\ASIO\Behringer UM2 ASIO"; ValueType: string; ValueName: "CLSID"; ValueData: "{#DriverCLSID}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\ASIO\Behringer UM2 ASIO"; ValueType: string; ValueName: "Description"; ValueData: "Behringer U-Phoria Low Latency ASIO Driver"; Flags: uninsdeletekey

; Legado HKLM / HKCU
Root: HKLM64; Subkey: "SOFTWARE\ASIO\Behringer U-Phoria Low Latency ASIO Driver"; ValueType: string; ValueName: "CLSID"; ValueData: "{#DriverCLSID}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\ASIO\Behringer U-Phoria Low Latency ASIO Driver"; ValueType: string; ValueName: "CLSID"; ValueData: "{#DriverCLSID}"; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Abrir Painel de Controle KaxWav UM2"; Flags: nowait postinstall skipifsilent
