# 收集发布文件：把 Release 构建产物收拾成 setup/stage/QmlScrcpy
#
# 用法（在仓库根目录跑）：
#   powershell -ExecutionPolicy Bypass -File setup\collect.ps1
#   powershell -ExecutionPolicy Bypass -File setup\collect.ps1 -SkipBuild
#
# 做完这一步，再跑 setup\build_installer.bat 生成安装包。

param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$releaseDir = Join-Path $repoRoot 'build\windows-release\bin\Release'
$stageRoot = Join-Path $PSScriptRoot 'stage'
$stageDir = Join-Path $stageRoot 'QmlScrcpy'

# 1. 构建（默认重新构建，避免打包到旧 exe）
if (-not $SkipBuild)
{
    Write-Host '[1/5] 配置 Release ...'
    cmake --preset windows-release
    if ($LASTEXITCODE -ne 0) { throw 'cmake --preset windows-release 失败' }

    Write-Host '[2/5] 构建 Release ...'
    cmake --build --preset build-windows-release
    if ($LASTEXITCODE -ne 0) { throw 'cmake --build --preset build-windows-release 失败' }
}
else
{
    Write-Host '[1/5] 跳过构建'
    Write-Host '[2/5] 跳过构建'
}

if (-not (Test-Path (Join-Path $releaseDir 'QmlScrcpy.exe')))
{
    throw "没找到 $releaseDir\QmlScrcpy.exe，先构建 Release"
}

# 3. 清空 stage，拷 Release 产物
Write-Host '[3/5] 清空 stage 并拷贝 Release 产物 ...'
if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
Copy-Item (Join-Path $releaseDir '*') $stageDir -Recurse -Force

# 4. 删掉运行期产物和调试符号：安装包不该带我们的配置、日志和截图
Write-Host '[4/5] 清理运行期产物 ...'
foreach ($name in @('Log', 'screenshots', 'config', 'QmlScrcpy.pdb'))
{
    $target = Join-Path $stageDir $name
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
}
Get-ChildItem $stageDir -Filter '*.pdb' -File | Remove-Item -Force
Get-ChildItem $stageDir -Filter 'userdata.ini.*' -File | Remove-Item -Force

# 5. 捎上 adb：没有 adb 的用户装完就能直接连
Write-Host '[5/5] 找 adb ...'
$adbPath = $null
if ($env:QS_ADB_PATH -and (Test-Path $env:QS_ADB_PATH))
{
    $adbPath = $env:QS_ADB_PATH
}
else
{
    $onPath = Get-Command adb.exe -ErrorAction SilentlyContinue
    if ($onPath) { $adbPath = $onPath.Source }
}

if ($adbPath)
{
    $toolsDir = Join-Path $stageDir 'Tools'
    New-Item -ItemType Directory -Path $toolsDir -Force | Out-Null
    Copy-Item $adbPath $toolsDir -Force
    # adb 依赖这两个 DLL，同一个目录里一般都有
    foreach ($dll in @('AdbWinApi.dll', 'AdbWinUsbApi.dll'))
    {
        $source = Join-Path (Split-Path $adbPath -Parent) $dll
        if (Test-Path $source) { Copy-Item $source $toolsDir -Force }
    }
    Write-Host "      已带上 adb：$adbPath"
}
else
{
    Write-Warning '没找到 adb.exe：安装包不带 adb，用户需要自己设 QS_ADB_PATH 或装 platform-tools'
}

$size = (Get-ChildItem $stageDir -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ''
Write-Host ("完成：$stageDir（{0:N1} MB）" -f $size)
Write-Host '下一步：setup\build_installer.bat'
