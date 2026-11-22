; Inno Setup script for the Halo 2 recompilation (Windows 10/11, 64-bit), after Marvel vs. Capcom 2's mvc2.iss.
; Built by:
;   python scripts/build_installer.py --version <version> (it runs the steps below)
; Output: installer\Output\Halo2-Recomp-Setup-<version>.exe
; The install is portable: the game, its game files (game_files) and its data folder (settings, key map, saves,
; console identity, logs) are all in the install folder. There is no uninstaller, no Programs and Features entry and
; nothing in the registry; deleting the folder (and the shortcuts) removes the game. If the folder chosen on the folder
; page already holds the game, Setup only updates the program files: it does not ask for the disc and does not copy
; the 4.7 GB of game files again.
; The look is the Halo 2 manuals', as in Config.exe and the F1 menu: the pictures are made by build_installer.py.
#ifndef AppVersion
#define AppVersion "1.0.0"
#endif
#ifndef BuildDir
#define BuildDir "..\build-release"
#endif
; The hi-res texture pack (scripts/build_texture_pack.py writes texture_payload.iss): an optional task that downloads
; its .bin parts from the textures-<n> release, or uses the ones beside Setup, and extracts them into textures.
#if FileExists(AddBackslash(SourcePath) + "texture_payload.iss")
#define HasTextures
#include "texture_payload.iss"
#endif

[Setup]
AppId={{6C1E2B7A-93D4-4F0E-B5A8-2D7F1C9E4A63}
AppName=Halo 2 Recomp
AppVersion={#AppVersion}
AppPublisher=dalves408/halo2-recomp
AppPublisherURL=https://github.com/dalves408/halo2-recomp
DefaultDirName=C:\Games\Halo 2 Recomp
DefaultGroupName=Halo 2 Recomp
DisableProgramGroupPage=yes
; portable: no uninstaller and no Programs and Features entry, so no previous folder is remembered either. The folder
; page always shows and an existing install is recognised in the folder chosen.
DisableDirPage=no
UsePreviousAppDir=no
Uninstallable=no
CreateUninstallRegKey=no
DirExistsWarning=no
; a running game or settings program is closed so its exe can be replaced
CloseApplications=yes
CloseApplicationsFilter=halo2_recomp.exe,halo2_downloader.exe,config.exe,Insignia Setup.exe,Halo2 Updater.exe
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=..\res\halo2.ico
; The Halo 2 manuals' look: dark blue pages over the menus' gradient, the angled side panel on the first and last
; pages, the icon at the top right, and the menus' font.
WizardStyle=modern dark polar hidebevels
WizardBackImageFile=wizard_back.png
WizardImageFile=wizard_image.png
WizardSmallImageFile=wizard_small_55.png,wizard_small_64.png,wizard_small_80.png,wizard_small_96.png,wizard_small_128.png
DefaultDialogFontName=Bahnschrift
OutputBaseFilename=Halo2-Recomp-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
ExtraDiskSpaceRequired=4800000000

[Messages]
WelcomeLabel1=Halo 2 Recomp
WelcomeLabel2=This installs Halo 2 (title update 1.5), recompiled to run natively on Windows.%n%nThe game's own files are not included. You need your own copy of the Xbox disc (a disc image or a folder with its files). Title update 1.5 is made from your disc, or Setup uses your own copy from your console's hard drive. Setup copies the files it needs, checking each one.
FinishedHeadingLabel=Halo 2 Recomp is ready

[Files]
Source: "{#BuildDir}\halo2_recomp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\halo2_downloader.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\Config.exe"; DestDir: "{app}"; DestName: "config.exe"; Flags: ignoreversion
Source: "{#BuildDir}\Insignia Setup.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\Halo2 Updater.exe"; DestDir: "{app}"; Flags: ignoreversion
; controllers (SDL3, zlib license): the game needs it beside halo2_recomp.exe
Source: "{#BuildDir}\SDL3.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\third_party\SDL3-3.4.20\LICENSE.txt"; DestDir: "{app}"; DestName: "SDL3-LICENSE.txt"; Flags: ignoreversion skipifsourcedoesntexist
; the default keyboard and mouse layout (a player's own changes are in data\keymap.ini)
Source: "..\keymap.ini"; DestDir: "{app}"; Flags: ignoreversion
; the import tool only runs during setup; it is not installed
Source: "{#BuildDir}\halo2_import.exe"; DestDir: "{tmp}"; Flags: ignoreversion deleteafterinstall
; title update 1.5 made from the disc when no update is given (scripts/convert_tu_patches.py); beside the importer
Source: "patches\default.xbe.tu15.h2p"; DestDir: "{tmp}\patches"; Flags: ignoreversion deleteafterinstall
Source: "patches\downloader.xbe.tu15.h2p"; DestDir: "{tmp}\patches"; Flags: ignoreversion deleteafterinstall
#ifdef HasTextures
; the hi-res texture pack's download helper, run from Setup's temp folder when the texture task is chosen
Source: "{#BuildDir}\halo2_texture_download.exe"; Flags: dontcopy
#endif

[Dirs]
Name: "{app}\data"

[Icons]
Name: "{autoprograms}\Halo 2 Recomp"; Filename: "{app}\halo2_recomp.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Halo 2 Recomp Settings"; Filename: "{app}\config.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Halo 2 Recomp Insignia Setup"; Filename: "{app}\Insignia Setup.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Halo 2 Recomp"; Filename: "{app}\halo2_recomp.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
#ifdef HasTextures
Name: "hires"; Description: "Install the hi-res texture pack (4x textures, about {#TextureDownloadBytes / 1000000} MB download)"; Flags: unchecked; Check: TexturePackNeedsInstall
#endif

[Run]
Filename: "{app}\halo2_recomp.exe"; Description: "Play Halo 2 now"; Flags: nowait postinstall skipifsilent unchecked; Check: GameFilesPresent(ExpandConstant('{app}'))

[Code]
var
  SrcPage, UpdatePage: TWizardPage;
  SrcEdit, TuEdit: TNewEdit;
  UpdateOnly, UpdateMode: Boolean;
  OldDir, OldVer, NewVer: String;
  VerRel: Integer;          { -1: the installed version is older, 0: same, 1: newer than this Setup }
  RepairRadio, QuitRadio: TNewRadioButton;
  QuitNow: Boolean;
  ImportError: String;

#ifdef HasTextures
#define TextureOptional
#include "texture_download_code.iss"
#else
function WantTexturePack: Boolean; begin Result := False; end;
function TexturePackInstalled: Boolean; begin Result := False; end;
#endif

{ The game files are in place: the title update's default.xbe and a few of the largest maps, so an install that was
  interrupted halfway is not mistaken for a finished one. }
function GameFilesPresent(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\game_files\default.xbe') and
            FileExists(Dir + '\game_files\downloader.xbe') and
            FileExists(Dir + '\game_files\maps\shared.map') and
            FileExists(Dir + '\game_files\maps\mainmenu.map');
end;

{ The version of the game in Dir, from its exe ('' when unknown) }
function InstalledVersion(Dir: String): String;
var
  V: String;
begin
  Result := '';
  if GetVersionNumbersString(Dir + '\halo2_recomp.exe', V) then Result := V;
end;

{ Saves, settings and the console identity live in the install folder (data), so it must be writable: Program Files
  is not, without administrator rights. A folder that did not exist is removed again after the test. }
function DirWritable(Dir: String): Boolean;
var
  F: String;
  Made: Boolean;
begin
  Made := not DirExists(Dir);
  Result := ForceDirectories(Dir);
  if not Result then Exit;
  F := AddBackslash(Dir) + 'halo2_setup_write_test.tmp';
  Result := SaveStringToFile(F, 'x', False);
  DeleteFile(F);
  if Made then RemoveDir(Dir);
end;

function VerPart(V: String; N: Integer): Integer;
var
  I, P: Integer;
begin
  for I := 1 to N do
  begin
    P := Pos('.', V);
    if P = 0 then begin V := ''; Break; end;
    V := Copy(V, P + 1, Length(V));
  end;
  P := Pos('.', V);
  if P > 0 then V := Copy(V, 1, P - 1);
  Result := StrToIntDef(V, 0);
end;

{ -1 if A is older than B, 0 if the same, 1 if newer }
function CompareVer(A, B: String): Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := 0 to 3 do
    if VerPart(A, I) <> VerPart(B, I) then
    begin
      if VerPart(A, I) < VerPart(B, I) then Result := -1 else Result := 1;
      Exit;
    end;
end;

var
  UpdY: Integer;
  UpdPanel: TPanel;

procedure AddLine(Text: String; Bold: Boolean);
var
  L: TNewStaticText;
begin
  L := TNewStaticText.Create(UpdatePage);
  L.Parent := UpdPanel;
  L.AutoSize := False;
  L.WordWrap := True;
  L.Left := 0;
  L.Top := UpdY;
  L.Width := UpdatePage.SurfaceWidth;
  if Bold then L.Font.Style := [fsBold];
  L.Caption := Text;
  L.Height := ScaleY(18) * (1 + Length(Text) div 70);
  UpdY := UpdY + L.Height + ScaleY(6);
end;

procedure RadioClick(Sender: TObject);
begin
  if QuitRadio.Checked then
    WizardForm.NextButton.Caption := 'Quit'
  else
    WizardForm.NextButton.Caption := 'Repair';
end;

procedure AddChoice(var R: TNewRadioButton; Text: String; Checked: Boolean);
begin
  R := TNewRadioButton.Create(UpdatePage);
  R.Parent := UpdPanel;
  R.Left := 0;
  R.Top := UpdY;
  R.Width := UpdatePage.SurfaceWidth;
  R.Height := ScaleY(20);
  R.Caption := Text;
  R.Checked := Checked;
  R.OnClick := @RadioClick;
  UpdY := UpdY + R.Height + ScaleY(6);
end;

procedure FillUpdatePage;
begin
  { Rebuilt each time it is shown: Back on the folder page can choose another folder. }
  if UpdPanel <> nil then UpdPanel.Free;
  UpdPanel := TPanel.Create(UpdatePage);
  UpdPanel.Parent := UpdatePage.Surface;
  UpdPanel.BevelOuter := bvNone;
  UpdPanel.ParentBackground := True;
  UpdPanel.Left := 0;
  UpdPanel.Top := 0;
  UpdPanel.Width := UpdatePage.SurfaceWidth;
  UpdPanel.Height := UpdatePage.SurfaceHeight;
  RepairRadio := nil;
  QuitRadio := nil;
  if VerRel = 0 then
  begin
    WizardForm.PageNameLabel.Caption := 'This version is already installed';
    WizardForm.PageDescriptionLabel.Caption := 'The folder you chose has the same version of Halo 2 Recomp.';
  end else
  begin
    WizardForm.PageNameLabel.Caption := 'Halo 2 Recomp is already installed';
    WizardForm.PageDescriptionLabel.Caption := 'The folder you chose already has the game.';
  end;
  UpdY := 0;
  AddLine('Installed in:  ' + OldDir, False);
  if OldVer <> '' then
    AddLine('Installed version:  ' + OldVer, False)
  else
    AddLine('Installed version:  (not recorded)', False);
  AddLine('This Setup:  version ' + NewVer, True);
  UpdY := UpdY + ScaleY(10);
  if VerRel = 0 then
  begin
    AddLine('The installed version matches this Setup. You can repair the installation, which reinstalls the ' +
            'program files and shortcuts. Your game files, saves, settings and console identity are kept, and you ' +
            'do not need your disc.', False);
    UpdY := UpdY + ScaleY(10);
    AddChoice(RepairRadio, 'Repair the installation', True);
    AddChoice(QuitRadio, 'Quit (leave everything as it is)', False);
    Exit;
  end;
  if VerRel < 0 then
    AddLine('Setup will update the program files from version ' + OldVer + ' to version ' + NewVer +
            '. Your game files, saves, settings and console identity are kept, and you do not need your disc.', False)
  else
    AddLine('A NEWER version than this Setup is installed. Continuing will replace it with the older version ' +
            NewVer + '. Your game files, saves, settings and console identity are kept.', False);
  UpdY := UpdY + ScaleY(10);
  AddLine('Click Update to continue, or Cancel to leave the installed version as it is.', False);
end;

procedure BrowseFile(Sender: TObject);
var
  F: String;
begin
  F := SrcEdit.Text;
  if GetOpenFileName('Select the disc image', F, '', 'Xbox disc images (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*', 'iso') then
    SrcEdit.Text := F;
end;

procedure BrowseFolder(Sender: TObject);
var
  D: String;
begin
  D := SrcEdit.Text;
  if BrowseForFolder('Select the folder that holds the game''s files (the one with default.xbe)', D, False) then
    SrcEdit.Text := D;
end;

procedure BrowseUpdate(Sender: TObject);
var
  D: String;
begin
  D := TuEdit.Text;
  if BrowseForFolder('Select title update 1.5''s folder (TDATA\4D530064\$u from your console, with default.xbe and downloader.xbe)', D, False) then
    TuEdit.Text := D;
end;

function MakeLabel(Page: TWizardPage; Text: String; Top: Integer): TNewStaticText;
begin
  Result := TNewStaticText.Create(Page);
  Result.Parent := Page.Surface;
  Result.AutoSize := False;
  Result.WordWrap := True;
  Result.Width := Page.SurfaceWidth;
  Result.Caption := Text;
  Result.Left := 0;
  Result.Top := Top;
  Result.Height := ScaleY(16) * (1 + Length(Text) div 80);
end;

procedure InitializeWizard;
var
  L: TNewStaticText;
  B1, B2, B3: TNewButton;
begin
  NewVer := '{#AppVersion}';
  { Shown after the folder page, only when that folder already holds the game (filled by FillUpdatePage). }
  UpdatePage := CreateCustomPage(wpSelectDir, 'Halo 2 Recomp is already installed', 'The folder you chose already has the game.');
  SrcPage := CreateCustomPage(wpSelectDir, 'Your copy of the game',
    'Select your Halo 2 disc. Setup copies the game files from it, brings them to title update 1.5 and checks each one.');
  L := MakeLabel(SrcPage, 'Halo 2 disc (USA, Europe): a disc image (.iso, .xiso) or a folder containing default.xbe:', 0);
  SrcEdit := TNewEdit.Create(SrcPage);
  SrcEdit.Parent := SrcPage.Surface;
  SrcEdit.Left := 0; SrcEdit.Top := L.Top + L.Height + ScaleY(4);
  SrcEdit.Width := SrcPage.SurfaceWidth;
  SrcEdit.Text := ExpandConstant('{param:SOURCE|}');   { setup /SOURCE=<image or folder> for silent installs }
  B1 := TNewButton.Create(SrcPage);
  B1.Parent := SrcPage.Surface;
  B1.Caption := 'Disc image...';
  B1.Left := 0; B1.Top := SrcEdit.Top + SrcEdit.Height + ScaleY(6);
  B1.Width := ScaleX(110);
  B1.OnClick := @BrowseFile;
  B2 := TNewButton.Create(SrcPage);
  B2.Parent := SrcPage.Surface;
  B2.Caption := 'Folder...';
  B2.Left := B1.Left + B1.Width + ScaleX(8); B2.Top := B1.Top;
  B2.Width := ScaleX(110);
  B2.OnClick := @BrowseFolder;

  L := MakeLabel(SrcPage, 'Title update 1.5 (optional): its folder from your console''s hard drive (E:\TDATA\4D530064\$u, ' +
    'with default.xbe and downloader.xbe). Leave it empty and Setup makes the update from your disc.',
    B1.Top + B1.Height + ScaleY(18));
  TuEdit := TNewEdit.Create(SrcPage);
  TuEdit.Parent := SrcPage.Surface;
  TuEdit.Left := 0; TuEdit.Top := L.Top + L.Height + ScaleY(4);
  TuEdit.Width := SrcPage.SurfaceWidth;
  TuEdit.Text := ExpandConstant('{param:UPDATE|}');   { setup /UPDATE=<title update folder> for silent installs }
  B3 := TNewButton.Create(SrcPage);
  B3.Parent := SrcPage.Surface;
  B3.Caption := 'Update folder...';
  B3.Left := 0; B3.Top := TuEdit.Top + TuEdit.Height + ScaleY(6);
  B3.Width := ScaleX(130);
  B3.OnClick := @BrowseUpdate;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if UpdateMode and (CurPageID = UpdatePage.ID) then
  begin
    FillUpdatePage;
    if VerRel = 0 then RadioClick(nil) else WizardForm.NextButton.Caption := 'Update';
  end
  else if (CurPageID = wpFinished) and UpdateMode and UpdateOnly then
  begin
    if VerRel = 0 then
      WizardForm.FinishedLabel.Caption := 'Halo 2 Recomp ' + NewVer + ' was repaired.' + #13#10#13#10 +
        'Your game files, saves, settings and console identity were kept.'
    else
      WizardForm.FinishedLabel.Caption := 'Halo 2 Recomp was updated to version ' + NewVer + '.' + #13#10#13#10 +
        'Your game files, saves, settings and console identity were kept.';
  end
  else if (CurPageID = wpFinished) and (ImportError = '') then
    WizardForm.FinishedLabel.Caption := 'Halo 2 Recomp ' + NewVer + ' is installed.' + #13#10#13#10 +
      'Settings and controls: the Halo 2 Recomp Settings shortcut, or F1 in the game. ' +
      'For Xbox Live through Insignia, run Halo 2 Recomp Insignia Setup first.';
  if (CurPageID = wpFinished) and WantTexturePack and TexturePackInstalled then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      'The hi-res texture pack is installed in the textures folder and loads when the game starts.';
end;

{ The "Ready to install" page says what is about to happen. }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  if UpdateMode and GameFilesPresent(WizardDirValue) then
  begin
    if VerRel = 0 then
      S := 'Repair Halo 2 Recomp ' + NewVer + ' in:'
    else
    begin
      S := 'Update Halo 2 Recomp';
      if OldVer <> '' then S := S + ' from version ' + OldVer;
      S := S + ' to version ' + NewVer + ' in:';
    end;
    S := S + NewLine + Space + WizardDirValue + NewLine + NewLine +
         'Only the program files are replaced. Your game files, saves, settings and console identity are kept.';
    if MemoTasksInfo <> '' then S := S + NewLine + NewLine + MemoTasksInfo;
#ifdef HasTextures
    if WantTexturePack and TexturePackNeedsInstall then
      if TextureAllPartsLocal then S := S + NewLine + NewLine + 'Hi-res texture pack: Setup will check and use the texture archive beside it.'
      else S := S + NewLine + NewLine + 'Hi-res texture pack: Setup will download it (about {#TextureDownloadBytes / 1000000} MB).';
#endif
    Result := S;
  end else
  begin
    S := '';
    if MemoDirInfo <> '' then S := S + MemoDirInfo + NewLine + NewLine;
    S := S + 'Game files (about 4.7 GB) from:' + NewLine + Space + Trim(SrcEdit.Text) + NewLine;
    if Trim(TuEdit.Text) <> '' then
      S := S + 'Title update 1.5 from:' + NewLine + Space + Trim(TuEdit.Text) + NewLine
    else
      S := S + 'Title update 1.5: made from your disc' + NewLine;
    if MemoTasksInfo <> '' then S := S + NewLine + MemoTasksInfo + NewLine;
#ifdef HasTextures
    if WantTexturePack and TexturePackNeedsInstall then
      if TextureAllPartsLocal then S := S + NewLine + 'Hi-res texture pack: Setup will check and use the texture archive beside it.'
      else S := S + NewLine + 'Hi-res texture pack: Setup will download it (about {#TextureDownloadBytes / 1000000} MB).';
#endif
    Result := S;
  end;
end;

{ No disc is needed when the game files are already in the chosen folder. }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := ((PageID = SrcPage.ID) and GameFilesPresent(WizardDirValue)) or
            ((PageID = UpdatePage.ID) and not GameFilesPresent(WizardDirValue));
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if QuitNow then Confirm := False;      { Quit on the matching-version page: no "Exit Setup?" question }
#ifdef HasTextures
  TextureCancelButtonClick(Cancel, Confirm);
#endif
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    if not DirWritable(RemoveBackslashUnlessRoot(WizardDirValue)) then
    begin
      MsgBox('Setup cannot write to this folder. The game keeps its saves, settings and console identity inside its ' +
             'own folder, so choose one you can write to, such as C:\Games\Halo 2 Recomp (not Program Files).', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    OldDir := RemoveBackslashUnlessRoot(WizardDirValue);
    UpdateMode := GameFilesPresent(OldDir);
    if UpdateMode then
    begin
      OldVer := InstalledVersion(OldDir);
      if OldVer <> '' then VerRel := CompareVer(OldVer, NewVer) else VerRel := -1;
    end;
  end;
  if UpdateMode and (VerRel = 0) and (CurPageID = UpdatePage.ID) and (QuitRadio <> nil) and QuitRadio.Checked then
  begin
    QuitNow := True;
    WizardForm.Close;
    Result := False;
    Exit;
  end;
  if CurPageID = SrcPage.ID then
  begin
    if (Trim(SrcEdit.Text) = '') or not (FileExists(Trim(SrcEdit.Text)) or DirExists(Trim(SrcEdit.Text))) then
    begin
      MsgBox('Select an existing disc image or folder.', mbError, MB_OK);
      Result := False;
    end
    else if (Trim(TuEdit.Text) <> '') and not (FileExists(Trim(TuEdit.Text)) or DirExists(Trim(TuEdit.Text))) then
    begin
      MsgBox('The title update folder you entered does not exist. Select it again, or leave it empty.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
#ifdef HasTextures
  { The texture parts are downloaded and checked before anything is installed; new game files need room too. }
  if GameFilesPresent(RemoveBackslashUnlessRoot(WizardDirValue)) then
    Result := PrepareTexturePayload(104857600)
  else
    Result := PrepareTexturePayload(4800000000);
#endif
end;

{ The importer's "PROGRESS done total" lines move the progress bar. }
var
  LastError: String;

procedure ImportLog(const S: String; const Error, FirstLine: Boolean);
var
  P, Q: Integer;
  Rest: String;
  Done, Total: Int64;
begin
  if Error then Exit;
  if Copy(S, 1, 9) = 'PROGRESS ' then
  begin
    Rest := Copy(S, 10, Length(S));
    P := Pos(' ', Rest);
    if P > 0 then
    begin
      Done := StrToInt64Def(Copy(Rest, 1, P - 1), 0);
      Total := StrToInt64Def(Copy(Rest, P + 1, Length(Rest)), 0);
      if Total > 0 then
      begin
        Q := Integer((Done * 1000) div Total);
        WizardForm.ProgressGauge.Max := 1000;
        WizardForm.ProgressGauge.Position := Q;
        WizardForm.FilenameLabel.Caption := Format('%d of %d MB', [Integer(Done div 1048576), Integer(Total div 1048576)]);
      end;
    end;
  end
  else if Copy(S, 1, 6) = 'ERROR ' then
    LastError := Copy(S, 7, Length(S))
  else
    Log(S);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Msg, Params: String;
begin
  if CurStep = ssInstall then
    UpdateOnly := GameFilesPresent(ExpandConstant('{app}'));
  if (CurStep = ssPostInstall) and not UpdateOnly then
  begin
    WizardForm.StatusLabel.Caption := 'Copying and checking the game files from your disc and title update (about 4.7 GB, a few minutes)...';
    WizardForm.FilenameLabel.Caption := '';
    WizardForm.ProgressGauge.Position := 0;
    LastError := '';
    Params := '"' + Trim(SrcEdit.Text) + '" "' + ExpandConstant('{app}\game_files') + '"';
    if Trim(TuEdit.Text) <> '' then Params := Params + ' --update "' + Trim(TuEdit.Text) + '"';
    if not ExecAndLogOutput(ExpandConstant('{tmp}\halo2_import.exe'), Params, '', SW_SHOWNORMAL,
        ewWaitUntilTerminated, Code, @ImportLog) then
      Code := 99;
    if Code <> 0 then
    begin
      case Code of
        2: Msg := 'The disc image or folder could not be found.';
        3: Msg := 'That file is not an Xbox disc image.';
        4: Msg := 'That is not the supported Halo 2 disc (USA, Europe).';
        5: Msg := 'Some game files are missing or damaged. The dump may be incomplete.';
        6: Msg := 'The game files could not be written to the install folder.';
        7: Msg := 'The title update you selected is not title update 1.5. Select its folder from your console''s ' +
                  'hard drive (E:\TDATA\4D530064\$u, with default.xbe and downloader.xbe), or leave the field empty.';
      else
        Msg := 'The game files could not be copied.';
      end;
      ImportError := Msg;
      if LastError <> '' then Log('importer: ' + LastError);
      MsgBox(Msg + #13#10#13#10 + 'Halo 2 Recomp was installed but cannot start yet. ' +
        'Run Setup again with the same folder and select a correct disc image or folder and title update.', mbError, MB_OK);
    end;
  end;
#ifdef HasTextures
  if (CurStep = ssPostInstall) and GameFilesPresent(ExpandConstant('{app}')) then InstallTexturePack;
  if CurStep = ssDone then TextureFinishCacheChoice;
#endif
end;

#ifdef HasTextures
procedure DeinitializeSetup;
begin
  TextureCleanupOnExit;
end;
#endif

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if not GameFilesPresent(ExpandConstant('{app}')) or (WantTexturePack and not TexturePackInstalled) then
    Result := 1;
end;
