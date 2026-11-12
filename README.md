# QmlScrcpy

用 Qt Quick 重写的安卓投屏客户端，协议对齐 scrcpy 3.x（`third_party/scrcpy-server` 固定 3.3.3）。

> 不是 [QtScrcpy](https://github.com/barry-ran/QtScrcpy) 的 fork。UI 和线程模型全部重写，
> 只沿用协议事实：12 字节包头、控制消息布局、keymap JSON 格式。

原生分辨率 1264×2776 实测 60fps，桌面端只负责 H.264 解码 + GPU 转 RGB，CPU 占用很低。

---

## 一、怎么用

### 1. 准备

| 需要什么 | 说明 |
|---|---|
| Qt 5.15.2 msvc2019_64 | 只影响编译，运行靠 `windeployqt` 拷出来的库 |
| `adb.exe` | 不入库。按顺序找：`SetAdbPath()` → `QS_ADB_PATH` → 程序目录 → `PATH` |
| 安卓设备 | 打开「开发者选项 → USB 调试」，插上数据线后在手机上点"允许调试" |

第一次用建议先手动确认 adb 能看见设备：

```powershell
adb devices
# List of devices attached
# e77121ed        device
```

### 2. 连接

**有线（最简单）**

1. 插数据线
2. 启动程序，点「刷新」→ `▾` 里会出现 `USB   e77121ed`
3. 点 `▾` 选中它（只把串号填进输入框）→ 点「连接」

> 程序启动时会自动连**上次用过的设备**，只要它出现在扫描结果里；扫不到就不连，不会白等。这个行为只发生一次。

**无线（不用插线、但第一次要插）**

1. 先按有线方式连上
2. 点 `▾` → 最下面一项「用 USB 设备开无线调试」
3. 程序执行三步：`adb tcpip 5555` → 问设备 IP（和本机同网段的优先）→ `adb connect IP:5555`
   - 这期间会主动断开当前连接（`tcpip` 会重启设备上的 adbd，不断开界面会报"视频连接出错"）
4. 输入框自动变成 `192.168.31.114:5555`，提示"可以拔线了，点「连接」"

之后每次启动都会自动连这台无线设备（只要它在线）。

**手动连**：直接在输入框里填 `串号` 或 `IP:端口`，点「连接」。

### 3. 投屏窗

程序只有两扇窗：**控制窗**（连接管理）和**投屏窗**（画面 + 工具栏）。

```
┌──────────────────────────────┬────────┐
│  e77121ed  60 fps  1264x2776 │  返回   │ ← 标题栏：设备名 + 状态
├──────────────────────────────┼────────┤
│                              │  任务   │
│                              │ 滑动 59 │
│        画面（1:1 比例）        │ −5 │ +5 │
│                              │ −1 │ +1 │
│                              │ 音量 +  │
│                              │ 音量 −  │
│                              │  截图   │
│                              │ 息屏/亮屏│
│                              │  置顶   │
│                              │  全屏   │
└──────────────────────────────┴────────┘
  ↑ 右下角把手可缩放
```

| 操作 | 怎么做 |
|---|---|
| 移动窗口 | 拖标题栏，或拖工具栏按钮之间的空地（光标变四向箭头） |
| 缩放窗口 | 拖右下角把手，或拖左上角 |
| 点按 / 长按 / 拖动 | 在画面上直接操作（鼠标事件转成安卓触摸） |
| 滚轮 | 默认发原生滚动事件；对不响应滚动的应用切「滑动」模式（模拟手指拖动） |
| 打字 | 直接敲键盘，含 `Shift + ?` 这类符号；中文输入法能出字 |
| 最小化 / 关闭 | 标题栏右侧两个小按钮；关闭只关画面，主窗还在 |
| 全屏 | 工具栏「全屏」，工具栏会自动收起，按 `Esc` 或再点「还原」回来 |

**滚轮两种模式**（工具栏「滚动/滑动」切换）

| 模式 | 发给设备什么 | 适合 |
|---|---|---|
| 滚动 | `INJECT_SCROLL` 原生滚动 | 微信、浏览器这类正常应用 |
| 滑动 | 按下 → 移动 N 像素 → 抬起 | 阅读器、小说、地图这类忽略滚动事件的应用 |

「滑动」模式下会出现 `−5/+5`（粗调）和 `−1/+1`（微调）两排，调的是"一格滚轮手指移动多少像素"，范围 10~300，改完立即生效并记住。

### 4. 工具栏按钮

| 按钮 | 做什么 | 光标 |
|---|---|---|
| 返回 | 返回上一级；设备息屏时还能点亮屏幕 | 手型 |
| 任务 | 打开最近任务列表（等同于手机方框键） | 手型 |
| 滚动/滑动 | 切滚轮模式，按钮上显示当前滑动像素 | 手型 |
| 音量 + / − | 设备音量加减一档 | 手型 |
| 截图 | 存到 `screenshots/`，**同时复制到剪贴板** | 手型 |
| 息屏/亮屏 | 只开关屏幕显示，不动锁屏 | 手型 |
| 置顶 | 投屏窗一直在最前面 | 手型 |
| 全屏 | 全屏 / 还原 | 手型 |

鼠标停在任意按钮上，**标题栏会变成一条蓝色提示条**说明这个按钮干什么；移开恢复成设备名 + 帧率 + 分辨率。

### 5. 配置文件在哪

| 文件 | 谁写 | 内容 |
|---|---|---|
| `config.ini` | 用户手改（程序只读） | 日志级别、采集参数、滚轮默认模式、默认滑动像素 |
| `window.ini` | 程序自己写 | 窗口位置尺寸、上次设备、命令行开关、当前滚轮模式 |

两个文件都在 exe 同目录。为什么分两个：`QSettings` 重写 `config.ini` 会把注释转义成 `%23%20…`，所以把人改的和程序写的分开。

```ini
# config.ini
[log]
level=info
to_file=true
keep_days=15

[input]
wheel_mode=scroll
drag_pixels=60

[video]
max_size=0
bit_rate=8000000
```

```ini
# window.ini（程序自动维护，一般不用手改）
[video_pos]
x=247
y=121

[video_window]
w=414
h=938

[video_window_landscape]
w=900
h=430

[control_window]
x=742
y=361
w=600
h=250

[input]
wheel_mode=drag
drag_pixels=59

[device]
last_serial=192.168.31.114:5555

[ui]
show_console=true
```

### 6. 环境变量（调试用）

| 变量 | 作用 |
|---|---|
| `QS_ADB_PATH` | 指定 adb.exe |
| `QS_DEVICE_SERIAL` | 启动时自动连这台（优先级低于"上次连的设备"） |
| `QS_SCRCPY_SERVER` | 指定 scrcpy-server 路径 |
| `QS_LOG_LEVEL` | 临时改日志级别（`debug` / `info` / `warn` / `error`） |
| `QS_PERF=1` | 每秒打印一次链路耗时（等数据 / 解码 / 上屏 / fps） |
| `QS_GRAB` | 截图到文件后退出（自检用） |
| `QS_FORWARD=1` | 用 `adb forward` 代替 `adb reverse` |

日志：`Log/LOG_yyyy_MM_dd.log`，保留 15 天。出问题先看这里。

---

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
