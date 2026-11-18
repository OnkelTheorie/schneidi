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
; An update keeps the kind of the installation found (only for me / for all users) without asking: choosing the other
; kind would leave a second schneidi in "Installed apps"
UsePreviousPrivileges=yes
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
; No language picker: the Windows UI language decides, English if Inno has no translation for it
ShowLanguageDialog=no
ChangesAssociations=yes
; Läuft schneidi noch, bietet der Installer an, es zu schließen
CloseApplications=yes

[Languages]
; Setup follows the Windows UI language (LanguageDetectionMethod=uilanguage, no language dialog). English comes first:
; Inno uses the first entry when no listed language matches. Then the official Inno Setup translations of the languages
; schneidi's UI has (Chinese, Hindi, Indonesian have none). Each only if the compiler has the file, so an older Inno
; Setup still builds the installer.
Name: "en"; MessagesFile: "compiler:Default.isl"
#if FileExists(AddBackslash(CompilerPath) + "Languages\German.isl")
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Ukrainian.isl")
Name: "uk"; MessagesFile: "compiler:Languages\Ukrainian.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Russian.isl")
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Polish.isl")
Name: "pl"; MessagesFile: "compiler:Languages\Polish.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\French.isl")
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Spanish.isl")
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Italian.isl")
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Dutch.isl")
Name: "nl"; MessagesFile: "compiler:Languages\Dutch.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Turkish.isl")
Name: "tr"; MessagesFile: "compiler:Languages\Turkish.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Japanese.isl")
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\Korean.isl")
Name: "ko"; MessagesFile: "compiler:Languages\Korean.isl"
#endif
#if FileExists(AddBackslash(CompilerPath) + "Languages\BrazilianPortuguese.isl")
Name: "ptBR"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
#endif

[CustomMessages]
; Unprefixed = English default for every language without its own text (en); de./uk./ru./pl./fr./es./it./nl./tr./ja./ko./ptBR.
; have their own (language names as in [Languages]); Inno picks the one for the setup language and falls back to the unprefixed text
de.AssocProjects=.schneidi-Projekte mit schneidi öffnen
AssocProjects=Open .schneidi projects with schneidi
uk.AssocProjects=Відкривати проєкти .schneidi у schneidi
ru.AssocProjects=Открывать проекты .schneidi в schneidi
pl.AssocProjects=Otwieraj projekty .schneidi w schneidi
fr.AssocProjects=Ouvrir les projets .schneidi avec schneidi
es.AssocProjects=Abrir proyectos .schneidi con schneidi
it.AssocProjects=Apri i progetti .schneidi con schneidi
nl.AssocProjects=Open .schneidi-projecten met schneidi
tr.AssocProjects=schneidi ile .schneidi projelerini aç
ja.AssocProjects=.schneidi プロジェクトを schneidi で開く
ko.AssocProjects=.schneidi 프로젝트를 schneidi로 열기
ptBR.AssocProjects=Abrir projetos .schneidi com o schneidi
de.ProjectFile=schneidi-Projekt
ProjectFile=schneidi project
uk.ProjectFile=Проєкт schneidi
ru.ProjectFile=Проект schneidi
pl.ProjectFile=Projekt schneidi
fr.ProjectFile=Projet schneidi
es.ProjectFile=Proyecto de schneidi
it.ProjectFile=Progetto schneidi
nl.ProjectFile=schneidi-project
tr.ProjectFile=schneidi projesi
ja.ProjectFile=schneidi プロジェクト
ko.ProjectFile=schneidi 프로젝트
ptBR.ProjectFile=Projeto do schneidi
de.ExtraComponents=Zusätzliche Komponenten (optional – schneidi funktioniert auch ohne):
ExtraComponents=Additional components (optional – schneidi works without them):
uk.ExtraComponents=Додаткові компоненти (необов’язково – schneidi працює і без них):
ru.ExtraComponents=Дополнительные компоненты (необязательно – schneidi работает и без них):
pl.ExtraComponents=Dodatkowe składniki (opcjonalne – schneidi działa także bez nich):
fr.ExtraComponents=Composants supplémentaires (facultatif – schneidi fonctionne aussi sans) :
es.ExtraComponents=Componentes adicionales (opcional: schneidi funciona también sin ellos):
it.ExtraComponents=Componenti aggiuntivi (facoltativi – schneidi funziona anche senza):
nl.ExtraComponents=Extra onderdelen (optioneel – schneidi werkt ook zonder):
tr.ExtraComponents=Ek bileşenler (isteğe bağlı – schneidi bunlar olmadan da çalışır):
ja.ExtraComponents=追加コンポーネント(任意 – schneidi はなくても動作します):
ko.ExtraComponents=추가 구성 요소 (선택 사항 – schneidi는 없어도 작동해요):
ptBR.ExtraComponents=Componentes adicionais (opcional – o schneidi funciona também sem eles):
de.EncoderExtension=Bessere Export-Qualität ohne Grafikkarte (x264/x265, GPL-Lizenz, etwa 90 MB Download)
EncoderExtension=Better export quality without a graphics card (x264/x265, GPL license, about 90 MB download)
uk.EncoderExtension=Краща якість експорту без відеокарти (x264/x265, ліцензія GPL, завантаження близько 90 МБ)
ru.EncoderExtension=Лучшее качество экспорта без видеокарты (x264/x265, лицензия GPL, загрузка около 90 МБ)
pl.EncoderExtension=Lepsza jakość eksportu bez karty graficznej (x264/x265, licencja GPL, pobieranie ok. 90 MB)
fr.EncoderExtension=Meilleure qualité d’export sans carte graphique (x264/x265, licence GPL, environ 90 Mo à télécharger)
es.EncoderExtension=Mejor calidad de exportación sin tarjeta gráfica (x264/x265, licencia GPL, unos 90 MB de descarga)
it.EncoderExtension=Migliore qualità di esportazione senza scheda grafica (x264/x265, licenza GPL, circa 90 MB di download)
nl.EncoderExtension=Betere exportkwaliteit zonder videokaart (x264/x265, GPL-licentie, ca. 90 MB download)
tr.EncoderExtension=Ekran kartı olmadan daha iyi dışa aktarma kalitesi (x264/x265, GPL lisansı, yaklaşık 90 MB indirme)
ja.EncoderExtension=グラフィックスカードなしで書き出し品質を向上(x264/x265、GPL ライセンス、約 90 MB をダウンロード)
ko.EncoderExtension=그래픽 카드 없이 더 나은 내보내기 품질 (x264/x265, GPL 라이선스, 약 90MB 다운로드)
ptBR.EncoderExtension=Melhor qualidade de exportação sem placa de vídeo (x264/x265, licença GPL, cerca de 90 MB para baixar)
de.EncoderDownload=Lade x264/x265 herunter …
EncoderDownload=Downloading x264/x265 …
uk.EncoderDownload=Завантаження x264/x265 …
ru.EncoderDownload=Загрузка x264/x265 …
pl.EncoderDownload=Pobieranie x264/x265 …
fr.EncoderDownload=Téléchargement de x264/x265 …
es.EncoderDownload=Descargando x264/x265 …
it.EncoderDownload=Download di x264/x265 …
nl.EncoderDownload=x264/x265 wordt gedownload …
tr.EncoderDownload=x264/x265 indiriliyor …
ja.EncoderDownload=x264/x265 をダウンロードしています …
ko.EncoderDownload=x264/x265 다운로드 중 …
ptBR.EncoderDownload=Baixando x264/x265 …
de.UpdateFrom=schneidi %1 ist installiert und wird auf %2 aktualisiert. Einstellungen, Projekte und Erweiterungen bleiben erhalten.
UpdateFrom=schneidi %1 is installed and will be updated to %2. Settings, projects and extensions are kept.
uk.UpdateFrom=schneidi %1 уже встановлено, його буде оновлено до %2. Налаштування, проєкти та розширення збережуться.
ru.UpdateFrom=schneidi %1 уже установлен и будет обновлён до %2. Настройки, проекты и расширения сохранятся.
pl.UpdateFrom=schneidi %1 jest zainstalowany i zostanie zaktualizowany do %2. Ustawienia, projekty i rozszerzenia zostaną zachowane.
fr.UpdateFrom=schneidi %1 est installé et va être mis à jour vers %2. Les réglages, projets et extensions sont conservés.
es.UpdateFrom=schneidi %1 está instalado y se actualizará a %2. Se conservan los ajustes, proyectos y extensiones.
it.UpdateFrom=schneidi %1 è installato e verrà aggiornato alla %2. Impostazioni, progetti ed estensioni vengono mantenuti.
nl.UpdateFrom=schneidi %1 is geïnstalleerd en wordt bijgewerkt naar %2. Instellingen, projecten en extensies blijven behouden.
tr.UpdateFrom=schneidi %1 yüklü ve %2 sürümüne güncellenecek. Ayarlar, projeler ve uzantılar korunur.
ja.UpdateFrom=schneidi %1 がインストールされており、%2 に更新されます。設定、プロジェクト、拡張機能はそのまま残ります。
ko.UpdateFrom=schneidi %1이(가) 설치되어 있으며 %2(으)로 업데이트돼요. 설정, 프로젝트, 확장 기능은 그대로 유지돼요.
ptBR.UpdateFrom=O schneidi %1 está instalado e será atualizado para %2. Configurações, projetos e extensões são mantidos.
de.Reinstall=schneidi %1 ist bereits installiert und wird neu installiert. Einstellungen, Projekte und Erweiterungen bleiben erhalten.
Reinstall=schneidi %1 is already installed and will be installed again. Settings, projects and extensions are kept.
uk.Reinstall=schneidi %1 уже встановлено, його буде встановлено повторно. Налаштування, проєкти та розширення збережуться.
ru.Reinstall=schneidi %1 уже установлен и будет установлен заново. Настройки, проекты и расширения сохранятся.
pl.Reinstall=schneidi %1 jest już zainstalowany i zostanie zainstalowany ponownie. Ustawienia, projekty i rozszerzenia zostaną zachowane.
fr.Reinstall=schneidi %1 est déjà installé et va être réinstallé. Les réglages, projets et extensions sont conservés.
es.Reinstall=schneidi %1 ya está instalado y se instalará de nuevo. Se conservan los ajustes, proyectos y extensiones.
it.Reinstall=schneidi %1 è già installato e verrà reinstallato. Impostazioni, progetti ed estensioni vengono mantenuti.
nl.Reinstall=schneidi %1 is al geïnstalleerd en wordt opnieuw geïnstalleerd. Instellingen, projecten en extensies blijven behouden.
tr.Reinstall=schneidi %1 zaten yüklü ve yeniden yüklenecek. Ayarlar, projeler ve uzantılar korunur.
ja.Reinstall=schneidi %1 は既にインストールされており、再インストールされます。設定、プロジェクト、拡張機能はそのまま残ります。
ko.Reinstall=schneidi %1이(가) 이미 설치되어 있으며 다시 설치돼요. 설정, 프로젝트, 확장 기능은 그대로 유지돼요.
ptBR.Reinstall=O schneidi %1 já está instalado e será instalado novamente. Configurações, projetos e extensões são mantidos.
de.Downgrade=Achtung: Installiert ist die neuere Version schneidi %1. Sie wird durch die ältere Version %2 ersetzt.
Downgrade=Note: the newer version schneidi %1 is installed. It will be replaced by the older version %2.
uk.Downgrade=Увага: встановлено новішу версію schneidi %1. Її буде замінено старішою версією %2.
ru.Downgrade=Внимание: установлена более новая версия schneidi %1. Она будет заменена более старой версией %2.
pl.Downgrade=Uwaga: zainstalowana jest nowsza wersja schneidi %1. Zostanie zastąpiona starszą wersją %2.
fr.Downgrade=Attention : la version plus récente schneidi %1 est installée. Elle sera remplacée par la version plus ancienne %2.
es.Downgrade=Atención: está instalada la versión más reciente schneidi %1. Se sustituirá por la versión más antigua %2.
it.Downgrade=Attenzione: è installata la versione più recente schneidi %1. Verrà sostituita dalla versione precedente %2.
nl.Downgrade=Let op: de nieuwere versie schneidi %1 is geïnstalleerd. Deze wordt vervangen door de oudere versie %2.
tr.Downgrade=Dikkat: daha yeni olan schneidi %1 sürümü yüklü. Daha eski olan %2 sürümüyle değiştirilecek.
ja.Downgrade=注意: より新しいバージョン schneidi %1 がインストールされています。古いバージョン %2 に置き換えられます。
ko.Downgrade=주의: 더 새로운 버전 schneidi %1이(가) 설치되어 있어요. 이전 버전 %2(으)로 대체돼요.
ptBR.Downgrade=Atenção: a versão mais recente schneidi %1 está instalada. Ela será substituída pela versão mais antiga %2.

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "assoc"; Description: "{cm:AssocProjects}"; GroupDescription: "{cm:AdditionalIcons}"
#ifdef GplFree
; Only the GPL-free build (build-windows.sh with GPL_ENCODERS=0 passes /DGplFree=1); the default download has
; x264/x265 in its FFmpeg already.
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

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6FDE4DB7-7D85-4887-A4C7-FE24396BF20A}_is1';

var
  InstalledVersion: String; // DisplayVersion of the installation found, empty = none
  ReadyGeometryKnown: Boolean; // the Ready page's original label height and memo position are saved below
  ReadyLabelHeight, ReadyMemoTop, ReadyMemoBottom: Integer;

// The installed version: the current mode's entry first (HKCU for "only for me", HKLM for "all users"), then the
// other one (an update in the other mode still says what is there)
function FindInstalledVersion(): String;
begin
  Result := '';
  if IsAdminInstallMode() then begin
    if not RegQueryStringValue(HKLM64, UninstallKey, 'DisplayVersion', Result) then
      if not RegQueryStringValue(HKLM32, UninstallKey, 'DisplayVersion', Result) then
        RegQueryStringValue(HKCU, UninstallKey, 'DisplayVersion', Result);
  end else begin
    if not RegQueryStringValue(HKCU, UninstallKey, 'DisplayVersion', Result) then
      if not RegQueryStringValue(HKLM64, UninstallKey, 'DisplayVersion', Result) then
        RegQueryStringValue(HKLM32, UninstallKey, 'DisplayVersion', Result);
  end;
end;

// Next number of a dotted version ("0.20.1"), 0 when none is left; Rest loses it
function NextPart(var Rest: String): Integer;
var
  Dot: Integer;
begin
  Dot := Pos('.', Rest);
  if Dot = 0 then begin
    Result := StrToIntDef(Rest, 0);
    Rest := '';
  end else begin
    Result := StrToIntDef(Copy(Rest, 1, Dot - 1), 0);
    Rest := Copy(Rest, Dot + 1, Length(Rest));
  end;
end;

// -1: A older than B, 0: same, 1: newer (three parts, missing ones count 0)
function CompareVersions(A, B: String): Integer;
var
  I, X, Y: Integer;
begin
  Result := 0;
  for I := 1 to 3 do begin
    X := NextPart(A);
    Y := NextPart(B);
    if X < Y then begin
      Result := -1;
      Exit;
    end;
    if X > Y then begin
      Result := 1;
      Exit;
    end;
  end;
end;

// The sentence for the "Ready to install" page (and the log); empty for a fresh installation
function UpdateText(): String;
begin
  Result := '';
  if InstalledVersion = '' then Exit;
  case CompareVersions(InstalledVersion, '{#Version}') of
    -1: Result := FmtMessage(CustomMessage('UpdateFrom'), [InstalledVersion, '{#Version}']);
    0: Result := FmtMessage(CustomMessage('Reinstall'), [InstalledVersion]);
    1: Result := FmtMessage(CustomMessage('Downgrade'), [InstalledVersion, '{#Version}']);
  end;
end;

function InitializeSetup(): Boolean;
begin
  InstalledVersion := FindInstalledVersion();
  if InstalledVersion = '' then Log('schneidi: no installation found')
  else Log('schneidi: ' + UpdateText());
  Result := True;
end;

// Setup sets the Ready label's text again on every visit of the page (Back and Next again), so the sentence goes in
// each time; the label and memo geometry is taken from the first visit, so repeated visits do not shift it further
procedure CurPageChanged(CurPageID: Integer);
var
  Text: String;
  Delta: Integer;
begin
  if (CurPageID <> wpReady) or (InstalledVersion = '') then Exit;
  if not ReadyGeometryKnown then begin
    ReadyLabelHeight := WizardForm.ReadyLabel.Height;
    ReadyMemoTop := WizardForm.ReadyMemo.Top;
    ReadyMemoBottom := WizardForm.ReadyMemo.Top + WizardForm.ReadyMemo.Height;
    ReadyGeometryKnown := True;
  end;
  Text := UpdateText();
  if Pos(Text, WizardForm.ReadyLabel.Caption) <> 1 then
    WizardForm.ReadyLabel.Caption := Text + #13#10#13#10 + WizardForm.ReadyLabel.Caption;
  // The label is only as high as the original two lines: grow it to the new text and move the memo below down,
  // otherwise "Click Install to continue ..." disappears behind the memo
  WizardForm.ReadyLabel.Height := ReadyLabelHeight;
  Delta := WizardForm.AdjustLabelHeight(WizardForm.ReadyLabel);
  WizardForm.ReadyMemo.Top := ReadyMemoTop + Delta;
  WizardForm.ReadyMemo.Height := ReadyMemoBottom - WizardForm.ReadyMemo.Top;
end;
