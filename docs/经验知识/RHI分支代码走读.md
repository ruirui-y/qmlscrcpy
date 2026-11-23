# RHI 分支代码走读（exp/zero-copy）

这篇是给"想自己慢慢读代码"用的地图：**先看结构和顺序，再钻细节**。

分支：`exp/zero-copy` @ `f45e16d`
对照：`master`（FBO + OpenGL，读回内存）

---

## 一、这个分支到底改了什么

| 维度 | master | exp/zero-copy |
|---|---|---|
| 画布基类 | `QQuickFramebufferObject` | **`QQuickRhiItem`**（`video_item.h:61`） |
| 渲染器 | `video_renderer.*`（裸 GL，599 行） | **`video_rhi_renderer.*`**（RHI，550 行） |
| 图形 API | `setGraphicsApi(OpenGL)`（`main.cpp:76`） | **`setGraphicsApi(Direct3D11)`**（`main.cpp:75`） |
| 着色器 | GLSL 字符串写在 cpp 里 | `ui/shaders/*.vert / *.frag` + 编译成 `.qsb` |
| 帧结构 | 只有内存里的 NV12 | 多一条「原生纹理指针 + 切片号 + keep_alive」 |
| 解码器 | 只读回内存 | 多 `SetExternalDevice`（共享 Qt 的 D3D11 设备）+ 零拷贝帧 |
| 结果 | 稳定（3.3ms/帧） | 能解出帧，一渲染就 TDR |

---

## 二、启动时定生死的一行

```cpp
// main.cpp:75
QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
// 注释原话：RHI 渲染器要 D3D11：和硬解共用设备，纹理可直接引用
```

这一行决定后面所有事：

```
Qt RHI 后端 = D3D11
    ↓
rhi->nativeHandles() 能拿到 ID3D11Device / ID3D11DeviceContext
    ↓
我们才能把 D3D11 解码纹理包成 QRhiTexture
```

---

## 三、文件职责

| 文件 | 干什么 |
|---|---|
| `ui/video_item.h/.cpp` | 画布：`class VideoItem : public QQuickRhiItem`，`createRenderer()`（`video_item.h:72`） |
| `ui/video_rhi_renderer.h/.cpp` | 真正的渲染逻辑：管线、纹理、上传、转色、绘制 |
| `ui/video_frame_source.h` | 帧来源接口（渲染线程向画布要帧，避免直接摸 GUI 对象） |
| `ui/shaders/quad.vert` | 顶点：把矩形铺满画布 |
| `ui/shaders/nv12.frag` | Y + UV 两平面转 RGB（软件路径用） |
| `ui/shaders/rgba.frag` | 已经转好的 RGBA（原生路径用） |
| `libs/stream/h264_decoder.*` | 多出「零拷贝帧」：纹理指针 + 切片 + keep_alive |

---

## 四、一帧的完整路径

```
socket 线程
  │  收包 → decoder->Push(...)
  │     硬解：avcodec_receive_frame
  ▼
Frame { 纹理指针, 切片号, keep_alive, 尺寸 }
  │  Present(frame) 交给画布（像素仍在显存）
  ▼
GUI 线程  synchronize(QQuickRhiItem *item)
  │  软件路径：把 Y/UV 拷进 plane_y_ / plane_uv_（紧凑排布）
  │  硬件路径：记下 native_ptr_ / native_slice_ / native_size_
  ▼
渲染线程  render(QRhiCommandBuffer *cb)          ← 核心，第 474 行
  │
  ├─ ① 顶点缓冲（只传一次，必须在 beginPass 之前）
  ├─ ② 软件分支：uploadTexture(texture_y_) + uploadTexture(texture_uv_)
  └─ ③ 原生分支：
        EnsureNativeConverter(size)      // 用 Qt 的设备建 VideoProcessor
          ↓
        ConvertNativeFrame()             // VideoProcessorBlt：NV12 → RGBA
          ↓
        NativeWrapFor(rgba, size)        // 包成 QRhiTexture
          ↓
        setBindings → setShaderResourceBindings → draw(4)
  ▼
Qt 把 render target 合成到窗口
```

---

## 四之二、补全后的完整链路（含前提与卡点）

上面那张图省略了五个必要前提，这里一次补齐。

```
你的版本                          缺什么
─────────────────────────────────────────────────────────────
① 解码 → NV12 纹理                 缺：纹理是数组的一层（slice）、属于哪个设备
② 转色 a) VideoProcessor → BGRA    缺：它要在"谁的上下文"上执行（决定生死）
   转色 b) 上传 Y/UV + 着色器       缺：上传之前必须先"读回内存"这一步
③ QRhiTexture(BGRA8 或 R8+RG8)     缺：createFrom 要求同一设备；两平面要建两张纹理
④ Qt 合成                          缺：尺寸变化重建、纹理生命周期、颜色范围
```

```
① 解码（硬件固定功能单元）
   H.264 码流
      ↓ avcodec_receive_frame
   NV12 纹理（在显存，属于某 ID3D11Device）
   ├─ 是 texture array 的一层：要带 slice 号
   └─ 谁建的设备就归谁账本（我们建的，FFmpeg 借用）

② 转色（二选一，这一步决定成败）
   a) GPU 固定功能：VideoProcessorBlt
        NV12 + slice → BGRA 纹理
        需要：ID3D11VideoDevice / ID3D11VideoContext，
              且这两个必须来自"能访问该纹理的那个设备"
        副作用：改渲染目标/视口/状态  ← 卡点在这
   b) CPU 读回 + 着色器：
        NV12 纹理 → CopySubresourceRegion 到 staging
                  → Map（等 GPU 写完）
                  → memcpy 到内存（Y 和 UV 两个平面）
        然后把两平面交给 ③ 上传

③ 变成 QRhi 能采样的纹理
   a) BGRA 纹理 → QRhiTexture::createFrom(NativeTexture{BGRA8})
        前提：纹理必须在 RHI 自己的设备上
   b) 两平面 → 两张 QRhiTexture（R8 + RG8），
        用 QRhiResourceUpdateBatch::uploadTexture 上传
        时机：必须在 cb->beginPass() 之前（注释原话：beginPass 之前才能传资源）

④ RHI 绘制 + Qt 合成
   setGraphicsPipeline → setShaderResources → draw(4) → endPass
        ↓
   Qt 把 render target 当纹理合成到窗口
   （此时我们画的矩形已经是 RGB，Qt 不做任何转色）
```

### 逐环节的坑

| 环节 | 容易漏的前提 |
|---|---|
| ① 解码 | 纹理是数组，必须带 slice；尺寸/格式变了要重建下游资源 |
| ②a VideoProcessor | 需要 `ID3D11VideoDevice` + `ID3D11VideoContext`，且必须和纹理同设备；**会改状态** |
| ②b 读回 | `Map` 是阻塞点（0.57~2.1ms）；RowPitch 有对齐填充，要逐行 memcpy |
| ③a createFrom | 必须同设备；声明格式必须和实际一致（NV12 当 RGBA 读会乱） |
| ③b 两平面上传 | 必须 beginPass 之前；`QRhiTexture::R8/RG8` 要按 plane 尺寸建 |
| ④ 绘制 | BGRA8 要用正确 swizzle；颜色范围（limited/full）不一致会发灰或过饱和 |

### 卡点（唯一一个真正的拦路虎）

```
②a 的 VideoProcessorBlt 执行时，会改 D3D11 的渲染目标、视口、管线状态。
而这些状态正被 Qt RHI 缓存着，它下一轮 draw 直接用缓存值
   ↓
状态错乱 → GPU 挂起 → 驱动重置设备（TDR）
   ↓
日志：Device loss detected in Present()
```

其他环节都是"多做几步"，只有这一环是"做了就崩"。

状态为什么会被改坏、缓存为什么救不了，见 [场景图绘制与D3D11状态](场景图绘制与D3D11状态.md)。

### 备选方案

| 方案 | 做法 | 代价 | 可行性 |
|---|---|---|---|
| **A 状态保存/恢复** | blit 前后保存 RTV/视口并恢复 | 10 行改动，风险中（RHI 可能还缓存别的状态） | 未验证，值得先试 |
| **B 读回内存（现状）** | ②b → ③b | 每帧 2~3ms | ✅ 已上线 |
| **C 换 D3D12/Vulkan** | 多平面格式直接采样，不用 VideoProcessor | FFmpeg 要 7.0+、渲染器重写、D3D12 样板 ×2~3 | 能根治，成本 1~2 周 |
| **D 独立设备 + 共享句柄** | 解码用另一设备，共享纹理给 RHI | Qt RHI 缺导入接口，要自己调原生 D3D | 理论可行，实操最难 |

---

## 五、七个关键函数

### 1. `initialize(QRhiCommandBuffer *cb)`（`video_rhi_renderer.cpp`）

首次初始化：确认后端是 D3D11、加载 QSB 着色器、建管线 / 采样器 / 顶点缓冲。

**坑**：RHI 头文件不在默认 include 路径里，`QRhi*` 要单独加。

### 2. `synchronize(QQuickRhiItem *item)`

GUI 线程执行，**没有 GPU 上下文**。所以只能做内存拷贝和状态记录：

```cpp
// 软件路径：紧凑拷贝，去掉行填充（RowPitch 可能有对齐填充）
CopyPlane(plane_y_, frame.Y(), width, height, frame.StrideY());

// 硬件路径：只记指针，不碰 GPU
native_ptr_ = frame.Texture();     // ID3D11Texture2D*
native_slice_ = frame.Slice();     // 数组层号
```

### 3. `render(QRhiCommandBuffer *cb)`（第 474 行）

三类分支按顺序走：

**① 顶点缓冲**

```cpp
if (!vertex_uploaded_ && vertex_buffer_ != nullptr)
{
    // beginPass 之前才能传资源：这里还没进 pass
    QRhiResourceUpdateBatch *batch = rhi_->nextResourceUpdateBatch();
    batch->uploadStaticBuffer(vertex_buffer_, kQuadVertices);
    cb->resourceUpdate(batch);
    vertex_uploaded_ = true;
}
```

**② 软件分支（两平面）**

```cpp
const QRhiTextureUploadDescription y_desc(QRhiTextureUploadEntry(
    0, 0, QRhiTextureSubresourceUploadDescription(plane_y_.constData(), plane_y_.size())));
batch->uploadTexture(texture_y_, y_desc);      // R8
batch->uploadTexture(texture_uv_, uv_desc);    // RG8（U/V 交错）
```

**③ 原生分支（零拷贝）**

```cpp
void *rgba = EnsureNativeConverter(native_size_) ? ConvertNativeFrame() : nullptr;
QRhiTexture *wrapped = rgba ? NativeWrapFor(rgba, native_size_) : nullptr;
rgba_bindings_->setBindings({QRhiShaderResourceBinding::sampledTexture(
    0, QRhiShaderResourceBinding::FragmentStage, wrapped, sampler_)});
```

### 4. `EnsureTextures(const QSize &size)`

建/重建两平面纹理：`texture_y_`（R8）、`texture_uv_`（RG8）。尺寸变了才重建。

### 5. `EnsureNativeConverter(const QSize &size)`（第 186 行）

用 **Qt 的设备**建 VideoProcessor：

```cpp
auto *handles = static_cast<const QRhiD3D11NativeHandles *>(rhi_->nativeHandles());
auto *device  = static_cast<ID3D11Device *>(handles->dev);
auto *context = static_cast<ID3D11DeviceContext *>(handles->context);

device->QueryInterface(__uuidof(ID3D11VideoDevice), ...);
context->QueryInterface(__uuidof(ID3D11VideoContext), ...);
```

**注意**：这里拿到的 `context` 就是 Qt RHI 正在使用的那个即时上下文。

### 6. `ConvertNativeFrame()`（第 275 行）

**崩溃点就在这个函数里。**

```cpp
D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
input_desc.Texture2D.ArraySlice = native_slice_;   // 解码纹理是数组，取哪一层
video_device->CreateVideoProcessorInputView(texture, enumerator, &input_desc, &input_view);

D3D11_VIDEO_PROCESSOR_STREAM stream = {};
stream.Enable = TRUE;
video_context->VideoProcessorBlt(processor, output_view, 0, 1, &stream);   // ← 这一句
```

`VideoProcessorBlt` 会改渲染目标、视口、状态，而这些资源正被 Qt RHI 使用。

### 7. `NativeWrapFor(void *source, const QSize &size)`（第 363 行）

把原生纹理包成 `QRhiTexture`，按源指针缓存：

```cpp
QRhiTexture *texture = rhi_->newTexture(QRhiTexture::RGBA8, size);
QRhiTexture::NativeTexture native_src;
native_src.object = reinterpret_cast<quint64>(source);   // ID3D11Texture2D*
native_src.layout = 0;
texture->createFrom(native_src);                          // ← Qt 的导入接口
```

**更正一处旧说法**：我之前说"Qt 6.6 不能导入外部纹理"，不准确。
`QRhiTexture::createFrom(NativeTexture)` 确实存在，但要求 **纹理在 RHI 自己的设备上**。

---

## 六、崩溃点定位

日志原文：

```
[decode] 首帧格式 172（D3D11 = 172）
Device loss detected in Present()
Graphics device lost, cleaning up scenegraph and releasing RHI
```

对应的代码位置：

```
video_rhi_renderer.cpp:275  ConvertNativeFrame()
    ↓
video_context->VideoProcessorBlt(...)        ← 抢 Qt RHI 的即时上下文
    ↓
命令顺序错乱 → GPU 挂起 → 驱动重置设备（TDR）
```

**为什么同线程也不行**：Qt RHI 假设自己独占即时上下文，这个假设被我们打破了。

---

## 七、两条可行的改造路径

### 路径 A：解码用独立设备 + 共享句柄

```
解码设备 D1（我们建）
   └─ 解码纹理（D3D11_RESOURCE_MISC_SHARED）→ 共享句柄
        ↓
渲染设备 D2（Qt RHI 的）
   └─ OpenSharedResource(handle) → 纹理
        ↓
同步：keyed mutex
```

**卡点**：`QRhiTexture::createFrom(NativeTexture)` 不能打开别设备的句柄；
要自己做就得直接调原生 D3D11 → 又回到抢上下文。

### 路径 B：多一次 GPU 拷贝

```
FFmpeg 解码（自己的设备 / 或 Qt 的设备）
    ↓
CopyResource 到一张 RHI 设备上的纹理（GPU 内拷贝，不经过 CPU）
    ↓
RHI 正常采样，不插原生命令
```

**关键**：`CopyResource` 也在 D3D11 上执行，但它是**纯拷贝**、不改渲染状态；
如果放在 `beginPass` 之前、由我们自己控制顺序，风险比 `VideoProcessorBlt` 小得多。

代价：GPU 内多一次 5MB 拷贝（比读回内存的 3.3ms 便宜很多）。

### 路径 B 的变体（你的单线程思路）

把解码也挪进渲染线程：

```
render() 里：
  ① 从线程安全队列取包
  ② avcodec_send_packet / receive_frame
  ③ CopyResource 解码纹理 → RHI 纹理
  ④ 正常绘制
```

好处：没有跨线程，顺序天然正确。
风险：解码慢帧会拖累渲染节拍；FFmpeg 内部仍会向同一个上下文提交命令。

---

## 八、遗留问题清单

| # | 问题 | 状态 |
|---|---|---|
| 1 | `VideoProcessorBlt` 抢上下文 → TDR | 已知，未解决 |
| 2 | 软解路径是否还能用 | 分支里保留了两平面上传路径 |
| 3 | `createFrom` 包装的纹理生命周期 | 有缓存，但没验证长时间运行 |
| 4 | QSB 着色器要随 exe 打包 | collect.ps1 要带上 `shaders/` |
| 5 | 设备和上下文归属 | FFmpeg 用 `SetExternalDevice` 拿 Qt 的（提交 `44c96aa`） |

---

## 九、master 和这个分支怎么选

```
master（FBO + GL + 读回）
  优点：稳、已验证、软解也能用
  缺点：每帧多 2ms 搬运

exp/zero-copy（RHI + D3D11 + 零拷贝）
  优点：理论省 2ms
  缺点：会 TDR，需要走路径 A/B 才能继续
```

**现在 master 是主线**，这个分支当资料保留。

---

## 十、一句话总结

```
这个分支做了三件事：
  ① 把画布从 FBO 换成 QQuickRhiItem，后端钉在 D3D11
  ② 让 FFmpeg 用 Qt 的设备解码（SetExternalDevice）
  ③ 在 render() 里用 VideoProcessorBlt 转 NV12 → RGBA 再包成 QRhiTexture

③ 是唯一崩的一步，因为它抢了 Qt RHI 的即时上下文。
剩下能走的路：多一次 GPU 拷贝（路径 B），或者换后端重写（成本高）。
```
