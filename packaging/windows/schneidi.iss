; Installer für schneidi (Inno Setup 6). Nimmt denselben Programmordner wie die ZIP (build-win/schneidi).
; Wird von build-windows.sh aufgerufen:  ISCC.exe /DVersion=<version> /DSourceDir=<Programmordner> /DOutputDir=<dist> schneidi.iss
; Installation nur für den eigenen Benutzer (keine Administratorrechte), Nutzerdaten in %APPDATA%\schneidi bleiben beim
; Deinstallieren erhalten.

#ifndef Version
  #define Version "0.6.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build-win\schneidi"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
; AppId nie ändern: daran erkennt Windows ein Update derselben App
AppId={{6FDE4DB7-7D85-4887-A4C7-FE24396BF20A}
AppName=schneidi
AppVersion={#Version}
AppVerName=schneidi {#Version}
AppPublisher=schneidi
DefaultDirName={autopf}\schneidi
; Startmenü-Eintrag direkt unter „Alle Apps“, ohne eigenen Ordner
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=schneidi-setup-{#Version}
SetupIconFile=..\..\assets\schneidi.ico
UninstallDisplayIcon={app}\schneidi.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=yes
; Läuft schneidi noch, bietet der Installer an, es zu schließen
CloseApplications=yes

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
de.AssocProjects=.schneidi-Projekte mit schneidi öffnen
en.AssocProjects=Open .schneidi projects with schneidi
de.ProjectFile=schneidi-Projekt
en.ProjectFile=schneidi project
de.ExtraComponents=Zusätzliche Komponenten (optional – schneidi funktioniert auch ohne):
en.ExtraComponents=Additional components (optional – schneidi works without them):
de.EncoderExtension=Bessere Export-Qualität ohne Grafikkarte (x264/x265, GPL-Lizenz, etwa 90 MB Download)
en.EncoderExtension=Better export quality without a graphics card (x264/x265, GPL license, about 90 MB download)
de.EncoderDownload=Lade x264/x265 herunter …
en.EncoderDownload=Downloading x264/x265 …

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assoc"; Description: "{cm:AssocProjects}"; GroupDescription: "{cm:AdditionalIcons}"
#ifdef GplFree
; x264/x265 as the program's own extension (engine/Extensions "x264"): never ticked by default. The installer only runs
; schneidi-cli, which downloads it exactly like the app (fixed URL, size and SHA-256, into the user's AppData) – as the
; user who started the setup, also when it runs elevated. No network: the setup still finishes, the app offers it later.
Name: "x264"; Description: "{cm:EncoderExtension}"; GroupDescription: "{cm:ExtraComponents}"; Flags: unchecked
#endif

[InstallDelete]
; Remove old program files before an update (the MLT modules of versions before 0.15 lay in lib/ and share/)
Type: filesandordirs; Name: "{app}\lib"
Type: filesandordirs; Name: "{app}\share"

[UninstallDelete]
; Downloaded extensions (x264/x265, Whisper and its models: engine/Extensions, %APPDATA%\schneidi\schneidi\extensions)
; are program parts that can be downloaded again, often hundreds of MB: they go with the program. Settings, backups
; and logs (user data) stay.
Type: filesandordirs; Name: "{userappdata}\schneidi\schneidi\extensions"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\schneidi"; Filename: "{app}\schneidi.exe"
Name: "{autodesktop}\schneidi"; Filename: "{app}\schneidi.exe"; Tasks: desktopicon

[Registry]
; Dateityp .schneidi (HKA = je nach Installationsart HKCU oder HKLM)
Root: HKA; Subkey: "Software\Classes\.schneidi"; ValueType: string; ValueName: ""; ValueData: "schneidi.Project"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\.schneidi\OpenWithProgids"; ValueType: string; ValueName: "schneidi.Project"; ValueData: ""; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\schneidi.Project"; ValueType: string; ValueName: ""; ValueData: "{cm:ProjectFile}"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\schneidi.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\schneidi.exe,0"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\schneidi.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\schneidi.exe"" ""%1"""; Tasks: assoc

[Run]
#ifdef GplFree
Filename: "{app}\schneidi-cli.exe"; Parameters: "extensions --install x264"; StatusMsg: "{cm:EncoderDownload}"; Flags: runasoriginaluser runhidden waituntilterminated; Tasks: x264
#endif
Filename: "{app}\schneidi.exe"; Description: "{cm:LaunchProgram,schneidi}"; Flags: nowait postinstall skipifsilent
