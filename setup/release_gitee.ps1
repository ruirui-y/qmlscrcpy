# Gitee 发版脚本：建发行版 + 上传安装包 + 更新 latest.json
#
# 用法：
#   $env:GITEE_TOKEN = '你的私人令牌'      # Gitee → 设置 → 私人令牌 → 勾 projects
#   powershell -ExecutionPolicy Bypass -File setup\release_gitee.ps1 -Version 0.1.5
#
# 前置条件：本地已经打了 tag 并推到 Gitee（git push gitee --tags），否则接口找不到 tag。

param(
    [Parameter(Mandatory = $true)][string]$Version,                                             # 例如 0.1.5，不带 v
    [string]$Token = $env:GITEE_TOKEN,
    [string]$Owner = 'bai-maxifeng',
    [string]$Repo = 'qmlscrcpy'
)

$ErrorActionPreference = 'Stop'
$repo_root = Split-Path -Parent $PSScriptRoot

function Fail([string]$message) {
    Write-Host "[失败] $message" -ForegroundColor Red
    exit 1
}

if ([string]::IsNullOrWhiteSpace($Token)) {
    Fail "没拿到 Gitee 令牌。先执行 `$env:GITEE_TOKEN = '你的私人令牌' 再跑本脚本。"
}

$tag = "v$Version"
$exe = Join-Path $repo_root "setup\Output\QmlScrcpy_Setup_$Version.exe"
$notes_path = Join-Path $repo_root "docs\release-notes\v$Version.md"
$latest_path = Join-Path $repo_root 'latest.json'

if (-not (Test-Path $exe)) {
    Fail "安装包不存在：$exe（先跑 setup\collect.ps1 和 setup\build_installer.bat）"
}
if (-not (Test-Path $notes_path)) {
    Fail "发布说明不存在：$notes_path"
}

# tag 必须已经在本地，否则建发行版会指向一个不存在的版本
git -C $repo_root rev-parse --verify --quiet "refs/tags/$tag" | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "本地没有 tag $tag，先执行：git tag -a $tag -m `"QmlScrcpy $Version`""
}

$api_base = "https://gitee.com/api/v5/repos/$Owner/$Repo"

Write-Host "[1/4] 检查发行版 $tag 是否已存在 ..."
$release = $null
try {
    $release = Invoke-RestMethod -Method Get -Uri "$api_base/releases/tags/$tag`?access_token=$Token" -TimeoutSec 20
    if ($release) {
        Write-Host "      已存在（id=$($release.id)），复用它上传附件"
    }
}
catch {
    Write-Host "      没找到，稍后新建"
    $release = $null
}

if ($null -eq $release) {
    Write-Host "[2/4] 创建发行版 $tag ..."
    $body_text = Get-Content -Raw -Encoding UTF8 $notes_path
    $payload = @{
        access_token     = $Token
        tag_name         = $tag
        target_commitish = 'master'
        name             = "QmlScrcpy $Version"
        body             = $body_text
    } | ConvertTo-Json -Depth 4

    try {
        $release = Invoke-RestMethod -Method Post -Uri "$api_base/releases" -Body $payload -ContentType 'application/json; charset=utf-8' -TimeoutSec 30
    }
    catch {
        Fail "创建发行版失败：$($_.Exception.Message)"
    }
}
else {
    Write-Host "[2/4] 复用已存在的发行版"
}

Write-Host "[3/4] 上传安装包 ..."
# multipart 用系统自带的 curl 更省事（PowerShell 5.1 传文件很别扭）
$upload_url = "$api_base/releases/$($release.id)/attach_files?access_token=$Token"
$size_mb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Write-Host "      $([System.IO.Path]::GetFileName($exe))（$size_mb MB）"
$upload_result = curl.exe -s -X POST $upload_url -F "file=@$exe"
if ($LASTEXITCODE -ne 0) {
    Fail "附件上传失败（curl 退出码 $LASTEXITCODE）"
}

# 上传成功返回文件信息；失败一般返回 error 字段
if ($upload_result -match '"error"') {
    Fail "附件上传被拒绝：$upload_result"
}

Write-Host "[4/4] 更新 latest.json ..."
$latest = Get-Content -Raw -Encoding UTF8 $latest_path | ConvertFrom-Json
$latest.latest = $Version
$latest.url = "https://gitee.com/$Owner/$Repo/releases"
$latest.published_at = (Get-Date -Format 'yyyy-MM-dd')

# 发布说明第一条非空正文行当备注（跳过标题和空行）
$note_line = $null
foreach ($line in (Get-Content -Encoding UTF8 $notes_path)) {
    $trimmed = $line.Trim()
    if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) {
        continue
    }
    $note_line = $trimmed
    break
}
if ($note_line) {
    $latest.notes = $note_line
}

($latest | ConvertTo-Json -Depth 4) | Set-Content -Encoding UTF8 $latest_path

Write-Host ""
Write-Host "完成" -ForegroundColor Green
Write-Host "  发行版：https://gitee.com/$Owner/$Repo/releases/tag/$tag"
Write-Host "  下载页：https://gitee.com/$Owner/$Repo/releases"
Write-Host "  latest.json 已更新，记得提交并双推："
Write-Host "    git add latest.json && git commit -m `"chore(release): latest.json 指向 $Version`" && git push all master"
