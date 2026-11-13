; ============================================================
; QmlScrcpy 安装包脚本（Inno Setup 6）
; ------------------------------------------------------------
; 编译方式二选一：
;   1. 装好 Inno Setup 后，右键本文件 -> 「Compile」
;   2. 命令行：
;        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" setup\QmlScrcpy.iss
;
; 前置步骤：先跑 setup\collect.ps1，把 Release 产物收进 setup\stage\QmlScrcpy
;
; 说明：
;   - 源目录是 setup\stage\QmlScrcpy（exe + Qt 运行库 + plugins + scrcpy-server + 可选 adb）
;   - 默认装到 D:\QmlScrcpy：程序要往自己目录写 config/Log/screenshots，
;     装进 Program Files 会因为权限写不进去
; ============================================================

#define MyAppName "QmlScrcpy"
#define MyAppPublisher "QmlScrcpy"
#define MyAppExeName "QmlScrcpy.exe"
#define MyAppVersion "0.1.7"
#define MyAppId "6f1e2c40-5b0a-4a71-9c2e-3d8a51b7c9f4"
#define StageDir "stage\QmlScrcpy"
#define OutputDir "Output"

[Setup]
AppId={{{#MyAppId}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName=D:\QmlScrcpy
DefaultGroupName={#MyAppName}
DisableDirPage=no
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename={#MyAppName}_Setup_{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
PrivilegesRequired=admin
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoDescription={#MyAppName} 安卓投屏客户端
VersionInfoCompany={#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}
WizardStyle=modern
SetupIconFile=..\resources\app.ico

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务"; Flags: unchecked

[Files]
; 递归打包整个 stage 目录
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; 首次安装时如果没有 config.ini，装完不用管，程序自己会生成

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifdoesntexist

[UninstallDelete]
; 卸载时把这些运行期目录一并删掉，否则重装会带上旧配置
Type: filesandordirs; Name: "{app}\Log"
Type: filesandordirs; Name: "{app}\screenshots"
Type: filesandordirs; Name: "{app}\config"
