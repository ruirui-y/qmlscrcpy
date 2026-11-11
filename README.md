# QmlScrcpy

用 Qt Quick 重写的安卓投屏客户端，协议对齐 scrcpy 3.x（推送的 `third_party/scrcpy-server` 固定 3.3.3）。

> 不是 [QtScrcpy](https://github.com/barry-ran/QtScrcpy) 的 fork。UI 和线程模型全部重写，
> 只沿用协议事实：12 字节包头、控制消息布局、keymap JSON 格式。

## 现状

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | CMake 骨架 + QML 窗口 | ✅ |
| P1 | 协议层：包头 / 控制消息 / 设备消息，23 项单测 | ✅ |
| P2a | adb 命令层：参数拼装 + 进程执行，14 项单测 | ✅ |
| P2b | 连接状态机：push → reverse/forward → execute → readInfo | ✅ 真机跑通 |
| P3 | 视频链路：socket → demux → 解码 → SceneGraph 上屏 | 未开始 |
| P4 | 键鼠注入 + keymap | 未开始 |

## 技术栈

| 项 | 选择 |
|---|---|
| 语言 | C++17 |
| UI | Qt Quick / QML（Qt 5.15.2 msvc2019_64） |
| 构建 | CMake ≥ 3.22 + CMakePresets |
| 编译器 | MSVC 2022（VS 17） |
| 视频 | FFmpeg（解码）+ OpenGL 纹理上传（P3 接入） |

不用 Qt6 的原因：5.15 已满足全部需求，且 `QWindow::startSystemMove()` 正是 5.15 才有的。

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

真机用例默认跳过，要跑就补两个环境变量：

```powershell
$env:QS_ADB_PATH = "C:\path\to\adb.exe"
$env:QS_DEVICE_SERIAL = "e77121ed"     # adb devices 里那一列
ctest --test-dir build\windows-debug -C Debug --output-on-failure
```

## 目录

```
QmlScrcpy/
├─ CMakeLists.txt               版本号 + 找 Qt + 进子目录
├─ CMakePresets.json            VS 2022 生成器，任何 shell 都能配，不需要开发者命令行
├─ cmake/
│  ├─ CompilerOptions.cmake     INTERFACE 目标 qs_options，所有目标继承
│  └─ DeployQt.cmake            windeployqt + qml 模块落盘校验
├─ libs/                        include 根；源文件和头文件平铺，不分子目录
│  ├─ protocol/                 12 字节包头、控制消息、设备消息（只依赖 QtCore）
│  └─ adb/                      adb 参数拼装 + 进程执行（只依赖 QtCore）
├─ tests/                       QTest 单测，ctest 按名字跑
├─ qml/Main.qml                 QML 界面
├─ src/                         main.cpp + 可执行文件
└─ third_party/scrcpy-server    安卓端服务，与 scrcpy 3.3.3 死绑，固定入库
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
