; Copyright (C) 2026 AnoY606-git and FrameScope contributors
; SPDX-License-Identifier: GPL-3.0-only
#ifndef AppVersion
  #error AppVersion must come from VERSION via scripts/package.ps1
#endif
#ifndef PublishDir
  #error PublishDir must name the explicit verified self-contained publish directory
#endif
#ifndef OutputDir
  #error OutputDir must be explicit
#endif
#ifndef BuildConfiguration
  #define BuildConfiguration "Release"
#endif
#if BuildConfiguration == "Debug"
  #define ProductName "FrameScope (Debug)"
  #define BuildSuffix "-debug"
#elif BuildConfiguration == "Release"
  #define ProductName "FrameScope"
  #define BuildSuffix ""
#else
  #error BuildConfiguration must be Debug or Release
#endif

[Setup]
#if BuildConfiguration == "Debug"
AppId={{6F27CBA1-8F93-4614-96A8-67F5C35C43D8}
DefaultDirName={localappdata}\Programs\FrameScope-Debug
#else
AppId={{A9B2687B-13C8-41CE-9E85-286728CC9D5B}
DefaultDirName={localappdata}\Programs\FrameScope
#endif
AppName={#ProductName}
AppVersion={#AppVersion}
AppPublisher=AnoY606-git
AppCopyright=Copyright (C) 2026 AnoY606-git and FrameScope contributors
DefaultGroupName={#ProductName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
LicenseFile={#PublishDir}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=FrameScope-{#AppVersion}{#BuildSuffix}-win-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\FrameScope.exe
CloseApplications=yes
RestartApplications=no
SetupLogging=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PublishDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\{#ProductName}"; Filename: "{app}\FrameScope.exe"
Name: "{userdesktop}\{#ProductName}"; Filename: "{app}\FrameScope.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\FrameScope.exe"; Description: "{cm:LaunchProgram,{#ProductName}}"; Flags: nowait postinstall skipifsilent

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}\portable.txt')) then
    Result := 'This directory contains a portable FrameScope installation. Choose a separate installation directory.';
end;
