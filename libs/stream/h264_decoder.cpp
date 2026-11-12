#include "stream/h264_decoder.h"

#include <QDebug>

#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace qsc::stream
{
namespace
{

QString AvErrorText(int code)
{
    char buffer[256] = {0};
    av_strerror(code, buffer, sizeof(buffer) - 1);
    return QString::fromUtf8(buffer);
}

// 把 AVFrame 包成 Frame。平面指针和行跨度直接拷出来，AVFrame 交给 shared_ptr 持有。
Frame WrapFrame(AVFrame *av_frame)
{
    std::shared_ptr<const void> owner(av_frame, [](const void *ptr)
    {
        // 必须走 FFmpeg 自己的释放函数：这块内存是它用 /MD 的 CRT 分配的，
        // 我们 delete 就是在两套堆上来回穿越。
        auto *frame = const_cast<AVFrame *>(static_cast<const AVFrame *>(ptr));
        av_frame_free(&frame);
    });

    return Frame(QSize(av_frame->width, av_frame->height), av_frame->data[0], av_frame->data[1],
                 av_frame->data[2], av_frame->linesize[0], av_frame->linesize[1],
                 av_frame->linesize[2], std::move(owner));
}

} // namespace

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder()
{
    Close();
}

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

    // 先单线程。帧级多线程在"只收到参数集、还没有完整帧"时会让 send_packet 返回
    // INVALIDDATA，这里先把它排除掉，确认后再决定要不要开。
    context_->thread_count = 1;

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

    const int ret = avcodec_send_packet(context_, packet_);
    // EAGAIN 表示解码器内部还堵着，先把帧取干净下次再送，不算错误
    if (ret < 0 && ret != AVERROR(EAGAIN))
    {
        *error = QStringLiteral("送包失败：%1（码 %2，%3 字节）")
                     .arg(AvErrorText(ret))
                     .arg(ret)
                     .arg(payload.size());
        return false;
    }

    return DrainFrames(frames, error);
}

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

        // 设备端固定吐 YUV420P；万一不是，说明编码参数被改过，早点报出来
        if (av_frame->format != AV_PIX_FMT_YUV420P)
        {
            av_frame_free(&av_frame);
            *error = QStringLiteral("不是 YUV420P，格式 = %1").arg(av_frame->format);
            return false;
        }

        frames.append(WrapFrame(av_frame));
    }
}

} // namespace qsc::stream
