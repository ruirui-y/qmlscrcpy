@echo off
rem 生成安装包：先跑 collect.ps1 收好发布文件，再调 Inno Setup 编译
setlocal

set "ROOT=%~dp0"
set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"

if not exist "%ISCC%" (
    echo [错误] 没找到 Inno Setup 6 的 ISCC.exe
    echo        下载安装：https://jrsoftware.org/isdl.php
    pause
    exit /b 1
)

echo 正在编译：%ROOT%QmlScrcpy.iss
"%ISCC%" "%ROOT%QmlScrcpy.iss"
if errorlevel 1 (
    echo.
    echo [错误] 编译失败，看上面的 Inno Setup 输出
    pause
    exit /b 1
)

echo.
echo 完成：%ROOT%Output
pause