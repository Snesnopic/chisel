; iscc /DAppVersion=x.y.z /DArch=x64|x86|arm64 /DBinDir=<folder holding chsl.exe and Chisel.exe> chisel.iss

#ifndef AppVersion
  #error AppVersion is not defined
#endif
#ifndef Arch
  #error Arch is not defined
#endif
#ifndef BinDir
  #error BinDir is not defined
#endif

[Setup]
; never change: winget and Windows tell the installed app by it
AppId={{0DD956F3-D080-4F33-B823-86858AF5B03B}
AppName=Chisel
AppVersion={#AppVersion}
AppVerName=Chisel {#AppVersion}
AppPublisher=Snesnopic
AppPublisherURL=https://github.com/Snesnopic/chisel
AppSupportURL=https://github.com/Snesnopic/chisel/issues
AppUpdatesURL=https://github.com/Snesnopic/chisel/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\Chisel
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
MinVersion=10.0.18362
ChangesEnvironment=yes
UninstallDisplayName=Chisel
UninstallDisplayIcon={app}\Chisel.exe
SetupIconFile=..\res\chisel.ico
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
OutputBaseFilename=chisel-setup-windows-{#Arch}
#if Arch == "x64"
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#elif Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#elif Arch != "x86"
  #error Arch must be x64, x86 or arm64
#endif

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BinDir}\Chisel.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\chsl.exe"; DestDir: "{app}"; Flags: ignoreversion
; the CLI again: a terminal looks for chisel.com before chisel.exe (PATHEXT), so "chisel" runs the CLI and the Start menu the GUI
Source: "{#BinDir}\chsl.exe"; DestDir: "{app}"; DestName: "chisel.com"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Chisel"; Filename: "{app}\Chisel.exe"
Name: "{autodesktop}\Chisel"; Filename: "{app}\Chisel.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Chisel.exe"; Description: "{cm:LaunchProgram,Chisel}"; Flags: nowait postinstall skipifsilent

[Code]
const
  EnvKey = 'Environment';

procedure AddToPath(Dir: string);
var
  Paths: string;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Paths) then
    Paths := '';
  if Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Paths) + ';') > 0 then
    exit;
  if (Paths <> '') and (Paths[Length(Paths)] <> ';') then
    Paths := Paths + ';';
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Paths + Dir);
end;

procedure RemoveFromPath(Dir: string);
var
  Paths: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Paths) then
    exit;
  Paths := ';' + Paths + ';';
  P := Pos(';' + Uppercase(Dir) + ';', Uppercase(Paths));
  if P = 0 then
    exit;
  Delete(Paths, P, Length(Dir) + 1);
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Copy(Paths, 2, Length(Paths) - 2));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    AddToPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath(ExpandConstant('{app}'));
end;
