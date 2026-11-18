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

## 五、四步改造计划（每步可回退）

| 步 | 内容 | 状态 |
|---|---|---|
| **1** | 解码器支持导入外部 D3D11 设备（`SetExternalDevice`） | ✅ 已完成（提交 `44c96aa`） |
| **2** | 重写 `video_renderer.*` 为 QRhi 版，先接现有 CPU 帧（YUV420P / NV12 上传） | 待做 |
| **3** | `VideoItem` 基类换 `QQuickRhiItem`，`main.cpp` 切 D3D11 后端 | 待做 |
| **4** | 原生纹理通路：VideoProcessor 输出 RGBA8 → `createFrom` → 直接画 | 待做 |

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
