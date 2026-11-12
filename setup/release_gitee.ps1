# Gitee 发版脚本：建/更新发行版 + 上传安装包 + 更新 latest.json
#
# 用法：
#   $env:GITEE_TOKEN = '你的私人令牌'      # Gitee → 设置 → 私人令牌 → 勾 projects
#   powershell -ExecutionPolicy Bypass -File setup\release_gitee.ps1 -Version 0.1.5
#
# 前置条件：本地已打 tag 并推到 Gitee（git push gitee --tags）。
# 为什么用 curl 而不是 Invoke-RestMethod：PowerShell 5.1 发中文 JSON 时编码会被改成
# 本地代码页，Gitee 直接返回 400 Bad Request；curl 配合 --data-binary @文件（UTF-8 无 BOM）才稳。

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

# 把 JSON 写成 UTF-8 无 BOM，交给 curl --data-binary 发出去
function Invoke-GiteeJson([string]$method, [string]$url, $object) {
    $tmp = Join-Path $env:TEMP "gitee_req_$([guid]::NewGuid().ToString('N')).json"
    $json = $object | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText($tmp, $json, (New-Object System.Text.UTF8Encoding($false)))
    try {
        $response = curl.exe -s -X $method $url -H "Content-Type: application/json;charset=UTF-8" --data-binary "@$tmp"
    }
    finally {
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    }
    return $response
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

git -C $repo_root rev-parse --verify --quiet "refs/tags/$tag" | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "本地没有 tag $tag，先执行：git tag -a $tag -m `"QmlScrcpy $Version`""
}

$api_base = "https://gitee.com/api/v5/repos/$Owner/$Repo"
$release_name = "QmlScrcpy $Version"
$body_text = Get-Content -Raw -Encoding UTF8 $notes_path

Write-Host "[1/4] 查找发行版 $tag ..."
$release = $null
$list_raw = curl.exe -s --max-time 25 "$api_base/releases?access_token=$Token&per_page=100"
if ($list_raw -and $list_raw.Trim().StartsWith('[')) {
    $all = $list_raw | ConvertFrom-Json
    $release = $all | Where-Object { $_.tag_name -eq $tag } | Select-Object -First 1
}
# id 为空说明拿到的不是有效发行版，必须当"不存在"，否则附件会传到空 id 上
if ($release -and -not $release.id) {
    $release = $null
}

if ($release) {
    Write-Host "      已存在（id=$($release.id)），更新说明后复用"
    [void](Invoke-GiteeJson 'PATCH' "$api_base/releases/$($release.id)" @{
        access_token = $Token
        name         = $release_name
        body         = $body_text
    })
    Write-Host "[2/4] 说明已更新"
}
else {
    Write-Host "[2/4] 创建发行版 $tag ..."
    $created = Invoke-GiteeJson 'POST' "$api_base/releases" @{
        access_token     = $Token
        tag_name         = $tag
        target_commitish = 'master'
        name             = $release_name
        body             = $body_text
    }
    if ($created -notmatch '"id"') {
        Fail "创建发行版失败：$created"
    }
    $release = $created | ConvertFrom-Json
    Write-Host "      已创建（id=$($release.id)）"
}

Write-Host "[3/4] 上传安装包 ..."
$upload_url = "$api_base/releases/$($release.id)/attach_files?access_token=$Token"
$size_mb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
Write-Host "      $([System.IO.Path]::GetFileName($exe))（$size_mb MB）"
$upload_result = curl.exe -s -X POST $upload_url -F "file=@$exe"
if ($LASTEXITCODE -ne 0) {
    Fail "附件上传失败（curl 退出码 $LASTEXITCODE）"
}
# 成功返回附件对象（含 id / name / size）；失败返回 message
if ($upload_result -notmatch '"id"') {
    Fail "附件上传被拒绝：$upload_result"
}
Write-Host "      上传成功"

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

# 必须不带 BOM：Qt 的 QJsonDocument 不跳过 BOM，带 BOM 会直接解析失败
[System.IO.File]::WriteAllText($latest_path, ($latest | ConvertTo-Json -Depth 4),
                               (New-Object System.Text.UTF8Encoding($false)))

Write-Host ""
Write-Host "完成" -ForegroundColor Green
Write-Host "  发行版：https://gitee.com/$Owner/$Repo/releases/tag/$tag"
Write-Host "  下载页：https://gitee.com/$Owner/$Repo/releases"
Write-Host "  latest.json 已更新，记得提交并双推："
Write-Host "    git add latest.json && git commit -m `"chore(release): latest.json 指向 $Version`" && git push all master"
