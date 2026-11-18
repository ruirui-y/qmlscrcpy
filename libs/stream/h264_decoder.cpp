#include "stream/h264_decoder.h"

#include <QDebug>

#include <cstring>

// D3D11 设备要自己建：这台机器上装了两个虚拟显示适配器（IddSampleDriver / OrayIddDriver），
// FFmpeg 默认挑第一个适配器，正好挑到虚拟的，建设备直接报 Cannot allocate memory
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}

using Microsoft::WRL::ComPtr;

namespace qsc::stream
{

// 自己建的 D3D11 设备 + 一张复用的暂存纹理。
// 复用是关键：FFmpeg 的 av_hwframe_transfer_data 每帧都新建一张暂存纹理，
// 分配 + 映射 + 拷贝 + 释放一趟下来 5 ms 就没了。
struct D3d11State
{
    ComPtr<ID3D11Device> device;                                                                // 和 FFmpeg 共用同一个设备
    ComPtr<ID3D11DeviceContext> context;                                                        // 立即上下文，拷贝和映射都靠它
    ComPtr<ID3D11Texture2D> staging;                                                            // STAGING + CPU 可读，尺寸变了才重建
    UINT width = 0;                                                                             // 暂存纹理当前宽
    UINT height = 0;                                                                            // 暂存纹理当前高

    // 零拷贝转色：VideoProcessor 在 GPU 内把 NV12 变成 BGRA，结果写进共享纹理给 OpenGL 用
    ComPtr<ID3D11VideoDevice> video_device;                                                     // 视频处理器入口
    ComPtr<ID3D11VideoContext> video_context;                                                   // 提交转换命令
    ComPtr<ID3D11VideoProcessorEnumerator> processor_enum;                                      // 支持的输入/输出格式表
    ComPtr<ID3D11VideoProcessor> processor;                                                     // 真正干活的处理器
    UINT processor_width = 0;                                                                   // 处理器当前的尺寸，变了要重建
    UINT processor_height = 0;
    ComPtr<ID3D11Texture2D> bgra[2];                                                            // 双缓冲：解码线程写一张，渲染线程读另一张
    ComPtr<ID3D11VideoProcessorOutputView> bgra_view[2];                                        // 两张 BGRA 的输出视图
    HANDLE bgra_share[2] = {nullptr, nullptr};                                                  // 给 GL interop 用的共享句柄
    int write_index = 0;                                                                        // 下一帧往哪张写
    bool ready = false;                                                                         // 处理器和纹理都建好了
};

namespace
{

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

// 选像素格式：优先 D3D11（硬解纹理），没有就交给 FFmpeg 自己挑（会退回软解）
enum AVPixelFormat PickPixelFormat(AVCodecContext *, const enum AVPixelFormat *formats)
{
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

H264Decoder::H264Decoder() = default;

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
    trace_enabled_ = qEnvironmentVariableIntValue("QS_TRACE_DECODE") == 1;
    QString hardware_error;
    if (OpenHardware(&hardware_error))
    {
        hardware_ = true;
        qInfo() << "[decode] 使用 D3D11VA 硬件解码";
    }
    else
    {
        qInfo() << "[decode] 硬件解码不可用，改用软解：" << hardware_error;
        // 帧级多线程：解一帧能快两倍上下，代价是解码器内部要攒几帧才出第一个结果，
        // 实际端到端会多出约 (thread_count - 1) 帧的延迟，用延迟换吞吐
        context_->thread_count = 4;
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

// 建 D3D11 设备并挂到解码器上下文：建不起来就返回 false，由调用方退回软解
bool H264Decoder::OpenHardware(QString *error)
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

    // 临时探针（已关）：重连后从零开始算，别把上一条连接的数字混进来
    stat_frames_ = 0;
    stat_pack_ns_ = 0;
    stat_send_ns_ = 0;
    stat_drain_ns_ = 0;
    stat_download_ns_ = 0;
    stat_copy_ns_ = 0;
    stat_max_total_ns_ = 0;
    window_timer_ = QElapsedTimer();
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

    // 探针：量「拼包 → 送包 → 收帧（含硬解下载）」三段
    if (trace_enabled_)
    {
        timer_.start();
    }

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

    const qint64 pack_ns = trace_enabled_ ? timer_.nsecsElapsed() : 0;                            // 拼包：一次分配加一次拷贝

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

    const qint64 send_ns = trace_enabled_ ? timer_.nsecsElapsed() - pack_ns : 0;                 // 送包：软解是同步解，硬解只是投递

    drain_download_ns_ = 0;
    drain_copy_ns_ = 0;
    if (!DrainFrames(frames, error))
    {
        return false;
    }

    if (trace_enabled_)
    {
        ReportDecodeTime(pack_ns, send_ns, timer_.nsecsElapsed() - pack_ns - send_ns);
    }
    return true;
}

// 每秒一行解码耗时：拼包 / 送包 / 收帧 / 其中下载 / 合计 / 本秒最慢一帧
void H264Decoder::ReportDecodeTime(qint64 pack_ns, qint64 send_ns, qint64 drain_ns)
{
    ++stat_frames_;
    stat_pack_ns_ += pack_ns;
    stat_send_ns_ += send_ns;
    stat_drain_ns_ += drain_ns;
    stat_download_ns_ += drain_download_ns_;
    stat_copy_ns_ += drain_copy_ns_;
    const qint64 total_ns = pack_ns + send_ns + drain_ns;
    if (total_ns > stat_max_total_ns_)
    {
        stat_max_total_ns_ = total_ns;
    }

    if (!window_timer_.isValid())
    {
        window_timer_.start();
    }
    if (window_timer_.elapsed() < 1000)
    {
        return;
    }

    const int n = stat_frames_;
    const qint64 total_avg_ns = (stat_pack_ns_ + stat_send_ns_ + stat_drain_ns_) / n;
    qInfo().nospace() << "[decode] 秒结 " << n << " 帧" << (hardware_ ? "（硬解）" : "（软解）")
                      << "：拼包 " << stat_pack_ns_ / n / 1000 << "us / 送包 " << stat_send_ns_ / n / 1000
                      << "us / 收帧 " << stat_drain_ns_ / n / 1000 << "us（其中拷贝 "
                      << stat_copy_ns_ / n / 1000 << " + 读回 " << (stat_download_ns_ - stat_copy_ns_) / n / 1000 << "us）/ 合计 " << total_avg_ns / 1000
                      << "us / 最慢 " << stat_max_total_ns_ / 1000 << "us";

    stat_frames_ = 0;
    stat_pack_ns_ = 0;
    stat_send_ns_ = 0;
    stat_drain_ns_ = 0;
    stat_download_ns_ = 0;
    stat_copy_ns_ = 0;
    stat_max_total_ns_ = 0;
    window_timer_.restart();
}

// 把硬解帧拷进自己的暂存纹理再读回内存。
// 和 av_hwframe_transfer_data 的区别只有一个：暂存纹理是复用的，不是每帧新建。
bool H264Decoder::DownloadFrame(AVFrame *hardware_frame, Frame &frame, QString *error)
{
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

    // 尺寸变了（横竖屏切换）才重建暂存纹理，其余时候一直复用
    if (!d3d_->staging || d3d_->width != width || d3d_->height != height)
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

        d3d_->staging.Reset();
        const HRESULT create_ret = d3d_->device->CreateTexture2D(&description, nullptr, d3d_->staging.GetAddressOf());
        if (FAILED(create_ret))
        {
            *error = QStringLiteral("建暂存纹理失败（HRESULT 0x%1）").arg(create_ret, 8, 16, QLatin1Char('0'));
            return false;
        }
        d3d_->width = width;
        d3d_->height = height;
    }

    const qint64 copy_start_ns = trace_enabled_ ? timer_.nsecsElapsed() : 0;
    // NV12 的 Y 和 UV 是两个子资源：下标 = 数组下标 × 2 + 平面号
    const UINT y_subresource = slice * 2;
    const UINT uv_subresource = slice * 2 + 1;
    d3d_->context->CopySubresourceRegion(d3d_->staging.Get(), 0, 0, 0, 0, texture, y_subresource, nullptr);
    d3d_->context->CopySubresourceRegion(d3d_->staging.Get(), 1, 0, 0, 0, texture, uv_subresource, nullptr);

    const int chroma_height = static_cast<int>((height + 1) / 2);
    const int y_size = static_cast<int>(width * height);
    const int uv_size = static_cast<int>(width * chroma_height);
    auto buffer = std::make_shared<QByteArray>(y_size + uv_size, '\0');

    const qint64 map_start_ns = trace_enabled_ ? timer_.nsecsElapsed() : 0;
    if (trace_enabled_)
    {
        drain_copy_ns_ += map_start_ns - copy_start_ns;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT map_ret = d3d_->context->Map(d3d_->staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_ret))
    {
        *error = QStringLiteral("映射暂存纹理失败（HRESULT 0x%1）").arg(map_ret, 8, 16, QLatin1Char('0'));
        return false;
    }
    for (UINT row = 0; row < height; ++row)
    {
        memcpy(buffer->data() + row * width, static_cast<const quint8 *>(mapped.pData) + row * mapped.RowPitch, width);
    }
    d3d_->context->Unmap(d3d_->staging.Get(), 0);

    // UV 平面单独映射一次：驱动不允许一次 Map 拿到两个平面
    D3D11_MAPPED_SUBRESOURCE mapped_uv = {};
    if (SUCCEEDED(d3d_->context->Map(d3d_->staging.Get(), 1, D3D11_MAP_READ, 0, &mapped_uv)))
    {
        for (int row = 0; row < chroma_height; ++row)
        {
            memcpy(buffer->data() + y_size + row * width,
                   static_cast<const quint8 *>(mapped_uv.pData) + row * mapped_uv.RowPitch, width);
        }
        d3d_->context->Unmap(d3d_->staging.Get(), 1);
    }
    else
    {
        // 少数驱动不支持按平面映射：退回"UV 紧跟在 Y 之后"的排布
        for (int row = 0; row < chroma_height; ++row)
        {
            memcpy(buffer->data() + y_size + row * width,
                   static_cast<const quint8 *>(mapped.pData) + (height + row) * mapped.RowPitch, width);
        }
    }

    const quint8 *base = reinterpret_cast<const quint8 *>(buffer->constData());
    frame = Frame(QSize(static_cast<int>(width), static_cast<int>(height)), PixelFormat::Nv12, base,
                  base + y_size, nullptr, static_cast<int>(width), static_cast<int>(width), 0, buffer);
    return true;
}

// 建视频处理器和双缓冲 BGRA 纹理：NV12 → BGRA 这一步在 GPU 内完成
bool H264Decoder::EnsureVideoProcessor(int width, int height, QString *error)
{
    if (!d3d_ || d3d_->device == nullptr)
    {
        *error = QStringLiteral("没有自己的 D3D11 设备");
        return false;
    }

    const UINT w = static_cast<UINT>(width);
    const UINT h = static_cast<UINT>(height);
    if (d3d_->ready && d3d_->processor_width == w && d3d_->processor_height == h)
    {
        return true;
    }

    d3d_->ready = false;
    d3d_->processor.Reset();
    d3d_->processor_enum.Reset();
    d3d_->video_context.Reset();
    d3d_->video_device.Reset();

    if (FAILED(d3d_->device.As(&d3d_->video_device)))
    {
        *error = QStringLiteral("设备不支持 ID3D11VideoDevice");
        return false;
    }
    if (FAILED(d3d_->context.As(&d3d_->video_context)))
    {
        *error = QStringLiteral("上下文不支持 ID3D11VideoContext");
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = w;
    content.InputHeight = h;
    content.OutputWidth = w;
    content.OutputHeight = h;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    const HRESULT enum_ret = d3d_->video_device->CreateVideoProcessorEnumerator(&content, d3d_->processor_enum.GetAddressOf());
    if (FAILED(enum_ret))
    {
        *error = QStringLiteral("建视频处理器枚举失败（HRESULT 0x%1）").arg(enum_ret, 8, 16, QLatin1Char('0'));
        return false;
    }

    // 先问驱动支不支持这两组格式，不支持就别往下走
    UINT in_flags = 0;
    UINT out_flags = 0;
    d3d_->processor_enum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &in_flags);
    d3d_->processor_enum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &out_flags);
    if ((in_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0
        || (out_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)
    {
        *error = QStringLiteral("驱动不支持 NV12 → BGRA 转换（输入 0x%1 / 输出 0x%2）")
                     .arg(in_flags, 0, 16).arg(out_flags, 0, 16);
        return false;
    }

    const HRESULT processor_ret = d3d_->video_device->CreateVideoProcessor(d3d_->processor_enum.Get(), 0, d3d_->processor.GetAddressOf());
    if (FAILED(processor_ret))
    {
        *error = QStringLiteral("建视频处理器失败（HRESULT 0x%1）").arg(processor_ret, 8, 16, QLatin1Char('0'));
        return false;
    }

    // scrcpy 用的是 BT.601 有限范围，和软解那条路径保持同一个色彩解释
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space = {};
    input_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    input_space.YCbCr_Matrix = 0;
    d3d_->video_context->VideoProcessorSetStreamColorSpace(d3d_->processor.Get(), 0, &input_space);

    for (int i = 0; i < 2; ++i)
    {
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = w;
        description.Height = h;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;                                      // GL interop 要求共享

        d3d_->bgra[i].Reset();
        d3d_->bgra_view[i].Reset();
        const HRESULT texture_ret = d3d_->device->CreateTexture2D(&description, nullptr, d3d_->bgra[i].GetAddressOf());
        if (FAILED(texture_ret))
        {
            *error = QStringLiteral("建 BGRA 纹理失败（HRESULT 0x%1）").arg(texture_ret, 8, 16, QLatin1Char('0'));
            return false;
        }

        ComPtr<IDXGIResource> resource;
        if (FAILED(d3d_->bgra[i].As(&resource)) || FAILED(resource->GetSharedHandle(&d3d_->bgra_share[i])))
        {
            *error = QStringLiteral("取共享句柄失败");
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view = {};
        view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;
        const HRESULT view_ret = d3d_->video_device->CreateVideoProcessorOutputView(
            d3d_->bgra[i].Get(), d3d_->processor_enum.Get(), &view, d3d_->bgra_view[i].GetAddressOf());
        if (FAILED(view_ret))
        {
            *error = QStringLiteral("建输出视图失败（HRESULT 0x%1）").arg(view_ret, 8, 16, QLatin1Char('0'));
            return false;
        }
    }

    d3d_->processor_width = w;
    d3d_->processor_height = h;
    d3d_->write_index = 0;
    d3d_->ready = true;
    qInfo() << "[decode] 视频处理器就绪" << width << "x" << height;
    return true;
}

// 把一帧 NV12 在 GPU 内转成 BGRA，写进双缓冲的下一张
bool H264Decoder::ConvertToBgra(AVFrame *hardware_frame, QString *error)
{
    if (!EnsureVideoProcessor(hardware_frame->width, hardware_frame->height, error))
    {
        return false;
    }

    auto *texture = reinterpret_cast<ID3D11Texture2D *>(hardware_frame->data[0]);
    const UINT slice = static_cast<UINT>(reinterpret_cast<quintptr>(hardware_frame->data[1]));

    // 输入视图按帧创建：它绑的是解码器池里的某一张切片，不能复用
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {};
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.ArraySlice = slice;                                                          // 解码器池里的第几张切片

    ComPtr<ID3D11VideoProcessorInputView> input_view;
    const HRESULT view_ret = d3d_->video_device->CreateVideoProcessorInputView(
        texture, d3d_->processor_enum.Get(), &desc, input_view.GetAddressOf());
    if (FAILED(view_ret))
    {
        *error = QStringLiteral("建输入视图失败（HRESULT 0x%1）").arg(view_ret, 8, 16, QLatin1Char('0'));
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.pInputSurface = input_view.Get();

    const int target = d3d_->write_index;
    const HRESULT blt_ret = d3d_->video_context->VideoProcessorBlt(d3d_->processor.Get(), d3d_->bgra_view[target].Get(), 0, 1, &stream);
    if (FAILED(blt_ret))
    {
        *error = QStringLiteral("视频处理器转换失败（HRESULT 0x%1）").arg(blt_ret, 8, 16, QLatin1Char('0'));
        return false;
    }

    d3d_->write_index ^= 1;
    if (!bgra_logged_)
    {
        bgra_logged_ = true;
        qInfo() << "[decode] BGRA 转换成功，共享句柄" << d3d_->bgra_share[target]
                << "/" << d3d_->bgra_share[target ^ 1];
    }
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
            qInfo() << "[decode] 首帧格式" << av_frame->format << "（D3D11 = " << AV_PIX_FMT_D3D11 << "）";
        }

        // 硬解给的是 D3D11 纹理，留到下一帧就可能被解码器池复用，必须当场拷走
        if (av_frame->format == AV_PIX_FMT_D3D11)
        {
            // 阶段 A1：先验证 GPU 内 NV12 → BGRA 能跑通，失败只记一次日志，不挡现有链路
            if (d3d_ && d3d_->device)
            {
                QString bgra_error;
                if (!ConvertToBgra(av_frame, &bgra_error) && !bgra_logged_)
                {
                    bgra_logged_ = true;
                    qWarning() << "[decode] BGRA 转换不可用：" << bgra_error;
                }
            }

            const qint64 download_start_ns = trace_enabled_ ? timer_.nsecsElapsed() : 0;
            Frame frame;
            bool downloaded = false;
            QString download_error;

            if (d3d_ && d3d_->device)
            {
                // 快路径：复用暂存纹理，省掉每帧新建纹理的分配开销
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

            if (trace_enabled_)
            {
                drain_download_ns_ += timer_.nsecsElapsed() - download_start_ns;
            }
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
