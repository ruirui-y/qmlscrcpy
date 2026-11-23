#include "stream/h264_decoder.h"

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>

#include <cstring>

// d3d12va 用画布给的设备（QQuickRhiItem 的 D3D12 后端），不再自己建
#include <windows.h>

#include <d3d12.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixfmt.h>
}


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

// 选像素格式：只认 d3d12va 的 D3D12；给不出就交给 FFmpeg 自己挑（会退回软解）
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

// 打开 H.264 解码器：先试 d3d12va 硬解，失败就退回软解
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
    qInfo() << "[decode] 硬解设备已就绪 d3d12va（画布的 D3D12 设备）";
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

// 硬解只走 d3d12va（跟 Qt 同一个设备），起不来就退软解
bool H264Decoder::OpenHardware(QString *error)
{
    if (external_device_ != nullptr)
    {
        QString d3d12_error;
        if (OpenD3d12(&d3d12_error))
        {
            return true;
        }
        qWarning() << "[decode] d3d12va 起不来，退回软解：" << d3d12_error;
    }
    *error = QStringLiteral("没有可用的 D3D12 设备，退回软解");
    return false;
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



// d3d12va 的读回：让 FFmpeg 把硬解帧转成 NV12 的软件帧（它内部有自己的暂存纹理和围栏），
// 再按行拷进缓冲池。这一步是第 2 步的过渡方案，第 3 步零拷贝做完整个删掉
// d3d12va 的零拷贝：不读回内存，只把纹理和围栏交出去。
// owner 持有 AVFrame 的一份引用，保证渲染期间这张纹理不会被解码器拿去解下一帧
bool H264Decoder::DownloadD3d12(AVFrame *hardware_frame, Frame &frame, QString *error)
{
    // AV_PIX_FMT_D3D12 的约定：data[0] 指向 AVD3D12VAFrame
    auto *native = static_cast<AVD3D12VAFrame *>(reinterpret_cast<void *>(hardware_frame->data[0]));
    if (native == nullptr || native->texture == nullptr)
    {
        *error = QStringLiteral("d3d12va 帧没有纹理");
        return false;
    }

    AVFrame *keep = av_frame_alloc();
    if (keep == nullptr || av_frame_ref(keep, hardware_frame) < 0)
    {
        if (keep != nullptr)
        {
            av_frame_free(&keep);
        }
        *error = QStringLiteral("引用 d3d12va 帧失败");
        return false;
    }
    std::shared_ptr<const void> owner(keep, [](const void *raw)
    {
        AVFrame *frame_to_free = const_cast<AVFrame *>(static_cast<const AVFrame *>(raw));
        av_frame_free(&frame_to_free);
    });

    NativeTexture texture;
    texture.texture = native->texture;                                                          // ID3D12Resource*
    texture.subresource = native->subresource_index;
    texture.fence = native->sync_ctx.fence;                                                     // 解码写完了没，渲染线程拿它等
    texture.fence_value = native->sync_ctx.fence_value;
    texture.width = hardware_frame->width;
    texture.height = hardware_frame->height;

    frame = Frame(QSize(texture.width, texture.height), texture, owner);
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

        // 硬解帧只有 d3d12va 一种，留到下一帧就可能被解码器池复用，必须当场把引用转出去
        // 不读回：只交纹理和解码围栏
        const bool is_hardware_frame =
            av_frame->format == AV_PIX_FMT_D3D11 || av_frame->format == AV_PIX_FMT_D3D12;
        if (is_hardware_frame)
        {
            const qint64 download_start_us = probe_.Now();
            Frame frame;
            QString download_error;
            const bool downloaded = DownloadD3d12(av_frame, frame, &download_error);
            probe_.AddDownload(probe_.Now() - download_start_us);
            av_frame_free(&av_frame);                                                       // 纹理引用已经转给 Frame 的 owner

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
