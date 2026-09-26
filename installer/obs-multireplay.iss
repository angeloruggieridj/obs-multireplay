; Windows installer for obs-multireplay (Inno Setup 6).
;
; It installs the tree `cmake --install` lays out -- the same one the release
; zip carries -- into the one folder OBS searches on Windows, in BOTH layouts:
;   C:\ProgramData\obs-studio\plugins\obs-multireplay\
;     obs-multireplay.dll            OBS 33+ layout (added here)
;     bin\64bit\obs-multireplay.dll  legacy layout, OBS 32 and earlier
;     data\                          shared by both
; OBS 33 loads the first and skips the second as a duplicate; OBS 32 and
; earlier only look at the second. The zip cannot carry the first one (the
; update helper inside 1.0.0 would mis-unpack it, see src/update-installer.hpp);
; an installer has no such constraint. Portable OBS is not covered: use the zip.
;
; Built by CI (the Windows job in .github/workflows/build-project.yaml):
;   ISCC /DAppVersion=1.0.1-beta2 /DAppNumVersion=1.0.1 /DPkgDir=<install tree>
;        /O<output dir> /F<output name> installer\obs-multireplay.iss

#ifndef AppVersion
  #error Pass the full plugin version: /DAppVersion=x.y.z[-betaN]
#endif
#ifndef AppNumVersion
  #error Pass the numeric version for the file properties: /DAppNumVersion=x.y.z
#endif
#ifndef PkgDir
  #error Pass the install tree holding obs-multireplay\: /DPkgDir=<dir>
#endif

[Setup]
; Never change the AppId: it is how an upgrade finds the previous install.
AppId={{75936079-F41F-48AC-86C7-169BDC21D51A}
AppName=OBS MultiReplay
AppVersion={#AppVersion}
AppVerName=OBS MultiReplay {#AppVersion}
AppPublisher=obs-multireplay contributors
AppPublisherURL=https://github.com/angeloruggieridj/obs-multireplay
AppSupportURL=https://github.com/angeloruggieridj/obs-multireplay/issues
AppUpdatesURL=https://github.com/angeloruggieridj/obs-multireplay/releases
VersionInfoVersion={#AppNumVersion}
DefaultDirName={commonappdata}\obs-studio\plugins\obs-multireplay
DisableDirPage=yes
DisableProgramGroupPage=yes
; ProgramData subfolders can belong to another account; one clear elevation
; prompt beats an install that fails halfway.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; OUTSIDE the plugin folder, and this is the difference from obs-playlist-deck:
; the in-app updater replaces the plugin folder whole, so an uninstaller kept in
; it would be deleted by the first update and leave an Apps & features entry
; that uninstalls nothing. OBS never looks in this folder.
UninstallFilesDir={commonappdata}\obs-multireplay\uninstall
UninstallDisplayName=OBS MultiReplay
; OBS is checked for explicitly below; the Restart Manager would only add a
; second, vaguer prompt about the same thing.
CloseApplications=no
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"

[CustomMessages]
english.ObsRunning=OBS Studio is running. Close it, then choose Retry: a plugin that OBS has loaded cannot be replaced.
italian.ObsRunning=OBS Studio è in esecuzione. Chiudilo, poi scegli Riprova: un plugin caricato da OBS non può essere sostituito.
english.NeedsBranchOutput=MultiReplay records through the Branch Output plugin. If it is not installed yet, MultiReplay offers to install it the first time OBS starts.
italian.NeedsBranchOutput=MultiReplay registra tramite il plugin Branch Output. Se non è ancora installato, MultiReplay si offre di installarlo al primo avvio di OBS.

[Messages]
english.FinishedLabel=Setup has installed [name] into OBS's plugin folder. Start OBS Studio and open it from Docks.
italian.FinishedLabel=[name] è stato installato nella cartella dei plugin di OBS. Avvia OBS Studio e aprilo dal menu Pannelli.

[InstallDelete]
; Upgrades replace the data folder whole, so a file a newer version dropped
; does not linger from an older one.
Type: filesandordirs; Name: "{app}\data"

[Files]
Source: "{#PkgDir}\obs-multireplay\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; The OBS 33 layout: the same binary, directly in the plugin folder.
Source: "{#PkgDir}\obs-multireplay\bin\64bit\obs-multireplay.dll"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
; Also what a zip install or the in-app updater left in the plugin folder
; (update backups included). Settings live in OBS's plugin_config folder and
; recordings in the session folder, not here, and are kept.
Type: filesandordirs; Name: "{app}"
Type: dirifempty; Name: "{commonappdata}\obs-multireplay"

[Code]
function IsObsRunning(): Boolean;
var
  Locator, Wmi, Processes: Variant;
begin
  Result := False;
  try
    { Pascal Script cannot call a method on a call's result: one step each. }
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Wmi := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Wmi.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := Processes.Count > 0;
  except
    { WMI unavailable: do not block the install on a check that cannot run. }
  end;
end;

{ Retry until OBS is closed, or give up. Silent installs get IDCANCEL, so an
  unattended run fails cleanly instead of looping. }
function WaitForObsClosed(): Boolean;
begin
  Result := True;
  while IsObsRunning() do
    if SuppressibleMsgBox(CustomMessage('ObsRunning'), mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;

function InitializeSetup(): Boolean;
begin
  Result := WaitForObsClosed();
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForObsClosed();
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  { Said once, on the page before the copy starts: the plugin is inert
    without Branch Output, and an operator who learns that from an empty
    REC button has lost a take. }
  if CurPageID = wpReady then
    WizardForm.ReadyMemo.Lines.Add(#13#10 + CustomMessage('NeedsBranchOutput'));
end;
