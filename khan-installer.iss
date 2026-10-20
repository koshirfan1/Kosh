; ============================================================================
; Khan — Windows GUI installer (Inno Setup script)
; ============================================================================
; Produces the "download an .exe, double-click, click through a wizard"
; experience Python's own official Windows installer gives — a proper
; setup wizard with an "Add Khan to PATH" checkbox, a Start Menu entry,
; and a normal Add/Remove Programs uninstaller entry. This is a DIFFERENT
; artifact from install.ps1 (which builds from source via a piped
; PowerShell script, closer to how rustup/deno install): this is the
; double-click GUI installer specifically, for people who want that
; experience rather than a terminal command.
;
; HOW TO BUILD THE ACTUAL INSTALLER .exe FROM THIS FILE
; ------------------------------------------------------
; This .iss file is a SOURCE script for Inno Setup, not the installer
; itself. To produce khan-setup.exe:
;   1. Install Inno Setup (free): https://jrsoftware.org/isinfo.php
;   2. Build khan.exe and kh.exe first — this script packages ALREADY-
;      BUILT binaries, it does not compile Khan itself (bundling a full
;      C toolchain just to build two binaries on install would defeat
;      the point of a simple double-click installer). From a Windows
;      machine with the same MSYS2/MinGW-w64 toolchain install.ps1 sets
;      up:
;          make
;      This produces khan.exe and kh.exe in the project root.
;   3. Open this file in Inno Setup (or run it via the command line:
;      "ISCC.exe khan-installer.iss"), with khan.exe/kh.exe/packages/
;      AND assets/khan-icon.ico (the project's icon — see that folder,
;      generated from assets/khan-icon.svg) sitting in the SAME
;      directory as this .iss file (adjust the
;      [Files] Source paths below if your layout differs).
;   4. Inno Setup produces khan-setup.exe in an Output\ subfolder —
;      THAT file is what gets distributed/attached to a GitHub Release
;      for people to download and double-click.
;
; NOT VERIFIED END-TO-END: this script was written carefully and
; reviewed against Inno Setup's documented behavior, but has not been
; run through the actual Inno Setup compiler or tested on a real
; Windows machine in the environment this was authored in (no such
; access there) — same class of caveat this project's CI workflow and
; install.ps1 already carry for their own Windows-specific pieces. If
; compiling or running this doesn't work as described, that's a bug —
; please open an issue with what happened.

#define KhanVersion "0.012.0"
#define KhanAppName "Khan"
#define KhanPublisher "Khan Language Project"
#define KhanURL "https://github.com/khandev1211-cpu/Khan"

[Setup]
; A fixed AppId (any valid GUID, generated once and never changed after
; the first real release) lets Inno Setup recognize "this is an
; upgrade of the same app" on future installer versions, rather than
; treating every version as an unrelated program. Generate your own via
; Tools > Generate GUID inside the Inno Setup IDE before a real release
; — this placeholder is fine for testing, but should be replaced once
; and then never touched again.
AppId={{B4A1E9C2-6F3D-4A8B-9E7C-2D5F8A9B1C3E}
AppName={#KhanAppName}
AppVersion={#KhanVersion}
AppPublisher={#KhanPublisher}
AppPublisherURL={#KhanURL}
AppSupportURL={#KhanURL}/issues
AppUpdatesURL={#KhanURL}/releases

; Per-user install by default (no admin prompt, installs under the
; current user's AppData) — matches the "just works, no elevation
; needed" spirit of install.ps1/install.sh. PrivilegesRequiredOverridesAllowed
; lets someone who WANTS a machine-wide install (e.g. a shared lab
; computer) choose that instead via a checkbox Inno Setup shows
; automatically, without forcing everyone through an admin prompt.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={autopf}\Khan
DefaultGroupName=Khan
DisableProgramGroupPage=yes

; Modern wizard look (matches recent Inno Setup's default styling,
; closer to what a 2020s-era installer is expected to look like rather
; than the classic blue-sidebar wizard from older Inno Setup defaults).
WizardStyle=modern

OutputDir=Output
OutputBaseFilename=khan-setup-{#KhanVersion}
; Both the installer .exe itself AND the shortcuts Inno Setup creates
; use this — without it, khan-setup.exe and its Start Menu/Desktop
; shortcuts all show Windows' generic blank-program icon.
SetupIconFile=assets\khan-icon.ico
Compression=lzma2
SolidCompression=yes

; Not code-signed — Windows SmartScreen will likely show an "unknown
; publisher" warning on first run until/unless this is signed with a
; real code-signing certificate, which costs money and isn't set up
; here. Stated plainly rather than surprising someone the first time
; they see that warning.
; SignTool=...

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Checked by default — for a language's own interpreter, being usable
; from any terminal right after install is the expected, helpful
; default (this mirrors the direction newer Python installers moved
; in; the option to uncheck it is still right there for anyone who'd
; rather manage PATH themselves).
Name: "addtopath"; Description: "Add Khan to PATH (recommended — lets you run 'khan' from any terminal)"; GroupDescription: "Setup options:"; Flags: checkedonce
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; Adjust these Source paths if khan.exe/kh.exe/packages aren't sitting
; directly next to this .iss file when you compile it — see the
; "HOW TO BUILD" comment at the top.
Source: "khan.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "kh.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "assets\khan-icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "packages\*"; DestDir: "{app}\packages"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "\.git\*"
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion; DestName: "README.txt"

[Icons]
Name: "{group}\Khan Interpreter"; Filename: "{app}\khan.exe"; IconFilename: "{app}\khan-icon.ico"; Comment: "Start the Khan interactive REPL"
Name: "{group}\Khan Documentation"; Filename: "{#KhanURL}"; IconFilename: "{app}\khan-icon.ico"
Name: "{group}\Uninstall Khan"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Khan"; Filename: "{app}\khan.exe"; IconFilename: "{app}\khan-icon.ico"; Tasks: desktopicon

[Run]
; Offers to open a REPL right when setup finishes — the installer
; equivalent of install.ps1/install.sh's own "here's how to verify it
; worked" step, just as a clickable checkbox instead of a printed
; command. Unchecked by default so setup doesn't unexpectedly pop a
; console window open for someone who just wanted the files installed.
Filename: "{app}\khan.exe"; Description: "Launch the Khan interactive REPL now"; Flags: postinstall nowait skipifsilent unchecked

[Code]
// ── PATH manipulation ──────────────────────────────────────────────
// Inno Setup has no built-in "add to PATH" feature — this is the
// well-established community pattern for it (reads/writes the
// per-user Environment registry key directly, the same key Python's
// own "Add to PATH" checkbox and install.ps1's PATH persistence both
// use — see install.ps1's own comment on why that specific key is
// the one that actually persists across new terminal windows, unlike
// $env:PATH-style session-only changes).

const
  EnvironmentKey = 'Environment';

function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  // Look for an exact, delimiter-bounded match rather than a plain
  // substring search — a substring check would wrongly treat
  // "C:\Khan2\bin" as already covering "C:\Khan\bin", or miss a
  // legitimate match sitting at the very start/end of the PATH string
  // (which has no leading/trailing semicolon to anchor against).
  Result := Pos(';' + ExpandConstant('{app}') + ';', ';' + OrigPath + ';') = 0;
end;

procedure AddToPath();
var
  OrigPath: string;
  NewPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', OrigPath) then
    OrigPath := '';

  if OrigPath = '' then
    NewPath := ExpandConstant('{app}')
  else if OrigPath[Length(OrigPath)] = ';' then
    NewPath := OrigPath + ExpandConstant('{app}')
  else
    NewPath := OrigPath + ';' + ExpandConstant('{app}');

  RegWriteStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', NewPath);
end;

procedure RemoveFromPath();
var
  OrigPath: string;
  NewPath: string;
  AppPath: string;
  P: integer;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', OrigPath) then
    exit;

  AppPath := ExpandConstant('{app}');
  NewPath := ';' + OrigPath + ';';
  P := Pos(';' + AppPath + ';', NewPath);
  if P > 0 then
  begin
    Delete(NewPath, P, Length(AppPath) + 1);
    // Trim the leading/trailing ';' we added above for the search,
    // so a PATH that was e.g. "C:\Khan;C:\Other" and had "C:\Khan"
    // removed ends up as "C:\Other", not ";C:\Other" or "C:\Other;".
    Delete(NewPath, 1, 1);
    if (Length(NewPath) > 0) and (NewPath[Length(NewPath)] = ';') then
      Delete(NewPath, Length(NewPath), 1);
    RegWriteStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', NewPath);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and IsTaskSelected('addtopath') then
  begin
    if NeedsAddPath('') then
      AddToPath();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath();
end;
