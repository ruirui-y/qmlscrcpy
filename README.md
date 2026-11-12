# QmlScrcpy

用 Qt Quick 重写的安卓投屏客户端，协议对齐 scrcpy 3.x（`third_party/scrcpy-server` 固定 3.3.3）。

> 不是 [QtScrcpy](https://github.com/barry-ran/QtScrcpy) 的 fork。UI 和线程模型全部重写，
> 只沿用协议事实：12 字节包头、控制消息布局、keymap JSON 格式。

## 现状

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | CMake 骨架 + QML 窗口 | ✅ |
| P1 | 协议层：包头 / 控制消息 / 设备消息（23 项单测） | ✅ |
| P2a | adb 命令层：参数拼装 + 进程执行（14 项单测） | ✅ |
| P2b | 连接状态机：push → reverse/forward → execute → readInfo | ✅ 真机跑通 |
| P3 | 视频链路：socket → 解码 → GPU 三平面着色器 → FBO 上屏 | ✅ 真机跑通 |
| P4a | 输入层：坐标换算 + 触摸/滚轮消息（5 项单测） | ✅ |
| P4b | 输入层：键盘映射 + metaState + 自动重复（7 项单测） | ✅ |
| P4c | 接线：画布收鼠标键盘 → 控制 socket | ✅ |
| P4d | 日志模块（按天分文件、保留 15 天）+ INI 配置 | ✅ |
| P4e | 独立工具栏窗口：滚轮双模式 / 音量 / 旋转 / 截图 / 息屏 / 置顶 / 全屏 | ✅ |

已经能用的功能：

- 设备列表 / 连接 / 断开
- 独立投屏窗（无边框、自绘标题栏、左上右下都能缩放、位置尺寸记忆）
- 原生分辨率 + GPU 转 RGB，实测 60fps，CPU 只剩 H.264 解码
- 拖动窗口时画面不冻、不积帧
- 输入层接好了：投屏窗里点按拖动滚轮、敲键盘都会送到设备
- 右侧独立工具栏：返回、菜单、滚轮模式（滚动/滑动，滑动像素在工具栏上 ± 调、立即生效）、音量、旋转、截图（存盘+进剪贴板）、息屏/亮屏、置顶、全屏；拖工具栏等于拖投屏窗

## 技术栈

| 项 | 选择 |
|---|---|
| 语言 | C++17 |
| UI | Qt Quick / QML（Qt 5.15.2 msvc2019_64） |
| 构建 | CMake ≥ 3.22 + CMakePresets |
| 编译器 | MSVC 2022（VS 17） |
| 视频 | FFmpeg 4.4（解码）+ OpenGL 三平面着色器（转 RGB） |

不用 Qt6 的原因：5.15 已满足全部需求，并且 `QWindow::startSystemMove()` 正是 5.15 才有的。

## 文档

| 文档 | 内容 |
|---|---|
| [开发日志](docs/开发日志.md) | 每个阶段的六节记录：目标 / 做法 / 踩坑 / 定位 / 解决 / 收获 |
| [架构总览](docs/架构总览.md) | 模块划分、线程模型、一帧的旅程、设计取舍 |
| [协议层](docs/协议层.md) | 12 字节包头、12 种控制消息、设备消息 |
| [连接层](docs/连接层.md) | adb reverse/forward、server 参数、76 字节握手 |
| [视频链路](docs/视频链路.md) | 切帧、解码、Frame 引用计数、耗时埋点 |
| [渲染方案](docs/渲染方案.md) | 三种渲染路径对比、为什么用 FBO、踩坑记录 |
| [输入层](docs/输入层.md) | 坐标换算公式、触摸/滚轮消息、键盘（进行中） |
| [日志与配置](docs/日志与配置.md) | config.ini / window.ini / Log 目录、日志格式、输入排查四步 |
| [构建与测试](docs/构建与测试.md) | 命令、环境变量、测试清单、环境坑 |

## 构建与测试

```powershell
cmake --preset windows-debug
cmake --build --preset build-windows-debug
ctest --test-dir build\windows-debug -C Debug --output-on-failure
```

Qt 装在别处就改 `CMakePresets.json` 里 `CMAKE_PREFIX_PATH` 那一行。
构建后自动跑 `windeployqt`，输出目录自带 Qt 运行库，双击也能跑。

`adb.exe` 不入库（5.8 MB，Google platform-tools 随时下）。运行时按顺序找：
`SetAdbPath()` → 环境变量 `QS_ADB_PATH` → 程序目录 → PATH。

## 运行

```powershell
$env:QS_ADB_PATH="C:\path\to\adb.exe"
$env:QS_DEVICE_SERIAL="192.168.31.115:5555"   # 可选，设了就自动连
$env:QS_PERF="1"                              # 可选，打印每秒链路耗时
$env:QS_LOG_LEVEL="debug"                     # 可选，临时提高日志级别
build\windows-debug\bin\Debug\QmlScrcpy.exe
```

真机用例默认跳过，要跑就补 `QS_ADB_PATH` 和 `QS_DEVICE_SERIAL` 两个环境变量。
完整环境变量表见 [构建与测试](docs/构建与测试.md)。

## 目录

```
QmlScrcpy/
├─ CMakeLists.txt               版本号 + 找 Qt + 进子目录
├─ CMakePresets.json            VS 2022 生成器，任何 shell 都能配
├─ cmake/
│  ├─ CompilerOptions.cmake     INTERFACE 目标 qs_options，所有目标继承
│  ├─ Ffmpeg.cmake              vendored FFmpeg 的 include/lib + DLL 部署
│  └─ DeployQt.cmake            windeployqt + qml 模块落盘校验
├─ libs/                        include 根；源文件和头文件平铺，不分子目录
│  ├─ protocol/                 包头、控制消息、设备消息（只依赖 QtCore）
│  ├─ adb/                      adb 参数拼装 + 进程执行（只依赖 QtCore）
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
