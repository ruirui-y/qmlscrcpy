# GPU 渲染改造（RHI 路线）

记录「把渲染从 OpenGL 换成 Qt RHI（D3D11）」这一轮改造：为什么换、spike 验了什么、分几步走、踩到哪些坑。

## 一、这一阶段要做什么

### 起点

硬解（B 方案）已经能用，但每帧有 6.2 ms 卡在 `Map` 读回：

```
D3D11 解码纹理 → 拷到 STAGING → Map 读回内存 → glTexImage2D 上传 → GL 着色器
                    ① 显存→内存（4.7ms）      ② 内存→显存（~0.5ms）
```

两趟搬运全是多余的：数据本来就在显卡里。

### 目标

```
D3D11 解码纹理 →（GPU 内转 RGBA）→ QRhiTexture 直接引用 → 画
```

单帧主线程目标：**6.2 ms → < 0.5 ms**。

### 不做什么

- 不碰 OpenGL 与 D3D11 的 interop（`wglDX*`）——上一轮崩在 `wglDXUnlockObjectsNV`，且需要跨线程纹理同步
- 不改解码协议、不改输入层

## 二、怎么做的（先看参考项目）

`H:\YJJ\Project\Player\Player\Client\Core` 里的做法直接点醒了方向：

| 文件 | 做法 |
|---|---|
| `VideoDecoder.cpp` | `av_hwdevice_ctx_create(d3d11va)`，硬解时 `thread_count = 1` |
| `Nv12GpuUploader.cpp` | `CopySubresourceRegion` 拷到自建 NV12 纹理（GPU 内），再用 **PlaneSlice SRV** 建 Y(R8)/UV(R8G8) 两个视图 |
| `D3D11Pipeline.cpp` | HLSL 采样两个 SRV，直接画到 D3D11 交换链 |

**关键结论：它没有做任何 GL/D3D 互操作，而是让渲染端也用 D3D11。**

对应到 Qt，就是换成 **Qt RHI 的 D3D11 后端**：

```
Qt 用 D3D11 后端 → rhi->nativeHandles() 拿 device/context
                 → 同一个设备交给 FFmpeg 硬解
                 → 自己建的纹理 QRhiTexture::createFrom 直接引用
                 → QSGRenderNode / QQuickRhiItem 里画
```

## 三、spike：先验证唯一的技术未知数

未知数只有一个：**RHI 能不能包住外部创建的 D3D11 纹理**（不是 RHI 分配的）。

spike 放在 `.refs/spike_rhi_d3d11/`（gitignore，不入库），120 行：

```cpp
auto *handles = const_cast<QRhiD3D11NativeHandles *>(
    static_cast<const QRhiD3D11NativeHandles *>(rhi->nativeHandles()));
auto *device = static_cast<ID3D11Device *>(handles->dev);

ComPtr<ID3D11Texture2D> texture = MakeTestTexture(device);   // 自己建 RGBA8 纹理
QRhiTexture *wrapped = rhi->newTexture(QRhiTexture::RGBA8, QSize(64, 64));
QRhiTexture::NativeTexture native;
native.object = reinterpret_cast<quint64>(texture.Get());
wrapped->createFrom(native);                                  // ✅ true
```

结果：

```
[spike] RHI 后端: D3D11
[spike] 设备指针: 0x1e834cedfc0  上下文: 0x1e834cef288
[spike] createFrom 结果: true
[spike] nativeTexture 回读: 0x1e87a111ca0
[spike] 管线创建结果: true
[spike] 已提交绘制（全屏四边形）
[spike] 抓图 spike_result.png QSize(400, 300) 成功
```

抓图是一整块纯色——**外部纹理真的被画到屏幕上了**。

## 四、踩的坑（全部实测）

### 坑 1：格式必须是 RGBA，不能是 BGRA

```
Failed to create srv: COM error 0x80070057 (E_INVALIDARG)
[spike] createFrom 结果: false
```

`QRhiTexture::RGBA8` ↔ `DXGI_FORMAT_R8G8B8A8_UNORM`。用 `B8G8R8A8` 会失败。

→ 现在解码侧 `VideoProcessor` 输出的是 BGRA，接 RHI 前**必须改成 RGBA 顺序**。

### 坑 2：`QQuickRhiItem` 不会替你开 render pass

```
ASSERT: "cbD->recordingPass == QD3D11CommandBuffer::RenderPass" (qrhid3d11.cpp:925)
```

必须自己：

```cpp
cb->beginPass(renderTarget(), Qt::black, {1.0f, 0});
... 画 ...
cb->endPass();
```

### 坑 3：不设视口就是全黑

第一次抓图全黑，日志一切正常。原因是 `beginPass` 之后视口是空的：

```cpp
const QSize size = renderTarget()->pixelSize();
cb->setViewport({0.0f, 0.0f, float(size.width()), float(size.height())});
```

### 坑 4：顶点缓冲要在 `initialize(cb)` 里上传

`render()` 里已经在 pass 中，`cb->resourceUpdate()` 会失败。资源上传放到 `initialize(QRhiCommandBuffer *cb)`，那里可以安全用 `cb->resourceUpdate(batch)`。

### 坑 5：QSB 着色器要随 exe 走

`qsb.exe` 编译出来的是 `quad.vert.qsb` / `quad.frag.qsb`，运行时按相对路径加载。构建时用 `copy_if_different` 拷到输出目录，否则运行时打不开、管线创建失败、画出来全黑。

### 坑 6：`uploadTexture(tex, QByteArray)` 这个重载不存在

第一次写成了：

```cpp
batch->uploadTexture(texture_y_, plane_y_);        // ❌ 没有这个重载
```

正确写法要套一层 entry（一个 entry = 一个 mip 层）：

```cpp
const QRhiTextureUploadDescription desc(QRhiTextureUploadEntry(
    0, 0, QRhiTextureSubresourceUploadDescription(data, size)));
batch->uploadTexture(texture_y_, desc);
```

### 坑 7：顶点缓冲建了不等于传了

`QRhiBuffer::create()` 只是分配，数据要用 `uploadStaticBuffer`。而且**必须放在 `beginPass` 之前**（`render()` 开头、还没进 pass 时），否则就是坑 4 那个断言。

### 坑 8：RHI 头文件不在默认 include 路径里

`#include <rhi/qrhi.h>` 编译不过，因为它在版本化目录：

```
include/QtGui/6.8.3/QtGui/rhi/qrhi.h
```

Qt 的 imported target 不会带上，要手动加：

```cmake
target_include_directories(QmlScrcpy PRIVATE "${QS_QT_ROOT}/include/QtGui/${Qt6_VERSION}/QtGui")
```

## 五、四步改造计划（每步可回退）

| 步 | 内容 | 状态 |
|---|---|---|
| **1** | 解码器支持导入外部 D3D11 设备（`SetExternalDevice`） | ✅ 已完成（提交 `44c96aa`） |
| **2** | 重写 `video_renderer.*` 为 QRhi 版，先接现有 CPU 帧（YUV420P / NV12 上传） | ✅ 已完成（提交 `6f21092`） |
| **3** | `VideoItem` 基类换 `QQuickRhiItem`，`main.cpp` 切 D3D11 后端 | ✅ 已完成（提交 `6f31530`），软解 97 fps、硬解 NV12 分支实测通过 |
| **4** | 原生纹理通路：VideoProcessor 输出 RGBA8 → `createFrom` → 直接画 | 4a 设备握手 ✅（`f537396` + `07195af`），4b 待做 |

### 第四步 4a：设备握手（已完成）

目的只有一个：**让解码器和 Qt 渲染后端用同一个 `ID3D11Device`**。不同设备上的纹理，`createFrom` 一定失败。

链路：

```
渲染线程 initialize()
  └─ rhi->nativeHandles() → ID3D11Device*
       └─ VideoItem::ProvideRhiDevice()      （存进成员 + 发信号）
            └─ DeviceController 开解码器前取 rhi_device_
                 └─ decoder->SetExternalDevice(device, context)
                      └─ OpenHardware 走"外部设备"分支
```

最终日志（实测）：

```
[rhi] 渲染器初始化，后端 D3D11
[decode] 使用外部 D3D11 设备（Qt RHI 提供）
[decode] 使用 D3D11VA 硬件解码
[decode] 首帧格式 172（D3D11 = 172）
```

### 4a 踩的三个坑（它们是串在一起的）

**坑 13：QML 里只有 `onConnectedChanged`**

原来窗口只跟着 `connected` 显示：

```qml
function onConnectedChanged()
{
    videoWindow.visible = deviceController.connected
}
```

加了 `connecting` 属性后，`connecting` 变了却没人处理，窗口不出现 → 渲染后端不初始化。必须再补一个 `onConnectingChanged()`。

**坑 14：信号比控制器的连接更早**

渲染器在窗口首次渲染时就发 `SigRhiDeviceReady`，而控制器要等 `SetVideo(qml item)` 才 `connect` 这个信号——**信号已经发完了**。

解法：画布把设备**存下来**（`rhi_device_value_` / `rhi_context_value_`），控制器开解码器前主动 `RhiDevice()` 读当前值，不依赖信号。

**坑 15：解码器开得比渲染后端还早**

时序是“连接 → 开解码器 → 弹窗 → 渲染器初始化”，解码器永远早一步。在 `ApplyConnected` 开头加等待：

```cpp
if (AppSettings().DecodeHardware() && rhi_device_ == 0 && gpu_wait_retries_ < 40)
{
    ++gpu_wait_retries_;
    QTimer::singleShot(50, this, [this, result]() { ApplyConnected(result); });
    return;
}
```

窗口既然已经在 `connecting` 阶段显示，渲染器 300ms 内就会初始化，重试必然成功。

**注意**：这个等待逻辑如果放在“窗口还没显示”的阶段会**死锁**——等设备，设备等窗口，窗口等连接成功。第一次尝试就死在这，表现为黑屏、日志停住、没有 `[rhi]`。必须先保证投屏窗在 `connecting` 阶段就显示。

### 4a 的遗留

- `gpu_wait_retries_` 只在 `ApplyConnected` 内使用，超时后退回自建设备（日志会打 warn）
- 渲染器析构里已经删掉 RHI 资源，退出时不再报 `unreleased resources`

### 第三步之后的实测（NV12 分支）

把 `config/config.ini` 的 `hardware` 临时改成 1 跑了一次：

```
[decode] D3D11 适配器 "NVIDIA GeForce RTX 4080 SUPER"
[decode] 使用 D3D11VA 硬件解码
[decode] 首帧格式 172（D3D11 = 172）
[rhi] 渲染器初始化，后端 D3D11
```

画面正常（聊天界面截图，文字和头像颜色都对），47 fps。说明：

- 硬解产物（NV12 两平面）→ 渲染器 R8/RG8 上传 → QSB 着色器 → D3D11 上屏，整条链路通
- 现在仍然走 **CPU 中转**（`DownloadFrame` 读回），第 4 步才是把这段删掉

测完已把 `hardware` 改回 0，配置没留改动。

### 第 2 步产出了什么

| 文件 | 作用 |
|---|---|
| `ui/video_frame_source.h` | 渲染器与画布的接口：`TakePendingFrame` / `NotifySyncPass` / `NotifyRenderEnd` / `KeepAspectEnabled` / `CanvasSize`。换基类时渲染器不用改 |
| `ui/video_rhi_renderer.{h,cpp}` | `QQuickRhiItemRenderer` 子类：加载 QSB、建 R8/RG8 两张平面纹理、上传、画全屏四边形 |
| `ui/shaders/quad.vert` + `nv12.frag` | YUV→RGB（BT.601）的 QSB 着色器，系数和 GL 版一致 |
| `src/CMakeLists.txt` | 登记文件 + 构建后拷贝 `shaders/*.qsb` + 加 RHI 头文件路径 |

编译结果：`QmlScrcpy.exe` 生成成功，`bin/Debug/shaders/` 里两个 qsb 都在。因为 `VideoItem` 还是 `QQuickFramebufferObject`，渲染器**还没被实例化**，运行行为不变。

### 第 3 步产出了什么

| 文件 | 变化 |
|---|---|
| `ui/video_item.h` | 基类换成 `QQuickRhiItem`，同时实现 `VideoFrameSource`；`createRenderer()` 不再 `const` |
| `ui/video_item.cpp` | `createRenderer()` 返回 `VideoRhiRenderer` 并 `SetSource(this)`；实现 6 个接口方法；删掉 GL 渲染器引用 |
| `ui/video_item_input.cpp` | 基类事件转发改名（`QQuickRhiItem::mousePressEvent` 等） |
| `src/main.cpp` | `setGraphicsApi(QSGRendererInterface::Direct3D11)` |
| `ui/video_renderer.{h,cpp}` | **删除**（GL 版渲染器作废，代码在 git 历史里） |

软解 `YUV420P` 没有单独做三平面管线，而是在 `synchronize()` 里把 U/V 拼成 NV12 交错：

```
YUV420P：U 平面 + V 平面        → 逐行拼成 U0 V0 U1 V1 …  → 同一套 NV12 着色器
NV12   ：UV 已经是交错的         → 直接按行拷
```

代价是每帧一次行拷贝，换掉整整一套着色器 + 三张纹理 + 第二套管线，划算。

### 第 3 步的坑

**坑 9：`createRenderer()` 不是 const**

`QQuickFramebufferObject::createRenderer() const` 到 `QQuickRhiItem::createRenderer()` 少了 `const`，原样照抄会报：

```
error C3668: 包含重写说明符 override 的方法没有重写任何基类方法
```

**坑 10：接口方法名和画布已有方法撞了**

画布本来就有 `AdvanceSyncPass()`（返回 qint64）、`ClockUs()`、`MarkRenderEnd(qint64, trace)`。接口里再声明同名不同签名的纯虚函数，直接编译失败：

```
error C2555: 重写虚函数返回类型有差异
error C2535: 已经定义或声明成员函数
```

解决办法是接口用另一套名字：`NotifySyncPass` / `SyncPassStartUs` / `NowUs` / `NotifyRenderEnd`，实现里转调已有方法。

**坑 11：删了 GL 渲染器要连带清三处**

- `video_item.cpp` 的 `#include "ui/video_renderer.h"`
- `src/CMakeLists.txt` 里的源文件条目
- `video_item_input.cpp` 里 `QQuickFramebufferObject::xxxEvent` 的基类调用

漏任何一处都是编译错误。

**坑 12：日志刷屏**

`QQuickRhiItem` 在窗口缩放/场景图重建时会重新 `createRenderer()`，初始化日志打了几百行。改成函数内 `static bool` 只打第一次。

**为什么 2 在 3 之前**：渲染器先能画 CPU 帧，切后端时才有画面可验证；否则一切换就是黑屏，分不清问题在哪。

### 第 1 步做了什么

```cpp
// h264_decoder.h
void SetExternalDevice(void *d3d11_device, void *d3d11_context);

// OpenHardware() 开头：外部设备优先
if (external_device_ != nullptr)
{
    device->AddRef();                    // FFmpeg 接管一份
    d3d11->device = device;
    ...
    qInfo() << "[decode] 使用外部 D3D11 设备（Qt RHI 提供）";
}
```

未接线时行为完全不变，属于纯增量。

## 六、现状与收获

### 现状

- 主工程**仍是 GL + B 方案**，可正常使用（软解 4.65ms / 硬解 B 6.2ms，配置可切）
- 解码器已具备"导入外部设备"能力，但没人调用
- spike 代码在 `.refs/spike_rhi_d3d11/`，不入库

### 学到什么

1. **卡住的时候先看同类项目怎么绕**：参考项目根本没做 interop，而是统一到一套图形 API
2. **RHI 包外部纹理是可行的**，但格式必须严格对齐（RGBA8 ↔ R8G8B8A8_UNORM）
3. **`QQuickRhiItem` 是 RHI 版的 FBO**：`initialize/synchronize/render` 三个回调与 `QQuickFramebufferObject` 一一对应，迁移成本比预想低
4. **黑屏先查视口和 pass**：spike 两次黑屏都不是着色器问题
5. **spike 的价值**：`createFrom` 这个唯一未知数用 120 行、两次运行就定死了，避免直接重构 450 行渲染器后才发现走不通

### 还没解决的

- `QQuickRhiItem` 迁移后，`VideoItem` 的输入/键位/统计代码不受影响，但**渲染统计钩子**（`beforeSynchronizing` 等）要换成 RHI 等价时机
- 软解路径上传纹理从 glTexSubImage2D 换成 `QRhiTexture::setData`
- 第 4 步做完才能删掉 `Map` 读回那条路径
## 4b 第一次尝试：为什么一开就崩

### 现象（两份不同的证据，同一个根因）

我这边（强制 `hardware=1` 跑）：

```
ExitCode = -1073741819  (0xC0000005 访问冲突)
Windows 事件日志：
  Faulting module: nvwgf2umx.dll        ← NVIDIA 的 D3D11 用户态驱动
  Exception code:  0xc0000005
  Fault offset:    0x0000000000606523
```

用户这边（VS 里跑）：

```
QThread: Destroyed while thread is still running
（qthread.cpp:470 断言，进程终止）
```

### 排查过程

1. 先怀疑"解码线程和渲染线程同时读写同一张双缓冲纹理" → 加了帧序号 `seq`、把 `QRhiTexture` 按纹理指针缓存（不再每帧 delete/create）
2. 重测：**仍然崩**，且崩在 `nvwgf2umx.dll` 内部
3. 结论：不是我们代码里的空指针，是**提交给驱动的 D3D11 操作本身非法**

### 真正的根因：两个线程共用同一个即时上下文

```
GUI 线程    ：建 ID3D11Device + 它的 ImmediateContext
视频线程    ：ConvertToBgra / VideoProcessorBlt 用这个上下文
渲染线程    ：Qt RHI 也用同一设备的上下文画
```

`ID3D11DeviceContext` **不是线程安全的**，同一时刻只能被一个线程使用。两个线程同时提交命令，轻则行为未定义，重则驱动里访问冲突（`nvwgf2umx.dll`）。

**为什么以前没事**：B 方案（`DownloadFrame`）也在视频线程用上下文，但那时渲染走 OpenGL，渲染线程根本不碰这个设备。换成 D3D11 RHI 后才出现并发。

### 正确做法（还没做完）

把 GPU 操作全部收到渲染线程：

```
视频线程：只填 Frame::Native{ NV12 纹理指针, 切片号, keep_alive(AVFrame) }
          完全不碰 D3D 上下文
渲染线程：用 RHI 的设备做 VideoProcessor NV12→RGBA，然后画
```

这样只有一个线程碰上下文，天然没有竞争。

### 当前代码状态

- `SetZeroCopy(false)`：零拷贝关闭，程序正常
- 第一次尝试的代码（帧序号、按指针缓存纹理）**已回退**，因为改到一半时渲染器头文件被写乱、编译不过
- 已确认可用的部分：设备握手（4a）、`QQuickRhiItem` 渲染器、RGBA 直通着色器 `ui/shaders/rgba.frag`

### 学到什么

1. **D3D11 的即时上下文不能跨线程共用一个设备**，这是这一轮最大的坑
2. 崩溃表现会骗人：驱动里崩（我的机器）和 Qt 断言（用户的机器）可以是同一个根因
3. 先做"最小可验证"再动手：如果一开始就只把 NV12 纹理丢给渲染线程，能省掉这一整轮返工
## 4b 第二次尝试：把转色搬到渲染线程

### 设计（和第一次的区别）

```
第一次（崩）：
  视频线程  ConvertToBgra（用解码器的 D3D 上下文）
  渲染线程  Qt RHI 用同一个设备的上下文画
  → 两个线程抢同一个 ImmediateContext → 驱动里访问冲突

第二次：
  视频线程  只填 Frame::Native{ NV12 纹理, 切片号, keep_alive }，一行 D3D 都不碰
  渲染线程  用 RHI 的设备建 VideoProcessor，做 NV12 → RGBA，然后画
  → 只有渲染线程碰上下文，没有竞争
```

### 第一步：帧结构与解码器（已完成，编译通过）

`frame.h` 的 `NativeTexture` 增加：

| 字段 | 用途 |
|---|---|
| `int slice` | 解码器池里的数组切片号 |
| `quint64 seq` | 出帧序号 |
| `std::shared_ptr<void> keep_alive` | 保住 AVFrame，否则解码器会立刻复用纹理 |

解码器零拷贝分支：

```cpp
if (zero_copy_ && external_device_ != nullptr)
{
    NativeTexture native;
    native.texture = av_frame->data[0];                                    // NV12 纹理
    native.slice   = int(reinterpret_cast<quintptr>(av_frame->data[1]));
    native.seq     = ++native_seq_;

    AVFrame *held = av_frame_clone(av_frame);
    native.keep_alive = std::shared_ptr<void>(held, [](void *p) {
        AVFrame *frame_ptr = static_cast<AVFrame *>(p);
        av_frame_free(&frame_ptr);
    });

    frame = Frame(size, native);
    downloaded = true;                                                     // 跳过 DownloadFrame
}
```

**注意**：`ConvertToBgra` 不再参与零拷贝路径，它的存在只是给别的实验用。

### 第二步：渲染器里做转色（待做）

要加的东西：

| 组件 | 作用 |
|---|---|
| `ID3D11VideoDevice` / `ID3D11VideoContext` | 从 RHI 的 device QI 出来（`rhi->nativeHandles()`） |
| `ID3D11VideoProcessorEnumerator` + `ID3D11VideoProcessor` | 按帧尺寸建一次，尺寸变了重建 |
| 输出 RGBA 纹理 + 输出视图 | 渲染器自己持有，只有渲染线程用 |
| `ConvertNativeFrame()` | 输入视图按 `slice` 建 → `VideoProcessorBlt` → 返回 RGBA 纹理 |
| 失败回退 | 转换失败就 `has_native_ = false`，走 CPU 路径（不会黑屏） |

注意点：

- 输出纹理格式必须是 `DXGI_FORMAT_R8G8B8A8_UNORM`（RHI 的 RGBA8 只认这个顺序，BGRA 会 `E_INVALIDARG`）
- `VideoProcessorBlt` 之后建议 `context->Flush()`，让提交顺序对 RHI 的采样可见
- 渲染器当前是**回退后的版本**（没有原生分支、没有 `rgba.frag` 管线），这两块要重新加回来