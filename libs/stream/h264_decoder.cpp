#include "stream/h264_decoder.h"

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>

#include <cstring>

// D3D11 设备要自己建：这台机器上装了两个虚拟显示适配器（IddSampleDriver / OrayIddDriver），
// FFmpeg 默认挑第一个适配器，正好挑到虚拟的，建设备直接报 Cannot allocate memory
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixfmt.h>
}

using Microsoft::WRL::ComPtr;

namespace qsc::stream
{

// 帧缓冲池：复用渲染器画完的 QByteArray，避免每帧分配 5MB（缺页是主要成本）
struct FrameBufferPool
{
    QByteArray *Take(int size);
    void Give(QByteArray *buffer);

    QMutex mutex;                                                                               // 解码线程取、渲染线程还，必须加锁
    QVector<QByteArray *> free_list;                                                            // 空闲缓冲，最多留 3 个
};

// 自己建的 D3D11 设备 + 一张复用的暂存纹理。
// 复用是关键：FFmpeg 的 av_hwframe_transfer_data 每帧都新建一张暂存纹理，
// 分配 + 映射 + 拷贝 + 释放一趟下来 5 ms 就没了。
struct D3d11State
{
    ComPtr<ID3D11Device> device;                                                                // 和 FFmpeg 共用同一个设备
    ComPtr<ID3D11DeviceContext> context;                                                        // 立即上下文，拷贝和映射都靠它
    ComPtr<ID3D11Texture2D> staging[2];                                                         // 双缓冲：一帧在拷，另一帧在读
    int write_slot = 0;                                                                         // 下一帧拷进哪个槽
    int pending_slot = -1;                                                                      // 已拷好、等着读的槽
    UINT width = 0;                                                                             // 暂存纹理当前宽
    UINT height = 0;                                                                            // 暂存纹理当前高
};

QByteArray *FrameBufferPool::Take(int size)
{
    QMutexLocker lock(&mutex);
    if (!free_list.isEmpty())
    {
        QByteArray *buffer = free_list.takeLast();
        if (buffer->capacity() >= size)
        {
            return buffer;
        }
        delete buffer;
    }
    return new QByteArray(size, Qt::Uninitialized);
}

// 渲染线程画完那一帧时调用：引用计数归零 → 缓冲回到池子
void FrameBufferPool::Give(QByteArray *buffer)
{
    QMutexLocker lock(&mutex);
    if (free_list.size() < 3)
    {
        free_list.append(buffer);
        return;
    }
    delete buffer;
}

namespace
{

// 单调时钟，微秒：只给临时探针用
qint64 NowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// 把 FFmpeg 的负错误码翻成能看懂的文字
QString AvErrorText(int code)
{
    char buffer[256] = {0};
    av_strerror(code, buffer, sizeof(buffer) - 1);
    return QString::fromUtf8(buffer);
}

// 枚举 DXGI 适配器，跳过纯软件/虚拟显示适配器，挑第一个能建 D3D11 设备的
bool CreateD3D11Device(ComPtr<ID3D11Device> &device, ComPtr<ID3D11DeviceContext> &context,
                       QString *error)
{
    ComPtr<IDXGIFactory1> factory;
    const HRESULT factory_ret = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(factory_ret))
    {
        *error = QStringLiteral("枚举显卡失败（HRESULT 0x%1）").arg(factory_ret, 8, 16, QLatin1Char('0'));
        return false;
    }

    // BGRA 是 FFmpeg 的硬性要求，VIDEO_SUPPORT 才能拿到视频解码接口
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    QString failed_adapters;
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 description = {};
        adapter->GetDesc1(&description);
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        D3D_FEATURE_LEVEL reached = D3D_FEATURE_LEVEL_11_0;
        const HRESULT device_ret = D3D11CreateDevice(
            adapter.Get(),                                                                       // 指定适配器，别让系统自己挑
            D3D_DRIVER_TYPE_UNKNOWN,                                                             // 指定适配器时必须写 UNKNOWN
            nullptr,                                                                             // 不用软件光栅化
            flags,                                                                               // 上面那两个能力位
            levels,                                                                              // 允许的特性等级
            2,                                                                                   // 特性等级个数
            D3D11_SDK_VERSION,                                                                   // SDK 版本
            device.ReleaseAndGetAddressOf(),                                                     // 出：设备
            &reached,                                                                            // 出：实际拿到的特性等级
            context.ReleaseAndGetAddressOf());                                                   // 出：立即上下文

        if (SUCCEEDED(device_ret))
        {
            qInfo() << "[decode] D3D11 适配器" << QString::fromWCharArray(description.Description);
            return true;
        }

        failed_adapters += QStringLiteral("%1(0x%2) ")
                               .arg(QString::fromWCharArray(description.Description))
                               .arg(device_ret, 8, 16, QLatin1Char('0'));
    }

    *error = failed_adapters.isEmpty() ? QStringLiteral("没有硬件显卡适配器")
                                       : QStringLiteral("所有适配器都建不起来：%1").arg(failed_adapters);
    return false;
}

// 选像素格式：d3d12va 和 d3d11va 都能用，但优先 d3d12（跟 Qt 同一个设备），都没有就交给 FFmpeg 自己挑（会退回软解）
enum AVPixelFormat PickPixelFormat(AVCodecContext *, const enum AVPixelFormat *formats)
{
    for (const enum AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format)
    {
        if (*format == AV_PIX_FMT_D3D12)
        {
            return *format;
        }
    }
    for (const enum AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format)
    {
        if (*format == AV_PIX_FMT_D3D11)
        {
            return *format;
        }
    }
    return formats[0];
}

// 把 AVFrame 包成 Frame。平面指针和行跨度直接拷出来，AVFrame 交给 shared_ptr 持有。
Frame WrapFrame(AVFrame *av_frame)
{
    std::shared_ptr<const void> owner(av_frame, [](const void *ptr)
    {
        // 必须用 FFmpeg 自己的释放函数：内存是它用 /MD 的 CRT 分配的
        auto *frame = const_cast<AVFrame *>(static_cast<const AVFrame *>(ptr));
        av_frame_free(&frame);
    });

    const PixelFormat format = av_frame->format == AV_PIX_FMT_NV12 ? PixelFormat::Nv12 : PixelFormat::Yuv420P;
    return Frame(QSize(av_frame->width, av_frame->height), format, av_frame->data[0], av_frame->data[1],
                 av_frame->data[2], av_frame->linesize[0], av_frame->linesize[1], av_frame->linesize[2],
                 std::move(owner));
}

} // namespace

H264Decoder::H264Decoder()
    : pool_(std::make_shared<FrameBufferPool>())
{
}

H264Decoder::~H264Decoder()
{
    Close();
}

// 打开 H.264 解码器：先试 D3D11 硬解，失败就退回软解
bool H264Decoder::Open(QString *error)
{
    if (context_ != nullptr)
    {
        return true;
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr)
    {
        *error = QStringLiteral("找不到 H.264 解码器");
        return false;
    }

    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr)
    {
        *error = QStringLiteral("分配解码器上下文失败");
        return false;
    }

    // 硬件设备建不起来不算失败：软解还能用，只是慢

    QString hardware_error;
    if (!hardware_allowed_)
    {
        hardware_error = QStringLiteral("配置里 [decode] hardware=0");
    }
    if (hardware_allowed_ && OpenHardware(&hardware_error))
    {
        hardware_ = true;
    qInfo() << "[decode] 硬解设备已就绪" << (d3d12_ ? "d3d12va（画布的 D3D12 设备）" : "d3d11va（自建）");
    }
    else
    {
        qInfo() << "[decode] 硬件解码不可用，改用软解：" << hardware_error;
        // 帧级多线程：解一帧能快两倍上下，代价是解码器内部要攒几帧才出第一个结果，
        // 实际端到端会多出约 (thread_count - 1) 帧的延迟，用延迟换吞吐
        context_->thread_count = soft_threads_ > 0 ? soft_threads_ : 4;                             // 软解线程数由配置决定
    }

    const int ret = avcodec_open2(context_, codec, nullptr);
    if (ret < 0)
    {
        *error = QStringLiteral("打开 H.264 解码器失败：%1").arg(AvErrorText(ret));
        Close();
        return false;
    }

    packet_ = av_packet_alloc();
    if (packet_ == nullptr)
    {
        *error = QStringLiteral("分配 AVPacket 失败");
        Close();
        return false;
    }

    return true;
}

// 把画布给的 D3D12 设备挂到上下文上。FFmpeg 销毁硬件上下文时会 Release 这个设备，
// 所以交出去之前要自己 AddRef，否则 Qt 的设备会被提前释放
bool H264Decoder::OpenD3d12(QString *error)
{
    AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
    if (device_ref == nullptr)
    {
        *error = QStringLiteral("分配 d3d12va 设备上下文失败");
        return false;
    }

    auto *hw_context = reinterpret_cast<AVHWDeviceContext *>(device_ref->data);
    auto *d3d12 = static_cast<AVD3D12VADeviceContext *>(hw_context->hwctx);
    d3d12->device = static_cast<ID3D12Device *>(external_device_);
    reinterpret_cast<IUnknown *>(d3d12->device)->AddRef();                                      // 引用交给 FFmpeg 持有

    if (av_hwdevice_ctx_init(device_ref) < 0)
    {
        av_buffer_unref(&device_ref);
        *error = QStringLiteral("初始化 d3d12va 设备上下文失败");
        return false;
    }

    hw_device_ = device_ref;
    context_->hw_device_ctx = av_buffer_ref(hw_device_);
    if (context_->hw_device_ctx == nullptr)
    {
        *error = QStringLiteral("引用 d3d12va 设备失败");
        return false;
    }

    context_->get_format = PickPixelFormat;
    d3d12_ = true;
    return true;
}

// 硬解先试 d3d12va（跟 Qt 同一个设备），起不来再退回自建 D3D11 设备
bool H264Decoder::OpenHardware(QString *error)
{
    if (external_device_ != nullptr)
    {
        QString d3d12_error;
        if (OpenD3d12(&d3d12_error))
        {
            return true;
        }
        qWarning() << "[decode] d3d12va 起不来，退回 d3d11va：" << d3d12_error;
    }
    return OpenHardwareFallback(error);
}

// 建 D3D11 设备并挂到解码器上下文：建不起来就返回 false，由调用方退回软解
bool H264Decoder::OpenHardwareFallback(QString *error)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> device_context;
    QString device_error;

    // 第一优先：自己枚举适配器建设备，避开虚拟显示适配器
    if (CreateD3D11Device(device, device_context, &device_error))
    {
        AVBufferRef *device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (device_ref == nullptr)
        {
            *error = QStringLiteral("分配硬解设备上下文失败");
            return false;
        }

        auto *hw_context = reinterpret_cast<AVHWDeviceContext *>(device_ref->data);
        auto *d3d11 = static_cast<AVD3D11VADeviceContext *>(hw_context->hwctx);
        // 引用交给 FFmpeg 持有，它释放设备上下文时会 Release，这里不能再自己放
        // 先自己留一份引用再 Detach：反过来的话 d3d_->device 拿到的是空指针
        d3d_ = std::make_unique<D3d11State>();
        d3d_->device = device;
        d3d_->context = device_context;

        d3d11->device = device.Detach();
        d3d11->device_context = device_context.Detach();

        if (av_hwdevice_ctx_init(device_ref) < 0)
        {
            av_buffer_unref(&device_ref);
            *error = QStringLiteral("初始化硬解设备上下文失败");
            return false;
        }

        hw_device_ = device_ref;
        context_->hw_device_ctx = av_buffer_ref(hw_device_);
        context_->get_format = PickPixelFormat;
        return context_->hw_device_ctx != nullptr;
    }

    // 第二优先：让 FFmpeg 用默认适配器再试一次（有些机器上只有这条路能成）
    const int device_ret = av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (device_ret < 0)
    {
        *error = QStringLiteral("建 D3D11 设备失败：%1；FFmpeg 默认创建也失败：%2")
                     .arg(device_error, AvErrorText(device_ret));
        return false;
    }

    context_->hw_device_ctx = av_buffer_ref(hw_device_);
    if (context_->hw_device_ctx == nullptr)
    {
        *error = QStringLiteral("引用 D3D11 设备失败");
        return false;
    }

    context_->get_format = PickPixelFormat;
    return true;
}

// 关掉解码器并释放所有缓冲
void H264Decoder::Close()
{
    if (packet_ != nullptr)
    {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
    if (context_ != nullptr)
    {
        avcodec_free_context(&context_);
        context_ = nullptr;
    }
    if (hw_device_ != nullptr)
    {
        av_buffer_unref(&hw_device_);
        hw_device_ = nullptr;
    }
    hardware_ = false;
    d3d_.reset();

    probe_ = DecodeProbe();                                                                     // 重连后从零开始算
}

bool H264Decoder::Push(const QByteArray &data, bool is_config, qint64 pts_us,
                       QVector<Frame> &frames, QString *error)
{
    if (context_ == nullptr || packet_ == nullptr)
    {
        *error = QStringLiteral("解码器还没打开");
        return false;
    }
    if (data.isEmpty())
    {
        return true;
    }

    // 参数集先收着，等下一帧一起送
    if (is_config)
    {
        pending_config_ = data;
        return true;
    }

    probe_.BeginPush();
                                
    QByteArray payload = data;
    if (!pending_config_.isEmpty())
    {
        payload = pending_config_ + data;
        pending_config_.clear();
    }

    av_packet_unref(packet_);
    if (av_new_packet(packet_, static_cast<int>(payload.size())) < 0)
    {
        *error = QStringLiteral("分配 %1 字节的包失败").arg(payload.size());
        return false;
    }
    std::memcpy(packet_->data, payload.constData(), static_cast<std::size_t>(payload.size()));
    packet_->pts = pts_us;
    packet_->dts = pts_us;

    probe_.MarkPack();

    int ret = avcodec_send_packet(context_, packet_);
    if (ret == AVERROR(EAGAIN))
    {
        // 解码器内部队列满了，这个包没被收下：先把已经解好的帧取走腾出位置，再重送一次
        if (!DrainFrames(frames, error))
        {
            return false;
        }
        ret = avcodec_send_packet(context_, packet_);
    }
    if (ret < 0)
    {
        *error = QStringLiteral("送包失败：%1（码 %2，%3 字节）")
                     .arg(AvErrorText(ret))
                     .arg(ret)
                     .arg(payload.size());
        return false;
    }

    probe_.MarkSend();

    if (!DrainFrames(frames, error))
    {
        return false;
    }

    probe_.EndFrame(hardware_);
    return true;
}

// 把硬解帧拷进自己的暂存纹理再读回内存。
// 和 av_hwframe_transfer_data 的区别只有一个：暂存纹理是复用的，不是每帧新建。
bool H264Decoder::DownloadFrame(AVFrame *hardware_frame, Frame &frame, QString *error)
{
    // d3d12va 的帧走另一条读回：FFmpeg 自己有 transfer 路径，省得我们录命令列表
    if (d3d12_)
    {
        return DownloadD3d12(hardware_frame, frame, error);
    }

    if (!d3d_ || d3d_->device == nullptr || d3d_->context == nullptr)
    {
        *error = QStringLiteral("没有自己的 D3D11 设备，无法下载");
        return false;
    }

    // AV_PIX_FMT_D3D11 的约定：data[0] 是 ID3D11Texture2D，data[1] 是数组下标
    auto *texture = reinterpret_cast<ID3D11Texture2D *>(hardware_frame->data[0]);
    const UINT slice = static_cast<UINT>(reinterpret_cast<quintptr>(hardware_frame->data[1]));
    const UINT width = static_cast<UINT>(hardware_frame->width);
    const UINT height = static_cast<UINT>(hardware_frame->height);

    // 尺寸变了（横竖屏切换）：重建两张 staging + 两个查询，丢掉在途帧
    if (!d3d_->staging[0] || d3d_->width != width || d3d_->height != height)
    {
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_NV12;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        for (int i = 0; i < 2; ++i)
        {
            d3d_->staging[i].Reset();
            if (FAILED(d3d_->device->CreateTexture2D(&description, nullptr, d3d_->staging[i].GetAddressOf())))
            {
                *error = QStringLiteral("建暂存纹理失败");
                return false;
            }
        }

        d3d_->write_slot = 0;
        d3d_->pending_slot = -1;
        d3d_->width = width;
        d3d_->height = height;
    }

    const int chroma_height = static_cast<int>((height + 1) / 2);
    const int y_size = static_cast<int>(width * height);
    const int uv_size = static_cast<int>(width * chroma_height);
    const int total_size = y_size + uv_size;

    // 同步模式：拷完立刻读同一张（更跟手，但每次等 GPU 约 2ms）
    if (!async_readback_)
    {
        const qint64 s0 = NowUs();
        d3d_->context->CopySubresourceRegion(d3d_->staging[0].Get(), 0, 0, 0, 0, texture, slice, nullptr);
        probe_.AddPrep((NowUs() - s0) * 1000);
        return ReadStaging(0, width, height, frame, error);
    }

    // ---- 第一步：把这一帧拷进当前槽，只提交、不等待 ----
    const qint64 t0 = NowUs();
    const int slot = d3d_->write_slot;
    d3d_->context->CopySubresourceRegion(d3d_->staging[slot].Get(), 0, 0, 0, 0, texture, slice, nullptr);
    const qint64 t1 = NowUs();
    probe_.AddPrep((t1 - t0) * 1000);

    // ---- 第二步：读上一帧（它早就拷完了，Map 不会等）----
    const int read_slot = d3d_->pending_slot;
    d3d_->pending_slot = slot;
    d3d_->write_slot = 1 - slot;

    if (read_slot < 0)
    {
        // 第一帧：没有上一帧可读，只能同步读当前槽
        return ReadStaging(slot, width, height, frame, error);
    }

    return ReadStaging(read_slot, width, height, frame, error);
}

// 把某个槽里的 staging 读成 Frame：Map → memcpy → Unmap
bool H264Decoder::ReadStaging(int slot, UINT width, UINT height, Frame &frame, QString *error)
{
    const int chroma_height = static_cast<int>((height + 1) / 2);
    const int y_size = static_cast<int>(width * height);
    const int uv_size = static_cast<int>(width * chroma_height);
    const int total_size = y_size + uv_size;

    auto pool = pool_;
    std::shared_ptr<QByteArray> buffer(pool->Take(total_size), [pool](QByteArray *raw)
    {
        pool->Give(raw);
    });

    const qint64 t0 = NowUs();
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT map_ret = d3d_->context->Map(d3d_->staging[slot].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_ret))
    {
        *error = QStringLiteral("映射暂存纹理失败（HRESULT 0x%1）").arg(map_ret, 8, 16, QLatin1Char('0'));
        return false;
    }
    const qint64 t1 = NowUs();
    probe_.AddWait((t1 - t0) * 1000);                                                           // Map 等 GPU 的时间

    for (UINT row = 0; row < height; ++row)
    {
        memcpy(buffer->data() + row * width, static_cast<const quint8 *>(mapped.pData) + row * mapped.RowPitch, width);
    }
    for (int row = 0; row < chroma_height; ++row)
    {
        memcpy(buffer->data() + y_size + row * width,
               static_cast<const quint8 *>(mapped.pData) + (height + row) * mapped.RowPitch, width);
    }
    d3d_->context->Unmap(d3d_->staging[slot].Get(), 0);
    const qint64 t2 = NowUs();
    probe_.AddRead((t2 - t1) * 1000);

    const quint8 *base = reinterpret_cast<const quint8 *>(buffer->constData());
    frame = Frame(QSize(static_cast<int>(width), static_cast<int>(height)), PixelFormat::Nv12, base,
                  base + y_size, nullptr, static_cast<int>(width), static_cast<int>(width), 0, buffer);
    return true;
}

// d3d12va 的读回：让 FFmpeg 把硬解帧转成 NV12 的软件帧（它内部有自己的暂存纹理和围栏），
// 再按行拷进缓冲池。这一步是第 2 步的过渡方案，第 3 步零拷贝做完整个删掉
bool H264Decoder::DownloadD3d12(AVFrame *hardware_frame, Frame &frame, QString *error)
{
    AVFrame *software = av_frame_alloc();
    if (software == nullptr)
    {
        *error = QStringLiteral("分配软件帧失败");
        return false;
    }
    software->format = AV_PIX_FMT_NV12;                                                        // 跟 d3d12va 的原生格式一致，不做额外转换

    const qint64 t0 = NowUs();
    const int transfer_ret = av_hwframe_transfer_data(software, hardware_frame, 0);
    if (transfer_ret < 0)
    {
        av_frame_free(&software);
        *error = QStringLiteral("d3d12va 帧读回失败：%1").arg(AvErrorText(transfer_ret));
        return false;
    }
    probe_.AddWait((NowUs() - t0) * 1000);                                                      // transfer 内部要等 GPU

    const int width = software->width;
    const int height = software->height;
    const int chroma_height = (height + 1) / 2;
    const int y_size = width * height;
    const int uv_size = width * chroma_height;
    const int total_size = y_size + uv_size;

    auto pool = pool_;
    std::shared_ptr<QByteArray> buffer(pool->Take(total_size), [pool](QByteArray *raw)
    {
        pool->Give(raw);
    });

    // NV12 两块：Y 在 data[0]，UV 交错在 data[1]，各自按自己的 linesize 走
    const qint64 t1 = NowUs();
    for (int row = 0; row < height; ++row)
    {
        memcpy(buffer->data() + row * width, software->data[0] + row * software->linesize[0], width);
    }
    for (int row = 0; row < chroma_height; ++row)
    {
        memcpy(buffer->data() + y_size + row * width, software->data[1] + row * software->linesize[1], width);
    }
    probe_.AddRead((NowUs() - t1) * 1000);
    av_frame_free(&software);

    const quint8 *base = reinterpret_cast<const quint8 *>(buffer->constData());
    frame = Frame(QSize(width, height), PixelFormat::Nv12, base, base + y_size, nullptr, width, width, 0, buffer);
    return true;
}

// 把解码器里已经解好的帧全取出来；硬解出来的纹理当场拷进暂存再读回内存
bool H264Decoder::DrainFrames(QVector<Frame> &frames, QString *error)
{
    for (;;)
    {
        // 每帧单独分配 AVFrame：所有权要交给 Frame 带走，不能复用同一个
        AVFrame *av_frame = av_frame_alloc();
        if (av_frame == nullptr)
        {
            *error = QStringLiteral("分配 AVFrame 失败");
            return false;
        }

        const int ret = avcodec_receive_frame(context_, av_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            av_frame_free(&av_frame);
            return true;
        }
        if (ret < 0)
        {
            av_frame_free(&av_frame);
            *error = QStringLiteral("收帧失败：%1").arg(AvErrorText(ret));
            return false;
        }

        static bool format_logged = false;
        if (!format_logged)
        {
            format_logged = true;
            qInfo() << "[decode] 首帧格式" << av_frame->format << "（D3D11 = " << AV_PIX_FMT_D3D11
                      << " D3D12 = " << AV_PIX_FMT_D3D12 << "）";
        }

        // 硬解给的是显存纹理（D3D11 或 D3D12），留到下一帧就可能被解码器池复用，必须当场拷走
        const bool is_hardware_frame =
            av_frame->format == AV_PIX_FMT_D3D11 || av_frame->format == AV_PIX_FMT_D3D12;

        // 硬解：读回内存，组装成 NV12 帧
        if (is_hardware_frame)
        {
            const qint64 download_start_us = probe_.Now();
            Frame frame;
            bool downloaded = false;
            QString download_error;

            if (d3d12_ || (d3d_ && d3d_->device))
            {
                // 快路径：d3d12va 走 FFmpeg 的 transfer，d3d11va 复用我们自己的暂存纹理
                downloaded = DownloadFrame(av_frame, frame, &download_error);
            }
            else
            {
                // 退路：设备不是我们建的（FFmpeg 默认创建），只能用它的通用下载
                AVFrame *software = av_frame_alloc();
                if (software != nullptr)
                {
                    const int transfer_ret = av_hwframe_transfer_data(software, av_frame, 0);
                    if (transfer_ret >= 0 && software->format == AV_PIX_FMT_NV12)
                    {
                        frame = WrapFrame(software);
                        downloaded = true;
                    }
                    else
                    {
                        av_frame_free(&software);
                        download_error = QStringLiteral("通用下载失败：%1").arg(AvErrorText(transfer_ret));
                    }
                }
            }

            probe_.AddDownload(probe_.Now() - download_start_us);
            av_frame_free(&av_frame);                                                           // 纹理交还给解码器池

            if (!downloaded)
            {
                *error = download_error;
                return false;
            }

            frames.append(frame);
            continue;
        }

        // 设备端软解固定吐 YUV420P；万一不是，说明编码参数被改过，早点报出来
        if (av_frame->format != AV_PIX_FMT_YUV420P)
        {
            const int format = av_frame->format;
            av_frame_free(&av_frame);
            *error = QStringLiteral("不是 YUV420P 也不是 NV12，格式 = %1").arg(format);
            return false;
        }

        frames.append(WrapFrame(av_frame));
    }
}

} // namespace qsc::stream
