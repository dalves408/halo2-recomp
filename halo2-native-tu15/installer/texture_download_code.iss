{ Shared by the game Setup and the extraction-only standalone texture addon (from Marvel vs. Capcom 2's installer).
  The caller includes texture_payload.iss first, for the pinned Texture* constants.

  For each part: use the copy beside Setup if it is the right one, else the cached download, else download it
  (resumable) into the game's data\Downloads\<part SHA-256> (the install is portable: nothing outside its folder). Every part must match its pinned size and
  SHA-256 before anything is extracted. The parts are extracted into a staging folder, the result is checked against
  the pinned manifest, and only then swapped into textures. /REPAIRTEXTURES=1 reinstalls a current pack,
  /KEEPTEXTUREBIN=1 keeps the download after a successful install (the default, and =0, deletes it; the question
  defaults to deleting), /HIRES=yes selects the game Setup's texture task.

  Updates: when texture_payload.iss defines TexturePatchFrom and the game folder holds that earlier revision (its
  pinned manifest, every file at its old size or at the size the update gives it), Setup downloads only the update
  archive (part 0), checks every file in it against the new manifest's SHA-256, swaps them in one by one and writes
  the new manifest last. An interrupted update leaves a folder that is still recognised and is finished next run.
  Any other folder gets the full pack, as before. }
var
  TextureProgress: TOutputProgressWizardPage;
  TextureTransferActive, TextureTransferCancelled, TextureExtractActive: Boolean;
  TexturePackInstalled: Boolean;
  TexturePackAlreadyCurrent: Boolean;
  TextureUsedCache, TextureCacheChoiceAsked, TextureDeleteCacheOnExit: Boolean;
  TexturePartPaths, TexturePartCaches: array[0..4] of String;  { 0 = the update archive }
  TextureUsePatch: Boolean;
  TexturePart: Integer;
  TextureLastUiTick: LongWord;
  TexturePayloadPath, TextureCacheDir, TextureCancelFile, TextureError: String;
  TextureRatePhase: String;
  TextureRateTick: LongWord;
  TextureRateBytes, TextureRatePerSecond: Int64;
  TextureRateAvailable: Boolean;

function TextureSetupProcessId: LongWord;
  external 'GetCurrentProcessId@kernel32.dll stdcall';
function TextureTick: LongWord;
  external 'GetTickCount@kernel32.dll stdcall';

function GetFileAttributes(FileName: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function TextureForceRepair: Boolean;
begin
  Result := ExpandConstant('{param:REPAIRTEXTURES|0}') = '1';
end;

#if FileExists(AddBackslash(SourcePath) + "texture_patch_files.iss")
#include "texture_patch_files.iss"
#else
function TexturePatchFileBytes(Relative: String): Int64;
begin
  Result := -1;
end;
#endif

{ The pinned download for part I }
function TexturePartUrl(I: Integer): String;
begin
  case I of
    1: Result := '{#TextureURL1}';
#if TexturePartCount >= 2
    2: Result := '{#TextureURL2}';
#endif
#if TexturePartCount >= 3
    3: Result := '{#TextureURL3}';
#endif
#if TexturePartCount >= 4
    4: Result := '{#TextureURL4}';
#endif
  else Result := '';
  end;
end;

function TexturePartSHA(I: Integer): String;
begin
  case I of
    1: Result := '{#TextureSHA2561}';
#if TexturePartCount >= 2
    2: Result := '{#TextureSHA2562}';
#endif
#if TexturePartCount >= 3
    3: Result := '{#TextureSHA2563}';
#endif
#if TexturePartCount >= 4
    4: Result := '{#TextureSHA2564}';
#endif
  else Result := '';
  end;
end;

function TexturePartBytes(I: Integer): Int64;
begin
  case I of
    1: Result := {#TextureBytes1};
#if TexturePartCount >= 2
    2: Result := {#TextureBytes2};
#endif
#if TexturePartCount >= 3
    3: Result := {#TextureBytes3};
#endif
#if TexturePartCount >= 4
    4: Result := {#TextureBytes4};
#endif
  else Result := 0;
  end;
end;

function TextureInstalledPackCurrent(Dir: String): Boolean;
var
  Manifest, Rest, Relative: String;
  Lines: TArrayOfString;
  I, Space: Integer;
  ExpectedSize, ActualSize: Int64;
begin
  Result := False;
  if Dir = '' then Exit;
  Dir := AddBackslash(Dir) + 'textures\';
  Manifest := Dir + 'manifest.txt';
  if not FileExists(Manifest) then Exit;
  try
    { Pin the expected file list rather than trusting a folder or an editable version label. Every listed file
      is checked for its size, so a missing or truncated one is caught. An explicit repair restores them all. }
    if CompareText(GetSHA256OfFile(Manifest), '{#TextureManifestSHA256}') <> 0 then Exit;
    if not LoadStringsFromFile(Manifest, Lines) then Exit;
    if GetArrayLength(Lines) <> {#TextureFileCount} then Exit;
    for I := 0 to GetArrayLength(Lines) - 1 do begin
      Rest := Lines[I];
      Space := Pos(' ', Rest);
      if Space <> 65 then Exit;
      Delete(Rest, 1, Space);
      Space := Pos(' ', Rest);
      if Space <= 1 then Exit;
      ExpectedSize := StrToInt64Def(Copy(Rest, 1, Space - 1), -1);
      Relative := Copy(Rest, Space + 1, Length(Rest));
      StringChangeEx(Relative, '/', '\', True);
      if (Relative = '') or (ExpectedSize < 0) then Exit;
      if not FileSize64(Dir + Relative, ActualSize) then Exit;
      if ActualSize <> ExpectedSize then Exit;
    end;
    Result := True;
  except
    Result := False; { Unreadable metadata or files never suppress an install. }
  end;
end;

{ Whether Dir holds the revision the update starts from: its pinned manifest, and each listed file at its old size,
  or at its new size when the update replaces it (an update that stopped half way). }
function TextureInstalledPackIsBase(Dir: String): Boolean;
#ifdef TexturePatchFrom
var
  Manifest, Rest, Relative: String;
  Lines: TArrayOfString;
  I, Space: Integer;
  ExpectedSize, ActualSize: Int64;
begin
  Result := False;
  if Dir = '' then Exit;
  Dir := AddBackslash(Dir) + 'textures\';
  Manifest := Dir + 'manifest.txt';
  if not FileExists(Manifest) then Exit;
  try
    if CompareText(GetSHA256OfFile(Manifest), '{#TextureBaseManifestSHA256}') <> 0 then Exit;
    if not LoadStringsFromFile(Manifest, Lines) then Exit;
    if GetArrayLength(Lines) <> {#TextureBaseFileCount} then Exit;
    for I := 0 to GetArrayLength(Lines) - 1 do begin
      Rest := Lines[I];
      Space := Pos(' ', Rest);
      if Space <> 65 then Exit;
      Delete(Rest, 1, Space);
      Space := Pos(' ', Rest);
      if Space <= 1 then Exit;
      ExpectedSize := StrToInt64Def(Copy(Rest, 1, Space - 1), -1);
      Relative := Copy(Rest, Space + 1, Length(Rest));
      StringChangeEx(Relative, '/', '\', True);
      if (Relative = '') or (ExpectedSize < 0) then Exit;
      if not FileSize64(Dir + Relative, ActualSize) then Exit;
      if (ActualSize <> ExpectedSize) and (ActualSize <> TexturePatchFileBytes(Relative)) then Exit;
    end;
    Result := True;
  except
    Result := False;
  end;
end;
#else
begin
  Result := False;
end;
#endif

function TexturePackNeedsInstall: Boolean;
begin
  Result := TextureForceRepair or not TextureInstalledPackCurrent(WizardDirValue);
  if not Result then Log('The current texture pack is installed; the texture download offer is hidden.');
end;

{ Whether Setup will apply the small update instead of the full pack (for the Ready page and the free-space check) }
function TextureWillUpdate: Boolean;
begin
  Result := not TextureForceRepair and not TextureInstalledPackCurrent(WizardDirValue) and
            TextureInstalledPackIsBase(WizardDirValue);
end;

function WantTexturePack: Boolean;
begin
#ifdef TextureOptional
  Result := WizardIsTaskSelected('hires') or
    (CompareText(ExpandConstant('{param:HIRES|no}'), 'yes') = 0);
#else
  Result := True;
#endif
end;

function TexturePartName: String;
begin
#ifdef TexturePatchFrom
  if TexturePart = 0 then begin
    Result := '{#TexturePatchName}';
    Exit;
  end;
#endif
  Result := Format('texture-%.2d.bin', [TexturePart]);
end;

function TextureLocalPart: String;
begin
  Result := AddBackslash(ExpandConstant('{src}')) + TexturePartName;
end;

{ Whether every part is beside Setup (the Ready page says so) }
function TextureAllPartsLocal: Boolean;
var I: Integer;
begin
  Result := True;
#ifdef TexturePatchFrom
  if TextureWillUpdate then begin
    Result := FileExists(AddBackslash(ExpandConstant('{src}')) + '{#TexturePatchName}');
    Exit;
  end;
#endif
  for I := 1 to {#TexturePartCount} do
    if not FileExists(AddBackslash(ExpandConstant('{src}')) + Format('texture-%.2d.bin', [I])) then
      Result := False;
end;

{ One sentence for the Ready page: what Setup will fetch for the texture pack }
function TextureDownloadPlanText: String;
begin
  if TextureAllPartsLocal then
    Result := 'Setup will check and use the texture archive beside it.'
#ifdef TexturePatchFrom
  else if TextureWillUpdate then
    Result := 'Texture pack revision {#TexturePatchFrom} is installed, so Setup will download only the update to revision {#TextureVersion} (about ' +
              IntToStr({#TexturePatchBytes} div 1000000 + 1) + ' MB) and replace the changed textures.'
#endif
  else
    Result := 'Setup will download it ({#TexturePartCount} parts, about {#TextureDownloadBytes / 1000000} MB).';
end;

procedure TextureCancelButtonClick(var Cancel, Confirm: Boolean);
begin
  if TextureTransferActive then begin
    TextureTransferCancelled := True;
    SaveStringToFile(TextureCancelFile, 'cancel', False);
    if TextureExtractActive then
      TextureProgress.SetText('Stopping extraction...', 'The verified download is kept for next time.')
    else
      TextureProgress.SetText('Stopping download...', 'Saved progress will be resumed next time.');
    Cancel := False;
    Confirm := False;
  end;
end;

function TextureNetworkPhase(Phase: String): Boolean;
begin
  Result := (Phase = 'DOWNLOADING') or (Phase = 'RESUMING') or (Phase = 'RESTARTING');
end;

function TextureFormatBytes(Bytes: Int64): String;
begin
  if Bytes >= 1000000000 then Result := Format('%.2f GB', [Bytes / 1000000000.0])
  else if Bytes >= 1000000 then Result := Format('%.1f MB', [Bytes / 1000000.0])
  else if Bytes >= 1000 then Result := Format('%.1f KB', [Bytes / 1000.0])
  else Result := IntToStr(Bytes) + ' B';
end;

procedure TextureUpdateDownloadRate(Phase: String; Done: Int64; Tick: LongWord);
begin
  { Begin at the first reported offset: saved or resumed bytes are not new network traffic. Verification,
    retries restarting at zero and clock wrap reset the sample. Refresh speed about once a second. }
  if not TextureNetworkPhase(Phase) or (Phase <> TextureRatePhase) or
     (Tick < TextureRateTick) or (Done < TextureRateBytes) then begin
    TextureRatePhase := Phase;
    TextureRateTick := Tick;
    TextureRateBytes := Done;
    TextureRatePerSecond := 0;
    TextureRateAvailable := False;
  end else if Tick - TextureRateTick >= 1000 then begin
    TextureRatePerSecond := (Done - TextureRateBytes) * 1000 div (Tick - TextureRateTick);
    TextureRateAvailable := True;
    TextureRateTick := Tick;
    TextureRateBytes := Done;
  end;
end;

function TextureProgressDetails(Phase: String; Done, Total: Int64): String;
var Percent: Extended; Speed: String;
begin
  if Done < 0 then Done := 0;
  if Done > Total then Done := Total;
  Percent := 0;
  if Total > 0 then Percent := Done * 100.0 / Total;
  if TextureNetworkPhase(Phase) then begin
    if TextureRateAvailable then Speed := TextureFormatBytes(TextureRatePerSecond) + '/s'
    else Speed := 'Measuring speed...';
    Result := Format('%.1f%% - %.1f / %.1f MB - %s', [Percent, Done / 1000000.0, Total / 1000000.0, Speed]);
  end else if Phase = 'VERIFYING' then
    Result := Format('%s - %s - %.1f%% verified', [TexturePartName, TextureFormatBytes(Total), Percent])
  else Result := TexturePartName + ' - Size: ' + TextureFormatBytes(Total);
end;

procedure TextureDownloadLog(const S: String; const Error, FirstLine: Boolean);
var
  Rest, Phase, LabelText: String;
  K: Integer;
  Done, Total: Int64;
begin
  if Error then begin TextureError := S; Log(S); Exit; end;
  if Copy(S, 1, 9) = 'PROGRESS ' then begin
    Rest := Copy(S, 10, Length(S));
    K := Pos(' ', Rest);
    Done := StrToInt64Def(Copy(Rest, 1, K - 1), 0);
    Delete(Rest, 1, K);
    K := Pos(' ', Rest);
    Total := StrToInt64Def(Copy(Rest, 1, K - 1), 0);
    Phase := Copy(Rest, K + 1, Length(Rest));
    if TextureTransferCancelled then LabelText := 'Stopping download...'
    else if Phase = 'WAITING' then LabelText := 'Waiting for another texture download to finish...'
    else if Phase = 'RESUMING' then LabelText := 'Resuming texture pack download...'
    else if Phase = 'VERIFYING' then LabelText := 'Verifying texture pack...'
    else if Phase = 'RESTARTING' then LabelText := 'The server cannot resume this copy. Restarting download...'
    else if Phase = 'CHECKING' then LabelText := 'Checking for a local texture pack...'
    else LabelText := 'Downloading texture pack...';
    TextureUpdateDownloadRate(Phase, Done, TextureTick);
    if TextureNetworkPhase(Phase) and not TextureTransferCancelled then
      LabelText := LabelText + ' (' + TextureFormatBytes(Total) + ')';
    if TexturePart = 0 then
      LabelText := 'Update: ' + LabelText
    else if {#TexturePartCount} > 1 then
      LabelText := 'Part ' + IntToStr(TexturePart) + ' of {#TexturePartCount}: ' + LabelText;
    TextureProgress.SetText(LabelText, TextureProgressDetails(Phase, Done, Total));
    TextureProgress.SetProgress(Done div 1024, Total div 1024);
  end else begin
    Log(S);
    if S = 'READY local' then begin
      TexturePayloadPath := TextureLocalPart;
      TextureUsedCache := False;
    end else if S = 'READY cache' then begin
      TexturePayloadPath := TextureCacheDir + '\texture.bin';
      TextureUsedCache := True;
    end
    else if Copy(S, 1, 6) = 'ERROR ' then TextureError := Copy(S, 7, Length(S));
  end;
end;

function PrepareTexturePart(Url, SHA: String; Bytes: Int64): String;
var
  Params, LocalParam: String;
  Code: Integer;
  Started, RetryWithoutLocal: Boolean;
begin
  Result := '';
  TextureCacheDir := ExpandConstant('{#TextureCacheRoot}') + '\' + SHA;
  TextureCancelFile := ExpandConstant('{tmp}\texture-download.cancel');
  if TextureProgress = nil then
    TextureProgress := CreateOutputProgressPage('Hi-res texture pack', 'Preparing the hi-res texture pack.');
  TextureProgress.SetText('Checking for a local texture pack...', TexturePartName);
  TextureProgress.SetProgress(0, 0);
  if not FileExists(TextureLocalPart) and
     not FileExists(TextureCacheDir + '\texture.bin') then begin
    if FileExists(TextureCacheDir + '\texture.bin.part') then
      TextureProgress.SetText('Resuming texture pack download...', 'Saved progress will be used.')
    else
      TextureProgress.SetText('The texture pack will be downloaded.', 'You can stop and resume it later.');
  end;
  TextureProgress.Show;
  TextureTransferActive := True;
  TextureTransferCancelled := False;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.CancelButton.Visible := True;
  LocalParam := ' --local ' + AddQuotes(TextureLocalPart);
  try
    repeat
      DeleteFile(TextureCancelFile);
      TexturePayloadPath := '';
      TextureUsedCache := False;
      TextureError := '';
      TextureRatePhase := '';
      TextureRateAvailable := False;
      Params := '--url ' + AddQuotes(Url) +
                ' --cache ' + AddQuotes(TextureCacheDir) +
                ' --sha256 ' + SHA + ' --bytes ' + IntToStr(Bytes) +
                ' --cancel ' + AddQuotes(TextureCancelFile) +
                ' --parent ' + IntToStr(TextureSetupProcessId) + LocalParam;
      Code := 99;
      Started := ExecAndLogOutput(ExpandConstant('{tmp}\halo2_texture_download.exe'), Params, '',
                                 SW_SHOWNORMAL, ewWaitUntilTerminated, Code, @TextureDownloadLog);
      RetryWithoutLocal := False;
      if Started and (Code = 5) and not TextureTransferCancelled then begin
        if WizardSilent then RetryWithoutLocal := True
        else RetryWithoutLocal := MsgBox('The ' + TexturePartName + ' beside Setup does not match this texture pack.' + #13#10#13#10 +
          'Download the correct copy instead? Your local file will be kept.', mbConfirmation, MB_YESNO) = IDYES;
        if RetryWithoutLocal then LocalParam := '';
      end;
    until not RetryWithoutLocal;
    if TextureTransferCancelled or (Code = 2) then
      Result := 'Download stopped. Saved progress is kept. Retry to resume, or Cancel to close Setup.'
    else if not Started or (Code <> 0) or (TexturePayloadPath = '') then begin
      if TextureError = '' then TextureError := 'The texture pack could not be prepared. Retry to continue the saved download.';
      Result := TextureError;
    end else begin
      TexturePartPaths[TexturePart] := TexturePayloadPath;
      if TextureUsedCache then TexturePartCaches[TexturePart] := TexturePayloadPath;
    end;
  finally
    TextureTransferActive := False;
    TextureProgress.Hide;
  end;
end;

function PrepareTexturePayload(OtherRequiredBytes: Int64): String;
var FreeBytes, TotalBytes: Int64; I: Integer;
begin
  Result := '';
  if not WantTexturePack then Exit;
  TexturePackAlreadyCurrent := not TextureForceRepair and TextureInstalledPackCurrent(WizardDirValue);
  TexturePackInstalled := TexturePackAlreadyCurrent;
  if TexturePackAlreadyCurrent then begin
    Log('The current texture pack is already installed; no download or extraction is needed.');
    Exit;
  end;
  if not GetSpaceOnDisk64(AddBackslash(ExtractFileDrive(WizardDirValue)), FreeBytes, TotalBytes) then begin
    Result := 'Could not check free space on the selected game drive.';
    Exit;
  end;
  for I := 0 to {#TexturePartCount} do begin
    TexturePartPaths[I] := '';
    TexturePartCaches[I] := '';
  end;
  TextureUsePatch := TextureWillUpdate;
#ifdef TexturePatchFrom
  if TextureUsePatch then begin
    Log('Texture pack revision {#TexturePatchFrom} found: applying the update to {#TextureDisplayVersion}.');
    if FreeBytes < ({#TexturePatchBytes} + 2 * {#TexturePatchExtractedBytes} + OtherRequiredBytes) then begin
      Result := 'There is not enough free space to download and apply the texture pack update.';
      Exit;
    end;
    MapArchiveExtensions('.bin', '.7z');
    if not FileExists(ExpandConstant('{tmp}\halo2_texture_download.exe')) then
      ExtractTemporaryFile('halo2_texture_download.exe');
    TexturePart := 0;
    Result := PrepareTexturePart('{#TexturePatchURL}', '{#TexturePatchSHA256}', {#TexturePatchBytes});
    Exit;
  end;
#endif
  if FreeBytes < ({#TextureExtractedBytes} + {#TextureDownloadBytes} + OtherRequiredBytes) then begin
    Result := 'There is not enough free space to download and stage the textures and install the selected game files.';
    Exit;
  end;
  MapArchiveExtensions('.bin', '.7z');
  if not FileExists(ExpandConstant('{tmp}\halo2_texture_download.exe')) then
    ExtractTemporaryFile('halo2_texture_download.exe');
  for I := 1 to {#TexturePartCount} do begin
    TexturePart := I;
    Result := PrepareTexturePart(TexturePartUrl(I), TexturePartSHA(I), TexturePartBytes(I));
    if Result <> '' then Exit;
  end;
end;

{ The size of what this run downloads: the update, or every part of the full pack }
function TextureDownloadSizeText: String;
begin
#ifdef TexturePatchFrom
  if TextureUsePatch then begin
    Result := TextureFormatBytes({#TexturePatchBytes});
    Exit;
  end;
#endif
  Result := TextureFormatBytes({#TextureDownloadBytes});
end;

procedure TextureFinishCacheChoice;
var Choice: String; I: Integer; Any: Boolean;
begin
  { ssDone is reached only after a successful install. Never offer to discard partial downloads or a verified
    archive still needed after a failed extraction. }
  Any := False;
  for I := 0 to {#TexturePartCount} do
    if TexturePartCaches[I] <> '' then Any := True;
  if not TexturePackInstalled or TexturePackAlreadyCurrent or TextureCacheChoiceAsked or not Any then Exit;
  TextureCacheChoiceAsked := True;
  { The default is to delete the download once the pack is installed: it is only needed to reinstall without
    downloading again. Keeping it is the choice: No in the question, or /KEEPTEXTUREBIN=1. A copy the player put
    beside Setup is never in TexturePartCaches, so it is never deleted. }
  Choice := ExpandConstant('{param:KEEPTEXTUREBIN|}');
  if Choice = '1' then TextureDeleteCacheOnExit := False
  else if (Choice = '0') or WizardSilent then TextureDeleteCacheOnExit := True
  else
    TextureDeleteCacheOnExit := MsgBox(
      'The hi-res textures are installed. Delete the downloaded texture archive (' + TextureDownloadSizeText + ') now?' + #13#10#13#10 +
      'Yes deletes it when Setup closes. No keeps it, so a repair or reinstall does not have to download it again.',
      mbConfirmation, MB_YESNO or MB_DEFBUTTON1) = IDYES;
  if TextureDeleteCacheOnExit then Log('The user chose to delete the cached texture archive on Setup exit.')
  else Log('The cached texture archive is kept for future repairs or reinstalls.');
end;

procedure TextureCleanupOnExit;
var I: Integer;
begin
  if not TextureDeleteCacheOnExit or not TexturePackInstalled then Exit;
  for I := 0 to {#TexturePartCount} do
    if (TexturePartCaches[I] <> '') and FileExists(TexturePartCaches[I]) then
      if not DeleteFile(TexturePartCaches[I]) then Log('A cached texture archive may still be in use.');
end;

function TextureExtractionProgress(const ArchiveName, FileName: String; const Progress, ProgressMax: Int64): Boolean;
var Tick: LongWord;
begin
  Tick := TextureTick;
  if (Tick - TextureLastUiTick >= 100) or (Progress = ProgressMax) then begin
    TextureLastUiTick := Tick;
    TextureProgress.SetText('Extracting hi-res textures...', ExtractFileName(FileName));
    TextureProgress.SetProgress(Progress div 1024, ProgressMax div 1024);
  end;
  Result := not TextureTransferCancelled;
end;

{ An existing textures folder this Setup recognises (an earlier copy of this revision, or of the revision the update
  starts from) can be deleted after the swap; anything else the player put there is kept beside it. }
function TextureKnownPack(PackDir: String): Boolean;
begin
  Result := False;
  if not FileExists(PackDir + '\manifest.txt') then Exit;
  try
    Result := CompareText(GetSHA256OfFile(PackDir + '\manifest.txt'), '{#TextureManifestSHA256}') = 0;
#ifdef TextureOlderManifests
    { an earlier official revision is ours too: deleted once the new one is in place }
    if not Result then
      Result := Pos(Lowercase(GetSHA256OfFile(PackDir + '\manifest.txt')), Lowercase('{#TextureOlderManifests}')) > 0;
#endif
#ifdef TexturePatchFrom
    { a damaged copy of the revision the update starts from is ours too }
    if not Result then
      Result := CompareText(GetSHA256OfFile(PackDir + '\manifest.txt'), '{#TextureBaseManifestSHA256}') = 0;
#endif
  except
    Result := False;
  end;
end;

#ifdef TexturePatchFrom
{ Moves Src over Dst, first moving any Dst into the backup folder; puts it back if the move fails. }
procedure TextureSwapFile(Src, Dst, Saved: String);
var HadOld: Boolean;
begin
  HadOld := FileExists(Dst);
  if HadOld then begin
    ForceDirectories(ExtractFileDir(Saved));
    if FileExists(Saved) then DeleteFile(Saved);
    if not RenameFile(Dst, Saved) then RaiseException('Cannot move aside ' + Dst);
  end else
    ForceDirectories(ExtractFileDir(Dst));
  if not RenameFile(Src, Dst) then begin
    if HadOld and not RenameFile(Saved, Dst) then
      RaiseException('Cannot restore ' + Dst + '. It is saved at ' + Saved);
    RaiseException('Cannot move the updated texture into place: ' + Dst);
  end;
end;

{ The update from revision {#TexturePatchFrom}: extract it beside the game, check every file in it against the new
  manifest (size and SHA-256) before touching the textures folder, swap the files in, write the manifest last and
  check the whole folder against it. Replaced files wait in a backup folder that is deleted once all is well. }
procedure InstallTexturePatch;
var
  AppDir, PackDir, Stage, Staged, Backup, Rest, Hash, Relative: String;
  Lines, Files: TArrayOfString;
  I, Space, Count: Integer;
  ExpectedSize, ActualSize: Int64;
begin
  if TexturePartPaths[0] = '' then RaiseException('The texture update must be verified before it is applied.');
  AppDir := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  PackDir := AddBackslash(AppDir) + 'textures';
  Stage := AddBackslash(AppDir) + '.texture-stage-' + IntToStr(TextureSetupProcessId);
  Backup := PackDir + '.update-backup-' + IntToStr(TextureSetupProcessId);
  if DirExists(Stage) or FileExists(Stage) or DirExists(Backup) or FileExists(Backup) then
    RaiseException('A texture staging or backup folder already exists.');
  if not CreateDir(Stage) then RaiseException('Cannot create the texture staging folder.');
  TextureTransferCancelled := False;
  TextureExtractActive := True;
  TextureTransferActive := True;
  TextureLastUiTick := 0;
  TextureProgress.SetText('Updating hi-res textures...', 'Preparing the update');
  TextureProgress.SetProgress(0, 0);
  TextureProgress.Show;
  try
    try
      ExtractArchive(TexturePartPaths[0], Stage, '', True, @TextureExtractionProgress);
      if TextureTransferCancelled then RaiseException('stopped');
      Staged := Stage + '\textures\';
      if CompareText(GetSHA256OfFile(Staged + 'manifest.txt'), '{#TextureManifestSHA256}') <> 0 then
        RaiseException('The update carries the wrong file list.');
      if not LoadStringsFromFile(Staged + 'manifest.txt', Lines) or
         (GetArrayLength(Lines) <> {#TextureFileCount}) then
        RaiseException('The update''s file list cannot be read.');
      { Check everything the update carries before anything in the game folder changes. }
      Count := 0;
      SetArrayLength(Files, {#TexturePatchFileCount});
      for I := 0 to GetArrayLength(Lines) - 1 do begin
        Rest := Lines[I];
        Space := Pos(' ', Rest);
        if Space <> 65 then RaiseException('The update''s file list is malformed.');
        Hash := Copy(Rest, 1, 64);
        Delete(Rest, 1, Space);
        Space := Pos(' ', Rest);
        if Space <= 1 then RaiseException('The update''s file list is malformed.');
        ExpectedSize := StrToInt64Def(Copy(Rest, 1, Space - 1), -1);
        Relative := Copy(Rest, Space + 1, Length(Rest));
        StringChangeEx(Relative, '/', '\', True);
        if not FileExists(Staged + Relative) then Continue;
        if Count >= {#TexturePatchFileCount} then RaiseException('The update carries unexpected files.');
        if not FileSize64(Staged + Relative, ActualSize) or (ActualSize <> ExpectedSize) or
           (CompareText(GetSHA256OfFile(Staged + Relative), Hash) <> 0) then
          RaiseException('An updated texture failed its check: ' + Relative);
        Files[Count] := Relative;
        Count := Count + 1;
        TextureProgress.SetText('Checking the update...', Relative);
        TextureProgress.SetProgress(Count, {#TexturePatchFileCount});
      end;
      if Count <> {#TexturePatchFileCount} then RaiseException('The update is incomplete.');
      for I := 0 to Count - 1 do begin
        TextureProgress.SetText('Updating hi-res textures...', Files[I]);
        TextureProgress.SetProgress(I + 1, Count);
        TextureSwapFile(Staged + Files[I], PackDir + '\' + Files[I], Backup + '\' + Files[I]);
      end;
      { The new file list goes in last: until then the folder still reads as revision {#TexturePatchFrom}. }
      TextureSwapFile(Staged + 'manifest.txt', PackDir + '\manifest.txt', Backup + '\manifest.txt');
      if FileExists(Staged + 'README.txt') then
        TextureSwapFile(Staged + 'README.txt', PackDir + '\README.txt', Backup + '\README.txt');
      if not TextureInstalledPackCurrent(AppDir) then
        RaiseException('The updated texture folder does not match {#TextureDisplayVersion}.');
      TexturePackInstalled := True;
      if not DelTree(Backup, True, True, True) then Log('Replaced textures remain at ' + Backup);
      Log('Texture pack updated from revision {#TexturePatchFrom} to {#TextureDisplayVersion}.');
    except
      if TextureTransferCancelled then
        RaiseException('Texture update stopped. The verified download is kept for the next run.')
      else RaiseException('The texture update could not finish. ' + GetExceptionMessage);
    end;
  finally
    DelTree(Stage, True, True, True);
    TextureTransferActive := False;
    TextureExtractActive := False;
    TextureProgress.Hide;
  end;
end;
#endif

procedure InstallTexturePack;
var
  AppDir, PackDir, Stage, Backup: String;
  I: Integer;
  OldMoved, KnownPack: Boolean;
begin
  if not WantTexturePack or TexturePackAlreadyCurrent then Exit;
#ifdef TexturePatchFrom
  if TextureUsePatch then begin
    InstallTexturePatch;
    Exit;
  end;
#endif
  for I := 1 to {#TexturePartCount} do
    if TexturePartPaths[I] = '' then
      RaiseException('Every texture archive must be verified before extraction.');
  AppDir := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  PackDir := AddBackslash(AppDir) + 'textures';
  Stage := AddBackslash(AppDir) + '.texture-stage-' + IntToStr(TextureSetupProcessId);
  Backup := PackDir + '.previous-' + IntToStr(TextureSetupProcessId);
  if DirExists(Stage) or FileExists(Stage) or DirExists(Backup) or FileExists(Backup) then
    RaiseException('A texture staging or backup folder already exists.');
  if not CreateDir(Stage) then RaiseException('Cannot create the texture staging folder.');
  OldMoved := False;
  TextureTransferCancelled := False;
  TextureExtractActive := True;
  TextureTransferActive := True;
  TextureLastUiTick := 0;
  TextureProgress.SetText('Extracting hi-res textures...', 'Preparing the archive');
  TextureProgress.SetProgress(0, 0);
  TextureProgress.Show;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.CancelButton.Visible := True;
  try
    try
      for I := 1 to {#TexturePartCount} do
        ExtractArchive(TexturePartPaths[I], Stage, '', True, @TextureExtractionProgress);
      if TextureTransferCancelled or not TextureInstalledPackCurrent(Stage) then
        RaiseException('The staged texture pack is incomplete.');
      KnownPack := TextureKnownPack(PackDir);
      if DirExists(PackDir) then begin
        if not RenameFile(PackDir, Backup) then RaiseException('Cannot move the old texture folder.');
        OldMoved := True;
      end;
      if not RenameFile(Stage + '\textures', PackDir) then begin
        if OldMoved and not RenameFile(Backup, PackDir) then
          RaiseException('Cannot restore the previous texture folder. It is saved at ' + Backup);
        RaiseException('Cannot move the verified texture pack into the game folder.');
      end;
      TexturePackInstalled := True;
      if OldMoved then begin
        if KnownPack and ((GetFileAttributes(Backup) and $400) = 0) then begin
          if not DelTree(Backup, True, True, True) then Log('Old texture backup remains at ' + Backup);
        end else Log('Previous texture folder preserved at ' + Backup);
      end;
      Log('{#TextureDisplayVersion} texture pack extraction completed.');
    except
      if TextureTransferCancelled then
        RaiseException('Texture extraction stopped. The verified downloads are kept for the next run.')
      else RaiseException('Texture installation could not finish. ' + GetExceptionMessage);
    end;
  finally
    DelTree(Stage, True, True, True);
    TextureTransferActive := False;
    TextureExtractActive := False;
    TextureProgress.Hide;
  end;
end;
