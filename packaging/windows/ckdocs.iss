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

#ifndef ProductExecutable
  #define ProductExecutable Product + ".exe"
#endif
#ifndef InstallerStateKey
  #define InstallerStateKey "Software\ckgit\" + Product + "\Installer"
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
#if Product != "ckgit"
UninstallDisplayIcon={app}\{#ProductExecutable}
#endif
CloseApplications=yes
RestartApplications=no
ChangesEnvironment=yes
LicenseFile=..\..\LICENSE

#if Product == "ckgit"
[Types]
Name: full; Description: "ckgit and ckdocs"
Name: cli; Description: "ckgit only"
Name: docs; Description: "ckdocs only"
Name: custom; Description: "Custom selection"; Flags: iscustom

[Components]
Name: ckgit; Description: "ckgit command-line client"; Types: full cli
Name: ckdocs; Description: "ckdocs documentation generator"; Types: full docs
#endif

[Files]
#if Product == "ckgit"
Source: "{#BinaryDir}\ckgit.exe"; DestDir: "{app}"; Components: ckgit; Flags: ignoreversion
Source: "{#BinaryDir}\ckdocs.exe"; DestDir: "{app}"; Components: ckdocs; Flags: ignoreversion
#endif
#if Product == "ck-git-runner"
Source: "{#BinaryDir}\ck-ci-runnerd.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinaryDir}\ckgit-job-launcher.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "runner.ini.example"; DestDir: "{app}"; Flags: ignoreversion
#elif Product == "ckdocs"
Source: "{#BinaryDir}\ckdocs.exe"; DestDir: "{app}"; Flags: ignoreversion
#endif
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: addtopath; Description: "Add {#Product} to PATH for this installation scope"

[Code]
function DependencyOnPath(Name: String): String;
var
  Paths, Directory, Candidate: String;
  Separator: Integer;
begin
  Result := '';
  Paths := GetEnv('PATH');
  while Paths <> '' do begin
    Separator := Pos(';', Paths);
    if Separator = 0 then begin Directory := Paths; Paths := ''; end
    else begin Directory := Copy(Paths, 1, Separator - 1); Delete(Paths, 1, Separator); end;
    Directory := Trim(Directory);
    if (Length(Directory) >= 2) and (Directory[1] = '"') and
       (Directory[Length(Directory)] = '"') then
      Directory := Copy(Directory, 2, Length(Directory) - 2);
    { Never search the installer's current directory or execute discovered files. }
    if ((Length(Directory) >= 3) and (Directory[2] = ':') and
        ((Directory[3] = '\') or (Directory[3] = '/'))) or
       ((Length(Directory) >= 2) and (Copy(Directory, 1, 2) = '\\')) then begin
      Candidate := AddBackslash(Directory) + Name;
      if FileExists(Candidate) then begin Result := Candidate; Exit; end;
    end;
  end;
end;

function DependencySummary(NewLine: String): String;
var
  Git, Ssh: String;
begin
  Git := DependencyOnPath('git.exe');
  Ssh := DependencyOnPath('ssh.exe');
  if Git = '' then Git := 'not found on PATH';
  if Ssh = '' then Ssh := 'not found on PATH';
  Result := 'Git on installer PATH: ' + Git + NewLine +
            'SSH on installer PATH: ' + Ssh + NewLine +
            'Git and SSH are needed for remote operations. Local documentation builds work without them.' + NewLine +
            'Setup only checks file presence. It does not install tools or change your tool configuration.';
#if Product == "ck-git-runner"
  Result := Result + NewLine + 'Before starting the runner, select the SSH client in runner.ini and check the configuration.';
#elif Product == "ckgit"
  Result := Result + NewLine + 'Run ckgit doctor after setup to test Git and the configured server connection.';
#else
  Result := Result + NewLine + 'Git is needed for documentation portals built from repository refs.';
#endif
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoUserInfoInfo + NewLine + MemoDirInfo + NewLine + MemoTypeInfo + NewLine +
            MemoComponentsInfo + NewLine + MemoGroupInfo + NewLine + MemoTasksInfo + NewLine + NewLine +
            DependencySummary(NewLine);
end;

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
    if RegQueryStringValue(ScopeRoot, '{#InstallerStateKey}', 'OwnedPath', Owned) and
       ((CompareText(Owned, ExpandConstant('{app}')) <> 0) or not WizardIsTaskSelected('addtopath')) then begin
      Value := WithoutPath(Value, Owned, Found);
      if not RegWriteExpandStringValue(ScopeRoot, EnvironmentKey, 'Path', Value) then
        RaiseException('Could not remove previous PATH entry');
      RegDeleteValue(ScopeRoot, '{#InstallerStateKey}', 'OwnedPath');
    end;
    Ignored := WithoutPath(Value, ExpandConstant('{app}'), Found);
    if WizardIsTaskSelected('addtopath') and not Found then begin
      if Value <> '' then Value := Value + ';';
      if not RegWriteExpandStringValue(ScopeRoot, EnvironmentKey, 'Path', Value + ExpandConstant('{app}')) then
        RaiseException('Could not update PATH');
      RegWriteStringValue(ScopeRoot, '{#InstallerStateKey}', 'OwnedPath', ExpandConstant('{app}'));
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Value, Owned: String;
  Found: Boolean;
begin
  if CurUninstallStep = usPostUninstall then begin
    if RegQueryStringValue(ScopeRoot, '{#InstallerStateKey}', 'OwnedPath', Owned) and
       (CompareText(Owned, ExpandConstant('{app}')) = 0) then begin
      if RegQueryStringValue(ScopeRoot, EnvironmentKey, 'Path', Value) then
        RegWriteExpandStringValue(ScopeRoot, EnvironmentKey, 'Path', WithoutPath(Value, Owned, Found));
      RegDeleteValue(ScopeRoot, '{#InstallerStateKey}', 'OwnedPath');
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  Log(DependencySummary(#13#10));
  if Pos(';', ExpandConstant('{app}')) <> 0 then
    Result := 'Choose an installation path without a semicolon.';
#if Product == "ckgit"
  if (Result = '') and not WizardIsComponentSelected('ckgit') and
     not WizardIsComponentSelected('ckdocs') then
    Result := 'Select at least one component: ckgit or ckdocs.';
#endif
end;
