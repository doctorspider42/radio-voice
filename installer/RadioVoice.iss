; RadioVoice installer (Inno Setup 7).
;
; Seven, not six, for SetupArchitecture=x64 - see the [Setup] section. Inno
; Setup 7 installs alongside 6 and keeps the script language compatible, so the
; only thing the version buys is a Setup that is not a 32-bit process.
;
; Build it with make-installer.cmd at the repository root, which stages the
; driver payload first. Compiling this file on its own still works; the driver
; component simply will not be offered, because the payload is what makes it
; appear.
;
; Comments and identifiers here are English, like the rest of the project. The
; strings it displays are given twice, under [CustomMessages]; Inno Setup picks
; by the system's language, so a Polish Windows gets a Polish wizard without
; anyone choosing. The installer is translated and the documentation is not
; because this is the only part a user meets before deciding whether to trust
; the thing - and one page of it is asking them to weaken their machine.
;
; Add both languages whenever a message is added. A half-translated wizard is
; worse than an untranslated one.

#define AppName        "RadioVoice"
#define AppPublisher   "Pawel Pajak"
#define AppUrl         "https://github.com/doctorspider42/radio-voice"
#define AppExe         "RadioVoice.exe"

; The version is not written down here. It is read from the VERSION file at the
; repository root, which is the single source every other part of the build
; reads too.
#define VersionHandle  FileOpen(SourcePath + "\..\VERSION")
#define AppVersion     Trim(FileRead(VersionHandle))
#expr FileClose(VersionHandle)

#if AppVersion == ""
  #error VERSION is empty - the installer cannot be built without one
#endif

; Where the application binary comes from. Overridable, so that a build in an
; unusual place can still be packaged:  iscc /DAppBinDir=...\some\bin
#ifndef AppBinDir
  #define AppBinDir SourcePath + "\..\build\bin"
#endif

; The driver is a separate, local build - it needs the WDK, and it needs to be
; signed before it is staged. Its presence in the payload directory is what
; decides whether this installer can offer it at all.
#define DriverPayload SourcePath + "\payload\driver"
#if FileExists(DriverPayload + "\RadioVoiceAudio.sys") && \
    FileExists(DriverPayload + "\RadioVoiceAudio.cat") && \
    FileExists(DriverPayload + "\RadioVoiceTest.cer")
  #define HaveDriver
#endif

[Setup]
; Never change AppId. It is what tells Windows that a new build is an upgrade
; of the old one rather than a second copy alongside it.
AppId={{7C1A4E90-2B65-4F0D-9E3A-5D8B1C4F62A7}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
VersionInfoVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases

DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile={#SourcePath}\..\LICENSE

; The driver half needs to write to the driver store and the machine
; certificate stores, so there is no useful per-user install to offer.
PrivilegesRequired=admin

; A 64-bit Setup, which needs Inno Setup 7. Not cosmetic: a 32-bit Setup is
; subject to WOW64 file system redirection, and the 32-bit view of System32 has
; no pnputil.exe - the one thing the driver component cannot do without.
SetupArchitecture=x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041

OutputDir={#SourcePath}\..\dist
OutputBaseFilename={#AppName}-{#AppVersion}-setup
SetupIconFile={#SourcePath}\..\res\RadioVoice.ico
UninstallDisplayIcon={app}\{#AppExe}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes

; The application must not be running while its executable is replaced, and it
; is a tray application - so it is routinely running with nothing on screen to
; suggest it.
CloseApplications=yes
RestartApplications=no

[Languages]
; English first: it is the fallback for every system whose language is neither.
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "pl"; MessagesFile: "compiler:Languages\Polish.isl"

[CustomMessages]
; --- setup types -----------------------------------------------------------
pl.TypeStandard=Instalacja standardowa (bez sterownika)
en.TypeStandard=Standard installation (no driver)
pl.TypeFull=Pelna instalacja (ze sterownikiem)
en.TypeFull=Full installation (with the driver)
pl.TypeCustom=Instalacja uzytkownika
en.TypeCustom=Custom installation

; --- components ------------------------------------------------------------
pl.CompApp=RadioVoice (aplikacja)
en.CompApp=RadioVoice (application)
pl.CompDriver=Sterownik wirtualnego kabla audio
en.CompDriver=Virtual audio cable driver

; --- tasks -----------------------------------------------------------------
pl.TaskDesktop=Utw&orz skrot na pulpicie
en.TaskDesktop=Create a &desktop shortcut
pl.TaskAutostart=Uruchamiaj RadioVoice przy starcie Windows (w zasobniku)
en.TaskAutostart=Start RadioVoice with Windows (in the notification area)
pl.TaskGroupOther=Dodatkowe zadania:
en.TaskGroupOther=Additional tasks:

; --- the driver warning page ----------------------------------------------
pl.DriverPageTitle=Sterownik wirtualnego kabla audio
pl.DriverPageSubtitle=Przeczytaj to, zanim przejdziesz dalej.
en.DriverPageTitle=Virtual audio cable driver
en.DriverPageSubtitle=Please read this before continuing.

pl.DriverWarning=Ten sterownik NIE jest podpisany przez Microsoft.%n%nPodpis WHQL wymaga certyfikatu EV i konta w Microsoft Partner Center - czyli pieniedzy, ktorych ten projekt nie ma. Sterownik jest wiec podpisany certyfikatem wygenerowanym na potrzeby tego projektu, a Windows domyslnie takich nie wpuszcza.%n%nZeby dzialal, trzeba zrobic dwie rzeczy, na ktore Windows musi Ci pozwolic:%n%n1. Wlaczyc tryb testowy (testsigning) i zrestartowac komputer. Po restarcie w prawym dolnym rogu pulpitu pojawi sie napis "Tryb testowy".%n%n2. Dodac certyfikat RadioVoice do zaufanych certyfikatow glownych tego komputera. Od tej chwili maszyna ufa wszystkiemu, co jest podpisane tym kluczem - nie tylko temu sterownikowi.%n%nTo realnie obniza bezpieczenstwo komputera: jedna z warstw chroniacych przed rootkitami przestaje obowiazywac. Na maszynie, na ktorej trzymasz cos waznego, zastanow sie dwa razy.%n%nJesli masz wlaczony Secure Boot, najpierw trzeba go wylaczyc w UEFI - inaczej tryb testowy nie zadziala, mimo ze Windows powie, ze go wlaczyl.%n%nWszystko da sie cofnac: odinstalowanie RadioVoice usuwa sterownik, a "bcdedit /set testsigning off" wylacza tryb testowy.%n%nNie chcesz tego robic? Cofnij sie i odznacz sterownik. RadioVoice dziala tak samo dobrze z VB-CABLE.
en.DriverWarning=This driver is NOT signed by Microsoft.%n%nA WHQL signature needs an EV certificate and a Microsoft Partner Center account - money this project does not have. The driver is therefore signed with a certificate generated for this project, and Windows does not accept those by default.%n%nTo make it work, two things have to happen, and Windows has to be told to allow both:%n%n1. Test signing must be turned on and the machine restarted. After the restart a "Test Mode" watermark appears in the bottom-right corner of the desktop.%n%n2. The RadioVoice certificate is added to this machine's trusted root store. From then on the machine trusts anything signed with that key - not only this driver.%n%nThat is a real reduction in the machine's security: one of the layers protecting against rootkits stops applying. On a machine holding anything valuable, think twice.%n%nIf Secure Boot is enabled it has to be turned off in the firmware first - otherwise test signing will not take effect, even though Windows reports that it did.%n%nAll of it is reversible: uninstalling RadioVoice removes the driver, and "bcdedit /set testsigning off" leaves test mode.%n%nWould rather not? Go back and clear the driver component. RadioVoice works just as well with VB-CABLE.

pl.DriverAccept=Rozumiem i chce zainstalowac ten sterownik
en.DriverAccept=I understand, and I want to install this driver

pl.DriverEnableTestSigning=Wlacz teraz tryb testowy Windows (wymaga restartu)
en.DriverEnableTestSigning=Turn on Windows test signing now (needs a restart)
pl.DriverTestSigningOn=Tryb testowy jest juz wlaczony na tym komputerze.
en.DriverTestSigningOn=Test signing is already enabled on this machine.

pl.DriverMustAccept=Zaznacz "Rozumiem", albo cofnij sie i odznacz sterownik.
en.DriverMustAccept=Tick "I understand", or go back and clear the driver component.

; --- progress --------------------------------------------------------------
pl.StatusAutostart=Ustawianie autostartu...
en.StatusAutostart=Setting up autostart...
pl.StatusTestSigning=Wlaczanie trybu testowego...
en.StatusTestSigning=Enabling test signing...
pl.StatusTrusting=Dodawanie certyfikatu do zaufanych...
en.StatusTrusting=Trusting the certificate...
pl.StatusDriver=Instalowanie sterownika...
en.StatusDriver=Installing the driver...
pl.StatusDriverRemove=Usuwanie sterownika...
en.StatusDriverRemove=Removing the driver...

; --- outcomes --------------------------------------------------------------
pl.DriverFailed=Nie udalo sie zainstalowac sterownika.%n%nAplikacja RadioVoice zostala zainstalowana i dziala - brakuje tylko wirtualnego kabla. Mozesz zamiast niego zainstalowac VB-CABLE, albo sprobowac recznie: install-driver.cmd w katalogu zrodlowym projektu.%n%nSzczegoly: %1
en.DriverFailed=The driver could not be installed.%n%nRadioVoice itself is installed and working - only the virtual cable is missing. You can install VB-CABLE instead, or try by hand: install-driver.cmd in the project source tree.%n%nDetails: %1

pl.RebootNeeded=Zeby sterownik zaczal dzialac, zrestartuj komputer.%n%nPo restarcie w Windows pojawia sie: "RadioVoice Output" (odtwarzanie) i "RadioVoice Microphone" (nagrywanie).
en.RebootNeeded=Restart the machine to finish installing the driver.%n%nAfter the restart Windows will show "RadioVoice Output" (playback) and "RadioVoice Microphone" (recording).

pl.TestSigningFailed=Nie udalo sie wlaczyc trybu testowego. Najczestsza przyczyna to wlaczony Secure Boot - wylacz go w UEFI i sprobuj ponownie.
en.TestSigningFailed=Test signing could not be enabled. The usual cause is Secure Boot; turn it off in the firmware and try again.

; --- finish page -----------------------------------------------------------
pl.LaunchApp=Uruchom RadioVoice
en.LaunchApp=Launch RadioVoice

; Components exist only when there is a choice to make. Without a driver
; payload there is exactly one thing to install, and a page listing it - under a
; type called "full installation (with the driver)" - promises a driver this
; build does not contain.
#ifdef HaveDriver

[Types]
; "standard" is first, so it is the default, and it does not include the
; driver. Installing a kernel driver is not something to arrive at by leaving
; the defaults alone.
Name: "standard"; Description: "{cm:TypeStandard}"
Name: "full";     Description: "{cm:TypeFull}"
Name: "custom";   Description: "{cm:TypeCustom}"; Flags: iscustom

[Components]
Name: "app";    Description: "{cm:CompApp}"; Types: standard full custom; Flags: fixed
Name: "driver"; Description: "{cm:CompDriver}"; Types: full

#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktop}"; GroupDescription: "{cm:TaskGroupOther}"
Name: "autostart";   Description: "{cm:TaskAutostart}"; GroupDescription: "{cm:TaskGroupOther}"

; A component can only be named where components exist at all, so the clause is
; substituted rather than written out.
#ifdef HaveDriver
  #define AppPart "Components: app; "
#else
  #define AppPart ""
#endif

[Files]
Source: "{#AppBinDir}\{#AppExe}"; DestDir: "{app}"; {#AppPart}Flags: ignoreversion
Source: "{#SourcePath}\..\LICENSE";       DestDir: "{app}"; DestName: "LICENSE.txt"; {#AppPart}Flags: ignoreversion
Source: "{#SourcePath}\..\README.md";     DestDir: "{app}"; {#AppPart}Flags: ignoreversion
Source: "{#SourcePath}\..\QUICKSTART.md"; DestDir: "{app}"; {#AppPart}Flags: ignoreversion
Source: "{#SourcePath}\..\NOTICE.md";     DestDir: "{app}"; {#AppPart}Flags: ignoreversion
Source: "{#SourcePath}\..\CHANGELOG.md";  DestDir: "{app}"; {#AppPart}Flags: ignoreversion

#ifdef HaveDriver
; Both the package and the scripts that install it, keeping the layout the
; scripts expect: tools\ beside the .inf.
Source: "{#DriverPayload}\*"; DestDir: "{app}\driver"; Components: driver; \
    Flags: ignoreversion recursesubdirs createallsubdirs
#endif

[Icons]
Name: "{group}\{#AppName}";       Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

; Settings -> Apps lists the uninstaller either way, but that is not where
; someone who installed from a Start menu entry goes looking for it.
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"

[Run]
; Autostart is a per-user setting, and this installer runs elevated - so
; HKEY_CURRENT_USER here is the hive of whoever answered the UAC prompt, which
; on a machine where an administrator installs for someone else is the wrong
; person entirely. Writing it from a [Registry] entry gets that wrong silently.
;
; `runasoriginaluser` runs the application as the user who is actually signing
; in, and it writes its own entry - the same one its Tray checkbox writes, so
; there is still exactly one place the setting lives.
Filename: "{app}\{#AppExe}"; Parameters: "--enable-autostart"; \
    StatusMsg: "{cm:StatusAutostart}"; Tasks: autostart; \
    Flags: runasoriginaluser runhidden waituntilterminated

Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchApp}"; \
    Flags: nowait postinstall skipifsilent

; What the application's own updater passes when it hands over. A silent
; install shows no finish page, so the checkbox above never appears - and an
; update that ends with nothing running is not an update anyone wants.
;
; `runasoriginaluser` for the same reason as the autostart entry: Setup is
; elevated, and RadioVoice is not something to leave running as an
; administrator.
;
; No --minimized here. Whether the window shows is the application's own saved
; setting, and an update the user asked for is a reasonable moment to see that
; it landed.
Filename: "{app}\{#AppExe}"; Flags: nowait runasoriginaluser; \
    Check: RelaunchRequested

[UninstallRun]
; The counterpart, so that uninstalling does not leave an entry pointing at a
; deleted executable. Runs before the files go.
;
; No runasoriginaluser here - [UninstallRun] does not accept it, so this removes
; the entry from the hive of whoever runs the uninstaller. Where that is not the
; person who installed it, the entry survives; it then points at nothing, which
; Windows ignores silently, and the Tray checkbox will clear it on the next
; install. Worth knowing about, not worth loading another user's hive over.
Filename: "{app}\{#AppExe}"; Parameters: "--disable-autostart"; \
    RunOnceId: "RemoveAutostart"; \
    Flags: runhidden waituntilterminated

[Code]
var
  DriverPage:        TInputOptionWizardPage;
  TestSigningWasOn:  Boolean;
  DriverRebootNeeded: Boolean;

// The explicit path, rather than trusting PATH to resolve it.
//
// With SetupArchitecture=x64 this Setup is a 64-bit process, so the system
// directory is the real System32 and there is no WOW64 redirection to think
// about. Naming the path outright keeps that true if the architecture is ever
// changed back - a 32-bit Setup would otherwise silently get the SysWOW64
// PowerShell, whose view of System32 contains no pnputil.exe.
//
// Line comments, not braces: a Pascal block comment ends at the first closing
// brace, which an Inno constant written in passing would supply.
function PowerShellPath: String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function DriverSelected: Boolean;
begin
  Result := WizardIsComponentSelected('driver');
end;

// Whether this Setup was started by RadioVoice's own updater, which passes
// /relaunch=yes. Inno Setup hands unknown parameters through untouched, so
// {param:...} is the whole mechanism - there is nothing to declare.
function RelaunchRequested: Boolean;
begin
  Result := CompareText(ExpandConstant('{param:relaunch|no}'), 'yes') = 0;
end;

// Reads the current boot configuration. bcdedit prints a "testsigning Yes" line
// only when it has been set, so its absence is the answer.
function IsTestSigningOn: Boolean;
var
  ResultCode: Integer;
  Output: AnsiString;
  TempFile: String;
begin
  Result := False;
  TempFile := ExpandConstant('{tmp}\bcdedit.txt');

  // bcdedit writes to stdout, and Exec cannot capture it, so it goes via cmd
// and a file.
  if Exec(ExpandConstant('{cmd}'),
          '/C bcdedit /enum "{current}" > "' + TempFile + '"',
          '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    if LoadStringFromFile(TempFile, Output) then
      Result := Pos('testsigning', LowerCase(String(Output))) > 0;
    DeleteFile(TempFile);
  end;
end;

// Runs one of the driver scripts. Returns the script's exit code, or -1 when
// PowerShell could not be started at all.
function RunDriverScript(const ScriptName, Arguments: String): Integer;
var
  ResultCode: Integer;
  Parameters: String;
begin
  Parameters := '-NoProfile -ExecutionPolicy Bypass -File "' +
                ExpandConstant('{app}\driver\tools\') + ScriptName + '"';
  if Arguments <> '' then
    Parameters := Parameters + ' ' + Arguments;

  if not Exec(PowerShellPath, Parameters, '', SW_HIDE, ewWaitUntilTerminated,
              ResultCode) then
    Result := -1
  else
    Result := ResultCode;
end;

procedure InitializeWizard;
var
  Warning: TNewMemo;
  ItemHeight, ListHeight: Integer;
begin
  // The page's own description is a plain label: it does not scroll, and the
  // warning is far longer than the wizard is tall, so as a description it
  // simply runs off the bottom of the page. It goes into a memo below instead,
  // which is why the page is created without one.
  DriverPage := CreateInputOptionPage(wpSelectTasks,
    CustomMessage('DriverPageTitle'),
    CustomMessage('DriverPageSubtitle'),
    '',
    False, False);

  DriverPage.Add(CustomMessage('DriverAccept'));
  DriverPage.Add(CustomMessage('DriverEnableTestSigning'));

  // Two rows, at a height this script sets rather than infers, so that the
  // memo above can have all the remaining space without either of them
  // guessing at the other's size.
  ItemHeight := ScaleY(20);
  ListHeight := 2 * ItemHeight;

  Warning := TNewMemo.Create(DriverPage);
  Warning.Parent := DriverPage.Surface;
  Warning.Left := 0;
  Warning.Top := DriverPage.CheckListBox.Top;
  Warning.Width := DriverPage.SurfaceWidth;
  Warning.Height := DriverPage.SurfaceHeight - Warning.Top - ListHeight -
                    ScaleY(12);
  Warning.Anchors := [akLeft, akTop, akRight, akBottom];
  Warning.ScrollBars := ssVertical;
  Warning.WordWrap := True;

  // Read-only rather than disabled: the text stays selectable, so anyone who
  // wants to paste "bcdedit /set testsigning off" somewhere can.
  Warning.ReadOnly := True;
  Warning.Text := CustomMessage('DriverWarning');

  // No border on the checkboxes: they are the page's controls, not a second
  // box competing with the memo, and this is how the Select Tasks page looks.
  DriverPage.CheckListBox.BorderStyle := bsNone;
  DriverPage.CheckListBox.MinItemHeight := ItemHeight;
  DriverPage.CheckListBox.Top := DriverPage.SurfaceHeight - ListHeight;
  DriverPage.CheckListBox.Height := ListHeight;
  DriverPage.CheckListBox.Anchors := [akLeft, akRight, akBottom];
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = DriverPage.ID then
    Result := not DriverSelected;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = DriverPage.ID then
  begin
    TestSigningWasOn := IsTestSigningOn;

    // Nothing to offer when it is already on, and an enabled checkbox there
// would suggest there is.
    DriverPage.CheckListBox.ItemEnabled[1] := not TestSigningWasOn;
    DriverPage.Values[1] := not TestSigningWasOn;

    if TestSigningWasOn then
      DriverPage.CheckListBox.ItemCaption[1] := CustomMessage('DriverTestSigningOn');
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = DriverPage.ID then
  begin
    // A silent Setup has no wizard, but it still comes through here: Inno
// Setup simulates the Next clicks, and this page's checkbox is one nobody was
// there to tick. Refusing to advance then is not consent - it is an update
// that dies at a dialog, which is precisely what the application's own updater
// hit, because it starts Setup with /SILENT.
    //
    // Silence is not being read as agreement. A silent run installs the driver
// only where the driver component is selected, and on an upgrade that
// selection is the one made on this page the first time round. It also leaves
// test signing alone - see InstallDriver.
    if WizardSilent then
      Exit;

    // Deliberately blocking. The whole point of the page is that this is not
// something to click past.
    if not DriverPage.Values[0] then
    begin
      MsgBox(CustomMessage('DriverMustAccept'), mbError, MB_OK);
      Result := False;
    end;
  end;
end;

// Every message box from here on is suppressible. A plain MsgBox is shown even
// when Setup was started with /SUPPRESSMSGBOXES - only SuppressibleMsgBox
// honours it - and an unattended update that stops on a dialog nobody is
// looking at is an update that does not happen.
procedure InstallDriver;
var
  Code: Integer;
begin
  // CurPageChanged only fires for a page that was actually shown, so on a
// silent run this is where the boot configuration gets read instead. The
// second checkbox is untouched there, still False from when the page was
// built: an update running by itself is not the moment to rewrite somebody's
// boot configuration and ask for a restart.
  if WizardSilent then
    TestSigningWasOn := IsTestSigningOn;

  if (not TestSigningWasOn) and DriverPage.Values[1] then
  begin
    WizardForm.StatusLabel.Caption := CustomMessage('StatusTestSigning');
    if not Exec(ExpandConstant('{cmd}'), '/C bcdedit /set testsigning on', '',
                SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
      SuppressibleMsgBox(CustomMessage('TestSigningFailed'), mbError, MB_OK, IDOK)
    else
      DriverRebootNeeded := True;
  end;

  WizardForm.StatusLabel.Caption := CustomMessage('StatusTrusting');
  Code := RunDriverScript('trust-cert.ps1',
                          '-Path "' + ExpandConstant('{app}\driver\RadioVoiceTest.cer') + '"');
  if Code <> 0 then
  begin
    SuppressibleMsgBox(FmtMessage(CustomMessage('DriverFailed'), ['trust-cert.ps1 -> ' + IntToStr(Code)]),
                       mbError, MB_OK, IDOK);
    Exit;
  end;

  WizardForm.StatusLabel.Caption := CustomMessage('StatusDriver');
  Code := RunDriverScript('install.ps1',
                          '-Path "' + ExpandConstant('{app}\driver') + '"');
  if Code <> 0 then
  begin
    SuppressibleMsgBox(FmtMessage(CustomMessage('DriverFailed'), ['install.ps1 -> ' + IntToStr(Code)]),
                       mbError, MB_OK, IDOK);
    Exit;
  end;

  // Test signing that was only just turned on has not taken effect yet, so the
// driver is in the store but the kernel will not load it until the reboot.
  if not TestSigningWasOn then
    DriverRebootNeeded := True;

  if DriverRebootNeeded then
    SuppressibleMsgBox(CustomMessage('RebootNeeded'), mbInformation, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and DriverSelected then
    InstallDriver;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
  Script: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    Script := ExpandConstant('{app}\driver\tools\uninstall.ps1');
    if FileExists(Script) then
      Exec(PowerShellPath,
           '-NoProfile -ExecutionPolicy Bypass -File "' + Script + '"',
           '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
