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
AppName=OZ Image Viewer
AppVersion={#OZ_VERSION}
AppVerName=OZ Image Viewer {#OZ_VERSION}
AppPublisher=OZ Image Viewer
AppPublisherURL=https://github.com/underoatz-netizen
DefaultDirName={localappdata}\Programs\OZ Image Viewer
DisableProgramGroupPage=yes
DefaultGroupName=OZ Image Viewer
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
UninstallDisplayName=OZ Image Viewer {#OZ_VERSION}
UninstallDisplayIcon={app}\{#OZ_EXE_NAME}
DisableDirPage=no

[Tasks]
Name: "associate"; Description: "Register OZ Image Viewer as an available image handler. Opens Windows Default Apps afterward. Your defaults are never changed automatically."; GroupDescription: "Integration (optional):"; Flags: unchecked
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Icons]
Name: "{userprograms}\OZ Image Viewer"; Filename: "{app}\{#OZ_EXE_NAME}"; WorkingDir: "{app}"; IconFilename: "{app}\{#OZ_EXE_NAME}"; Comment: "OZ Image Viewer"
Name: "{userdesktop}\OZ Image Viewer"; Filename: "{app}\{#OZ_EXE_NAME}"; WorkingDir: "{app}"; IconFilename: "{app}\{#OZ_EXE_NAME}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#OZ_EXE_NAME}"; Description: "Launch OZ Image Viewer"; Flags: nowait postinstall skipifsilent

[Code]
const
  OZ_APP_DISPLAY_NAME = 'OZ Image Viewer';
  // Application-specific ProgID under HKCU\Software\Classes. Never a generic
  // or another application's ProgID.
  OZ_PROGID = 'OZImageViewer.Image';
  OZ_PROGID_SUBKEY = 'Software\Classes\OZImageViewer.Image';
  OZ_COMMAND_SUBKEY = 'Software\Classes\OZImageViewer.Image\shell\open\command';
  // Application-specific ownership marker and capabilities namespace.
  OZ_OWNER_ID = '{7E3B1C9A-4D2F-4B6E-9C31-0A5F2D8B47E1}';
  OZ_APP_SUBKEY = 'Software\OZImageViewer';
  OZ_MARKER_SUBKEY = 'Software\OZImageViewer\Install';
  OZ_CAPABILITIES_SUBKEY = 'Software\OZImageViewer\Capabilities';
  OZ_FILEASSOC_SUBKEY = 'Software\OZImageViewer\Capabilities\FileAssociations';
  OZ_REGISTERED_APPS = 'Software\RegisteredApplications';
  OZ_REGISTERED_NAME = 'OZ Image Viewer';
  OZ_TYPE_NAME = 'OZ Image Viewer Image';

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

// Fail-closed collision preflight. Returns '' when registration can proceed and
// a human-readable reason otherwise. Runs before any file is copied.
function PreflightRegistration(): String;
var
  Value, Ext: String;
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
            and (Value = OZ_CAPABILITIES_SUBKEY)) then
    begin
      Result := 'another program is already registered under the name "' + OZ_REGISTERED_NAME + '"';
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
end;

// Register OZ as an available handler. Never writes defaults/UserChoice, never
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

  RegWriteStringValue(HKCU, OZ_MARKER_SUBKEY, 'AppId', OZ_OWNER_ID);
  RegWriteStringValue(HKCU, OZ_MARKER_SUBKEY, 'ProgID', OZ_PROGID);
  RegWriteStringValue(HKCU, OZ_MARKER_SUBKEY, 'Capabilities', OZ_CAPABILITIES_SUBKEY);
  RegWriteStringValue(HKCU, OZ_MARKER_SUBKEY, 'Version', '{#OZ_VERSION}');
end;

// Remove only values/keys that still match exactly what this installer wrote.
// Foreign or user-modified data is preserved; Windows defaults are never
// inferred, changed, or restored.
procedure UnregisterApplication();
var
  Value, Ext: String;
  Names: TArrayOfString;
  I: Integer;
begin
  if not IsOwned() then
    Exit;

  if RegValueEquals(OZ_REGISTERED_APPS, OZ_REGISTERED_NAME, OZ_CAPABILITIES_SUBKEY) then
    RegDeleteValue(HKCU, OZ_REGISTERED_APPS, OZ_REGISTERED_NAME);

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

  if RegValueEquals(OZ_CAPABILITIES_SUBKEY, 'ApplicationName', OZ_APP_DISPLAY_NAME) then
    RegDeleteValue(HKCU, OZ_CAPABILITIES_SUBKEY, 'ApplicationName');
  if RegValueEquals(OZ_CAPABILITIES_SUBKEY, 'ApplicationDescription',
       'View and edit images with ' + OZ_APP_DISPLAY_NAME) then
    RegDeleteValue(HKCU, OZ_CAPABILITIES_SUBKEY, 'ApplicationDescription');
  RegDeleteKeyIfEmpty(HKCU, OZ_CAPABILITIES_SUBKEY);

  if RegValueEquals(OZ_COMMAND_SUBKEY, '', QuotedCommand()) then
    RegDeleteValue(HKCU, OZ_COMMAND_SUBKEY, '');
  RegDeleteKeyIfEmpty(HKCU, OZ_COMMAND_SUBKEY);
  RegDeleteKeyIfEmpty(HKCU, OZ_PROGID_SUBKEY + '\shell\open');
  RegDeleteKeyIfEmpty(HKCU, OZ_PROGID_SUBKEY + '\shell');
  if RegValueEquals(OZ_PROGID_SUBKEY, 'DefaultIcon', DefaultIconValue()) then
    RegDeleteValue(HKCU, OZ_PROGID_SUBKEY, 'DefaultIcon');
  if RegValueEquals(OZ_PROGID_SUBKEY, 'FriendlyTypeName', OZ_TYPE_NAME) then
    RegDeleteValue(HKCU, OZ_PROGID_SUBKEY, 'FriendlyTypeName');
  if RegValueEquals(OZ_PROGID_SUBKEY, '', OZ_TYPE_NAME) then
    RegDeleteValue(HKCU, OZ_PROGID_SUBKEY, '');
  RegDeleteKeyIfEmpty(HKCU, OZ_PROGID_SUBKEY);

  RegDeleteValue(HKCU, OZ_MARKER_SUBKEY, 'AppId');
  RegDeleteValue(HKCU, OZ_MARKER_SUBKEY, 'ProgID');
  RegDeleteValue(HKCU, OZ_MARKER_SUBKEY, 'Capabilities');
  RegDeleteValue(HKCU, OZ_MARKER_SUBKEY, 'Version');
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
  Result := '';
  if WizardIsTaskSelected('associate') then
  begin
    Result := PreflightRegistration();
    if Result <> '' then
      Result := 'OZ Image Viewer cannot register image handlers because ' + Result
        + '. No changes were made. Clear the optional integration task and try again.';
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('associate') then
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

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    UnregisterApplication();
end;
