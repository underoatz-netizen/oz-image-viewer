; ===========================================================================
; OZ Image Viewer - unsigned per-user x64 installer (Stage 9.10)
; ===========================================================================
;
; This script is compiled ONLY through packaging\build-installer.ps1, which
; first validates the staged Stage 9.9 portable payload (manifest schema,
; version, platform, entrypoint, extension tokens, exact file hashes and the
; expected payload ZIP hash) and then writes the generated include
; "ozviewer-installer-generated.iss" next to a copy of this file. The include
; supplies:
;
;   #define OZ_VERSION            package version (validated manifest)
;   #define OZ_EXE_NAME           entrypoint (validated manifest)
;   #define OZ_EXT_LIST           space-separated supported extensions
;   #define OZ_EXT_COUNT          number of supported extensions
;   #define OZ_OUTPUT_DIR         installer output directory (forward slashes)
;   #define OZ_OUTPUT_BASENAME    installer output base name
;   #define OZ_SETUP_ICON         repository icon path
;   [Files]                   exactly the validated payload files
;
; Build contract (do not bypass):
;   * per-user only: PrivilegesRequired=lowest, {localappdata}\Programs\...
;   * x64 only: ArchitecturesAllowed=x64compatible
;   * the installer never sets a default handler, never writes UserChoice,
;     OpenWithProgids, extension default values, or any HKLM/other-app key.
;   * association support is opt-in (unchecked task) and only registers OZ as
;     an available handler; Windows Default Apps is opened after completion so
;     the user decides. No default is changed automatically.
;   * all registry work happens in the bounded [Code] section below, which
;     refuses to run when it cannot prove ownership of the values it would
;     touch, and which removes on uninstall only values that still match what
;     this installer wrote.
;   * the installer executable is unsigned.
;
; See docs/stage9-installer.md.
; ===========================================================================

#include "ozviewer-installer-generated.iss"

[Setup]
AppId={{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}
AppName=PikaView
AppVersion={#OZ_VERSION}
AppVerName=PikaView {#OZ_VERSION}
AppPublisher=OZ Lab
AppPublisherURL=https://underoatz-netizen.github.io/software/oz-image-viewer/
AppCopyright=Copyright (c) 2026 OZ Lab
LicenseFile=LICENSE
DefaultDirName={localappdata}\Programs\OZ Image Viewer
DisableProgramGroupPage=yes
DefaultGroupName=PikaView
AllowNoIcons=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OZ_OUTPUT_DIR}
OutputBaseFilename={#OZ_OUTPUT_BASENAME}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile={#OZ_SETUP_ICON}
UninstallDisplayName=PikaView {#OZ_VERSION}
UninstallDisplayIcon={app}\{#OZ_EXE_NAME}
DisableDirPage=no

[Tasks]
Name: "associate"; Description: "Register PikaView as an available image handler. Opens Windows Default Apps afterward. Your defaults are never changed automatically."; GroupDescription: "Integration (optional):"; Flags: unchecked
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Icons]
Name: "{userprograms}\PikaView"; Filename: "{app}\{#OZ_EXE_NAME}"; WorkingDir: "{app}"; IconFilename: "{app}\{#OZ_EXE_NAME}"; Comment: "PikaView"; Flags: uninsneveruninstall
Name: "{userdesktop}\PikaView"; Filename: "{app}\{#OZ_EXE_NAME}"; WorkingDir: "{app}"; IconFilename: "{app}\{#OZ_EXE_NAME}"; Tasks: desktopicon; Flags: uninsneveruninstall

[Run]
Filename: "{app}\{#OZ_EXE_NAME}"; Description: "Launch PikaView"; Flags: nowait postinstall skipifsilent

[Code]
const
  OZ_APP_DISPLAY_NAME = 'PikaView';
  OZ_LEGACY_APP_DISPLAY_NAME = 'OZ Image Viewer';
  // Application-specific ProgID under HKCU\Software\Classes. Never a generic
  // or another application's ProgID.
  OZ_PROGID = 'OZImageViewer.Image';
  OZ_PROGID_SUBKEY = 'Software\Classes\OZImageViewer.Image';
  OZ_COMMAND_SUBKEY = 'Software\Classes\OZImageViewer.Image\shell\open\command';
  // Application-specific ownership marker and capabilities namespace.
  OZ_OWNER_ID = '{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}';
  OZ_UNINSTALL_SUBKEY = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}_is1';
  OZ_APP_SUBKEY = 'Software\OZImageViewer';
  OZ_MARKER_SUBKEY = 'Software\OZImageViewer\Install';
  OZ_CAPABILITIES_SUBKEY = 'Software\OZImageViewer\Capabilities';
  OZ_FILEASSOC_SUBKEY = 'Software\OZImageViewer\Capabilities\FileAssociations';
  OZ_REGISTERED_APPS = 'Software\RegisteredApplications';
  OZ_REGISTERED_NAME = 'PikaView';
  OZ_LEGACY_REGISTERED_NAME = 'OZ Image Viewer';
  OZ_TYPE_NAME = 'PikaView Image';
  OZ_LEGACY_TYPE_NAME = 'OZ Image Viewer Image';

function RegValueEquals(const SubKey, ValueName, Expected: String): Boolean;
var
  Value: String;
begin
  Result := RegQueryStringValue(HKCU, SubKey, ValueName, Value)
    and (Value = Expected);
end;

// True only when this specific application installed the registration. Without
// the marker we refuse to touch anything.
function IsOwned(): Boolean;
var
  Value: String;
begin
  Result := RegKeyExists(HKCU, OZ_MARKER_SUBKEY)
    and RegQueryStringValue(HKCU, OZ_MARKER_SUBKEY, 'AppId', Value)
    and (Value = OZ_OWNER_ID);
end;

function SplitExtensions(): TArrayOfString;
var
  Rest, Item: String;
  P: Integer;
begin
  SetArrayLength(Result, 0);
  Rest := Trim('{#OZ_EXT_LIST}');
  while Length(Rest) > 0 do
  begin
    P := Pos(' ', Rest);
    if P = 0 then
    begin
      Item := Rest;
      Rest := '';
    end
    else
    begin
      Item := Copy(Rest, 1, P - 1);
      Rest := Trim(Copy(Rest, P + 1, Length(Rest)));
    end;
    if Length(Item) > 0 then
    begin
      SetArrayLength(Result, GetArrayLength(Result) + 1);
      Result[GetArrayLength(Result) - 1] := Item;
    end;
  end;
end;

function QuotedCommand(): String;
begin
  Result := '"' + ExpandConstant('{app}') + '\' + '{#OZ_EXE_NAME}' + '" "%1"';
end;

function DefaultIconValue(): String;
begin
  Result := '"' + ExpandConstant('{app}') + '\' + '{#OZ_EXE_NAME}' + '",0';
end;

function IsValueAbsentOrOwned1(const SubKey, ValueName, Expected: String): Boolean;
begin
  Result := (not RegValueExists(HKCU, SubKey, ValueName))
    or RegValueEquals(SubKey, ValueName, Expected);
end;

function IsValueAbsentOrOwned2(const SubKey, ValueName, CurrentVal, LegacyVal: String): Boolean;
begin
  Result := (not RegValueExists(HKCU, SubKey, ValueName))
    or RegValueEquals(SubKey, ValueName, CurrentVal)
    or RegValueEquals(SubKey, ValueName, LegacyVal);
end;

// Recognized install-marker versions for an already owned registration. This is
// deliberately not an ownership check: IsOwned() (AppId) still gates every
// mutation. It only lets supported prior versions pass preflight and be removed
// by uninstall. Unknown/future/malformed versions stay fail-closed.
function IsRecognizedRegistrationVersion(const Value: String): Boolean;
begin
  Result := (CompareText(Value, '{#OZ_VERSION}') = 0)
    or (CompareText(Value, '0.1.0') = 0)
    or (CompareText(Value, '0.1.1') = 0);
end;

// Fail-closed collision preflight. Returns '' when registration can proceed and
// a human-readable reason otherwise. Runs before any file is copied.
function PreflightRegistration(): String;
var
  Value, Ext, VersionValue: String;
  Names: TArrayOfString;
  I: Integer;
begin
  Result := '';

  if RegKeyExists(HKCU, OZ_APP_SUBKEY) and (not IsOwned()) then
  begin
    Result := 'the registry key "' + OZ_APP_SUBKEY + '" already exists but is not owned by ' + OZ_APP_DISPLAY_NAME;
    Exit;
  end;

  if RegKeyExists(HKCU, OZ_PROGID_SUBKEY) and (not IsOwned()) then
  begin
    Result := 'the image handler ProgID "' + OZ_PROGID + '" is already registered by another program';
    Exit;
  end;

  if RegValueExists(HKCU, OZ_REGISTERED_APPS, OZ_REGISTERED_NAME) then
  begin
    if not (RegQueryStringValue(HKCU, OZ_REGISTERED_APPS, OZ_REGISTERED_NAME, Value)
            and (Value = OZ_CAPABILITIES_SUBKEY) and IsOwned()) then
    begin
      Result := 'another program is already registered under the name "' + OZ_REGISTERED_NAME + '"';
      Exit;
    end;
  end;

  // If the legacy registered application value exists, verify it points to our capabilities subkey.
  // If it points to something else and is unowned, abort before mutation.
  if RegValueExists(HKCU, OZ_REGISTERED_APPS, OZ_LEGACY_REGISTERED_NAME) then
  begin
    if not (RegQueryStringValue(HKCU, OZ_REGISTERED_APPS, OZ_LEGACY_REGISTERED_NAME, Value)
            and (Value = OZ_CAPABILITIES_SUBKEY) and IsOwned()) then
    begin
      Result := 'a foreign registration exists under the legacy name "' + OZ_LEGACY_REGISTERED_NAME + '"';
      Exit;
    end;
  end;

  if RegKeyExists(HKCU, OZ_FILEASSOC_SUBKEY) and RegGetValueNames(HKCU, OZ_FILEASSOC_SUBKEY, Names) then
  begin
    for I := 0 to GetArrayLength(Names) - 1 do
    begin
      Ext := Names[I];
      if Copy(Ext, 1, 1) = '.' then
      begin
        if not (RegQueryStringValue(HKCU, OZ_FILEASSOC_SUBKEY, Ext, Value) and (Value = OZ_PROGID)) then
        begin
          Result := 'the image extension "' + Ext + '" is registered to a different handler ("' + Value + '")';
          Exit;
        end;
      end;
    end;
  end;

  if not IsValueAbsentOrOwned2(OZ_PROGID_SUBKEY, '', OZ_TYPE_NAME, OZ_LEGACY_TYPE_NAME) then
  begin
    Result := 'the ProgID "' + OZ_PROGID + '" default value has been customized or belongs to another program';
    Exit;
  end;

  if not IsValueAbsentOrOwned2(OZ_PROGID_SUBKEY, 'FriendlyTypeName', OZ_TYPE_NAME, OZ_LEGACY_TYPE_NAME) then
  begin
    Result := 'the ProgID "' + OZ_PROGID + '" FriendlyTypeName has been customized or belongs to another program';
    Exit;
  end;

  if not IsValueAbsentOrOwned1(OZ_PROGID_SUBKEY, 'DefaultIcon', DefaultIconValue()) then
  begin
    Result := 'the ProgID "' + OZ_PROGID + '" DefaultIcon does not match this install path';
    Exit;
  end;

  if not IsValueAbsentOrOwned1(OZ_COMMAND_SUBKEY, '', QuotedCommand()) then
  begin
    Result := 'the ProgID "' + OZ_PROGID + '" open command does not match this install path';
    Exit;
  end;

  if not IsValueAbsentOrOwned2(OZ_CAPABILITIES_SUBKEY, 'ApplicationName', OZ_APP_DISPLAY_NAME, OZ_LEGACY_APP_DISPLAY_NAME) then
  begin
    Result := 'the capabilities ApplicationName has been customized or belongs to another program';
    Exit;
  end;

  if not IsValueAbsentOrOwned2(OZ_CAPABILITIES_SUBKEY, 'ApplicationDescription',
       'View and edit images with ' + OZ_APP_DISPLAY_NAME,
       'View and edit images with ' + OZ_LEGACY_APP_DISPLAY_NAME) then
  begin
    Result := 'the capabilities ApplicationDescription has been customized or belongs to another program';
    Exit;
  end;

  if not IsValueAbsentOrOwned1(OZ_MARKER_SUBKEY, 'ProgID', OZ_PROGID) then
  begin
    Result := 'the install marker ProgID has been modified';
    Exit;
  end;

  if not IsValueAbsentOrOwned1(OZ_MARKER_SUBKEY, 'Capabilities', OZ_CAPABILITIES_SUBKEY) then
  begin
    Result := 'the install marker Capabilities has been modified';
    Exit;
  end;

  if RegValueExists(HKCU, OZ_MARKER_SUBKEY, 'Version') then
  begin
    if not (RegQueryStringValue(HKCU, OZ_MARKER_SUBKEY, 'Version', VersionValue)
            and IsRecognizedRegistrationVersion(VersionValue)) then
    begin
      Result := 'the install marker Version has been modified';
      Exit;
    end;
  end;
end;

// Register PikaView as an available handler. Never writes defaults/UserChoice, never
// touches HKLM or another application's keys. Idempotent; reconciles only
// extension values that still point at our own ProgID.
procedure RegisterApplication();
var
  Value, Ext: String;
  Extensions, Names: TArrayOfString;
  I, J: Integer;
  Found: Boolean;
begin
  RegWriteStringValue(HKCU, OZ_PROGID_SUBKEY, '', OZ_TYPE_NAME);
  RegWriteStringValue(HKCU, OZ_PROGID_SUBKEY, 'FriendlyTypeName', OZ_TYPE_NAME);
  RegWriteStringValue(HKCU, OZ_PROGID_SUBKEY, 'DefaultIcon', DefaultIconValue());
  RegWriteStringValue(HKCU, OZ_COMMAND_SUBKEY, '', QuotedCommand());

  RegWriteStringValue(HKCU, OZ_CAPABILITIES_SUBKEY, 'ApplicationName', OZ_APP_DISPLAY_NAME);
  RegWriteStringValue(HKCU, OZ_CAPABILITIES_SUBKEY, 'ApplicationDescription',
    'View and edit images with ' + OZ_APP_DISPLAY_NAME);

  Extensions := SplitExtensions();
  for I := 0 to GetArrayLength(Extensions) - 1 do
    RegWriteStringValue(HKCU, OZ_FILEASSOC_SUBKEY, Extensions[I], OZ_PROGID);

  // Reap only our own stale extension values (same ProgID, no longer supported).
  // Anything pointing elsewhere is preserved and was already rejected above.
  if RegGetValueNames(HKCU, OZ_FILEASSOC_SUBKEY, Names) then
  begin
    for I := 0 to GetArrayLength(Names) - 1 do
    begin
      Ext := Names[I];
      if Copy(Ext, 1, 1) = '.' then
      begin
        Found := False;
        for J := 0 to GetArrayLength(Extensions) - 1 do
          if Extensions[J] = Ext then Found := True;
        if (not Found)
           and RegQueryStringValue(HKCU, OZ_FILEASSOC_SUBKEY, Ext, Value)
           and (Value = OZ_PROGID) then
          RegDeleteValue(HKCU, OZ_FILEASSOC_SUBKEY, Ext);
      end;
    end;
  end;

  RegWriteStringValue(HKCU, OZ_REGISTERED_APPS, OZ_REGISTERED_NAME, OZ_CAPABILITIES_SUBKEY);
  // Migrate legacy RegisteredApplications value if it was ours
  if RegValueEquals(OZ_REGISTERED_APPS, OZ_LEGACY_REGISTERED_NAME, OZ_CAPABILITIES_SUBKEY) and IsOwned() then
    RegDeleteValue(HKCU, OZ_REGISTERED_APPS, OZ_LEGACY_REGISTERED_NAME);

  RegWriteStringValue(HKCU, OZ_MARKER_SUBKEY, 'AppId', OZ_OWNER_ID);
  RegWriteStringValue(HKCU, OZ_MARKER_SUBKEY, 'ProgID', OZ_PROGID);
  RegWriteStringValue(HKCU, OZ_MARKER_SUBKEY, 'Capabilities', OZ_CAPABILITIES_SUBKEY);
  RegWriteStringValue(HKCU, OZ_MARKER_SUBKEY, 'Version', '{#OZ_VERSION}');
end;

// Returns True when an owned v0.1.0 registration exists and still has legacy
// values that need migration to PikaView.
function NeedsLegacyMigration(): Boolean;
begin
  Result := False;
  if not IsOwned() then
    Exit;

  if RegValueEquals(OZ_REGISTERED_APPS, OZ_LEGACY_REGISTERED_NAME, OZ_CAPABILITIES_SUBKEY) or
     RegValueEquals(OZ_CAPABILITIES_SUBKEY, 'ApplicationName', OZ_LEGACY_APP_DISPLAY_NAME) or
     RegValueEquals(OZ_CAPABILITIES_SUBKEY, 'ApplicationDescription', 'View and edit images with ' + OZ_LEGACY_APP_DISPLAY_NAME) or
     RegValueEquals(OZ_PROGID_SUBKEY, '', OZ_LEGACY_TYPE_NAME) or
     RegValueEquals(OZ_PROGID_SUBKEY, 'FriendlyTypeName', OZ_LEGACY_TYPE_NAME) then
    Result := True;
end;

function IsInstalledByAppId(): Boolean;
var
  InstalledDir: String;
begin
  Result := False;
  if not RegKeyExists(HKCU, OZ_UNINSTALL_SUBKEY) then
    Exit;

  InstalledDir := '';
  if not RegQueryStringValue(HKCU, OZ_UNINSTALL_SUBKEY, 'InstallLocation', InstalledDir) then
  begin
    if not RegQueryStringValue(HKCU, OZ_UNINSTALL_SUBKEY, 'Inno Setup: App Path', InstalledDir) then
      Exit;
  end;

  if InstalledDir = '' then
    Exit;

  Result := CompareText(RemoveBackslashUnlessRoot(InstalledDir),
                        RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0;
end;

function IsOwnedShortcut(const LinkPath, ExpectedDesc: String): Boolean;
var
  Shell, Shortcut: Variant;
  Target, Args, WorkDir, IconLoc, Desc: String;
  ExpectedTarget, ExpectedWorkDir, ExpectedIcon: String;
begin
  Result := False;
  if not IsInstalledByAppId() then
    Exit;

  ExpectedTarget := ExpandConstant('{app}\{#OZ_EXE_NAME}');
  ExpectedWorkDir := ExpandConstant('{app}');
  ExpectedIcon := ExpectedTarget + ',0';

  try
    Shell := CreateOleObject('WScript.Shell');
    Shortcut := Shell.CreateShortcut(LinkPath);
    Target := Shortcut.TargetPath;
    Args := Shortcut.Arguments;
    WorkDir := Shortcut.WorkingDirectory;
    IconLoc := Shortcut.IconLocation;
    Desc := Shortcut.Description;

    Result := (CompareText(Target, ExpectedTarget) = 0)
      and (Args = '')
      and (CompareText(WorkDir, ExpectedWorkDir) = 0)
      and ((CompareText(IconLoc, ExpectedIcon) = 0) or (CompareText(IconLoc, ExpectedTarget) = 0))
      and (Desc = ExpectedDesc);
  except
    Result := False;
  end;
end;

function PreflightShortcuts(): String;
begin
  Result := '';
  if FileExists(ExpandConstant('{userprograms}\PikaView.lnk')) and
     (not IsOwnedShortcut(ExpandConstant('{userprograms}\PikaView.lnk'), OZ_APP_DISPLAY_NAME)) then
  begin
    Result := 'a Start Menu shortcut "PikaView.lnk" already exists and is customized or not owned by ' + OZ_APP_DISPLAY_NAME;
    Exit;
  end;
  if WizardIsTaskSelected('desktopicon') and
     FileExists(ExpandConstant('{userdesktop}\PikaView.lnk')) and
     (not IsOwnedShortcut(ExpandConstant('{userdesktop}\PikaView.lnk'), '')) then
  begin
    Result := 'a Desktop shortcut "PikaView.lnk" already exists and is customized or not owned by ' + OZ_APP_DISPLAY_NAME;
    Exit;
  end;
end;

// Remove only values/keys that still match exactly what this installer wrote.
// Recognizes both current (PikaView) and legacy (OZ Image Viewer) owned values.
// Foreign or user-modified data is preserved; Windows defaults are never
// inferred, changed, or restored.
procedure UnregisterApplication();
var
  Value, Ext, VersionValue: String;
  Names: TArrayOfString;
  I: Integer;
begin
  if not IsOwned() then
    Exit;

  if RegValueEquals(OZ_REGISTERED_APPS, OZ_REGISTERED_NAME, OZ_CAPABILITIES_SUBKEY) then
    RegDeleteValue(HKCU, OZ_REGISTERED_APPS, OZ_REGISTERED_NAME);
  if RegValueEquals(OZ_REGISTERED_APPS, OZ_LEGACY_REGISTERED_NAME, OZ_CAPABILITIES_SUBKEY) then
    RegDeleteValue(HKCU, OZ_REGISTERED_APPS, OZ_LEGACY_REGISTERED_NAME);

  if RegGetValueNames(HKCU, OZ_FILEASSOC_SUBKEY, Names) then
  begin
    for I := 0 to GetArrayLength(Names) - 1 do
    begin
      Ext := Names[I];
      if RegQueryStringValue(HKCU, OZ_FILEASSOC_SUBKEY, Ext, Value) and (Value = OZ_PROGID) then
        RegDeleteValue(HKCU, OZ_FILEASSOC_SUBKEY, Ext);
    end;
  end;
  RegDeleteKeyIfEmpty(HKCU, OZ_FILEASSOC_SUBKEY);

  if RegValueEquals(OZ_CAPABILITIES_SUBKEY, 'ApplicationName', OZ_APP_DISPLAY_NAME) or
     RegValueEquals(OZ_CAPABILITIES_SUBKEY, 'ApplicationName', OZ_LEGACY_APP_DISPLAY_NAME) then
    RegDeleteValue(HKCU, OZ_CAPABILITIES_SUBKEY, 'ApplicationName');

  if RegValueEquals(OZ_CAPABILITIES_SUBKEY, 'ApplicationDescription',
       'View and edit images with ' + OZ_APP_DISPLAY_NAME) or
     RegValueEquals(OZ_CAPABILITIES_SUBKEY, 'ApplicationDescription',
       'View and edit images with ' + OZ_LEGACY_APP_DISPLAY_NAME) then
    RegDeleteValue(HKCU, OZ_CAPABILITIES_SUBKEY, 'ApplicationDescription');
  RegDeleteKeyIfEmpty(HKCU, OZ_CAPABILITIES_SUBKEY);

  if RegValueEquals(OZ_COMMAND_SUBKEY, '', QuotedCommand()) then
    RegDeleteValue(HKCU, OZ_COMMAND_SUBKEY, '');
  RegDeleteKeyIfEmpty(HKCU, OZ_COMMAND_SUBKEY);
  RegDeleteKeyIfEmpty(HKCU, OZ_PROGID_SUBKEY + '\shell\open');
  RegDeleteKeyIfEmpty(HKCU, OZ_PROGID_SUBKEY + '\shell');
  if RegValueEquals(OZ_PROGID_SUBKEY, 'DefaultIcon', DefaultIconValue()) then
    RegDeleteValue(HKCU, OZ_PROGID_SUBKEY, 'DefaultIcon');
  if RegValueEquals(OZ_PROGID_SUBKEY, 'FriendlyTypeName', OZ_TYPE_NAME) or
     RegValueEquals(OZ_PROGID_SUBKEY, 'FriendlyTypeName', OZ_LEGACY_TYPE_NAME) then
    RegDeleteValue(HKCU, OZ_PROGID_SUBKEY, 'FriendlyTypeName');
  if RegValueEquals(OZ_PROGID_SUBKEY, '', OZ_TYPE_NAME) or
     RegValueEquals(OZ_PROGID_SUBKEY, '', OZ_LEGACY_TYPE_NAME) then
    RegDeleteValue(HKCU, OZ_PROGID_SUBKEY, '');
  RegDeleteKeyIfEmpty(HKCU, OZ_PROGID_SUBKEY);

  if RegValueEquals(OZ_MARKER_SUBKEY, 'ProgID', OZ_PROGID) then
    RegDeleteValue(HKCU, OZ_MARKER_SUBKEY, 'ProgID');
  if RegValueEquals(OZ_MARKER_SUBKEY, 'Capabilities', OZ_CAPABILITIES_SUBKEY) then
    RegDeleteValue(HKCU, OZ_MARKER_SUBKEY, 'Capabilities');
  if RegQueryStringValue(HKCU, OZ_MARKER_SUBKEY, 'Version', VersionValue) and
     IsRecognizedRegistrationVersion(VersionValue) then
    RegDeleteValue(HKCU, OZ_MARKER_SUBKEY, 'Version');
  if RegValueEquals(OZ_MARKER_SUBKEY, 'AppId', OZ_OWNER_ID) then
    RegDeleteValue(HKCU, OZ_MARKER_SUBKEY, 'AppId');
  RegDeleteKeyIfEmpty(HKCU, OZ_MARKER_SUBKEY);
  RegDeleteKeyIfEmpty(HKCU, OZ_APP_SUBKEY);
end;

// Opt-in only, after the install has completed. Opens Windows Default Apps so
// the user can choose a default; falls back to generic Settings if the app does
// not support the defaultapps URI. Never changes a default itself.
procedure LaunchDefaultApps();
var
  ResultCode: Integer;
begin
  if not ShellExec('open', 'ms-settings:defaultapps', '', '', SW_SHOWNORMAL, ewNoWait, ResultCode) then
    ShellExec('open', 'ms-settings:', '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := PreflightShortcuts();
  if Result <> '' then
  begin
    Result := OZ_APP_DISPLAY_NAME + ' cannot create shortcuts because ' + Result
      + '. No changes were made.';
    Exit;
  end;

  if WizardIsTaskSelected('associate') then
  begin
    Result := PreflightRegistration();
    if Result <> '' then
      Result := OZ_APP_DISPLAY_NAME + ' cannot register image handlers because ' + Result
        + '. No changes were made. Clear the optional integration task and try again.';
  end
  else if NeedsLegacyMigration() then
  begin
    Result := PreflightRegistration();
    if Result <> '' then
      Result := OZ_APP_DISPLAY_NAME + ' cannot migrate the existing image handler registration because ' + Result
        + '. No changes were made. Resolve the conflicting registration and try again.';
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('associate') or NeedsLegacyMigration() then
      RegisterApplication();
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if IsUninstaller then
    Exit;
  if (CurPageID = wpFinished) and (not WizardSilent()) and WizardIsTaskSelected('associate') then
    LaunchDefaultApps();
end;

procedure RemoveOwnedShortcuts();
var
  StartMenuLink, DesktopLink: String;
begin
  StartMenuLink := ExpandConstant('{userprograms}\PikaView.lnk');
  if FileExists(StartMenuLink) and IsOwnedShortcut(StartMenuLink, OZ_APP_DISPLAY_NAME) then
    DeleteFile(StartMenuLink);

  DesktopLink := ExpandConstant('{userdesktop}\PikaView.lnk');
  if FileExists(DesktopLink) and IsOwnedShortcut(DesktopLink, '') then
    DeleteFile(DesktopLink);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RemoveOwnedShortcuts();
    UnregisterApplication();
  end;
end;
