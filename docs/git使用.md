# Git 使用文档

记录这个仓库的分支模型、常见操作和踩过的坑。面向"只在这个项目里用 git"的场景，不追求完整。

## 一、分支模型

```
master            主线：只放"能跑、能发布"的代码
exp/xxx           实验分支：能做失败尝试，可以长期不合
tag v0.x.x        发布点：每个版本一个
tag exp/xxx       实验存档点：分支被删了也能找回来
```

### 当前状态（2026-11-18）

| 引用 | 指向 | 说明 |
|---|---|---|
| `master` | `3439854` | 渲染后端还是 OpenGL，硬解走 CPU 读回 |
| `exp/zero-copy` | `f45e16d` | D3D11 RHI 改造 + 零拷贝三次尝试（失败，保留） |
| `tag exp/zero-copy-rhi-d3d11` | `f45e16d` | 实验存档点 |
| `tag v0.3.0` | 发布提交 | Gitee 安装包用的版本 |

## 二、核心概念：提交不会消失，只是"指针"移动

```
提交是一条链，每个提交只指向自己的父提交：

  146 ──147 ──148 ──… ──153 ──154
   │                            │
 3439854                    f45e16d

分支 = 指向某个提交的"标签"：

  master        → 154
  exp/zero-copy → 154
```

`git reset --hard 3439854` 之后：

```
  master        → 146   （指针往回挪）
  exp/zero-copy → 154   （还指着最新）
```

**147~154 一个都没消失**，因为 `exp/zero-copy` 还指着它们。只要有一个分支或标签指向某个提交，git 就认为它"可达"，永远不会被清理。

### 所以"改历史"到底改了什么

| 操作 | 实际发生 | 风险 |
|---|---|---|
| `git reset --hard <commit>` | 本地分支指针往回挪 | 无，`git reflog` 能找回 |
| `git push --force` | **远程**分支指针也被挪回去 | 远程若有别人依赖，会看不到回退后的提交 |

改的是**分支指向**，不是提交内容。

## 三、保住实验 + 回退主线的正确顺序

```
① 先把实验存成分支并推上去        ← 关键，顺序不能反
② 再 reset 主线 + 强推
```

### 实际用过的命令

```powershell
# ① 建分支 + 打标签，两端都推
git branch exp/zero-copy
git tag -f exp/zero-copy-rhi-d3d11 f45e16d
git push gitee exp/zero-copy
git push gitee exp/zero-copy-rhi-d3d11
git push origin exp/zero-copy
git push origin exp/zero-copy-rhi-d3d11

# ② 主线回退
git reset --hard 3439854
git push gitee master --force
git push origin master --force
```

### 为什么顺序不能反

如果先强推 master、而 exp 分支还没推：

```
远程 master  → 146
远程没有分支指向 147~154
                 ↓
        这几个提交变成"悬空提交"
                 ↓
        服务器 GC 时可能被清掉
```

## 四、日常操作

### 在实验分支上继续干活

```powershell
git checkout exp/zero-copy
# 改代码、提交
git push gitee exp/zero-copy
```

### 回主线

```powershell
git checkout master
# 注意：切换分支后必须重新编译
cmake --preset qt6-debug
cmake --build build/qt6-debug --config Debug
```

**踩过的坑**：`build/qt6-debug/bin/Debug/QmlScrcpy.exe` 是上一次编译的产物，切了分支不重编，跑的还是旧代码（曾经因此误判"绿屏"是主线问题）。

### 把主线的更新合到实验分支

```powershell
git checkout exp/zero-copy
git merge master
```

### 看两条线的关系

```powershell
git log --oneline --all --graph
git branch -vv                 # 每个分支指向哪个提交、跟踪哪个远程
```

## 五、出错怎么救

| 情况 | 命令 |
|---|---|
| 刚 reset 错了 | `git reflog` 找到原提交，`git reset --hard <那个 commit>` |
| 误删分支 | `git reflog` 或 `git fsck --lost-found` 找回 |
| 想回到某个发布点 | `git checkout v0.3.0`（只读查看）或 `git reset --hard v0.3.0`（移动分支） |
| 工作区改乱了想全丢 | `git checkout -- .`（丢弃未提交改动） |
| 想暂存不想要 | `git stash` → 之后 `git stash pop` |

**`reflog` 是保命符**：本地所有引用变动都记在 `git reflog` 里，默认保留 90 天。

## 六、这个项目踩过的 git 坑

### 坑 1：以为"回退分支"等于"删代码"

第一次讨论回退时以为会丢实验，其实只要先建分支，代码一条都不会少。

### 坑 2：切了分支没重编

`master` 回退到 `3439854`（OpenGL 后端）后，`build/qt6-debug` 里还是 exp 分支编译的 D3D11 版本。直接跑会得到和当前源码不符的结果。

**规则：任何分支切换之后，先重新编译再判断现象。**

### 坑 3：强推远程的风险

`git push --force` 会改写远程分支。这个项目只有一个人开发，所以可接受；多人协作时应该用 `--force-with-lease`，并且优先考虑"新建分支"而不是移动已有分支。

### 坑 4：提交超大二进制

`third_party/ffmpeg` 有 ~83 MB（139 个文件），`Tools/` 有 6 MB。每次 clone 都要拉。
以后如果要瘦身：把 `third_party/ffmpeg` 加进 `.gitignore`，用脚本下载固定版本。

## 七、Draft PR 的用途

实验分支推上去后，GitHub 会提示"要不要开 PR"。开一个 **Draft（草稿）PR** 的好处：

| 好处 | 说明 |
|---|---|
| 有专门的讨论区 | 可以在里面记笔记、贴失败日志 |
| 随时看差异 | PR 页面直接显示 exp 和 master 的文件差异 |
| 不会误合并 | Draft 状态没有"合并"按钮 |
| 分支不会被忘掉 | PR 列表里一直能看到 |

创建方式：点绿色按钮右边的下拉箭头 → **创建拉取请求草案**。

研究完想合并：点「准备审阅」→ 变成普通 PR → 再合并。

## 八、一句话总结

```
提交只有一份，分支只是"从哪往回看"
先建分支再回退，顺序反了才危险
切分支必重编，否则看到的不是你以为的代码
```
