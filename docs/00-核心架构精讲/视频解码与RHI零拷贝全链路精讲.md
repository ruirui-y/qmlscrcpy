# 视频解码与 RHI 零拷贝全链路精讲

> 本文是整条视频链路的**核心必读主线**：一帧手机画面从 TCP 收包第一行，到屏幕亮起最后一个像素。
> 建议先读这篇，再去看其它模块文档；后续所有深挖细节都往这篇的占位块里追加，随代码演进持续维护。
>
> 基于当前最新代码（v0.4.2：`video_reader / h264_decoder / video_item / video_rhi_renderer / d3d12_nv12_convert`）。
> 文中数字全部来自真机实测日志（1264×2780、d3d12va 硬解），复现方法见[第六节](#六怎么验证这条链路)。

## 背景：这条链路在解决什么问题

一帧画面从网线进来到亮在屏幕上，要过**四个执行体**（视频线程、GUI 线程、渲染线程、两个 GPU 队列），
还要跨**两块内存**（系统内存、显存）。链路上每多一次拷贝、每多一次等待，都会变成延迟。

整条链路的设计围绕三个"为什么"：

1. **为什么解码器、转色器、Qt 渲染器必须是同一块 `ID3D12Device`**——设备不统一，纹理就出不了"别家的显存"，零拷贝不成立
2. **为什么等待要放在解码线程而不是渲染线程**——渲染线程卡 1.25ms 是全窗口掉帧，解码线程闲着也是闲着
3. **为什么资源状态要成对切回来**——D3D12 是显式状态机，漏一次就报错/花屏

---

## 一、全局俯瞰：一块设备，四处引用

零拷贝成立的唯一前提是：**三方认的是同一块 GPU 账本**。设备由 Qt RHI 创建
（`src/main.cpp::PrepareSurfaceFormat()` 里 `setGraphicsApi(Direct3D12)` 定死——
Qt 默认在 Windows 上挑 D3D11，不指定就玩不了）。

```
                 Qt RHI 建的 ID3D12Device（COM 对象，侵入式引用计数）
                              │
        ┌─────────────────────┼─────────────────────────┐
        │                     │                         │
   VideoRhiRenderer      VideoItem（画布）        D3d12Nv12Convert
   initialize() 时        ProvideDevice()           Initialize()
   借指针用，             → 手动 AddRef (+1)        → ComPtr 赋值 (+1)
   不自己留引用                │                         │
        │                解码线程经 D3dDevice()          │
        │                取走，交给 FFmpeg               │
        ▼                     ▼                         ▼
   退出时由 Qt 释放      ~VideoItem() Release()     ~Convert() Release()
                              │
                        H264Decoder::OpenD3d12
                        av_buffer_ref 给 FFmpeg（FFmpeg 内部 AddRef）
                              │
                        Close() 里 av_buffer_unref（FFmpeg 内部 Release）
```

### 1.1 引用计数闭环（谁加谁减、什么时候）

| 持有者 | 加引用 | 减引用 | 生命周期 |
|---|---|---|---|
| Qt RHI | 建设备时 +1 | RHI 销毁时 -1 | 基准引用 |
| `VideoItem` | `ProvideDevice()` 里 `AddRef()` | `~VideoItem()` 里 `Release()` | 画布活着设备就活着 |
| `D3d12Nv12Convert` | `Initialize()` 里 `ComPtr` 赋值（自动 +1） | 析构时 `Release()` | 渲染器销毁即归还 |
| `H264Decoder` | `OpenD3d12()` 把设备挂进 `AVBufferRef`（FFmpeg 内部 +1） | `Close()` 里 `av_buffer_unref` → FFmpeg 内部 -1 | 断开连接即归还 |

**为什么要这么小心**：`ID3D12Device` 是 Windows COM 对象，不是普通 C++ 对象——
靠引用计数决定生死，谁 `AddRef` 就必须谁 `Release`，编译器不会替你收拾。
FFmpeg 关闭硬件上下文时会 `Release` 它拿到的设备；如果 `VideoItem` 没自己加一份，
Qt 的设备可能在解码器还活着时被提前释放——表现是随机崩溃或 `DeviceRemoved`。

### 1.2 设备交接的时序问题

RHI 初始化（渲染线程、投屏窗首次绘制）比连接建立（视频线程）**晚**——连接约 1.4s，
首次渲染才建设备。所以解码线程要轮询等：

```
渲染线程                                视频线程（VideoReader::run）
initialize()                            run() 开头
  ├─ getResource(DeviceResource)          ├─ 每 5ms 问一次 D3dDevice()
  ├─ getResource(CommandQueueResource)    ├─ 最多等 500ms（100 次 × 5ms）
  ├─ ProvideDevice() → AddRef             ├─ 拿到 → SetExternalDevice → Open()
  └─ convert_.Initialize() → AddRef       └─ 超时 → 交空设备 → 退软解
```

这是零拷贝链路上唯一的"排队点"。

> **[待补充深挖细节]**：`QSGRendererInterface::getResource` 返回的 DeviceResource /
> CommandQueueResource 是借用指针还是自有指针（Qt 6.8 文档口径）；多次 `initialize()`
> 时引用计数的幂等性；设备丢失（`DXGI_ERROR_DEVICE_REMOVED`）后的重建路径。

---

## 二、第 1 步：收包与硬解（`VideoReader` 线程）

整段全在独立后台线程跑，和界面完全隔离。

### 2.1 TCP 字节流自己拆帧

TCP 没有消息边界，帧边界得自己数。`FillBuffer()` 只往尾部追加（半帧留在 buffer 跨循环保留），
先凑够 **12 字节包头**（`VideoPacketHeader::kSize`）才知道这一帧有多长：

```
12 字节包头（全大端）：
字节 0                              7 8                 11
┌────────────────────────────────────┬───────────────────┐
│ 标志位 + 时间戳（64 位）             │ 负载长度（32 位）   │
└──────────────────┬─────────────────┴───────────────────┘
                   │
   bit63 = SESSION（尺寸变化，scrcpy 5.x 新增）
   bit62 = Config（SPS/PPS 配置包）
   bit61 = 关键帧
   bit0..60 = 展示时间戳 PTS（微秒）
```

**SESSION 包是个特例**：12 字节就是全部，没有负载——高 32 位是标志位，低 32 位是宽，
后 4 字节（`size_` 字段）是高。设备旋转时靠它知道新尺寸。

**这个尺寸是"钦定"的吗？能改吗？** 能影响，但协议只给两个阀门，没有"精确宽×高"：

```
设备屏幕原生分辨率（如 RMX5010 的 1264×2780）
    │  server 端采集后、编码前的两个可调阀门：
    ├─ max_size   长边压到 N 像素，等比缩放，只缩不放（server_args.cpp；0 = 不传 = 原生分辨率）
    └─ crop       裁一块区域（"800:1200:100:200"）
    ▼
编码 → 传输 → 客户端 info 包 / SESSION 包里报的 frame_size_
```

没有精确宽高参数的原因：server 端编码器只接受等比缩放/裁剪，非等比会破坏
输入注入的坐标映射（触摸坐标要按帧尺寸换算）。本项目配置键 `video_wired/max_size`
当前为 0，所以链路全程跑原生分辨率——想省码率/解码开销就把它调成如 1600，
server 端先缩小再编码，一帧数据量全线下降（代价是清晰度）。

`Parse()` 只读不消费：那 12 字节原地留在缓冲里，等整帧到齐再一起删——
避免"包头没了但负载没凑够"的半状态。

### 2.2 配置包要攒着送

SPS/PPS 配置包先存进 `pending_config_`，等下一个视频包到来时拼在前面一起喂
`avcodec_send_packet()`。FFmpeg 自己按 in-band 参数集解析，不用手工拼 extradata。

### 2.3 收帧：硬解返回的是句柄，不是像素

**最核心的认知**：`avcodec_receive_frame()` 在软解下返回 = 像素已算完（CPU 亲自算的）；
**在硬解（d3d12va）下它是"异步交单"**——FFmpeg 在 CPU 上解析完 H.264 包头，
把解码任务提交给显卡解码硬件，然后**不等显卡算完**，立刻返回一个 `AV_PIX_FMT_D3D12` 的帧。
此时 `data[0]` 里装的不是像素，而是一个结构体：

```c
AVD3D12VAFrame {
    ID3D12Resource *texture;       // 显存里的 NV12 纹理（一个数组，装多帧）
    int subresource_index;         // 这张纹理数组里的第几层切片
    AVD3D12VASyncContext sync_ctx  // { fence, fence_value } —— 解码写完没
}
```

所以"解码完成"这件事在 CPU 侧根本看不见，只能靠围栏问。

### 2.4 围栏（Fence）与事件（Event）

- **fence（`ID3D12Fence*`）**：一块 CPU/GPU 共享内存里的 64 位整数计分牌
  （餐厅柜台的电子叫号屏）。**单调递增，不是单帧进度条**——第 105 帧的编号就是 105。
- **fence_value**：印在取餐小票上的目标号码。GPU 把最后一个像素写进显存后，
  亲手把计分牌拨到该值（Signal 指令）。
- **fence_event_（`CreateEventW`）**：Windows 内核事件闹钟。等的时候不忙等
  （自旋烧满一个核），而是定闹钟 + 睡觉（0% CPU），到点被内核微秒级叫醒。

`WaitForDecodeFence()`（h264_decoder.cpp）三步走：

```cpp
if (sync.fence->GetCompletedValue() >= sync.fence_value) return true;   // ① 抬头看叫号屏：多数已解完，0 等待
fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);          // ② 申请闹钟（只建一次，复用）
sync.fence->SetEventOnCompletion(sync.fence_value, fence_event_);       // ③ 定闹钟 + 睡觉，50ms 兜底防死锁
::WaitForSingleObject(fence_event_, 50);
```

> 50ms 是兜底不是常态：真等不到也别把解码线程钉死，渲染时还有一道 GPU 侧兜底（见 4.4）。

### 2.5 `WrapHardwareFrame`：用副单锁住切片（零拷贝不花屏的命门）

关键动作是 `av_frame_ref(keep, hardware_frame)`。先分清 FFmpeg 里的两个东西：

- **AVFrame**：几百字节的"运货单外壳"，本身不装像素
- **AVBufferRef（底层缓冲）**：真正指向显存 DPB 解码池里某层切片的引用计数对象

`DrainFrames()` 的真实执行顺序：

```
avcodec_receive_frame(...)          ② FFmpeg 把第 3 号显存切片填进单子（切片引用计数 = 1）
av_frame_alloc()                    ① 申请一张空运货单
WrapHardwareFrame(av_frame, ...)    ③ 克隆副单 keep = av_frame_ref(keep, hardware_frame)
                                       —— 一个像素都没拷，只是引用计数 1 → 2
av_frame_free(&av_frame)            ④ 撕掉临时单，引用计数 2 → 1（keep 还活着）
frames.append(frame)                ⑤ keep 装进 shared_ptr<const void> owner，跟着 Frame 走
```

**如果没有 keep**：第 ④ 步会把切片引用计数直接打到 0，FFmpeg 认为"3 号坑位没人用了"，
下一毫秒就把新解码的第 4 帧覆盖写进 3 号坑位——而渲染线程可能才刚准备画第 3 帧，
当场撕裂花屏。

owner 的自定义删除器在最后一次引用销毁时调 `av_frame_free`（软解的 `WrapFrame` 也是
同样的 owner 结构——内存是 FFmpeg 用 `/MD` CRT 分配的，必须用它自己的释放函数），
切片引用计数才正式归零，安全还池复用。**「显存切片的生死由 owner 撑着」是本链路第二条纪律。**

### 2.6 为什么等待留在解码线程

这是全项目最漂亮的一次性能调优（`Frame` 交出去之前 CPU 等一次 `sync_ctx.fence`）：

| 方案 | 等待发生在哪 | 实测 |
|---|---|---|
| 旧：解码线程不等，渲染线程转色前等 | 渲染线程 | 每帧多卡 **1.25ms**，上屏延迟 2.5~3.5ms |
| 新：解码线程 `WaitForDecodeFence` 等完再交 | 解码线程 | 解码线程每帧才花 **0.5ms**，余量巨大 |

原因看每帧的工作量分布：

| 线程 | 每帧要干的事 | 余量 |
|---|---|---|
| 解码线程 | 拼包 5µs + 送包 225µs + 收帧 238µs ≈ **0.5ms** | 8.3ms 里剩 7.8ms |
| 渲染线程 | 整个窗口所有元素（工具栏、文字、视频） | 卡 1.25ms 就是全窗口掉帧 |

**等待放在"闲的那条线程"上。** 等 `Frame` 交出去时，显存里的 NV12 已是 100%
解码完毕的成品；渲染线程拿到直接转色上屏，绘制耗时 1.7ms → **0.3ms**，
上屏延迟中位数压进 **1.5~2.5ms**。

> **[待补充深挖细节]**：DPB 纹理数组的层数怎么定（跟编码器参考帧数的关系）；
> `fence_value` 的确切语义（"本帧写完"还是"下一个可用值"）；软解分支 `WrapFrame`
> 与硬解的逐行对比。

---

## 三、第 2 & 3 步：无队列交接与双线程同步

### 3.1 `DirectConnection`：绕开 GUI 事件循环

```cpp
// src/device_controller.cpp:602
QObject::connect(reader_, &VideoReader::SigFrameReady, video_->Surface(), &VideoItem::Present,
                 Qt::DirectConnection);
```

`Present()` **直接在视频线程里原地执行**。哪怕用户正疯狂拖窗口把 GUI 线程卡住
（模态循环），视频帧也不会在事件队列里排队——排队意味着延迟无上限地涨
（拖窗口卡 5 秒的根因就是当初没直连）。

### 3.2 单槽覆盖：永远只画最新一帧

```cpp
// ui/video_item.cpp::Present()，加锁覆盖
{
    QMutexLocker locker(&mutex_);
    replaced = has_pending_;      // 上一帧还没被取走 → 记一次丢帧
    pending_ = frame;             // 直接覆盖
    has_pending_ = true;
    frame_size_ = frame.Size();
}
```

不排队、不做环形缓冲。旧帧被覆盖时它的 `owner` 析构，显存切片立刻还给 FFmpeg——
**画面永远是最新的，延迟不会累积**。投屏要的就是低延迟，队列只会累积延迟。

最后向 GUI 线程投递一次 `update()` 标脏（视频线程直接调 `update()` 不安全，
它是 QQuickItem 的方法，用 `QMetaObject::invokeMethod(..., QueuedConnection)` 投递）。
配合 `QT_QPA_UPDATE_IDLE_TIME=0`（干掉 Qt 给 requestUpdate 留的 5ms 余量），
唤醒延迟 4.41ms → **0.11ms**。设备旋转导致尺寸变化时，同样投递一次 `NotifyFrameSize()`。

### 3.3 `synchronize()`：只搬结构体，不搬像素

`VideoItem` 住 GUI 线程，`VideoRhiRenderer` 住 Qt 渲染线程（QSGRenderThread），
两个线程平时各跑各的，直接互相读变量必然内存冲突。Qt 场景图的解法是**交接班机制**：

```
视频线程 Present() 塞 pending_，投递 update()
        │
GUI 线程收到 update()，标脏
        │
渲染线程准备画下一帧前，被拦住，在 GUI 线程上下文里调一次：
VideoRhiRenderer::synchronize(item)
  ├─ item_->BeginSyncPass()                 埋点：这一轮 synchronize 开始
  ├─ TakePendingFrame(&trace, &arrive_us)   取走 pending_（没有就返回无效帧）
  ├─ frame.HasNative() ?
  │     ├─ SyncNativeFrame()    硬解：只搬 texture/层号/fence/宽高 ≈ 40 字节（0.002ms）
  │     └─ SyncSoftwareFrame()  软解：逐行去掉 stride 填充，拷 Y/UV 两平面
  ├─ item_->EndSyncPass(...)                记到达时刻与耗时
  └─ trace_ = trace                         留给 render 那边结算用
        │
        ▼ 框架保证顺序（synchronize 一定先于 render）
     render() 开始画
```

这个瞬间的特点是**绝对同步、互不打架**——交接本身不需要互斥锁
（`pending_` 的读写互斥只在 Present/TakePendingFrame 之间生效）。
`synchronize()` 一结束，GUI 线程回去响应鼠标点击，渲染线程带着 `native_frame_`
冲进 `render()`，全程无锁。

硬解路径上跨线程"搬运"的仅是约 40 字节的描述符（纹理地址 + 切片号 + 围栏 +
shared_ptr）——**这就是零拷贝的"零"**。

> **[待补充深挖细节]**：`QQuickRhiItem` 内部 sync 阶段的调用时序；为什么
> `synchronize()` 里不能碰 GPU 上下文；多窗口时每窗口一套 renderer 的隔离边界；
> `FrameProbe` 的九段耗时埋点口径与被覆盖旧帧的生命周期结算（OnReplaced）。

---

## 四、第 4 步：GPU 色域转换与 RHI 合成上屏（渲染线程）

> 本节涉及的所有底层概念（命令队列/命令列表、两条队列并行、围栏、资源状态账本），
> 在姊妹篇《[GPU命令模型从零讲起](GPU命令模型从零讲起.md)》里有完整推导——本文只讲项目里怎么用，那边讲为什么。

格式问题：手机发来的是 **NV12**（一张全分辨率亮度图 Y + 一张半宽半高、U/V 交错的色度图 UV），Qt RHI 画布要 **RGBA8**。转换在 GPU 里做，叫"两棒接力"。

### 4.1 第一棒：`D3d12Nv12Convert::Convert`

```
① 视图切片（EnsureViews，同一个资源、不同 PlaneSlice）
   PlaneSlice 0 → DXGI_FORMAT_R8_UNORM   → 当 Y 采样（全分辨率亮度）
   PlaneSlice 1 → DXGI_FORMAT_R8G8_UNORM → 当 UV 采样（半宽半高交错色度）

② ResourceBarrier（成对，见 4.2）
   解码 NV12：COMMON               → PIXEL_SHADER_RESOURCE   （空闲态 → 可读素材）
   rgba_texture_：PSR             → RENDER_TARGET           （可采样 → 可写画纸）

③ DrawInstanced(3, 1, 0, 0)  全屏三角形，SV_VertexID 算顶点，不用顶点缓冲
   VSMain：id = 0/1/2 生成 (-1,-1) (3,-1) (-1,3)，铺满视口
   PSMain：y = (Y - 0.0625) * 1.164; uv -= 0.5;   ← BT.601 有限范围
           rgb = (y + 1.596·U, y − 0.391·U − 0.813·V, y + 2.018·U)

④ ResourceBarrier（立刻原样切回）
   解码 NV12：PIXEL_SHADER_RESOURCE → COMMON        （还给 FFmpeg 解码池）
   rgba_texture_：RENDER_TARGET     → PSR           （还给 Qt RHI）

⑤ 解码围栏 GPU 侧兜底 → ExecuteCommandLists（自己的队列）
   → Signal(自己的围栏) → CPU WaitFence（约 0.15~0.2ms）
```

> `1.164` 这个系数漏了画面就会发灰发闷（对比度被压平）——0.4.1 修过的真实事故；
> 顺带查清 `setSwapInterval(0)` 在 D3D12 下从来没生效过（全仓库没人设 `NoVSync`）。

### 4.2 为什么状态必须成对切回来

D3D12 的资源状态是**显式声明**的，不是驱动兜底。同一块显存扮演不同角色时，
GPU 内部缓存和存储格式完全不同（写缓存/块压缩 vs 只读纹理缓存），切换必须由代码亲手拨：

| 资源 | 切进去 | 切回来 | 漏了会怎样 |
|---|---|---|---|
| 解码 NV12 纹理 | `COMMON → PIXEL_SHADER_RESOURCE` | `PSR → COMMON` | 解码器下帧要把它当写入目标，状态不对报错或画错 |
| 转色目标 RGBA | `PSR → RENDER_TARGET` | `RENDER_TARGET → PSR` | Qt RHI 内部状态缓存认定它是 PSR，不会再发切换命令，直接采样 → `RESOURCE_BARRIER_BEFORE_AFTER_MISMATCH`，轻则脏数据花屏，重则 Device Lost |

对比 D3D11：状态是"当前设置"，谁都能改、没人还；D3D12 把这件事变成每份资源自己的账，
**漏一次立刻报错**——这也是它比 D3D11 零拷贝靠谱的根本原因（三代 TDR 崩溃的解药）。

### 4.3 为什么要 CPU 等一次转色围栏（~0.17ms）

Qt 的 QRhi 在 `render()` 里只给 `QRhiCommandBuffer`，**不暴露原生 D3D12 CommandQueue
的插队等待接口**——没法让 Qt 的队列在采样前等我们的转色队列。务实折中：

```
录命令（Reset allocator/list）→ Close() → ExecuteCommandLists(自己的队列)
   → queue->Signal(++fence_value)
   → 计分牌没到就 SetEventOnCompletion + WaitForSingleObject 睡觉（INFINITE）
```

`WaitFence()` 返回 = RGBA 纹理 100% 写完 + 状态开关全部拨回原位。
**0.17ms 的极短 CPU 等待，换掉了原本 10.5 MB/帧的 PCIe 内存往返。**

### 4.4 解码围栏的 GPU 侧兜底（v0.4.2 加的）

解码线程里的 `WaitForDecodeFence` 是 **CPU 侧等值到**；万一 50ms 超时提前放行，
采样就可能读到解了一半的纹理（实测症状：偶发半帧新半帧旧、色度发绿，一帧即逝）。
`Convert()` 在提交前补一道 GPU 侧兜底：

```cpp
// 解码围栏没到就排队等一次：正常路径零开销（值早就到了），异常路径才真正等
if (decode_fence != nullptr) {
    auto *fence = static_cast<ID3D12Fence *>(decode_fence);
    if (fence->GetCompletedValue() < decode_fence_value) {
        d.queue->Wait(fence, decode_fence_value);
    }
}
```

这条等待排进**我们自己的转色队列**，保证队列里后续的转色命令一定排在解码完成之后。
延迟收益没有回吐（实测转色等围栏 ~157µs，与 0.4.1 持平）。

### 4.5 归还 DPB 切片的时机

```cpp
// ui/video_rhi_renderer.cpp::RenderNativeFrame()
has_frame_ = false;
native_frame_ = qsc::stream::Frame();   // owner 析构 → av_frame_free → 切片还给解码池
item_->RecordConvert(...);              // 转色耗时交给探针
```

转色一完成就松手，**不能等这一帧画完再放**：DPB 池子层数有限，早还一层，
解码器就能早一步解下一帧。

### 4.6 第二棒：Qt RHI 合成上屏

```cpp
const QRhiCommandBuffer::VertexInput input(vertex_buffer_, 0);
cb->beginPass(renderTarget(), 黑底, {1.0f, 0});
cb->setGraphicsPipeline(rgba_pipeline_);   // 采样 rgba_texture_ 的着色器
cb->setViewport(...);
cb->setShaderResources(rgba_bindings_);    // 指向刚画好的彩色照片
cb->setVertexInput(0, 1, &input);
cb->draw(4);                               // 4 顶点矩形
cb->endPass();                             // 交给 Qt 场景图叠工具栏、标题栏，再推显示器
```

> 注意：`QQuickRhiItem` **不会替你开 render pass**，不调 `beginPass` 就是纯黑。

### 4.7 尺寸与缩放：为什么找不到一行"缩小"的代码

帧是 1264×2780（原生分辨率），窗口视频区可能只有 412×908——尺寸在哪一步缩小的？
答案：**没有"缩小"这个代码步骤，缩放是转色那次 Draw 采样的副产品**。

关键在 `rgba_texture_` 是按**窗口尺寸**建的，不是按帧尺寸：

```cpp
// RenderNativeFrame() 里
const QSize target_size = renderTarget()->pixelSize();      // 窗口视频区的像素大小
convert_.Convert(native.texture, ...,
                 reinterpret_cast<void *>(target_object),   // 目标 = rgba_texture_
                 target_size.width(), target_size.height()) // ← 转色目标就是窗口尺寸
```

所以转色那次 `DrawInstanced` 一次干完两件事：

```
NV12 纹理 1264×2780  ──采样──→  rgba_texture_ 412×908
（显存里的大图）      GPU 纹理单元     （窗口大小）
                    线性插值缩放
                    + BT.601 转色
```

VSMain 铺满整个视口（UV 0→1 覆盖整张源纹理），PSMain 每输出一个像素采样一次源
纹理——源大目标小，**纹理单元的线性插值顺手完成缩放**，硬件本职、零额外开销。
之后 `draw(4)` 把已是窗口尺寸的 rgba 贴进 Qt 画布，1:1 不再缩。
缩放比永远隐式等于"源纹理 : 画布"，窗口拖大拖小、旋转换尺寸都是同一套逻辑
（`EnsureRgbaPipeline` 检测到 renderTarget 尺寸变化就重建 rgba_texture_）。

窗口与画面的适配方向也值得记一笔——不是"画面塞进窗口"，而是**窗口反过来套画面**
（`VideoWindow.qml::fitToFrame()`）：首帧到达时按帧比例算出"刚好装下画面 + 工具栏"
的窗口大小（受桌面可用区 90%/85% 封顶），几何记进设置下次恢复；
`VideoSurface { keepAspect: false }` 敢于拉满，正因为窗口长宽比永远跟帧一致。

> 想真正降低链路里的数据量（而不是只缩小显示），阀门在 server 端：
> 配置键 `video_wired/max_size`（见 2.1）——编码前缩小，码率/传输/解码全线受益。

### 4.8 软解路径（对称的另一条腿）

驱动不支持 d3d12va 时退回软解：`SyncSoftwareFrame` 把 YUV420P 逐行去 stride 拷进
`plane_y_/plane_uv_`（紧凑拷贝，无行填充），`RenderSoftwareFrame` 上传两张 RHI 纹理
（R8 + RG8），用 `nv12.frag.qsb` 转色画进 item。与硬解路径完全对称，
`render()` 里一个 `if` 分发，零冗余判断。

> **[待补充深挖细节]**：根签名的两个参数（根常量 vs 描述符表）、描述符堆的
> CPU/GPU 句柄区别、静态采样器为什么省一个堆、HLSL 运行时 D3DCompile 的参数、
> `rgba_texture_->nativeTexture().object` 桥接 Qt RHI 纹理与原生
> `ID3D12Resource*` 的细节。

---

## 五、四代技术演进因果

这条链路不是设计出来的，是被需求和事故逼出来的：

| 代 | 做法 | 一帧搬多少 | 上屏帧率 | 死在哪 |
|---|---|---|---|---|
| 一 | CPU 软解 + OpenGL FBO 三平面 | 0（全在内存算） | 60 | CPU 算不动；4 线程并行换 3 帧延迟 |
| 二 | D3D11 硬解 + 读回内存 | 21.1 MB（其中 10.5 MB 走 PCIe） | 100~120 | 读回 3.3ms，其中 2.1ms 在等 Map |
| 三 | D3D11 同设备零拷贝 | 0（理论） | 崩溃 | 共用 ImmediateContext，隐式状态互相踩 → TDR（三次实验全部失败，证据见《零拷贝为什么失败》） |
| 四 | **D3D12 + Qt6 RHI 零拷贝** | 9.8 MB，全在显存 | 110~120 | 只剩 Qt 提交节拍 |

一句话因果链：**先让 GPU 解码（一到二），再想办法别搬回内存（二到三），
最后发现搬不搬不是问题、谁改状态才是（三到四）。**

④ 相对 ② 的收益：每帧 10.5MB PCIe 往返 → **0**；客户端总延迟 6.3ms → **~2.4ms**
（5ms 来自关掉 Qt 重绘余量，1.25ms 来自围栏等待搬家，收尾是转色围栏 0.17ms）。

> 每一步演进都有文档存证：`docs/03-视频与解码/` 下的 Qt6迁移 / GPU渲染改造 / D3D12迁移 /
> 零拷贝架构，以及《视频链路与图形API演进全景图》的完整因果链。

> **[待补充深挖细节]**：三代崩溃时的具体调用栈（`nvwgf2umx.dll`）；Gen1/Gen4 的
> PCIe 带宽实测；Qt `QQuickFramebufferObject` 与 `QQuickRhiItem` 的插口差异。

---

## 六、怎么验证这条链路

环境变量（都是启动前设，程序只在启动时读一次）：

```powershell
$env:QS_LOG_LEVEL='info'      # 临时改日志级别（debug/info/warn/error）
$env:QS_TRACE_FRAMES='1'      # 渲染侧逐帧：到达间隔 / synchronize / 绘制 / 提交 / 上屏延迟分档
$env:QS_TRACE_DECODE='1'      # 解码侧：拼包 / 送包 / 收帧 / 合计 / 最慢（每秒一行）
$env:QS_GRAB='D:\grab.png'    # 启动 6 秒后抓一张投屏窗截图再退出（无人值守自检）
```

健康的样子（本机 1264×2780，逐行解释）：

```
[decode] 硬解 d3d12va                              ← OpenHardware 成功走 d3d12va（失败会打"退回软解"）
[decode] 首帧格式 121（D3D11 = 114 D3D12 = 121）    ← AV_PIX_FMT_D3D12，硬解链路成立
[decode] 秒结 121 帧（硬解）：拼包 5us / 送包 225us / 收帧 238us / 合计 468us / 最慢 492us
                                                   ← 解码线程每帧 0.5ms，等围栏的余量就是从这里算出来的
[rhi] 零拷贝管线就绪 QSize(412, 908)                ← rgba_texture_ 建好，转色管线可用
[convert] 164 帧：录命令+提交 92 us，等围栏 202 us   ← 两段耗时分开了：录命令+提交 ~92µs，CPU 睡觉等 GPU ~0.2ms
帧 1112 上屏：synchronize 0.003 ms，绘制 0.419 ms，提交 0.68 ms
                                                   ← 逐帧九段里的三段；synchronize 0.003ms 就是"只搬 40 字节"
[perf] 1s 到达 120 帧，上屏 120 帧，丢 0（0%），实际 X Mbps
                                                   ← 丢 0 且到达=上屏，说明单槽覆盖没丢帧、链路没堵
[perf] 上屏延迟 <1.5ms 74% / 1.5~2.5ms 20% / ...，最大 X ms
                                                   ← 延迟分档，健康时绝大部分落在 2.5ms 以内
```

排查口诀：**丢帧看 `[perf]` 的"丢"字，卡顿看"上屏延迟"分档，解码瓶颈看"合计/最慢"，
转色瓶颈看 `[convert]` 两段**。

---

## 七、四根骨架（读代码前只需要记住这些）

D3D12 的根签名、描述符堆、HLSL 编译参数这类八股样板，本来就是写给机器看的。
作为读者真正需要装进脑子的只有四根：

1. **线程边界**：收包解码在视频线程（VideoReader），交接在视频线程→GUI 线程（Present 写 /
   synchronize 读，互斥护住），画图在渲染线程（render / Convert），
   所有 D3D12 提交只发生在渲染线程自己的队列上。
2. **资源所有权**：`ID3D12Device` 四方 AddRef/Release 成对闭环；DPB 显存切片的生死由
   `Frame.owner`（keep 副单）撑着，转色完成立刻 `native_frame_ = Frame()` 归还。
3. **状态机隔离**：D3D11 共用设备死于隐式状态污染（TDR），D3D12 靠独立 CommandList +
   **成对 ResourceBarrier** 解决——切出去的每个状态都必须切回来。
4. **同步卡点**：三处围栏各管一件事——解码围栏（解码线程等，~1.1ms）、
   转色围栏（渲染线程等自己的队列，~0.17ms）、解码围栏 GPU 侧兜底
   （转色队列 `queue->Wait`，正常路径零开销）。

---

## 附：一帧的完整旅程（速查）

```
设备 MediaCodec（手机端 scrcpy-server 5.0.1）
    │ 12 字节头 + H.264 负载（大端）
    ▼
adb 隧道（reverse tcp:27183，失败降级 forward）
    ▼
【视频线程 VideoReader::run】
    FillBuffer 凑 12 字节包头 → Parse 标志位/PTS/长度（只读不消费）
      SESSION 包：只有宽高，continue；Config 包 → H264Decoder 暂存 pending_config_
      → H264Decoder::Push（下一帧拼着送）→ avcodec_send_packet
      → DrainFrames → avcodec_receive_frame（异步交单，返回 AV_PIX_FMT_D3D12）
      → WrapHardwareFrame：keep 副单引用计数 1→2，提取纹理/切片/围栏
      → WaitForDecodeFence（解码线程等 ~1.1ms：GetCompletedValue ≥ fence_value?）
      → SigFrameReady(Frame)
        │ Qt::DirectConnection，原地执行
        ▼
【仍在视频线程】VideoItem::Present
    加锁 pending_ = frame（覆盖丢旧帧，旧帧 owner 析构即还切片）→ 投递 update()
        │ QueuedConnection + QT_QPA_UPDATE_IDLE_TIME=0（唤醒 0.11ms）
        ▼
【GUI 线程】Qt 场景图拦截渲染线程 → VideoRhiRenderer::synchronize
    TakePendingFrame（≈40 字节描述符，0.002ms）→ SyncNativeFrame
        │
        ▼
【渲染线程 QSGRenderThread】VideoRhiRenderer::render → RenderNativeFrame
    EnsureRgbaPipeline（缩放时重建 rgba_texture_）
    D3d12Nv12Convert::Convert
      ① EnsureViews：NV12 拆 Y/UV 两个 SRV
      ② Barrier：COMMON→PSR / PSR→RT
      ③ DrawInstanced(3,1,0,0)：SV_VertexID 全屏三角形 + BT.601 → rgba_texture_
      ④ Barrier：原样切回（还 FFmpeg / 还 RHI）
      ⑤ queue->Wait(解码围栏兜底) → ExecuteCommandLists → WaitFence（~0.17ms）
    native_frame_ = Frame()（还 DPB 切片）
    cb->beginPass → rgba 管线 draw(4) → endPass
        ▼
Qt 合成（视频矩形 + QML 控件）→ DWM → 屏幕
```
