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

[CustomMessages]
brazilianportuguese.AppStillRunning=O Maracujá ainda está usando os arquivos da instalação. Aguarde o término da operação, escolha Sair do Maracujá na bandeja e tente novamente.
english.AppStillRunning=Maracujá is still using the installation files. Wait for the operation to finish, choose Exit Maracujá in the system tray, and try again.

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
function OpenExistingForReplacement(FileName: String; DesiredAccess, ShareMode: DWORD;
  SecurityAttributes: NativeUInt; CreationDisposition, FlagsAndAttributes: DWORD;
  TemplateFile: THandle): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseReplacementProbe(FileHandle: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

function ExecutableIsReleased(const FileName: String): Boolean;
var
  FileHandle: THandle;
begin
  Result := True;
  if not FileExists(FileName) then Exit;
  // A write-capable, exclusive handle cannot be opened while Windows still
  // maps the executable. This probe changes no bytes and handles both names.
  FileHandle := OpenExistingForReplacement(FileName, $40000000, 0, 0, 3, $80, 0);
  Result := FileHandle <> THandle(-1);
  if Result then CloseReplacementProbe(FileHandle);
end;

procedure RequestCooperativeExit(const FileName: String);
var
  ExitCode: Integer;
begin
  if FileExists(FileName) and not ExecutableIsReleased(FileName) then
    // --quit acknowledges the request before the resident instance finishes
    // cancelling. Do not confuse the forwarding process exiting with shutdown.
    Exec(FileName, '--quit', '', SW_HIDE, ewNoWait, ExitCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  CurrentExecutable, LegacyExecutable: String;
  Attempt: Integer;
begin
  CurrentExecutable := ExpandConstant('{app}\maracuja.exe');
  LegacyExecutable := ExpandConstant('{app}\mandarina.exe');
  RequestCooperativeExit(CurrentExecutable);
  RequestCooperativeExit(LegacyExecutable);
  // Limit the cooperative wait to 30 seconds. A retry remains possible after
  // the user finishes a job; never kill unrelated processes or force a reboot.
  for Attempt := 1 to 300 do begin
    if ExecutableIsReleased(CurrentExecutable) and ExecutableIsReleased(LegacyExecutable) then begin
      Result := '';
      Exit;
    end;
    Sleep(100);
  end;
  Result := CustomMessage('AppStillRunning');
end;
