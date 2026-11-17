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

// 把 FFmpeg 的负错误码翻成能看懂的文字
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
        // 必须用 FFmpeg 自己的释放函数：内存是它用 /MD 的 CRT 分配的
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

// 打开 H.264 解码器，失败时把 FFmpeg 的原因带出来
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

    // 帧级多线程：解一帧能快两倍上下，代价是解码器内部要攒几帧才出第一个结果，
    // 实际端到端会多出约 (thread_count - 1) 帧的延迟，用延迟换吞吐
    context_->thread_count = 4;

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

    // 临时探针（已关）：重连后从零开始算，别把上一条连接的数字混进来
    // window_timer_ = QElapsedTimer();
    // stat_frames_ = 0;
    // stat_pack_ns_ = 0;
    // stat_send_ns_ = 0;
    // stat_drain_ns_ = 0;
    // stat_max_total_ns_ = 0;
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

    // 临时探针（已关）：量「拼包 → 送包 → 收帧」三段耗时，重新测量时取消下面三行的注释
    // timer_.start();

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

    // const qint64 pack_ns = timer_.nsecsElapsed();                                            // 拼包：两次拷贝加一次分配

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

    // const qint64 send_ns = timer_.nsecsElapsed() - pack_ns;                                  // 送包：单线程时真正解码的地方

    // ReportDecodeTime(pack_ns, send_ns, timer_.nsecsElapsed() - pack_ns - send_ns);
    return DrainFrames(frames, error);
}

/*
// 临时探针（已关）：每秒一行，给三段平均值和本秒最慢一帧
void H264Decoder::ReportDecodeTime(qint64 pack_ns, qint64 send_ns, qint64 drain_ns)
{
    ++stat_frames_;
    stat_pack_ns_ += pack_ns;
    stat_send_ns_ += send_ns;
    stat_drain_ns_ += drain_ns;
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
    qInfo() << "[decode] 秒结" << n << "帧：拼包" << stat_pack_ns_ / n / 1000 << "us / 送包"
            << stat_send_ns_ / n / 1000 << "us / 收帧" << stat_drain_ns_ / n / 1000 << "us / 合计"
            << (stat_pack_ns_ + stat_send_ns_ + stat_drain_ns_) / n / 1000 << "us（最慢"
            << stat_max_total_ns_ / 1000 << "us）";

    stat_frames_ = 0;
    stat_pack_ns_ = 0;
    stat_send_ns_ = 0;
    stat_drain_ns_ = 0;
    stat_max_total_ns_ = 0;
    window_timer_.restart();
}
*/

// 把解码器里已经解好的帧全取出来
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
