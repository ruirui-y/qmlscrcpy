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

    // 临时探针（已关）：重连后从零开始算，别把上一条连接的数字混进来
    stat_frames_ = 0;
    stat_pack_ns_ = 0;
    stat_send_ns_ = 0;
    stat_drain_ns_ = 0;
    stat_download_ns_ = 0;
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
                      << "us / 收帧 " << stat_drain_ns_ / n / 1000 << "us（其中下载 "
                      << stat_download_ns_ / n / 1000 << "us）/ 合计 " << total_avg_ns / 1000
                      << "us / 最慢 " << stat_max_total_ns_ / 1000 << "us";

    stat_frames_ = 0;
    stat_pack_ns_ = 0;
    stat_send_ns_ = 0;
    stat_drain_ns_ = 0;
    stat_download_ns_ = 0;
    stat_max_total_ns_ = 0;
    window_timer_.restart();
}

// 把解码器里已经解好的帧全取出来；硬解出来的纹理当场下载成 NV12
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

        // 硬解给的是 D3D11 纹理，留到下一帧就可能被解码器池复用，必须当场下载
        if (av_frame->format == AV_PIX_FMT_D3D11)
        {
            AVFrame *software = av_frame_alloc();
            if (software == nullptr)
            {
                av_frame_free(&av_frame);
                *error = QStringLiteral("分配下载帧失败");
                return false;
            }

            const qint64 transfer_start_ns = trace_enabled_ ? timer_.nsecsElapsed() : 0;
            const int transfer_ret = av_hwframe_transfer_data(software, av_frame, 0);
            if (trace_enabled_)
            {
                // 下载是硬解特有的开销：GPU 纹理拷回系统内存，零拷贝阶段就是要去掉它
                drain_download_ns_ += timer_.nsecsElapsed() - transfer_start_ns;
            }
            av_frame_free(&av_frame);                                                           // 纹理交还给解码器池
            if (transfer_ret < 0)
            {
                av_frame_free(&software);
                *error = QStringLiteral("下载硬解帧失败：%1").arg(AvErrorText(transfer_ret));
                return false;
            }

            // D3D11 下载下来应该是 NV12；别的格式说明驱动给了非常规排布，早点报出来
            if (software->format != AV_PIX_FMT_NV12)
            {
                const int format = software->format;
                av_frame_free(&software);
                *error = QStringLiteral("硬解帧不是 NV12，格式 = %1").arg(format);
                return false;
            }

            frames.append(WrapFrame(software));
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
