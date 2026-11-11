# QmlScrcpy

用 Qt Quick 重写的安卓投屏客户端，协议对齐 scrcpy 3.x（推送的 `scrcpy-server` 为 3.3.3）。

> 不是 [QtScrcpy](https://github.com/barry-ran/QtScrcpy) 的 fork。UI 和线程模型全部重写，
> 只沿用协议事实：12 字节包头、控制消息布局、keymap JSON 格式。

## 现状

**P0 · 骨架** — 能配置、能编译、能弹出一个空窗口。还没有任何投屏代码。

## 技术栈

| 项 | 选择 |
|---|---|
| 语言 | C++17 |
| UI | Qt Quick / QML（Qt 5.15.2 msvc2019_64） |
| 构建 | CMake ≥ 3.21 + CMakePresets |
| 编译器 | MSVC 2022（VS 17） |
| 视频 | FFmpeg（解码）+ OpenGL 纹理上传（P2 接入） |

不用 Qt6 的原因：5.15 已满足全部需求，且 `QWindow::startSystemMove()` 正是 5.15 才有的。

## 构建

```powershell
cmake --preset windows-debug
cmake --build --preset build-windows-debug
.\build\windows-debug\bin\Debug\QmlScrcpy.exe
```

Qt 装在别处就改 `CMakePresets.json` 里 `CMAKE_PREFIX_PATH` 那一行。
构建后自动跑 `windeployqt`，输出目录自带 Qt 运行库，双击也能跑。

## 目录

```
QmlScrcpy/
├─ CMakeLists.txt            版本号 + 找 Qt + 进子目录
├─ CMakePresets.json         只用 VS 2022 生成器：不需要开发者命令行，任何 shell 都能配
├─ cmake/CompilerOptions.cmake   INTERFACE 目标 qs_options，所有目标继承
├─ qml.qrc                   / 前缀，引用 qml/ 目录
├─ qml/Main.qml              P0 占位窗口
└─ src/
   ├─ CMakeLists.txt         exe + windeployqt
   └─ main.cpp               QML 引擎启动、高 DPI、GL 格式
```

## 路线图

| 阶段 | 内容 |
|---|---|
| P0 | 骨架：CMake + QML 窗口 ← 当前 |
| P1 | 协议层：adb 封装、server 启动状态机、报文序列化（可单测） |
| P2 | 视频链路：socket → demux → 解码 → SceneGraph 节点显示 |
| P3 | 键鼠注入 + keymap |
| P4 | 录制 / 截图 / 群控 / 音频 |

## 协议来源

- [Genymobile/scrcpy](https://github.com/Genymobile/scrcpy) — 服务端与传输协议
- [barry-ran/QtScrcpy](https://github.com/barry-ran/QtScrcpy) — 早期客户端实现与踩坑记录

## 许可

Apache-2.0，见 `LICENSE`。
