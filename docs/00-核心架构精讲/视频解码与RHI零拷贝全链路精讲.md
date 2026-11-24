# 视频解码与 RHI 零拷贝全链路精讲

> 这是本项目**最核心的一条主线**，建议先读这篇，再去看其它模块文档。
> 后续所有细节都往这篇里追加，不再另开零散文档。

## 背景

一帧手机画面从网线进来到亮在 Windows 屏幕上，中间要过四个执行体（视频线程、GUI 线程、渲染线程、两个 GPU 队列），
还要跨两块内存（系统内存、显存）。这条链路上每多一次拷贝、每多一次等待，都会变成延迟。

这篇按"一帧的旅行顺序"讲透整条链路，重点是三个"为什么"：

1. 为什么解码器、转色器、Qt 渲染器必须是同一块 `ID3D12Device`
2. 为什么等待要放在解码线程而不是渲染线程
3. 为什么资源状态要成对切回来

数字全部来自实测日志（1264×2780、硬解、d3d12va），日志开关见文末。

## 一、全局俯瞰：一块设备，四处引用

零拷贝能成立的唯一前提是：**三方认的是同一块 GPU 账本**。Qt 建了设备，谁想用都得自己 `AddRef` 留一份，
否则 Qt 一释放，别人的指针就成了野指针。

```
                 Qt RHI 建的 ID3D12Device（COM 引用计数）
                              │
        ┌─────────────────────┼─────────────────────────┐
        │                     │                         │
   VideoRhiRenderer      VideoItem（画布）        D3d12Nv12Convert
   只是"借指针"，         ProvideDevice()           Initialize()
   自己不留引用            → AddRef (+1)             → AddRef (+1)
        │                     │                         │
        │                解码线程经 D3dDevice()     impl_->device 再 +1
        │                取走，交给 FFmpeg              （ComPtr 自己加）
        ▼                     ▼                         ▼
   退出时由 Qt 释放      ~VideoItem() Release()     Release() 放掉两份
```

引用计数闭环（谁加谁减、什么时候）：

| 持有者 | 加引用 | 减引用 | 结果 |
|---|---|---|---|
| Qt RHI | 建设备时 +1 | RHI 销毁时 -1 | 基准引用 |
| `VideoItem` | `ProvideDevice()` 里 `AddRef()` | `~VideoItem()` 里 `Release()` | 画布活着设备就活着 |
| `H264Decoder` | `OpenD3d12()` 把设备挂进 `AVBufferRef` | `Close()` 里 `av_buffer_unref` → FFmpeg 释放 | 断开连接即归还 |
| `D3d12Nv12Convert` | `Initialize()` 里 `AddRef()` + `ComPtr` 赋值 | `Release()` 里放掉 | 渲染器销毁即归还 |

为什么要这么小心：FFmpeg 关闭硬件上下文时会 `Release` 它拿到的设备。如果 `VideoItem` 没自己加一份，
Qt 的设备可能在解码器还活着的时候被提前释放 —— 表现是随机崩溃或 `DeviceRemoved`。

设备交接的两个时间点：RHI 初始化（渲染线程）比连接建立（视频线程）晚，所以解码线程要等：

```
渲染线程                                视频线程
initialize()                            run()
  ├─ 拿 DeviceResource                   ├─ 每 5ms 问一次 D3dDevice()
  ├─ ProvideDevice() → AddRef            ├─ 最多等 500ms
  └─ convert_.Initialize() → AddRef      ├─ 拿到 → SetExternalDevice → Open()
                                         └─ 超时 → 退软解
```

> [待补充深挖细节]：RHI 的 `getResource` 返回的是借用指针还是自有指针（Qt 6.8 文档口径）、
> 多次 `initialize()` 时的引用计数幂等性、设备丢失（`DXGI_ERROR_DEVICE_REMOVED`）后的重建路径。

## 二、第 1 步：收包与硬解（`VideoReader` 线程）

全在后台线程，和界面完全隔离。

### 2.1 TCP 字节流自己拆帧

TCP 没有消息边界，所以先凑够 **12 字节包头**（`VideoPacketHeader::kSize`）才知道这一帧多长：

```
┌──────────────── 12 字节 ────────────────┐
│ 8 字节 pts_flags │  4 字节 payload 长度 │
└──────────────────┬──────────────────────┘
                   │
   bit63 = SESSION（尺寸变化，后面没负载，只有宽高）
   bit62 = Config（SPS/PPS）
   bit61 = 关键帧
   bit0~60 = PTS 时间戳
```

Parse 只读不消费：那 12 字节原地留着，等整帧到齐再一起从缓冲里删 —— 避免"包头没了但负载没凑够"的半状态。

### 2.2 送包：配置包要攒着

SPS/PPS 配置包先存进 `pending_config_`，等下一个视频包到来时拼在前面一起喂 `avcodec_send_packet()`。
FFmpeg 自己按 in-band 参数集解析，不用我们手工拼 extradata。

### 2.3 收帧：硬解返回的是句柄，不是像素

`avcodec_receive_frame()` 在硬解下**不等显卡算完**，立刻返回一个 `AV_PIX_FMT_D3D12` 的帧，
里面装的是：

```c
AVD3D12VAFrame {
    ID3D12Resource *texture;      // 显存里的 NV12 纹理（一个数组，装多帧）
    int subresource_index;        // 这张纹理里的第几层
    AVD3D12VASyncContext sync_ctx // { fence, fence_value } —— 解码写完没
}
```

所以"解码完成"这件事，我们在 CPU 侧根本看不见，只能靠围栏问。

### 2.4 `WrapHardwareFrame`：用副单锁住切片

关键动作是 `av_frame_ref(keep, hardware_frame)`：给这帧加一份引用，装进 `Frame` 的 `owner`（`shared_ptr`）。
只要 `owner` 没析构，FFmpeg 的解码池就**不会拿这层切片去解下一帧** —— 这是零拷贝安全的基础，
否则你正在采样，解码器把同一块显存改写成下一帧，画面就是撕裂的。

```
avcodec_receive_frame → 硬件帧（池里的一层）
        │
        ├─ av_frame_ref(keep)          ← 引用计数 +1，锁住这一层
        ├─ WaitForDecodeFence(sync)    ← CPU 等 GPU 写完（约 1.1ms）
        └─ Frame{ texture, 层号, fence, fence_value, owner=keep }
```

### 2.5 为什么这个等待要留在解码线程

实测：如果让渲染线程去等这个围栏，每帧要卡 **1.25ms**；挪到解码线程后，渲染线程等自己的围栏只要 **0.17ms**。

原因很简单：

| 线程 | 每帧要干的事 | 余量 |
|---|---|---|
| 解码线程 | 拼包 5us + 送包 225us + 收帧 238us ≈ 0.5ms | 8.3ms 里剩 7.8ms |
| 渲染线程 | 整个窗口所有元素（工具栏、文字、视频） | 卡 1.25ms 就是全窗口掉帧 |

所以等待放在"闲的那条线程"上。等它返回时，显存里的 NV12 已经是 100% 解码完毕的成品。

> [待补充深挖细节]：`AVD3D12VAFrame` 的数组层数是怎么定的（DPB 大小）、
> `fence_value` 的语义（是"本帧写完"还是"下一个可用的值"）、软解分支的 `WrapFrame` 对比。

## 三、第 2 & 3 步：无队列交接与双线程同步

### 3.1 `DirectConnection`：绕开 GUI 事件循环

```cpp
// device_controller.cpp:602
QObject::connect(reader_, &VideoReader::SigFrameReady, video_->Surface(), &VideoItem::Present,
                 Qt::DirectConnection);
```

`Present()` **直接在视频线程里执行**。哪怕用户正疯狂拖窗口把 GUI 线程卡住，视频帧也不会在事件队列里排队
—— 排队意味着延迟无上限地涨。

### 3.2 单槽覆盖：永远只画最新一帧

```cpp
{
    QMutexLocker locker(&mutex_);
    replaced = has_pending_;      // 上一帧还没被取走 → 记一次丢帧
    pending_ = frame;             // 直接覆盖
    has_pending_ = true;
}
```

不排队、不做环形缓冲。旧帧被覆盖时它的 `owner` 析构，显存切片立刻还给 FFmpeg ——
**画面永远是最新的，延迟不会累积**。

最后向 GUI 线程投递一次 `update()` 标脏。配合 `QT_QPA_UPDATE_IDLE_TIME=0`（见 `main.cpp`），
唤醒延迟从默认 5ms 降到 0.1ms。

### 3.3 `synchronize()`：只搬结构体，不搬像素

Qt 在开画前会短暂拦住渲染线程，在 GUI 线程上下文里调 `synchronize()`：

```
GUI 线程                               渲染线程
synchronize(item)
  ├─ TakePendingFrame()  取走 pending_
  ├─ HasNative() ?
  │    ├─ 是 → SyncNativeFrame()     只搬 texture/层号/fence/宽高（约 40 字节）
  │    └─ 否 → SyncSoftwareFrame()   逐行去掉 stride 填充，拷 Y/UV 两平面
  ├─ EndSyncPass()       记到达时刻与耗时
  └─ trace_ = trace      留给 render 用
        │
        ▼ 框架保证顺序（synchronize 一定先于 render）
     render() 开始画
```

实测 `synchronize` 耗时 **0.002ms** —— 因为零拷贝路径上一个像素都没搬。

`synchronize` 一结束，GUI 线程和渲染线程就分道扬镳，后面画图不需要任何互斥锁。

> [待补充深挖细节]：Qt 内部 `synchronize` 与 `render` 的调用时序（`QQuickRhiItem` 的 sync 阶段）、
> 为什么 `synchronize` 里不能碰 GPU 上下文、多窗口时每窗口一套 renderer 的隔离边界。

## 四、第 4 步：GPU 色域转换与 RHI 合成（`QSGRenderThread`）

手机发来的是 **NV12**：一张全分辨率亮度图 Y + 一张半宽半高、U/V 交错排列的色度图 UV。
Qt 的画布要 **RGBA8**。中间这一步在 GPU 里做，叫"两棒接力"。

### 4.1 第一棒：`D3d12Nv12Convert::Convert`

```
① 描述符视图（EnsureViews，同一个资源、不同 PlaneSlice）
   PlaneSlice = 0 → DXGI_FORMAT_R8_UNORM    → 当 Y 采样
   PlaneSlice = 1 → DXGI_FORMAT_R8G8_UNORM  → 当 UV 采样

② ResourceBarrier（成对，见下）
   nv12: COMMON                  → PIXEL_SHADER_RESOURCE
   rgba: PIXEL_SHADER_RESOURCE   → RENDER_TARGET

③ DrawInstanced(3, 1, 0, 0)  全屏三角形，SV_VertexID 算顶点，不用顶点缓冲
   PSMain: y = (Y - 16/255) * 1.164;  rgb = f(y, U, V)   ← BT.601 有限范围

④ ResourceBarrier（切回去）
   nv12: PIXEL_SHADER_RESOURCE   → COMMON
   rgba: RENDER_TARGET           → PIXEL_SHADER_RESOURCE

⑤ ExecuteCommandLists → Signal(自己的围栏) → CPU WaitFence（约 0.17ms）
```

### 4.2 为什么状态必须成对切回来

D3D12 的资源状态是**显式声明**的，不是驱动兜底：

| 资源 | 切进去 | 切回来 | 漏了会怎样 |
|---|---|---|---|
| 解码 NV12 纹理 | `COMMON → PIXEL_SHADER_RESOURCE` | `PIXEL_SHADER_RESOURCE → COMMON` | 下一帧解码器要用它当写入目标，状态不对会报错或画错 |
| 转色目标 RGBA | `PIXEL_SHADER_RESOURCE → RENDER_TARGET` | `RENDER_TARGET → PIXEL_SHADER_RESOURCE` | 紧接着 Qt RHI 要采样它，状态还是 RENDER_TARGET 直接报 state mismatch |

对比 D3D11：那边状态是"当前设置"，谁都能改、没人还；D3D12 把这件事变成每份资源自己的账，
**漏一次就立刻报错**，这也是它比 D3D11 零拷贝靠谱的根本原因。

（BT.601 的 `1.164` 这个系数漏了画面会发灰 —— 0.4.0 就是这么翻车的，0.4.1 修回来。）

### 4.3 归还 DPB 切片的时机

```cpp
has_frame_ = false;
native_frame_ = qsc::stream::Frame();   // owner 析构 → av_frame_free → 切片还给解码池
item_->RecordConvert(...);              // 转色耗时交给探针
```

转色一完成就松手，**不能等这一帧画完再放**：DPB 池子层数有限，早还一层，解码器就能早一步解下一帧。

### 4.4 第二棒：Qt RHI 合成上屏

```cpp
cb->beginPass(renderTarget(), 黑底, {1.0f, 0});
cb->setGraphicsPipeline(rgba_pipeline_);   // 采样 rgba_texture_ 的着色器
cb->setViewport(...); cb->setShaderResources(rgba_bindings_);
cb->draw(4);
cb->endPass();                             // 交给 Qt 场景图叠工具栏、标题栏，再推显示器
```

注意：`QQuickRhiItem` **不会替你开 render pass**，不调 `beginPass` 就是纯黑。

> [待补充深挖细节]：根签名的两个参数（根常量 vs 描述符表）、描述符堆的 CPU/GPU 句柄区别、
> 静态采样器为什么省一个堆、`rgba_texture_` 尺寸与画布尺寸不一致时的缩放行为。

## 五、四代技术演进因果

| 代 | 做法 | 一帧搬多少 | 上屏帧率 | 死在哪 |
|---|---|---|---|---|
| 一 | CPU 软解 | 0（全在内存算） | 60 | CPU 算不动；4 线程并行换 3 帧延迟 |
| 二 | D3D11 硬解 + 读回 | 21.1 MB（10.5 MB 走 PCIe） | 100~120 | 读回 3.3ms，其中 2.1ms 在等 Map |
| 三 | D3D11 同设备零拷贝 | 0（理论） | 崩溃 | 共用 ImmediateContext，状态互相踩 → TDR |
| 四 | D3D12 + Qt6 RHI 零拷贝 | 9.8 MB，全在显存 | 110~120 | 只剩 Qt 提交节拍 |

一句话：**先让 GPU 解码（一二之间），再想办法别搬回内存（二三之间），
最后发现搬不搬不是问题、谁改状态才是（三四之间）。**

> [待补充深挖细节]：三代崩溃时的具体栈（`nvwgf2umx.dll`）、Gen1/Gen4 的 PCIe 带宽实测、
> Qt `QQuickFramebufferObject` 与 `QQuickRhiItem` 的插口差异。

## 六、怎么验证这条链路

```powershell
$env:QS_LOG_LEVEL='info'
$env:QS_TRACE_FRAMES='1'      # 逐帧：到达间隔 / synchronize / 绘制 / 提交
$env:QS_TRACE_DECODE='1'      # 解码侧：拼包 / 送包 / 收帧 / 合计 / 最慢
$env:QS_GRAB='D:\grab.png'    # 6 秒后抓一张投屏窗截图
```

健康的样子（本机 1264×2780）：

```
[decode] 硬解 d3d12va
[decode] 秒结 121 帧（硬解）：拼包 5us / 送包 225us / 收帧 238us / 合计 468us / 最慢 492us
[rhi] 零拷贝管线就绪 QSize(412, 908)
[convert] 164 帧：录命令+提交 92 us，等围栏 202 us
帧 1112 上屏：synchronize 0.003 ms，绘制 0.419 ms，提交 0.68 ms
[perf] 1s 到达 120 帧，上屏 120 帧，丢 0（0%）
[perf] 上屏延迟 1.5~2.5ms 94%，最大 2.3 ms
```
