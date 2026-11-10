; SPDX-License-Identifier: BSD-3-Clause
; Copyright 2026 Intel Corporation
;
; The Windows installer of MTL, for Inno Setup 6. windows/scripts/package.sh
; makes the folder of the files and compiles this script:
;   iscc /DMtlVersion=<version> /DMtlNumVersion=<a.b.c.d> /DStageDir=<folder> mtl.iss
;
; Silent install (all options are optional):
;   mtl-<version>-win64-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
;     /TASKS="host,drivers,desktopicon" /SCHEDCPUS=auto /MTLUSER=<user>
;     /HWIDS="PCI\VEN_8086&DEV_1592" /EWDK=<EWDK folder or ISO>

#ifndef MtlVersion
  #error Give /DMtlVersion=<version>
#endif
#ifndef MtlNumVersion
  #error Give /DMtlNumVersion=<a.b.c.d>
#endif
#ifndef StageDir
  #error Give /DStageDir=<folder of package.sh>
#endif

[Setup]
AppId={{3601FA9F-92CD-452F-9B6C-331102B80776}
AppName=Media Transport Library
AppVersion={#MtlVersion}
AppVerName=Media Transport Library {#MtlVersion}
AppPublisher=Intel Corporation
AppPublisherURL=https://github.com/OpenVisualCloud/Media-Transport-Library
VersionInfoVersion={#MtlNumVersion}
DefaultDirName={autopf}\MTL
DefaultGroupName=MTL
DisableProgramGroupPage=yes
LicenseFile={#StageDir}\licenses\MTL-LICENSE.txt
OutputBaseFilename=mtl-{#MtlVersion}-win64-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
; pnputil /remove-device of install_drivers.ps1 needs Windows 10 2004.
MinVersion=10.0.19041
ChangesEnvironment=yes
SetupLogging=yes
WizardStyle=modern

[Tasks]
Name: host; Description: "Set up Windows for MTL: test signing, large pages, reserved CPUs, power plan (windows_setup.ps1)"
Name: drivers; Description: "Build and install the NetUIO and virt2phys drivers with the EWDK (install_drivers.ps1)"; Check: EwdkFound
Name: desktopicon; Description: "Put ""MTL test"" on the desktop: 2 video, 2 audio and 2 ancillary streams, port 0 to port 1"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; The ice PMD of DPDK reads C:\dpdk\lib\ice.pkg (DPDK patch 0001) for an E810 or
; E830 PF. Setup keeps an ice.pkg that is already there.
Source: "{#StageDir}\ddp\ice.pkg"; DestDir: "C:\dpdk\lib"; Flags: onlyifdoesntexist

; Each entry opens a PowerShell window as administrator (admin_shell.ps1).
[Icons]
Name: "{group}\MTL status"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\scripts\admin_shell.ps1"" status"; Comment: "The Windows settings and the drivers of MTL"
Name: "{group}\MTL install drivers"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\scripts\admin_shell.ps1"" drivers"; Comment: "Build and install the NetUIO and virt2phys drivers with the EWDK"
Name: "{group}\MTL test"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\scripts\admin_shell.ps1"" test"; Comment: "RxTxApp: 2 video, 2 audio and 2 ancillary streams for 30 s"
Name: "{autodesktop}\MTL test"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""{app}\scripts\admin_shell.ps1"" test"; Comment: "RxTxApp: 2 video, 2 audio and 2 ancillary streams for 30 s"; Tasks: desktopicon

[Code]
var
  HostPage: TInputQueryWizardPage;

function LogFile(Name: String): String;
begin
  Result := ExpandConstant('{commonappdata}\MTL\logs\') + Name;
end;

// A string in single quotes for PowerShell.
function PsQuote(S: String): String;
begin
  StringChangeEx(S, '''', '''''', True);
  Result := '''' + S + '''';
end;

// "a, b" -> 'a','b'
function PsList(S: String): String;
var
  Part: String;
  P: Integer;
begin
  Result := '';
  S := S + ',';
  P := Pos(',', S);
  while P > 0 do begin
    Part := Trim(Copy(S, 1, P - 1));
    if Part <> '' then begin
      if Result <> '' then Result := Result + ',';
      Result := Result + PsQuote(Part);
    end;
    Delete(S, 1, P);
    P := Pos(',', S);
  end;
end;

// Run a script of {app}\scripts. Its output goes to a log file.
function RunScript(Script, Args, LogName: String): Integer;
var
  Params: String;
begin
  ForceDirectories(ExtractFileDir(LogFile(LogName)));
  Params := '-NoProfile -ExecutionPolicy Bypass -Command "& ' +
    PsQuote(ExpandConstant('{app}\scripts\') + Script) + ' ' + Args +
    ' *> ' + PsQuote(LogFile(LogName)) + '; exit $LASTEXITCODE"';
  Log('powershell.exe ' + Params);
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Params, '',
              SW_HIDE, ewWaitUntilTerminated, Result) then
    Result := -1;
  Log(Script + ' exit code ' + IntToStr(Result));
end;

// An EWDK ISO of the Downloads folder, or ''. install_drivers.ps1 mounts it.
function EwdkIso(): String;
var
  F: TFindRec;
  Dir: String;
begin
  Result := '';
  Dir := ExpandConstant('{%USERPROFILE}\Downloads\');
  if FindFirst(Dir + '*ewdk*.iso', F) then begin
    Result := Dir + F.Name;
    FindClose(F);
  end;
end;

// The root folder of a mounted EWDK, or ''.
function EwdkRoot(): String;
var
  I: Integer;
begin
  Result := '';
  for I := Ord('C') to Ord('Z') do
    if FileExists(Chr(I) + ':\BuildEnv\SetupBuildEnv.cmd') then begin
      Result := Chr(I) + ':\';
      Exit;
    end;
end;

function EwdkFound(): Boolean;
begin
  Result := (ExpandConstant('{param:EWDK|}') <> '') or (EwdkRoot() <> '') or (EwdkIso() <> '');
end;

function SecureBootOn(): Boolean;
var
  V: Cardinal;
begin
  Result := RegQueryDWordValue(HKLM64, 'SYSTEM\CurrentControlSet\Control\SecureBoot\State',
                               'UEFISecureBootEnabled', V) and (V = 1);
end;

procedure InitializeWizard();
begin
  HostPage := CreateInputQueryPage(wpSelectTasks, 'Host settings',
    'The settings of windows_setup.ps1 and install_drivers.ps1.',
    'Scheduler CPUs: the CPUs that only the MTL scheduler threads use. auto: the last half of ' +
    'the cores, at most 8 CPUs. Or give a list, for example 8-15: not CPU 0, two CPUs or more ' +
    'for Windows, and with SMT both threads of a core. Empty: no CPU reserved.' + #13#10#13#10 +
    'NetUIO device IDs: NetUIO takes each device with one of these IDs. Empty: the IDs of the ' +
    'last install, or the iavf VF of E810 and E830. For a PF, give its ID, for example ' +
    'PCI\VEN_8086&DEV_1592 (E810-C for QSFP) or PCI\VEN_8086&DEV_12D2 (E830-CC for QSFP). ' +
    'Never give the ID of the NIC that Windows uses for its network.');
  HostPage.Add('Scheduler CPUs:', False);
  HostPage.Add('User that runs MTL (gets "Lock pages in memory"):', False);
  HostPage.Add('NetUIO device IDs, comma-separated (empty: see above):', False);
  HostPage.Values[0] := ExpandConstant('{param:SCHEDCPUS|auto}');
  HostPage.Values[1] := ExpandConstant('{param:MTLUSER|' + GetUserNameString() + '}');
  HostPage.Values[2] := ExpandConstant('{param:HWIDS|}');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = HostPage.ID) and
            not (WizardIsTaskSelected('host') or WizardIsTaskSelected('drivers'));
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  I: Integer;
  Cpus: String;
begin
  Result := True;
  if CurPageID = wpSelectTasks then begin
    if WizardIsTaskSelected('host') and SecureBootOn() then
      SuppressibleMsgBox('Secure Boot is on. The NetUIO and virt2phys drivers are test-signed, ' +
        'and Windows loads them only with Secure Boot off. Setup goes on. Then turn off Secure ' +
        'Boot in the firmware, and run windows_setup.ps1 apply again.', mbInformation, MB_OK, IDOK);
    if not EwdkFound() then
      SuppressibleMsgBox('No EWDK found (mounted, or an EWDK ISO in Downloads), so setup does not ' +
        'install the NetUIO and virt2phys drivers. Put the EWDK ISO in Downloads, then open ' +
        'Start > MTL > MTL install drivers.', mbInformation, MB_OK, IDOK);
  end;
  if CurPageID = HostPage.ID then begin
    Cpus := Trim(HostPage.Values[0]);
    for I := 1 to Length(Cpus) do
      if (Cpus <> 'auto') and (Pos(Cpus[I], '0123456789,-') = 0) then begin
        MsgBox('Scheduler CPUs: auto, or numbers and ranges, for example 8-15 or 8-11,20-23.', mbError, MB_OK);
        Result := False;
        Exit;
      end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Args, Ewdk: String;
  Code: Integer;
begin
  if CurStep <> ssPostInstall then Exit;
  // A moving bar while the scripts run: the driver build takes a minute.
  WizardForm.ProgressGauge.Style := npbstMarquee;
  if WizardIsTaskSelected('host') then begin
    WizardForm.StatusLabel.Caption := 'Setting up Windows for MTL...';
    // The script finds the bin folder and the Defender exclusions of {app}.
    Args := 'apply -User ' + PsQuote(Trim(HostPage.Values[1]));
    if Trim(HostPage.Values[0]) <> '' then
      Args := Args + ' -SchedulerCpus ' + PsQuote(Trim(HostPage.Values[0]))
    else
      Args := Args + ' -Items testsign,lockpages,dirs,path,power,defender,vbs';
    Code := RunScript('windows_setup.ps1', Args, 'windows_setup.log');
    if Code <> 0 then
      SuppressibleMsgBox('windows_setup.ps1 did not apply each setting (exit code ' + IntToStr(Code) +
        '). See ' + LogFile('windows_setup.log') + '.', mbError, MB_OK, IDOK);
  end;
  if WizardIsTaskSelected('drivers') then begin
    WizardForm.StatusLabel.Caption := 'Building and installing the NetUIO and virt2phys drivers...';
    Args := 'apply';
    if Trim(HostPage.Values[2]) <> '' then Args := Args + ' -HardwareIds ' + PsList(HostPage.Values[2]);
    Ewdk := ExpandConstant('{param:EWDK|}');
    if Ewdk <> '' then Args := Args + ' -Ewdk ' + PsQuote(Ewdk);
    Code := RunScript('install_drivers.ps1', Args, 'install_drivers.log');
    if Code <> 0 then
      SuppressibleMsgBox('install_drivers.ps1 did not install the drivers (exit code ' + IntToStr(Code) +
        '). See ' + LogFile('install_drivers.log') + '.', mbError, MB_OK, IDOK);
  end;
  WizardForm.ProgressGauge.Style := npbstNormal;
end;

function NeedRestart(): Boolean;
begin
  Result := WizardIsTaskSelected('host') or WizardIsTaskSelected('drivers');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    RunScript('install_drivers.ps1', 'undo', 'uninstall_drivers.log');
    RunScript('windows_setup.ps1', 'undo', 'uninstall_windows_setup.log');
  end;
end;

function UninstallNeedRestart(): Boolean;
begin
  Result := True;
end;
