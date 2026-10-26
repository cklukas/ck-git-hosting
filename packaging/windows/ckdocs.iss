; Copyright (c) 2026 C. Klukas. All rights reserved.
; SPDX-License-Identifier: MIT
; ISCC /DBinaryDir=... /DOutputDir=... /DReleaseVersion=0.8.2 ckdocs.iss
#ifndef BinaryDir
  #error BinaryDir must name the verified Release binary directory
#endif
#ifndef OutputDir
  #error OutputDir must be explicit
#endif
#ifndef ReleaseVersion
  #error ReleaseVersion must be explicit
#endif

#ifndef Product
  #define Product "ckdocs"
  #define ProductAppId "{{9BA75E20-ED1F-4FBE-8F01-0F052E0F9700}"
#endif

[Setup]
AppId={#ProductAppId}
AppName={#Product}
AppVersion={#ReleaseVersion}
AppPublisher=C. Klukas
DefaultDirName={autopf}\{#Product}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputDir={#OutputDir}
OutputBaseFilename={#Product}-{#ReleaseVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#Product}.exe
CloseApplications=yes
RestartApplications=no
ChangesEnvironment=yes
LicenseFile=..\..\LICENSE

[Files]
#if Product == "ckgit"
Source: "{#BinaryDir}\ckgit.exe"; DestDir: "{app}"; Flags: ignoreversion
#endif
Source: "{#BinaryDir}\ckdocs.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: addtopath; Description: "Add {#Product} to PATH for this installation scope"

[Code]
function EnvironmentKey: String;
begin
  if IsAdminInstallMode then
    Result := 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
  else
    Result := 'Environment';
end;

function ScopeRoot: Integer;
begin
  if IsAdminInstallMode then Result := HKLM else Result := HKCU;
end;

function WithoutPath(Value, Directory: String; var Found: Boolean): String;
var
  Part: String;
  Separator: Integer;
  LastPart, HasPart: Boolean;
begin
  Result := '';
  Found := False;
  HasPart := False;
  while True do begin
    Separator := Pos(';', Value);
    LastPart := Separator = 0;
    if Separator = 0 then begin Part := Value; Value := ''; end
    else begin Part := Copy(Value, 1, Separator - 1); Delete(Value, 1, Separator); end;
    if CompareText(Trim(Part), Directory) = 0 then Found := True
    else begin
      if HasPart then Result := Result + ';';
      Result := Result + Part;
      HasPart := True;
    end;
    if LastPart then Break;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Value, Ignored, Owned: String;
  Found: Boolean;
begin
  if CurStep = ssPostInstall then begin
    RegQueryStringValue(ScopeRoot, EnvironmentKey, 'Path', Value);
    if RegQueryStringValue(ScopeRoot, 'Software\ckgit\{#Product}\Installer', 'OwnedPath', Owned) and
       ((CompareText(Owned, ExpandConstant('{app}')) <> 0) or not WizardIsTaskSelected('addtopath')) then begin
      Value := WithoutPath(Value, Owned, Found);
      if not RegWriteExpandStringValue(ScopeRoot, EnvironmentKey, 'Path', Value) then
        RaiseException('Could not remove previous PATH entry');
      RegDeleteValue(ScopeRoot, 'Software\ckgit\{#Product}\Installer', 'OwnedPath');
    end;
    Ignored := WithoutPath(Value, ExpandConstant('{app}'), Found);
    if WizardIsTaskSelected('addtopath') and not Found then begin
      if Value <> '' then Value := Value + ';';
      if not RegWriteExpandStringValue(ScopeRoot, EnvironmentKey, 'Path', Value + ExpandConstant('{app}')) then
        RaiseException('Could not update PATH');
      RegWriteStringValue(ScopeRoot, 'Software\ckgit\{#Product}\Installer', 'OwnedPath', ExpandConstant('{app}'));
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Value, Owned: String;
  Found: Boolean;
begin
  if CurUninstallStep = usPostUninstall then begin
    if RegQueryStringValue(ScopeRoot, 'Software\ckgit\{#Product}\Installer', 'OwnedPath', Owned) and
       (CompareText(Owned, ExpandConstant('{app}')) = 0) then begin
      if RegQueryStringValue(ScopeRoot, EnvironmentKey, 'Path', Value) then
        RegWriteExpandStringValue(ScopeRoot, EnvironmentKey, 'Path', WithoutPath(Value, Owned, Found));
      RegDeleteValue(ScopeRoot, 'Software\ckgit\{#Product}\Installer', 'OwnedPath');
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if Pos(';', ExpandConstant('{app}')) <> 0 then
    Result := 'Choose an installation path without a semicolon.';
end;
