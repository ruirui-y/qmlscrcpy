# Qt 6.8 迁移

> 追踪文档：记录 0.2.1 之后从 **Qt 5.15.2 → Qt 6.8.3** 的整轮迁移：改了什么、崩在哪、怎么定位的、还剩什么。
> 解码那一层没有改动，单独记在 [解码链路](解码链路.md)。

## 一、这一阶段要做什么

把整个工程从 Qt 5.15.2 迁到 Qt 6.8.3（MSVC 2022 64 位），要求：

| 目标 | 约束 |
|---|---|
| 能编译 | 两套构建目录并存：`build/windows-debug`（Qt5）暂时保留，新加 `build/qt6-debug` |
| 能跑起来 | 主窗、投屏窗、设置窗都正常，视频能出画 |
| 行为不变 | 键位映射、连点器、滚轮这些已经验收的功能不能回退 |
| 不留隐雷 | 不允许出现"能跑但靠运气"的写法（比如断言被静默吞掉） |

**不做的**：顺手重构、顺手加功能。这一轮只做"能编、能跑、行为一致"。

## 二、怎么做的

### 2.1 CMake：从 Qt5 找到 Qt6

根 [CMakeLists.txt:12-16](H:/YJJ/Project/QmlScrcpy/CMakeLists.txt#L12-L16)：

```cmake
# 6.5 是底线：VideoItem 是 QQuickFramebufferObject，Qt6 起它只能挂在 OpenGL 后端上，
# 而"指定后端"这件事要有 RHI（6.0 才有），6.5 之后 API 才稳定
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network OpenGL)
get_filename_component(QS_QT_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)
```

三个要点：

| 改动 | 原因 |
|---|---|
| `find_package(Qt6 6.5 …)` | 6.0~6.4 的 RHI 指定后端 API 还在变，6.5 之后才稳定 |
| 组件里加 `OpenGL` | `VideoItem` 用 `QOpenGLShaderProgram` / FBO，Qt6 里这些在 `Qt6::OpenGL` 模块下 |
| `QS_QT_ROOT` 从 `Qt6_DIR` 退三级 | 部署脚本要拿 Qt 安装根目录，`Qt6_DIR` 是 `<root>/lib/cmake/Qt6` |

### 2.2 必须显式指定 OpenGL 后端（最隐蔽的一条）

[src/main.cpp:72-75](H:/YJJ/Project/QmlScrcpy/src/main.cpp#L72-L75)：

```cpp
// VideoItem 是 QQuickFramebufferObject，它的 render() 里全是裸 GL 调用，只在 OpenGL
// 后端下会被调用。Qt6 在 Windows 上默认挑 D3D11，不指定的话这个节点直接被跳过（画面全黑）
QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
```

**Qt5 默认就是 OpenGL，Qt6 在 Windows 上默认变成 D3D11。** 不指定的话程序不报错、窗口也开，只是画面永远黑的——这是整轮迁移里最容易被误判成"解码坏了"的一条。

### 2.3 高 DPI 属性被删

[src/main.cpp:63](H:/YJJ/Project/QmlScrcpy/src/main.cpp#L63)：

```cpp
// Qt6 的高 DPI 缩放默认就开着，AA_EnableHighDpiScaling 这个属性已经删掉，只剩取整策略要定
QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
```

Qt6 里 `Qt::AA_EnableHighDpiScaling` 已经不存在，写了编译不过；缩放默认开，只需要定取整策略。

### 2.4 `geometryChanged` → `geometryChange`

[tests/CMakeLists.txt](H:/YJJ/Project/QmlScrcpy/tests/CMakeLists.txt) 与 `ui/video_item.h` 里同步改了：

```cpp
// Qt5 叫 geometryChanged，Qt6 改名成 geometryChange，参数含义没变
void geometryChange(const QRectF &new_geometry, const QRectF &old_geometry) override;
```

漏改的表现：**编译报 "marked override, but does not override"**，一看就懂，属于低风险项。

### 2.5 部署脚本要认两种目录布局

[cmake/DeployQt.cmake:25-31](H:/YJJ/Project/QmlScrcpy/cmake/DeployQt.cmake#L25-L31)：

```cmake
# 注意两代 windeployqt 布局不一样：Qt5 平铺拷到 exe 旁边（QtQuick.2/、QtQuick/Controls.2/），
# Qt6 改成放进 exe 旁边的 qml/ 子目录。所以按"任意一层里有 qmldir"去找，两边都能命中。
file(GLOB_RECURSE deployed_qmldir "${binary_dir}/*/qmldir")
```

原来写死了 `QtQuick*/qmldir`，Qt6 的布局进 `qml/` 子目录，匹配不到就 `FATAL_ERROR`。

### 2.6 测试也要跟着改

`tests/CMakeLists.txt` 里 Qt5 的 `Qt5::Test` 换成 `Qt6::Test`，`add_test` 的目标名不变。

## 三、写完为什么不是理想效果

迁移不是"编译过了就完事"，实际踩了四类问题：

### 3.1 启动直接崩，异常码 `0x80000003`

Windows 事件日志：

```
Faulting module name: Qt6Cored.dll, version: 6.8.3.0
Exception code: 0x80000003
```

`0x80000003` 是**断点指令**，不是野指针也不是栈溢出——意思是"Qt 内部断言被触发"。

抓 stderr 拿到断言原文（这一段是关键，Qt 的断言只在 stderr 里，不进我们自己的日志）：

```
ASSERT failure in : "QObject::connect: Unique connection requires the slot to be a
pointer to a member function of a QObject subclass.",
file H:\Qt\6.8.3\msvc2022_64\include\QtCore/qobject.h, line 265
```

对应代码是给 `frameSwapped` 挂统计钩子：

```cpp
// 改前：Qt5 静默忽略这个组合，Qt6 直接断言
QObject::connect(window(), &QQuickWindow::frameSwapped, this, [this]() { MarkSwapped(); },
                 Qt::UniqueConnection);
```

**结论：Qt5 里 `Qt::UniqueConnection` 配 lambda 是"没效果但能跑"，Qt6 变成"直接终止"。**

### 3.2 栈溢出（上一版修完崩得更高）

把 `connect` 从每帧的队列回调里挪出来之前，栈是这样的（同一段反复出现，典型爆栈形状）：

```
Present::lambda_2 → QObject::connect → … → user32 → … → Present::lambda_2 → connect → …
```

原因：`Present()` 的队列回调里先调 `update()`，在 basic 渲染循环下 `update()` 会同步进渲染、发出 `frameSwapped`，再回到回调继续执行后面的 `connect`，于是每帧都进一次 `connect`，层层嵌套。

### 3.3 一堆控件外观退回系统默认

```
QML QQuickRectangle: The current style does not support customization of this control
(property: "background" ...). Please customize a non-native style (such as Basic, Fusion, ...)
```

Qt6 的 Controls 默认样式是平台原生的，原生样式**不允许**改 `background` / `contentItem`。我们的 ComboBox、TextField 都自定义了这些属性，于是全部失效、外观退回系统默认。

### 3.4 adb 找不到设备 + 主窗冻结 5 秒

Qt6 的构建目录是全新的，`Tools\adb.exe` 没跟过去，`AdbPath()` 于是降级到 PATH 上的 adb；第一次运行时 adb daemon 冷启动实测 **5128 ms**，而设备查询是同步跑在 GUI 线程、超时正好 5000 ms——到点杀进程，daemon 反复被杀死在半启动状态。

## 四、怎么定位的

| 线索 | 怎么拿到的 | 排除了什么 |
|---|---|---|
| `0x80000003` | Windows 事件查看器 → 应用程序日志 | 排除野指针（`0xC0000005`）、栈溢出（`0xC00000FD`） |
| 断言原文 | 用 `Start-Process -RedirectStandardError` 把 stderr 落盘 | 排除"解码坏了""显卡驱动"这类猜测 |
| 栈的形状 | VS 调用栈里同一段重复 8 次以上 | 确认是递归/爆栈，不是单点异常 |
| adb 冷启动 5.1 s | 手动跑 `adb devices` 计时 | 确认卡顿来自 daemon 启动，不是设备问题 |
| `[adb] 使用 "adb"` | 启动日志 | 确认路径降级，而不是 adb 本身坏了 |

**这一轮最有价值的方法论**：崩溃先看异常码再猜。

```
0xC0000005  野指针
0xC00000FD  栈溢出
0x80000003  断点 / 断言
```

异常码不同，排查方向完全不同，别一上来就重编。

## 五、怎么解决的

| 问题 | 改动 | 文件 |
|---|---|---|
| 断言终止 | 去掉 `Qt::UniqueConnection`，靠 `swap_hooked_` 保证只接一次 | `ui/video_item.cpp` |
| 栈溢出 | 两处 `connect` 从每帧队列回调挪进 `componentComplete()` | `ui/video_item.cpp` / `.h` |
| 画面全黑 | 显式 `setGraphicsApi(OpenGL)` | `src/main.cpp` |
| 控件外观回退 | 未修（见"还差的"） | — |
| adb 找不到 | `Tools/` 入库；构建时 `copy_if_different` 到输出目录；`AdbPath()` 只认 `Tools\adb.exe` | `CMakeLists` / `src/CMakeLists.txt` / `libs/adb/adb_runner.cpp` / `setup/collect.ps1` |

### 5.1 构建期顺带修的两个坑

**① `add_custom_command(TARGET ...)` 必须写在创建 target 的同一目录**

写在根 `CMakeLists.txt` 直接报：

```
CMake Error at CMakeLists.txt:35 (add_custom_command):
  TARGET 'QmlScrcpy' was not created in this directory.
```

→ 挪进 `src/CMakeLists.txt`（`QmlScrcpy` 在那里 `add_executable`）。

**② `copy_directory` 在 adb server 跑着时会失败**

```
Error copying directory from ".../Tools" to ".../bin/Debug/Tools".
```

因为 `adb.exe` 正被 adb server 占用。→ 改成 `make_directory` + `copy_if_different`：目标没变就不写盘，自然不碰被占用的文件。

**③ `LNK1168: 无法打开 QmlScrcpy.exe 进行写入`**

链接时 exe 还在跑（或 VS 调试会话没结束）。→ 先结束 `QmlScrcpy` 进程再构建。这个跟 Qt6 无关，但迁移期反复遇到。

## 六、最终效果 + 学到什么

### 现状

```
Qt 5.15.2  ──┐
             ├─ 两套构建目录并存（windows-debug / qt6-debug）
Qt 6.8.3   ──┘
```

Qt6 下已验收：主窗、投屏窗、设置窗、视频出画、键位映射、连点器、滚轮。

### 学到的

1. **迁移的坑不在语法，在"默认值变了"**。OpenGL → D3D11、controls 样式、高 DPI，都是"没写错但行为变了"。
2. **Qt 的断言原文字在 stderr，不在自己的日志里**。不把 stderr 落盘就只能看着 `0x80000003` 猜。
3. **异常码是分诊台**：`0xC0000005` / `0xC00000FD` / `0x80000003` 三条路完全不一样。
4. **Qt5 里"能跑"的写法，Qt6 可能直接断言**。`Qt::UniqueConnection` + lambda 就是典型。
5. **开发目录也要能自包含**。adb 这种外部 exe 一旦依赖"手工拷"，新构建目录必然踩坑——所以这次把 `Tools/` 直接入库。

### 还差的

- **Qt6 控件样式**没处理：现在走平台原生样式，自定义 `background` 全失效，外观和 Qt5 版不一致。修法是启动处加 `QQuickStyle::setStyle("Basic")`，但要逐屏看效果。
- **Qt5 构建还没删**：两套并存便于对照，但 CMake 里还有 Qt5 兼容分支要清理。
- **设备查询仍是同步阻塞**：adb daemon 冷启动时主窗会卡 5 秒（这次只解决了"路径找错"，没解决"阻塞"）。
- **首次启动超时没放宽**：`RefreshDevices()` 仍是 5000 ms 超时，冷启动时可能刚好撞上。
