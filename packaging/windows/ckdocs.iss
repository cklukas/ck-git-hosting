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
Source: "register-service.ps1"; DestDir: "{app}"; Flags: ignoreversion
#elif Product == "ckdocs"
Source: "{#BinaryDir}\ckdocs.exe"; DestDir: "{app}"; Flags: ignoreversion
#endif
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\third_party\libarchive\COPYING"; DestDir: "{app}\licenses\libarchive"; Flags: ignoreversion
Source: "..\..\third_party\libarchive\SOURCE-NOTICES.txt"; DestDir: "{app}\licenses\libarchive"; Flags: ignoreversion

[Tasks]
Name: addtopath; Description: "Add {#Product} to PATH for this installation scope"

[Code]
#if Product == "ck-git-runner"
var
  StoppedRunnerServices: TArrayOfString;
  RunnerRestartFailed: Boolean;

function SafeServiceName(const Name: String): Boolean;
var I: Integer;
begin
  Result := Length(Name) > 0;
  for I := 1 to Length(Name) do
    if not (((Name[I] >= 'a') and (Name[I] <= 'z')) or
            ((Name[I] >= 'A') and (Name[I] <= 'Z')) or
            ((Name[I] >= '0') and (Name[I] <= '9')) or
            (Name[I] = '-') or (Name[I] = '_')) then Result := False;
end;

function StopInstalledRunnerServices: String;
var
  Names: TArrayOfString;
  I, J, N, Code: Integer;
  Key, Image, Account, Prefix, State: String;
  Locator, Wmi, Service: Variant;
begin
  Result := '';
  if not IsAdminInstallMode then Exit;
  Key := 'SYSTEM\CurrentControlSet\Services';
  if not RegGetSubkeyNames(HKLM, Key, Names) then begin
    Result := 'Unable to inspect Windows services before upgrading the runner.'; Exit;
  end;
  Prefix := '"' + ExpandConstant('{app}\ck-ci-runnerd.exe') + '" service ';
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Wmi := Locator.ConnectServer('.', 'root\cimv2');
    for I := 0 to GetArrayLength(Names)-1 do begin
      if SafeServiceName(Names[I]) and
         RegQueryStringValue(HKLM, Key + '\' + Names[I], 'ImagePath', Image) and
         RegQueryStringValue(HKLM, Key + '\' + Names[I], 'ObjectName', Account) and
         (CompareText(Account, 'NT SERVICE\' + Names[I]) = 0) and
         (CompareText(Copy(Image, 1, Length(Prefix)), Prefix) = 0) then begin
        Service := Wmi.Get('Win32_Service.Name="' + Names[I] + '"');
        State := Service.State;
        if CompareText(State, 'Stopped') <> 0 then begin
          if CompareText(State, 'Running') <> 0 then begin
            Result := 'Runner service ' + Names[I] + ' is changing state; retry when it is stable.'; Exit;
          end;
          Code := Service.StopService();
          if Code <> 0 then begin
            Result := 'Could not stop runner service ' + Names[I] + ' (code ' + IntToStr(Code) + ').'; Exit;
          end;
          N := GetArrayLength(StoppedRunnerServices);
          SetArrayLength(StoppedRunnerServices, N + 1);
          StoppedRunnerServices[N] := Names[I];
          for J := 1 to 120 do begin
            Service := Wmi.Get('Win32_Service.Name="' + Names[I] + '"');
            if CompareText(Service.State, 'Stopped') = 0 then Break;
            Sleep(250);
          end;
          if CompareText(Service.State, 'Stopped') <> 0 then begin
            Result := 'Timed out stopping runner service ' + Names[I] + '.'; Exit;
          end;
        end;
      end;
    end;
  except
    Result := 'Unable to manage runner services: ' + GetExceptionMessage;
  end;
end;

procedure RestartRunnerServices;
var I, J, Code: Integer; Locator, Wmi, Service: Variant;
begin
  if GetArrayLength(StoppedRunnerServices) = 0 then Exit;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Wmi := Locator.ConnectServer('.', 'root\cimv2');
    for I := 0 to GetArrayLength(StoppedRunnerServices)-1 do begin
      try
      Service := Wmi.Get('Win32_Service.Name="' + StoppedRunnerServices[I] + '"');
      Code := Service.StartService();
      if Code <> 0 then begin
        RunnerRestartFailed := True;
        Log('Could not restart runner service ' + StoppedRunnerServices[I] + ': ' + IntToStr(Code));
      end else begin
        for J := 1 to 120 do begin
          Service := Wmi.Get('Win32_Service.Name="' + StoppedRunnerServices[I] + '"');
          if CompareText(Service.State, 'Running') = 0 then Break;
          Sleep(250);
        end;
        if CompareText(Service.State, 'Running') <> 0 then begin
          RunnerRestartFailed := True;
          Log('Runner service did not reach Running: ' + StoppedRunnerServices[I]);
        end;
      end;
      except
        RunnerRestartFailed := True;
        Log('Could not restart runner service ' + StoppedRunnerServices[I] + ': ' + GetExceptionMessage);
      end;
    end;
  except
    RunnerRestartFailed := True;
    Log('Could not restart runner services: ' + GetExceptionMessage);
  end;
  SetArrayLength(StoppedRunnerServices, 0);
end;

procedure DeinitializeSetup;
begin
  RestartRunnerServices;
end;

function GetCustomSetupExitCode: Integer;
begin
  if RunnerRestartFailed then Result := 20 else Result := 0;
end;
#endif

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
#if Product == "ck-git-runner"
    RestartRunnerServices;
#endif
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
#if Product == "ck-git-runner"
  if Result = '' then Result := StopInstalledRunnerServices;
#endif
#if Product == "ckgit"
  if (Result = '') and not WizardIsComponentSelected('ckgit') and
     not WizardIsComponentSelected('ckdocs') then
    Result := 'Select at least one component: ckgit or ckdocs.';
#endif
end;
