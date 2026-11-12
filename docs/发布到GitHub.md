# 发布到 GitHub


安装包通过 **GitHub Releases** 发布，不 commit 进仓库（45 MB 的二进制进 git 历史会让仓库永久膨胀，而且 clone 会变慢）。

## 1. 发版的完整流程

```
① 改版本号（三处必须一致）
   ② 打包（collect.ps1 → build_installer.bat）
   ③ 提交 + 打 tag + 推 tag
   ④ 创建 Release 并上传 exe
```

### ① 改版本号（三处）

| 文件 | 位置 |
|---|---|
| `CMakeLists.txt` | `project(QmlScrcpy VERSION 0.1.0 ...)` |
| `setup\QmlScrcpy.iss` | `#define MyAppVersion "0.1.0"` |
| tag | `v0.1.0` |

三处不一致的后果：exe 属性里显示一个版本、安装包文件名另一个、Release 页面第三个。

### ② 打包

```powershell
powershell -ExecutionPolicy Bypass -File setup\collect.ps1
setup\build_installer.bat
```

产物：`setup\Output\QmlScrcpy_Setup_0.1.0.exe`（同时 `setup\Output\pdb\<时间戳>` 里留了符号，见 [打包发布](打包发布.md)）。

### ③ 提交 + 打 tag

```powershell
git add -A
git commit -m "chore: 版本号升到 0.1.0"
git push

git tag -a v0.1.0 -m "QmlScrcpy 0.1.0"
git push origin v0.1.0
```

### ④ 创建 Release

**推荐：gh CLI**（装一次就够）

```powershell
winget install --id GitHub.cli
# 装完重开终端，否则 gh 不在 PATH 里
gh auth login          # GitHub.com → HTTPS → Login with a web browser → 复制设备码去网页授权
```

发版：

```powershell
# cmd 里必须写成一行（反引号续行只有 PowerShell 认）
gh release create v0.1.0 "H:\YJJ\Project\QmlScrcpy\setup\Output\QmlScrcpy_Setup_0.1.0.exe" --title "QmlScrcpy 0.1.0" --notes-file "H:\YJJ\Project\QmlScrcpy\docs\release-notes\v0.1.0.md"
```

说明文字放在 `docs\release-notes\v0.1.0.md` 里，随仓库走，改起来比命令行内联方便。

**或者网页手动**：仓库 → Releases → *Draft a new release* → 选 tag → 填标题、贴说明 → 把 exe 拖进 *Attach binaries* → *Publish release*。

## 2. 发布后的地址

```
发布页    https://github.com/ruirui-y/qmlscrcpy/releases
最新版    https://github.com/ruirui-y/qmlscrcpy/releases/latest
直链      https://github.com/ruirui-y/qmlscrcpy/releases/download/v0.1.0/QmlScrcpy_Setup_0.1.0.exe
```

直链可以直接贴给别人，点开就是下载。

## 3. 为什么不用别的办法

| 做法 | 为什么不 |
|---|---|
| 把 exe commit 进仓库 | git 历史永久保留每个版本，clone 越来越慢；GitHub 单文件超 100 MB 直接拒收 |
| Git LFS 存 exe | LFS 有流量配额，且用户 clone 时要额外拉 LFS |
| GitHub Packages | 面向容器/npm 这类包管理器，桌面安装包用 Releases 更贴合用户习惯 |

Release 的 asset 不计入仓库体积，也没有 LFS 流量限制（单文件上限 2 GB）。

## 4. 常见问题

| 报错 | 原因 / 解决 |
|---|---|
| `no matches found for ``` | 在 **cmd** 里用了 PowerShell 的反引号续行。cmd 里把命令写成一行，或改用 PowerShell |
| `'--title' 不是内部或外部命令` | 同上，续行没生效，后半行被当成新命令 |
| `gh : 无法将"gh"项识别为...` | 刚装完没重开终端，PATH 没刷新；或直接用完整路径 `"C:\Program Files\GitHub CLI\gh.exe"` |
| `HTTP 422: Validation Failed (tag_name already exists)` | tag 已存在（重复发版）。要么换个版本号，要么给已有 tag 补 Release：`gh release create v0.1.0 --verify-tag ...` |
| `authentication failed` | `gh auth login` 没走完，重跑一次 |
| 上传卡住 / 中断 | 45 MB 一般十几秒；卡住就 `Ctrl+C` 重跑，`gh` 会重建 Release（先 `gh release delete v0.1.0`） |

## 5. 发版检查清单

- [ ] 三处版本号一致（CMakeLists / iss / tag）
- [ ] `config\`、`Log\`、`screenshots\` 没有被打进安装包（`collect.ps1` 自动清理）
- [ ] `Tools\adb.exe` 在 stage 里（`collect.ps1` 会打印"已带上 adb"）
- [ ] 安装包图标正常（文件管理器里能看到手机图标）
- [ ] 全新安装后：只有主窗、无黑窗口、能扫到设备、能连上
- [ ] Release 说明里写清"装到 D 盘，别装 Program Files"
- [ ] 已知限制写进去（中文输入、小米安全设置、无签名）

## 6. 以后想自动化（可选）

可以加 `.github/workflows/release.yml`：push tag 时在 `windows-latest` 上跑 CMake + 打包 + 上传。代价是要在 CI 里装 Qt 5.15.2（官方安装器需要账号，一般用 `aqtinstall` 装），首次配置约半天，而且 Release 的说明文字还是要手写。

**当前阶段建议手动发版**：一次 2 分钟，且能人工确认装出来的包没问题。

## 别忘了改 latest.json

发完 Release 还要改仓库根目录的 `latest.json`，否则客户端永远收不到更新提示：

```json
{
  "latest": "0.1.5",
  "minimum": "0.1.0",
  "url": "https://github.com/ruirui-y/qmlscrcpy/releases/latest",
  "notes": "这次改了什么",
  "published_at": "2026-11-13"
}
```

改完 push 就生效（Gitee 源要同步推一份到 `gitee.com/bai-maxifeng/qmlscrcpy`）。
`minimum` 决定"必须更新"的门槛，`latest` 决定"有新版"的提示。

## 现在的主流程：代码 GitHub，安装包 Gitee

```
① 改 CMakeLists 版本号 → setup\collect.ps1 → setup\build_installer.bat
② git commit；git push all master
③ git tag -a v0.1.5 -m "QmlScrcpy 0.1.5"；git push all v0.1.5
④ $env:GITEE_TOKEN = '私人令牌'
   powershell -ExecutionPolicy Bypass -File setup\release_gitee.ps1 -Version 0.1.5
⑤ git add latest.json && git commit -m "chore(release): latest.json 指向 0.1.5" && git push all master
⑥ （可选）GitHub 上只发说明、不传 exe：
   gh release create v0.1.5 --title "QmlScrcpy 0.1.5" --notes-file docs\release-notes\v0.1.5.md
```

`all` 是双推远程：`git remote set-url --add --push all https://gitee.com/bai-maxifeng/qmlscrcpy.git`。

## release_gitee.ps1 做什么

| 步骤 | 内容 |
|---|---|
| 1 | 查 `releases/tags/vX`，已有就复用（可重复跑，不会报"已存在"） |
| 2 | 用发布说明当正文建发行版 |
| 3 | `curl -F file=@安装包` 传附件（PS 5.1 传 multipart 太别扭） |
| 4 | 改 `latest.json` 的 `latest` / `url` / `notes` / `published_at` |

**令牌**：Gitee → 设置 → 私人令牌 → 勾 `projects`。只放在环境变量 `GITEE_TOKEN`，不要写进任何文件。

**tag 必须先推上去**，否则第 2 步会指向一个不存在的版本。

### 令牌放哪

只放在被 git 忽略的本地文件里，别写进仓库、别写进脚本：

```powershell
# .refs/ 已在 .gitignore 里
Set-Content -Path .refs\gitee_token.txt -Value '你的私人令牌'

$env:GITEE_TOKEN = (Get-Content -Raw .refs\gitee_token.txt).Trim()
powershell -ExecutionPolicy Bypass -File setup\release_gitee.ps1 -Version 0.1.5
```

### 脚本踩过的两个坑

| 坑 | 现象 | 解法 |
|---|---|---|
| PowerShell 发中文 JSON | `400 Bad Request` | 改用 `curl --data-binary @文件`（UTF-8 无 BOM） |
| 用 `/releases/tags/{tag}` 查发行版 | 返回 `null`，脚本当成"已存在"，附件传到空 id | Gitee 没这个端点，改拉列表按 `tag_name` 匹配，并校验 `id` 非空 |
| `Set-Content -Encoding UTF8` 写 latest.json | 带 BOM，客户端 JSON 解析失败 | 改 `[IO.File]::WriteAllText` + `UTF8Encoding($false)` |

> **现在的主流程见 [双仓库发版](双仓库发版.md)**：代码 GitHub、安装包 Gitee，这篇留作历史参考。