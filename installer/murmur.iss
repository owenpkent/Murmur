; Murmur installer (Inno Setup 6). Build via: .\build.ps1 -Installer
; The auto-updater requires the output filename to be exactly MurmurSetup-{version}.exe.

#ifndef Version
  #define Version "0.1.0"
#endif

[Setup]
AppId={{5D3F8A21-7C94-4E60-B1A7-2E9C0D46F813}
AppName=Murmur
AppVersion={#Version}
AppPublisher=OK Studio
AppPublisherURL=https://github.com/owenpkent/Murmur

; {app} is the VST3 *folder*; the bundle is installed into it as Murmur.vst3.
; UsePreviousAppDir stays off so a remembered {app} can never nest the bundle inside
; an older one (...\VST3\Murmur.vst3\Murmur.vst3), which no host would ever find.
;
; The default is the canonical location every VST3 host scans. It is a default
; rather than a fixed destination because a DAW can be pointed at a custom VST3
; folder instead (Ableton's "VST3 Plug-In Custom Folder" is the common case),
; and an installer that cannot reach it is one that installs where the user
; cannot load from.
DefaultDirName={commoncf64}\VST3
UsePreviousAppDir=no
; The VST3 folder always already exists, so Inno's "that folder exists, install
; anyway?" prompt would fire on the default path every single time.
DirExistsWarning=no

DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\release
OutputBaseFilename=MurmurSetup-{#Version}

; Branding. All three are generated from assets/Murmur.svg by
; installer/generate_brand.py and committed, so a build (and CI) never needs
; Pillow - rerun the script only when the mark itself changes.
SetupIconFile=..\assets\Murmur.ico
WizardStyle=modern
WizardImageFile=wizard_large.bmp
WizardSmallImageFile=wizard_small.bmp
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
; If the DAW has Murmur loaded, Inno's restart-manager page asks the user to close it.
; We never kill a DAW (unsaved projects). No silent-install support for the same reason.
CloseApplications=yes
RestartApplications=no
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
UninstallDisplayName=Murmur VST3
UninstallDisplayIcon={app}\Murmur.vst3\Contents\x86_64-win\Murmur.vst3

[Files]
Source: "..\build\Murmur_artefacts\Release\VST3\Murmur.vst3\*"; DestDir: "{app}\Murmur.vst3"; Flags: ignoreversion recursesubdirs

[UninstallDelete]
; The bundle is a folder Setup created inside {app}; remove it whole rather than
; leaving the empty Contents tree behind.
Type: filesandordirs; Name: "{app}\Murmur.vst3"

[Messages]
SetupWindowTitle=Murmur %1 Setup
WizardSelectDir=Select VST3 folder
SelectDirDesc=Where should the Murmur plug-in be installed?
SelectDirLabel3=Setup will install Murmur.vst3 into the folder below. The default is the standard VST3 location, which every host scans. If your DAW is set to scan a custom VST3 folder, point Setup at that folder instead.
SelectDirBrowseLabel=To continue, click Next. To choose a different folder, click Browse.
