#ifndef AppVersion
  #define AppVersion "0.9.5"
#endif
#ifndef StageDir
  #error StageDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif

[Setup]
AppId={{5828FD70-EC52-4DC1-9C4D-3F4FA2263D80}
AppName=Maracujá
AppVersion={#AppVersion}
AppPublisher=Maracujá
AppPublisherURL=https://github.com/BradleyMafra/Maracuj-
DefaultDirName={localappdata}\Programs\Maracuja
DefaultGroupName=Maracujá
UninstallDisplayIcon={app}\maracuja.exe
OutputDir={#OutputDir}
OutputBaseFilename=Maracuja-{#AppVersion}-Setup-x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
PrivilegesRequired=lowest
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=no
RestartApplications=no
#if FileExists(StageDir + "\assets\maracuja.ico")
SetupIconFile={#StageDir}\assets\maracuja.ico
#endif
#if FileExists(StageDir + "\LICENSE")
LicenseFile={#StageDir}\LICENSE
#endif

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "background"; Description: "Iniciar o Maracujá em segundo plano ao entrar no Windows"; Flags: unchecked
Name: "contextmenu"; Description: "Adicionar Converter com Maracujá ao menu dos arquivos"; Flags: checkedonce
Name: "desktopicon"; Description: "Criar atalho na área de trabalho"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: files; Name: "{app}\mandarina.exe"

[Icons]
Name: "{autoprograms}\Maracujá"; Filename: "{app}\maracuja.exe"
Name: "{autodesktop}\Maracujá"; Filename: "{app}\maracuja.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Maracuja"; ValueData: """{app}\maracuja.exe"" --background"; Tasks: background; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\*\shell\Maracuja.Convert"; ValueType: string; ValueName: ""; ValueData: "Converter com Maracujá"; Tasks: contextmenu; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\*\shell\Maracuja.Convert"; ValueType: string; ValueName: "Icon"; ValueData: "{app}\maracuja.exe"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\*\shell\Maracuja.Convert"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Single"; Tasks: contextmenu
Root: HKCU; Subkey: "Software\Classes\*\shell\Maracuja.Convert\command"; ValueType: string; ValueName: ""; ValueData: """{app}\maracuja.exe"" --wheel ""%1"""; Tasks: contextmenu

[Run]
Filename: "{app}\maracuja.exe"; Parameters: "--background"; Description: "Iniciar Maracujá em segundo plano"; Flags: nowait postinstall skipifsilent; Tasks: background
Filename: "{app}\maracuja.exe"; Description: "Abrir Maracujá"; Flags: nowait postinstall skipifsilent unchecked

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}\maracuja.exe')) then
    Exec(ExpandConstant('{app}\maracuja.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  // Preserve settings and user files while retiring the old executable name.
  if FileExists(ExpandConstant('{app}\mandarina.exe')) then
    Exec(ExpandConstant('{app}\mandarina.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
end;
