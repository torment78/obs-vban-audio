; VBAN Audio: install the already-tested release payload into an OBS root.
#ifndef AppVersion
  #error AppVersion must be supplied by tools/package-installer.ps1
#endif
#ifndef PayloadDir
  #error PayloadDir must be supplied by tools/package-installer.ps1
#endif
#ifndef OutputPath
  #error OutputPath must be supplied by tools/package-installer.ps1
#endif

[Setup]
AppId={{CBB7B1A9-2B1D-4AA5-8494-82CBA4EA194C}
AppName=VBAN Audio
AppVersion={#AppVersion}
AppPublisher=VBAN Audio contributors
AppPublisherURL=https://github.com/torment78/obs-vban-audio
AppSupportURL=https://github.com/torment78/obs-vban-audio/issues
AppUpdatesURL=https://github.com/torment78/obs-vban-audio/releases
DefaultDirName={autopf}\obs-studio
UsePreviousAppDir=no
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
LicenseFile=..\LICENSE
InfoAfterFile=installer-finish.txt
OutputDir={#OutputPath}
OutputBaseFilename=obs-vban-audio-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
SetupIconFile=vban-audio.ico
WizardStyle=modern
WizardSizePercent=110
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
; Allows isolated, non-elevated packaging tests using the same public EXE.
PrivilegesRequiredOverridesAllowed=commandline
UsePreviousPrivileges=no
SetupLogging=yes
CloseApplications=no
RestartApplications=no
Uninstallable=yes
UninstallFilesDir={app}\data\obs-plugins\obs-vban-audio
CreateUninstallRegKey=IsStandard
UninstallDisplayName=VBAN Audio {#AppVersion}
UninstallDisplayIcon={uninstallexe}
; A reinstall after moving portable OBS must record only the current root.
UninstallLogMode=overwrite
DirExistsWarning=no
AllowRootDirectory=no
AllowNoIcons=yes
VersionInfoDescription=VBAN Audio setup for installed or portable OBS
VersionInfoVersion={#AppVersion}.0

[Files]
Source: "{#PayloadDir}\obs-plugins\64bit\obs-vban-audio.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#PayloadDir}\data\obs-plugins\obs-vban-audio\locale\en-US.ini"; DestDir: "{app}\data\obs-plugins\obs-vban-audio\locale"; Flags: ignoreversion
Source: "{#PayloadDir}\data\obs-plugins\obs-vban-audio\vban-audio.png"; DestDir: "{app}\data\obs-plugins\obs-vban-audio"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}\data\obs-plugins\obs-vban-audio"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\docs\INSTALL-OBS-ROOT.txt"; DestDir: "{app}\data\obs-plugins\obs-vban-audio"; DestName: "VBAN-INSTALL.txt"; Flags: ignoreversion

[Code]
var
  ModePage: TWizardPage;
  StandardRadio, PortableRadio: TNewRadioButton;
  FolderEdit: TNewEdit;
  BrowseButton: TNewButton;
  FolderLabel, FolderHelp, StandardHelp, PortableHelp: TNewStaticText;
  StandardRoot, PortableRoot: String;
  ShowingPortable: Boolean;

function GetBinaryType(FileName: String; var BinaryType: Cardinal): Boolean;
  external 'GetBinaryTypeW@kernel32.dll stdcall';

function IsStandard: Boolean;
begin
  Result := True;
  if Assigned(StandardRadio) then
    Result := StandardRadio.Checked;
end;

function DetectOBSRoot: String;
var
  Candidate: String;
begin
  Result := ExpandConstant('{autopf}\obs-studio');
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Candidate) or
     RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', Candidate) then
    if FileExists(AddBackslash(Candidate) + 'bin\64bit\obs64.exe') then
      Result := Candidate;
end;

function CleanRoot(Value: String): String;
begin
  Result := RemoveBackslashUnlessRoot(Trim(Value));
end;

function ValidateRoot(Root: String): String;
var
  BinaryType: Cardinal;
begin
  Result := '';
  if (Root = '') or not DirExists(Root) then begin
    Result := 'Choose an existing OBS Studio folder.';
    Exit;
  end;
  if not FileExists(AddBackslash(Root) + 'bin\64bit\obs64.exe') or
     not FileExists(AddBackslash(Root) + 'bin\64bit\obs.dll') or
     not DirExists(AddBackslash(Root) + 'data') or
     not DirExists(AddBackslash(Root) + 'obs-plugins') then begin
    Result := 'This is not an OBS Studio root folder.' + #13#10#13#10 +
      'Select the folder containing bin, data, and obs-plugins. ' +
      'Do not select bin, 64bit, or obs-plugins themselves.';
    Exit;
  end;
  if not GetBinaryType(AddBackslash(Root) + 'bin\64bit\obs64.exe', BinaryType) or
     (BinaryType <> 6) then
    Result := 'Select a 64-bit (x64) OBS Studio installation.';
end;

procedure UpdateMode(Sender: TObject);
begin
  if ShowingPortable then
    PortableRoot := FolderEdit.Text
  else
    StandardRoot := FolderEdit.Text;
  ShowingPortable := PortableRadio.Checked;
  if ShowingPortable then begin
    FolderLabel.Caption := '&Portable OBS root folder:';
    FolderEdit.Text := PortableRoot;
  end else begin
    FolderLabel.Caption := '&Installed OBS root folder:';
    FolderEdit.Text := StandardRoot;
  end;
end;

procedure BrowseForOBS(Sender: TObject);
var
  Selected: String;
begin
  Selected := FolderEdit.Text;
  if BrowseForFolder('Choose the OBS root folder containing bin, data, and obs-plugins',
      Selected, False) then
    FolderEdit.Text := Selected;
end;

procedure AddText(var Control: TNewStaticText; Text: String; X, Y, H: Integer);
begin
  Control := TNewStaticText.Create(ModePage);
  Control.Parent := ModePage.Surface;
  Control.AutoSize := False;
  Control.WordWrap := True;
  Control.SetBounds(ScaleX(X), ScaleY(Y), ModePage.SurfaceWidth - ScaleX(X), ScaleY(H));
  Control.Caption := Text;
end;

function InitializeSetup: Boolean;
var
  Mode: String;
begin
  Mode := Lowercase(ExpandConstant('{param:MODE|standard}'));
  Result := (Mode = 'standard') or (Mode = 'portable');
  if not Result then
    SuppressibleMsgBox('Unknown installation mode. Use /MODE=standard or /MODE=portable.',
      mbError, MB_OK, IDOK);
end;

procedure InitializeWizard;
var
  InitialRoot: String;
begin
  StandardRoot := DetectOBSRoot;
  PortableRoot := '';
  InitialRoot := ExpandConstant('{param:OBSROOT|}');
  if InitialRoot = '' then
    InitialRoot := ExpandConstant('{param:DIR|}');
  ShowingPortable := Lowercase(ExpandConstant('{param:MODE|standard}')) = 'portable';
  if InitialRoot <> '' then begin
    if ShowingPortable then PortableRoot := InitialRoot
    else StandardRoot := InitialRoot;
  end;

  ModePage := CreateCustomPage(wpLicense, 'Choose your OBS installation',
    'Install into a standard Windows installation or a portable OBS folder.');

  StandardRadio := TNewRadioButton.Create(ModePage);
  StandardRadio.Parent := ModePage.Surface;
  StandardRadio.SetBounds(0, 0, ModePage.SurfaceWidth, ScaleY(22));
  StandardRadio.Caption := '&Standard OBS installation (default)';
  StandardRadio.Checked := not ShowingPortable;
  StandardRadio.OnClick := @UpdateMode;
  AddText(StandardHelp, 'Finds OBS installed on Windows. Check the detected folder below; ' +
    'use Browse if OBS is installed somewhere else.', 20, 25, 36);

  PortableRadio := TNewRadioButton.Create(ModePage);
  PortableRadio.Parent := ModePage.Surface;
  PortableRadio.SetBounds(0, ScaleY(74), ModePage.SurfaceWidth, ScaleY(22));
  PortableRadio.Caption := '&Portable OBS';
  PortableRadio.Checked := ShowingPortable;
  PortableRadio.OnClick := @UpdateMode;
  AddText(PortableHelp, 'Choose the root folder of your portable OBS copy. ' +
    'Only that copy receives the plugin.', 20, 99, 34);

  AddText(FolderLabel, '', 0, 149, 20);
  FolderEdit := TNewEdit.Create(ModePage);
  FolderEdit.Parent := ModePage.Surface;
  FolderEdit.SetBounds(0, ScaleY(173), ModePage.SurfaceWidth - ScaleX(92), ScaleY(23));
  FolderLabel.FocusControl := FolderEdit;
  BrowseButton := TNewButton.Create(ModePage);
  BrowseButton.Parent := ModePage.Surface;
  BrowseButton.SetBounds(ModePage.SurfaceWidth - ScaleX(84), ScaleY(172), ScaleX(84), ScaleY(25));
  BrowseButton.Caption := '&Browse...';
  BrowseButton.OnClick := @BrowseForOBS;
  AddText(FolderHelp, 'Select the folder containing bin, data, and obs-plugins.' + #13#10 +
    'Example: C:\Program Files\obs-studio or D:\OBS-Portable' + #13#10#13#10 +
    'Close OBS before continuing. Your scenes and audio settings are kept.',
    0, 207, 74);
  if ShowingPortable then begin
    FolderLabel.Caption := '&Portable OBS root folder:';
    FolderEdit.Text := PortableRoot;
  end else begin
    FolderLabel.Caption := '&Installed OBS root folder:';
    FolderEdit.Text := StandardRoot;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ErrorText, Root: String;
begin
  Result := True;
  if CurPageID <> ModePage.ID then Exit;
  Root := CleanRoot(FolderEdit.Text);
  ErrorText := ValidateRoot(Root);
  if ErrorText <> '' then begin
    Log('OBS folder validation failed: ' + ErrorText);
    SuppressibleMsgBox(ErrorText, mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
  FolderEdit.Text := Root;
  WizardForm.DirEdit.Text := Root;
end;

function OBSRunning: Boolean;
var
  Service, Processes: Variant;
begin
  { Read-only process query: never close a running OBS session automatically. }
  Service := CreateOleObject('WbemScripting.SWbemLocator');
  Service := Service.ConnectServer('', 'root\CIMV2');
  Processes := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
  Result := Processes.Count > 0;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  OtherCopy: String;
begin
  Result := ValidateRoot(WizardDirValue);
  if Result <> '' then Exit;
  try
    if OBSRunning then begin
      Result := 'OBS Studio is running. Close OBS completely, then click Retry.';
      Exit;
    end;
  except
    Log('OBS process check failed: ' + GetExceptionMessage);
    Result := 'Setup could not check whether OBS is running. Close OBS, then try again.';
    Exit;
  end;
  if IsStandard then begin
    OtherCopy := ExpandConstant('{commonappdata}\obs-studio\plugins\obs-vban-audio\bin\64bit\obs-vban-audio.dll');
    if FileExists(OtherCopy) then
      Result := 'Another copy of VBAN Audio exists here:' + #13#10 + OtherCopy + #13#10#13#10 + 'Remove that plugin copy first, or keep using its ZIP installation layout. ' +
        'This installer uses the selected OBS folder to avoid loading the plugin twice.';
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Mode: String;
begin
  if IsStandard then Mode := 'Standard OBS installation' else Mode := 'Portable OBS';
  Result := Mode + NewLine + NewLine +
    'OBS folder:' + NewLine + Space + WizardDirValue + NewLine + NewLine +
    'Plugin DLL:' + NewLine + Space + WizardDirValue + '\obs-plugins\64bit\obs-vban-audio.dll' +
    NewLine + NewLine + 'Plugin data and uninstaller:' + NewLine +
    Space + WizardDirValue + '\data\obs-plugins\obs-vban-audio' + NewLine + NewLine +
    'Your OBS scenes, profiles and VBAN settings are preserved.';
end;

function InitializeUninstall: Boolean;
var
  ExpectedFolder: String;
begin
  Result := False;
  ExpectedFolder := ExpandConstant('{app}\data\obs-plugins\obs-vban-audio');
  if CompareText(RemoveBackslashUnlessRoot(ExtractFilePath(ExpandConstant('{uninstallexe}'))),
      ExpectedFolder) <> 0 then begin
    SuppressibleMsgBox('This OBS folder has moved since installation. Run the installer again ' +
      'for its new location before uninstalling, or remove only the plugin DLL and data folder manually.',
      mbError, MB_OK, IDOK);
    Exit;
  end;
  try
    if OBSRunning then begin
      SuppressibleMsgBox('Close OBS Studio before uninstalling VBAN Audio.',
        mbError, MB_OK, IDOK);
      Exit;
    end;
  except
    SuppressibleMsgBox('Unable to check whether OBS is running. Close OBS and try again.',
      mbError, MB_OK, IDOK);
    Exit;
  end;
  Result := True;
end;
