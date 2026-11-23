; Standalone, extraction-only Setup for the hi-res texture pack (after Marvel vs. Capcom 2's mvc2_textures.iss).
; Built by scripts/build_installer.py once scripts/build_texture_pack.py has written texture_payload.iss. It finds the
; game folder (beside this EXE, or C:\Games\Halo 2 Recomp, or the one the player picks: the game is portable and not
; registered anywhere), downloads the pack's .bin parts from the textures-<n> release (or uses the ones beside it),
; checks each against texture_payload.iss and extracts them into the game's textures folder.
#include "texture_payload.iss"
#ifndef BuildDir
#define BuildDir "..\build-release"
#endif

[Setup]
AppId={{{#PackGuid}}
AppName=Halo 2 Recomp Hi-Res Textures
AppVersion={#TextureVersion}
AppPublisher=dalves408/halo2-recomp
AppPublisherURL=https://github.com/dalves408/halo2-recomp
DefaultDirName={code:GameDir}
UsePreviousAppDir=no
AppendDefaultDirName=no
DisableDirPage=no
DisableProgramGroupPage=yes
DirExistsWarning=no
Uninstallable=no
CreateUninstallRegKey=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=yes
CloseApplicationsFilter=halo2_recomp.exe,halo2_downloader.exe
SetupIconFile=..\res\halo2.ico
WizardStyle=modern dark polar hidebevels
WizardBackImageFile=wizard_back.png
WizardImageFile=wizard_image.png
WizardSmallImageFile=wizard_small_55.png,wizard_small_64.png,wizard_small_80.png,wizard_small_96.png,wizard_small_128.png
DefaultDialogFontName=Bahnschrift
OutputBaseFilename=Halo2-Recomp-HiRes-Textures-{#TextureVersion}-Setup
; the folder page's free-space line: the extracted pack plus the downloaded parts while it is extracted
ExtraDiskSpaceRequired={#TextureExtractedBytes + TextureDownloadBytes}
ArchiveExtraction=enhanced/nopassword
Compression=lzma2/max
SolidCompression=yes

[Messages]
WelcomeLabel1=Halo 2 Recomp Hi-Res Textures
#ifdef TexturePatchFrom
WelcomeLabel2=Add the hi-res texture pack revision {#TextureVersion} ({#TextureFileCount} textures, upscaled 4x) to Halo 2 Recomp.%n%nIf texture pack revision {#TexturePatchFrom} is already installed, Setup downloads only a small update (about {#TexturePatchBytes / 1000000 + 1} MB) and replaces the changed textures. Otherwise it uses the texture-*.bin files beside this EXE, or downloads the full pack ({#TexturePartCount} parts, about {#TextureDownloadBytes / 1000000} MB in all). An interrupted download resumes when you run Setup again.%n%nRequires Halo 2 Recomp 1.0.0 or newer.
#else
WelcomeLabel2=Add the hi-res texture pack ({#TextureFileCount} textures, upscaled 4x) to Halo 2 Recomp.%n%nSetup uses the texture-*.bin files beside this EXE if they are there; otherwise it downloads them ({#TexturePartCount} parts, about {#TextureDownloadBytes / 1000000} MB in all). An interrupted download resumes when you run Setup again.%n%nRequires Halo 2 Recomp 1.0.0 or newer.
#endif
SelectDirLabel3=Choose your installed Halo 2 Recomp folder (the one with halo2_recomp.exe).
ButtonInstall=&Extract
ReadyLabel1=Ready to extract the hi-res texture pack.
ReadyLabel2a=Click Extract to continue, or Back to choose another game folder.
StatusExtractFiles=Extracting texture pack...
FinishedHeadingLabel=Texture pack installed
FinishedLabel=The hi-res textures are in the game's textures folder and load when Halo 2 Recomp starts.

[Files]
Source: "{#BuildDir}\halo2_texture_download.exe"; Flags: dontcopy

[Code]
#include "texture_download_code.iss"
function GameExists(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\halo2_recomp.exe');
end;

{ The game is portable (no registry entry): the folder this EXE was started from when it is the game folder, else
  the default install folder, else '' and the player picks it on the folder page. }
function FindGameDir: String;
begin
  Result := '';
  if GameExists(RemoveBackslashUnlessRoot(ExpandConstant('{src}'))) then
    Result := RemoveBackslashUnlessRoot(ExpandConstant('{src}'))
  else if GameExists('C:\Games\Halo 2 Recomp') then
    Result := 'C:\Games\Halo 2 Recomp';
end;

function GameDir(Param: String): String;
begin
  Result := FindGameDir;
  if Result = '' then Result := 'C:\Games\Halo 2 Recomp';
end;

function SupportedGame(Exe: String): Boolean;
var MS, LS: Cardinal;
begin
  Result := False;
  if GetVersionNumbers(Exe, MS, LS) then
    Result := MS >= $00010000;
end;

function CheckGame(Dir: String): String;
begin
  Result := '';
  if not GameExists(Dir) then
    Result := 'Choose the installed Halo 2 Recomp folder (the one with halo2_recomp.exe).'
  else if not SupportedGame(Dir + '\halo2_recomp.exe') then
    Result := 'This texture pack requires Halo 2 Recomp 1.0.0 or newer. Update the game first.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var Error: String;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not WizardSilent then begin
    Error := CheckGame(RemoveBackslashUnlessRoot(WizardDirValue));
    if Error <> '' then begin
      MsgBox(Error, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { Silent installs must fail cleanly instead of waiting at a directory-page dialog. }
  Result := CheckGame(RemoveBackslashUnlessRoot(ExpandConstant('{app}')));
  if Result = '' then Result := PrepareTexturePayload(1048576);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpReady then begin
    if TexturePackNeedsInstall then begin
      WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
      WizardForm.PageNameLabel.Caption := SetupMessage(msgWizardReady);
      WizardForm.PageDescriptionLabel.Caption := SetupMessage(msgReadyLabel1);
      WizardForm.ReadyLabel.Caption := SetupMessage(msgReadyLabel2a);
    end else begin
      WizardForm.NextButton.Caption := '&Continue';
      WizardForm.PageNameLabel.Caption := 'Texture pack already installed';
      WizardForm.PageDescriptionLabel.Caption := 'This revision is already in this game folder.';
      WizardForm.ReadyLabel.Caption := 'Click Continue to finish. No download or extraction is needed.';
    end;
  end;
  if (CurPageID = wpFinished) and TexturePackAlreadyCurrent then begin
    WizardForm.FinishedHeadingLabel.Caption := 'Texture pack already installed';
    WizardForm.FinishedLabel.Caption := 'This hi-res texture pack is already installed. No download or extraction was needed.';
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo;
  if not TexturePackNeedsInstall then
    Result := Result + NewLine + NewLine + 'This hi-res texture pack is already installed. No download or extraction is needed.'
  else
    Result := Result + NewLine + NewLine + TextureDownloadPlanText;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  TextureCancelButtonClick(Cancel, Confirm);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    InstallTexturePack;
  if CurStep = ssDone then TextureFinishCacheChoice;
end;

procedure DeinitializeSetup;
begin
  TextureCleanupOnExit;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if not TexturePackInstalled then Result := 1;
end;
