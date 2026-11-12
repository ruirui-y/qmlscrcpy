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
$binRoot = Join-Path $repoRoot 'build\windows-release\bin'
$stageRoot = Join-Path $PSScriptRoot 'stage'
$stageDir = Join-Path $stageRoot 'QmlScrcpy'

# Release preset 用的是 RelWithDebInfo 配置（见 CMakePresets.json），
# 所以不能写死 bin\Release，按"哪个配置目录里有 exe"来认
function Find-ReleaseDir {
    if (-not (Test-Path $binRoot)) {
        return $null
    }

    $found = @()
    foreach ($dir in Get-ChildItem $binRoot -Directory) {
        if (Test-Path (Join-Path $dir.FullName 'QmlScrcpy.exe')) {
            $found += $dir.FullName
        }
    }
    if ($found.Count -eq 0) {
        return $null
    }

    # 优先 RelWithDebInfo，其次 Release，最后其它
    $ordered = $found | Sort-Object {
        switch -Wildcard ($_) {
            '*RelWithDebInfo*' { 0 }
            '*Release*'        { 1 }
            default            { 2 }
        }
    }
    return $ordered[0]
}

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

$releaseDir = Find-ReleaseDir
if (-not $releaseDir)
{
    throw "没在 $binRoot 下找到 QmlScrcpy.exe，先跑一次 cmake --build --preset build-windows-release"
}
Write-Host "      使用产物目录：$releaseDir"

# 3. 清空 stage，拷 Release 产物
Write-Host '[3/5] 清空 stage 并拷贝 Release 产物 ...'
if (Test-Path $stageRoot) { Remove-Item $stageRoot -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
Copy-Item (Join-Path $releaseDir '*') $stageDir -Recurse -Force

# 4. 先把 pdb 和 exe 留一份到 Output\pdb，再清掉安装包里的
Write-Host '[4/5] 保留符号并清理运行期产物 ...'
$pdbDir = Join-Path $PSScriptRoot ('Output\pdb\' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
$pdbFiles = @(Get-ChildItem $releaseDir -Filter '*.pdb' -File -ErrorAction SilentlyContinue)
if ($pdbFiles.Count -gt 0)
{
    New-Item -ItemType Directory -Path $pdbDir -Force | Out-Null
    $pdbFiles | Copy-Item -Destination $pdbDir -Force
    # 分析 dump 要 exe 和 pdb 版本对得上，exe 一起存
    Copy-Item (Join-Path $releaseDir 'QmlScrcpy.exe') $pdbDir -Force
    Write-Host "      符号已留存：$pdbDir"
}
else
{
    Write-Host '      这次构建没有 pdb（Release 配置可能不带调试信息）'
}

# 安装包不该带我们的配置、日志、截图和调试符号
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
