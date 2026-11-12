# QmlScrcpy

用 Qt Quick 重写的安卓投屏客户端，协议对齐 scrcpy 3.x（`third_party/scrcpy-server` 固定 3.3.3）。

> 不是 [QtScrcpy](https://github.com/barry-ran/QtScrcpy) 的 fork。UI 和线程模型全部重写，
> 只沿用协议事实：12 字节包头、控制消息布局、keymap JSON 格式。

原生分辨率 1264×2776 实测 60fps，桌面端只负责 H.264 解码 + GPU 转 RGB，CPU 占用很低。

**只是来用的**：跳到 [一、使用指南](#一使用指南从零开始)，装好就能连，零配置。
**要读代码 / 自己构建**：看 [二、看代码从哪看起](#二看代码从哪看起) 和 [三、构建与测试](#三构建与测试)。

安装包：`setup\Output\QmlScrcpy_Setup_x.y.z.exe`（自带 Qt 运行库和 adb，装完即用）。

---

## 一、使用指南（从零开始）

### 0. 三分钟上手

```
① 手机上打开「USB 调试」
② 插上数据线，手机弹窗点「允许」（勾"一律允许"）
③ 打开程序 → 点「刷新」→ 点 ▾ 选设备 → 点「连接」
④ 投屏窗出现，鼠标键盘就能操作手机了
```

程序会记住上一次连的设备，**下次启动自动连**（约 1.5 秒），但前提是它还在线；扫不到就不连。

### 1. 手机端准备（每台新手机做一次）

**打开开发者选项**

1. 设置 → 关于手机 → 连续点「版本号」7 次，提示"已进入开发者模式"
2. 设置 → 系统 → 开发者选项 → 打开「USB 调试」

**小米 / 红米 / POCO 额外要开**

- 开发者选项里的「USB 调试（安全设置）」——不开这个，程序能看画面但**点击、滑动都不生效**
- 部分系统还需要登录小米账号、插卡，才能打开"安全设置"

**插线时手机上的弹窗**

```
是否允许 USB 调试？
□ 一律允许使用这台计算机进行调试     ← 勾上
          [取消]   [允许]
```

点「允许」；如果手机没弹窗，拔插一次数据线，或把 USB 模式从"仅充电"改成"文件传输"。

### 2. 有线连接（最简单，推荐第一次用这个）

| 步骤 | 看到什么 |
|---|---|
| 插数据线 | — |
| 打开程序 | 只有主窗，投屏窗还不出现 |
| 点「刷新」 | 状态栏显示"找到 N 台设备" |
| 点 `▾` | 弹出列表：`USB   e77121ed`、`无线  192.168.x.x:5555` |
| 选中一行 | 串号填进输入框（**不会自动连接**） |
| 点「连接」 | 状态栏"正在连接 …"，约 1.5 秒后投屏窗弹出 |

### 3. 无线连接（第一次必须插线，之后就不用）

```
① 先按有线方式连上（USB 线还插着）
② 点 ▾ → 最下面一项「用 USB 设备开无线调试」
③ 等 3~10 秒：程序自动执行
     adb tcpip 5555         让手机在 5555 端口开调试服务
     问手机 IP               优先取和电脑同网段那个
     adb connect IP:5555    连过去
④ 输入框变成 192.168.x.x:5555，状态栏提示"可以拔线了"
⑤ 拔线 → 点「连接」
```

之后每次启动都会自动连这台无线设备（手机和电脑要在同一个 WiFi）。

> 切无线时程序会**主动断开当前连接**——因为 `adb tcpip` 会重启手机上的 adbd，不断开的话界面会冒一句"视频连接出错"，看着像坏了。

**手动连**：输入框里直接填串号或 `IP:端口`，点「连接」。

### 4. 主窗导览

```
┌──────────────────────────────────────────────────────────┐
│ QmlScrcpy          [画质 8M·有线] [重置] [命令行]          │
├──────────────────────────────────────────────────────────┤
│ [e77121ed          ] [▾] [刷新] [连接] [断开]              │
├──────────────────────────────────────────────────────────┤
│ ● 已连接 RMX5010  1264x2776                              │
└──────────────────────────────────────────────────────────┘
```

| 控件 | 作用 |
|---|---|
| 输入框 | 手填串号 / `IP:端口` |
| `▾` | 打开设备菜单（在线设备 + 「用 USB 设备开无线调试」） |
| 刷新 | 重新扫描（`adb devices` + `adb mdns services`） |
| 连接 / 断开 | 建立 / 结束投屏 |
| 画质 | 调帧率和码率，改完自动重连 |
| 重置 | 恢复默认配置（窗口位置、上次设备、画质、命令行开关） |
| 命令行 | 显示/隐藏那个黑色控制台窗口（**默认隐藏**） |
| 底部圆点 | 绿色 = 已连接，灰色 = 未连接 |

### 5. 投屏窗怎么操作

```
┌──────────────────────────────┬────────┐
│ e77121ed  60 fps  1264x2776  │  返回   │ ← 标题栏：设备名 + 帧率 + 分辨率
├──────────────────────────────┼────────┤
│                              │  任务   │
│                              │ 滑动 59 │
│        手机画面               │ −5 │ +5 │
│      （1:1 像素比例）          │ −1 │ +1 │
│                              │ 音量 +  │
│                              │ 音量 −  │
│                              │  截图   │
│                              │ 息屏/亮屏│
│                              │  置顶   │
│                              │  全屏   │
│                              │  复位   │
└──────────────────────────────┴────────┘
  ↑ 右下角把手缩放
```

| 想做的事 | 怎么做 |
|---|---|
| 移动窗口 | 拖标题栏（光标变四向箭头），或拖工具栏按钮之间的空地 |
| 缩放窗口 | 拖右下角把手，或拖左上角 |
| 点一下 | 鼠标左键点画面 |
| 长按 | 按住不放 |
| 拖动/滑动 | 按住拖动鼠标 |
| 滚轮 | 默认发原生滚动；不响应滚动的应用切「滑动」模式 |
| 打字 | 直接敲键盘（英文、数字、标点、`Shift + ?` 这类都支持） |
| 返回键 | 工具栏「返回」 |
| 多任务 | 工具栏「任务」 |
| 截图 | 工具栏「截图」→ 存到 `screenshots\shot_日期_时间.png`，**同时复制到剪贴板**，可以直接粘贴 |
| 关屏但不锁屏 | 工具栏「息屏」；再点一次「亮屏」 |
| 窗口置顶 | 工具栏「置顶」 |
| 全屏 | 工具栏「全屏」；工具栏会保留在右侧，再点「还原」退出 |
| 窗口尺寸乱了 | 工具栏「复位」→ 恢复默认尺寸 |
| 最小化 / 关闭 | 标题栏右侧两个小按钮；关闭只关画面，主窗和进程还在 |

**鼠标停在任意工具栏按钮上**，标题栏会临时变成一条蓝色提示，说明这个按钮干什么。

### 6. 滚轮两种模式

| 模式 | 发什么给手机 | 适合 |
|---|---|---|
| 滚动（默认） | 原生滚动事件 | 微信、浏览器、设置列表 |
| 滑动 | 按下 → 移动 N 像素 → 抬起，模拟手指拖动 | 小说阅读器、地图、部分游戏 |

切到「滑动」后多出两排按钮：`−5/+5`（粗调）、`−1/+1`（微调），调的是"滚一格滚轮手指移动多少像素"，范围 10~300，改完立即生效并记住。

### 7. 画质怎么调

点「画质」→ 面板里填两个数 → 「应用并重连」（约 1.5 秒）：

| 项 | 填什么 | 说明 |
|---|---|---|
| 帧率上限 | 0 | 0 = 不限。**实测这项对观感影响很小，一般不用改** |
| 码率（Mbps） | 无线 6.0 / 有线 8.0 | 数字越大越清晰、越吃带宽 |

- **画面糊** → 码率调高（10、12）
- **无线卡顿/掉帧** → 码率调低（4、5），或换 5GHz 频段
- 有线和无线**各存一套**，互不影响；改完记在 `config\window.ini`

### 8. 配置文件在哪

程序目录下：

```
QmlScrcpy\
├─ QmlScrcpy.exe
├─ config\
│   ├─ config.ini      ← 你可以手改（注释保留）；默认码率等
│   └─ window.ini      ← 程序自己写：窗口位置、上次设备、画质
├─ screenshots\        ← 截图
├─ Log\                ← 日志，按天一个文件，留 15 天
└─ Tools\adb.exe       ← 安装包自带
```

改完 `config.ini` 重启生效。想全部回到默认：主窗点「重置」。

### 9. 常见问题

| 现象 | 原因 / 解决 |
|---|---|
| `▾` 列表空的、状态栏"没找到在线设备" | 数据线只充电不传数据；换线/换口；手机没点"允许 USB 调试"；先跑一次 `Tools\adb.exe devices` 看得到吗 |
| 连接失败，提示 timeout | 手机锁屏了、或另一台电脑上的 adb 占着 5037 端口（关掉扫码软件、手机助手、其它投屏工具） |
| 连上了但画面是黑的 | 重新点一次「连接」；或手机侧 scrcpy-server 被杀（重启程序即可） |
| 能看到画面，但点击没反应 | **小米/红米要开「USB 调试（安全设置）」**；或者画面坐标错位（点「复位」） |
| 打字没反应 | 点在手机上需要输入的位置再敲；**中文输入不支持**（只发按键码，需要中文请在手机输入法里打） |
| 无线连上后卡顿 | 画质里把码率降到 6M 以下；手机和电脑尽量连 5GHz；路由器双频合一会有干扰 |
| 画面糊、有色块 | 码率太低，画质里调高（8~12M） |
| 投屏窗跑到屏幕外，看不见了 | 主窗「重置」→ 重启程序；或用「复位」恢复尺寸 |
| 启动瞬间闪一下黑窗口 | exe 是控制台子系统，程序启动后会立刻隐藏；介意就用 `QS_HIDE_CONSOLE=ON` 重新构建 |
| Windows 提示"未知发布者" | 安装包没做代码签名，点「更多信息 → 仍要运行」 |
| 截图找不到 | 在程序目录的 `screenshots\` 下；同时已在剪贴板里，直接 Ctrl+V 也行 |

### 10. 命令行环境变量（调试用）

| 变量 | 作用 |
|---|---|
| `QS_ADB_PATH` | 指定 adb.exe（不用安装包自带的） |
| `QS_DEVICE_SERIAL` | 启动时自动连这台（优先级低于"上次连的设备"） |
| `QS_LOG_LEVEL` | 临时改日志级别（`debug` / `info` / `warn` / `error`） |
| `QS_PERF` | 设成 `0` 可关掉每秒一行 `[perf]` 统计 |
| `QS_SCRCPY_SERVER` | 指定 scrcpy-server 路径 |
| `QS_GRAB` | 截图到文件后退出（自检用） |
| `QS_FORWARD=1` | 用 `adb forward` 代替 `adb reverse` |
## 二、看代码从哪看起

### 三条主链路，挑一条顺着读

**① 画面链路**（想改渲染、性能从这条读）

```
src/main.cpp
  └─ qml/MainWindow.qml                 控制窗
       └─ qml/VideoWindow.qml           投屏窗（标题栏 + 画面 + 工具栏）
            └─ VideoSurface             = ui/video_item.h  (QQuickFramebufferObject)
                 ├─ VideoItem::Present  视频线程送帧进来（DirectConnection）
                 └─ YuvRenderer         渲染线程
                      ├─ synchronize()   只取数据，不碰 GL
                      └─ render()        3 张 GL_LUMINANCE 纹理 + BT.601 着色器 → FBO

libs/stream/video_reader.cpp            读 socket → 切包 → 丢给解码器
libs/stream/h264_decoder.cpp            FFmpeg 解出 YUV420P
libs/protocol/video_packet.cpp          12 字节包头：bit63=配置帧，bit62=关键帧，低 62 位=µs 时间戳
```

**② 连接链路**（想改 adb / 隧道从这条读）

```
src/device_controller.cpp::Connect
  └─ libs/session/server_session.cpp
       ├─ PushServer()          adb push scrcpy-server.jar /data/local/tmp/
       ├─ SetUpTunnel()         adb reverse tcp:27183 tcp:27183
       ├─ StartServerProcess()  adb shell CLASSPATH=... app_process ...
       ├─ AcceptForReverse()    本地 listen 27183，等 server 连回来
       ├─ ReadInfo()            读 76 字节握手：设备名 64 + 宽 2 + 高 2
       ├─ TakeVideoSocket()     裸视频流
       └─ TakeControlSocket()   控制消息
```

**③ 输入链路**（想改鼠标键盘从这条读）

```
ui/video_item.cpp  mousePressEvent / keyPressEvent ...
  └─ libs/input/coordinate_mapper.cpp   窗口坐标 → 设备坐标（含黑边偏移）
  └─ libs/input/touch_controller.cpp    按下 / 移动 / 抬起、滚动、滑动模拟
  └─ libs/input/key_controller.cpp      Qt::Key → 安卓 keycode，按住计数
  └─ libs/input/keycode_table.cpp       键码表、metaState（含 Shift 符号键）
       └─ libs/protocol/control_message.cpp  拼消息
            └─ DeviceController::SendControl  写控制 socket
```

### 目录结构

```
QmlScrcpy/
├─ CMakeLists.txt               版本号 + 找 Qt + 进子目录
├─ CMakePresets.json            VS 2022 生成器
├─ cmake/
│  ├─ CompilerOptions.cmake     INTERFACE 目标 qs_options，所有目标继承
│  ├─ Ffmpeg.cmake              vendored FFmpeg 的 include/lib + DLL 部署
│  └─ DeployQt.cmake            windeployqt + qml 模块落盘校验
├─ libs/                        include 根；源文件和头文件平铺，不分子目录
│  ├─ base/                     日志（按天分文件）、控制台窗口开关
│  ├─ protocol/                 包头、控制消息、设备消息（只依赖 QtCore）
│  ├─ adb/                      adb 参数拼装 + 进程执行 + 网段解析
│  ├─ session/                  server 参数、建链、隧道、握手
│  ├─ stream/                   读包、解码、帧所有权、耗时埋点
│  └─ input/                    坐标换算、触摸/滚轮/键盘消息
├─ ui/video_item.*              QQuickFramebufferObject + 三平面着色器
├─ src/                         设备控制器、窗口配置、main
├─ qml/                         控制窗（MainWindow）+ 投屏窗（VideoWindow）
├─ tests/                       QTest 单测，ctest 按名字跑
├─ docs/                        架构与各层文档
└─ third_party/                 FFmpeg 4.4 + scrcpy-server 3.3.3
```

### 线程模型（读代码前先记住这个）

```
GUI 线程          Qt 事件循环、QML、窗口
渲染线程          QQuickWindow 的 render thread：只在这里碰 GL
视频线程          Qt 线程：阻塞读 socket、解码，解完发 FrameReady（DirectConnection）
控制 socket 写    在 GUI 线程直接写（一次 14~32 字节，不会阻塞）
```

**「不要跨线程碰 GL」**和**「synchronize() 里不许调 GL」**是这个项目最容易踩的两条。

### 从哪开始改

| 想做的事 | 从哪下手 |
|---|---|
| 加一个工具栏按钮 | `qml/VideoWindow.qml` 加 `ToolButton` → `DeviceController` 加方法 → `libs/protocol/control_message.cpp` 拼消息 |
| 调整画质 / 分辨率 | `config.ini` 的 `[video]`，或 `libs/session/server_args.cpp` |
| 换渲染方式 | `ui/video_item.cpp` 的 `YuvRenderer` |
| 支持新控制消息 | `libs/protocol/control_message.h` 加构造 + `libs/protocol` 单测 |
| 改键盘映射 | `libs/input/keycode_table.cpp`，配 `tests/tst_key.cpp` 验证 |

写代码前建议先读 [架构总览](docs/架构总览.md)，里面有一帧数据的完整旅程。

---

## 三、构建与测试

```powershell
cmake --preset windows-debug
cmake --build --preset build-windows-debug
ctest --test-dir build\windows-debug -C Debug --output-on-failure
```

Qt 装在别处就改 `CMakePresets.json` 里 `CMAKE_PREFIX_PATH` 那一行。
构建后自动跑 `windeployqt`，输出目录自带 Qt 运行库，双击也能跑。

**CMake 选项**

| 选项 | 默认 | 作用 |
|---|---|---|
| `QS_HIDE_CONSOLE` | `OFF` | `ON` 时去掉控制台窗口（GUI 子系统），发布版打开 |
| `QS_DEPLOY_RUNTIME` | `ON` | 构建后拷 Qt 运行库 |

跑起来：

```powershell
Get-Process QmlScrcpy -ErrorAction SilentlyContinue | Stop-Process -Force   # 重新编译前先杀，否则 LNK1168
$env:QS_ADB_PATH="C:\path\to\adb.exe"
build\windows-debug\bin\Debug\QmlScrcpy.exe
```

真机用例默认跳过，要跑就补 `QS_ADB_PATH` 和 `QS_DEVICE_SERIAL`。
当前 9 个测试全绿：`protocol` / `adb` / `server_args` / `server_session` / `ffmpeg` / `stream` / `input` / `key` / `net_address`。

---

## 四、现状

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | CMake 骨架 + QML 窗口 | ✅ |
| P1 | 协议层：包头 / 控制消息 / 设备消息（23 项单测） | ✅ |
| P2 | adb 命令层 + 连接状态机（push → reverse → execute → readInfo） | ✅ 真机跑通 |
| P3 | 视频链路：socket → 解码 → GPU 三平面着色器 → FBO 上屏 | ✅ 真机跑通 |
| P4 | 输入层：坐标换算、触摸/滚轮/键盘、接线到控制 socket | ✅ |
| P5 | 日志（按天分文件、保留 15 天）+ INI 配置 | ✅ |
| P6 | 控制窗重做、标题显示设备、启动自动连上次设备 | ✅ |
| P7 | 在线设备列表、一键切无线调试、控制窗位置记忆 | ✅ |
| P8 | 横竖屏各自记窗口尺寸、扫描式设备列表 | ✅ |
| P9 | 符号键映射、缩放把手加粗、删掉不稳定的主窗/旋转按钮 | ✅ |
| P10 | 窗口解耦（transientParent）、状态归到标题栏 | ✅ |
| P11 | 工具栏并进投屏窗，只剩控制窗 + 投屏窗两扇 | ✅ |
| P12 | 主窗「命令行」开关，控制台显隐记进配置 | ✅ |
| P13 | 帧链路埋点 + 到帧即画、画质可调（有线/无线分档）、配置进 config 目录 | ✅ |
| P14 | 横竖屏位置分开记、横屏默认 1300×600、全屏保留工具栏、Inno Setup 打包 | ✅ |

## 已知限制

- **旋转设备**没做：scrcpy 的 `INJECT_ROTATE_DEVICE` 靠隐藏 API 反射，Android 15 会拦掉且不报错。消息格式是对的（1 字节 type=11），通道也通，纯粹是服务端注入失败
- **控制台开关**只在 `QS_HIDE_CONSOLE=OFF`（控制台子系统）下有效；换成 GUI 子系统后按钮变空操作
- 目前只支持单设备投屏，群控未做

---

## 五、文档

| 文档 | 内容 |
|---|---|
| [开发日志](docs/开发日志.md) | 每个阶段的六节记录：目标 / 做法 / 踩坑 / 定位 / 解决 / 收获 |
| [架构总览](docs/架构总览.md) | 模块划分、线程模型、一帧的旅程、设计取舍 |
| [协议层](docs/协议层.md) | 12 字节包头、控制消息、设备消息 |
| [连接层](docs/连接层.md) | adb reverse/forward、server 参数、76 字节握手 |
| [视频链路](docs/视频链路.md) | 切帧、解码、Frame 引用计数、耗时埋点 |
| [渲染方案](docs/渲染方案.md) | 三种渲染路径对比、为什么用 FBO、换分辨率重建纹理 |
| [输入层](docs/输入层.md) | 坐标换算公式、触摸/滚轮消息、键盘与符号键 |
| [主窗口](docs/主窗口.md) | 控制窗布局、按键与光标、状态文字来源 |
| [工具栏](docs/工具栏.md) | 为什么并进同一扇窗、按钮清单、滑动像素、提示条 |
| [窗口几何](docs/窗口几何.md) | 两扇窗的关系、横竖屏两套尺寸、防抖、换屏兜底 |
| [设备列表与无线调试](docs/设备列表与无线调试.md) | 在线设备、切无线四步、为什么先断开、怎么挑 IP |
| [自动连接](docs/自动连接.md) | 启动自动连上次设备、只做一次、存哪 |
| [命令行窗口](docs/命令行窗口.md) | 控制台开关、为什么用 ShowWindow 而不是 FreeConsole |
| [日志与配置](docs/日志与配置.md) | config.ini / window.ini / Log 目录、日志格式、排查四步 |
| [崩溃排查](docs/崩溃排查.md) | 怎么抓 dump、pdb 怎么对上、WinDbg/VS 怎么看栈、没 dump 时看日志 |
| [打包发布](docs/打包发布.md) | Inno Setup 打包、stage 收集、为什么不能装 Program Files |
| [构建与测试](docs/构建与测试.md) | 命令、环境变量、测试清单、环境坑 |

---

## 命名约定

- 文件：`video_packet.h` / `video_packet.cpp`，全小写下划线
- 类型：`VideoPacketHeader`，大驼峰；成员变量结尾带下划线
- include 一律从 `libs/` 写起：`#include "protocol/video_packet.h"`，
  一眼看出属于哪个模块，加新模块也不会撞名

## 协议来源

- [Genymobile/scrcpy](https://github.com/Genymobile/scrcpy) — 服务端与传输协议
- [barry-ran/QtScrcpy](https://github.com/barry-ran/QtScrcpy) — 早期客户端实现与踩坑记录

## 许可

Apache-2.0，见 `LICENSE`。
