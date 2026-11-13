; Inside The Dream for Windows: the installer (12/11/2026), Port0r's one-click installer (tcvr2 linux/windows/port0r.iss)
; made for this app: no page to read, a progress bar, then the start screen opens by itself (PLAY, PLAY IN VR, the game
; files' setup). One shortcut, the start screen (PLAY IN VR is on it). Per user, no administrator rights. The
; player's data (game files set up, saves, settings: the platform's data folder, runtime/paths.c) is never removed.
; Built by tools/windows/itd_installer.sh (ISCC, under wine is fine), every value below passed with /D.
#ifndef StageDir
  #error StageDir is passed by tools/windows/itd_installer.sh
#endif

[Setup]
AppId={{6D1B7C2E-3F4A-5B6C-8D9E-0A1B2C3D4E5F}
AppName=Inside The Dream
AppVersion={#AppVersion}
AppVerName=Inside The Dream {#AppVersion}
AppPublisher=Port0r
AppPublisherURL=https://port0r.com
AppSupportURL=https://port0r.com
DefaultDirName={localappdata}\Programs\Port0r\InsideTheDream
DefaultGroupName=Port0r
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\lasrecomp.exe
UninstallDisplayName=Inside The Dream
; one click: every page but the progress bar off; the start screen follows
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableFinishedPage=yes
; the progress bar in port0r.com's colours: the page's paper, its night in dark mode
WizardStyle=modern dynamic windows11 hidebevels
WizardBackColor=#f3f5f9
WizardBackColorDynamicDark=#0b0f19
WizardSmallImageFile={#WizardSmall}
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
LZMANumBlockThreads=2
ShowLanguageDialog=no
CloseApplications=force

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Inside The Dream"; Filename: "{app}\lasrecomp.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Inside The Dream"; Filename: "{app}\lasrecomp.exe"; WorkingDir: "{app}"
; (PC VR goes through the start screen: PLAY IN VR, with its sound and controller choices; Guillaume 12/11: one
; launcher, like Rally VR and Scud Race VR - no second icon going round it)

[Run]
Filename: "{app}\lasrecomp.exe"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent
