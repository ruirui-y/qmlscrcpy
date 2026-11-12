#include "stream/video_reader.h"

#include <QDebug>
#include <QTcpSocket>

#include "protocol/video_packet.h"
#include "stream/h264_decoder.h"
#include "stream/perf_stats.h"

namespace qsc::stream
{

using protocol::VideoPacketHeader;

VideoReader::VideoReader(QTcpSocket *socket, H264Decoder *decoder, QObject *parent)
    : QThread(parent), socket_(socket), decoder_(decoder)
{
    report_timer_.start();
}

void VideoReader::Stop()
{
    stop_.store(true);
    wait(1000);
}

bool VideoReader::FillBuffer(QByteArray &buffer, int want, QString *reason)
{
    while (buffer.size() < want)
    {
        if (stop_.load())
        {
            *reason = QStringLiteral("已停止");
            return false;
        }

        // 200ms 一跳：既能及时响应 Stop，也不会白占 CPU
        if (!socket_->waitForReadyRead(200))
        {
            if (socket_->state() != QAbstractSocket::ConnectedState)
            {
                *reason = QStringLiteral("视频连接断开：%1").arg(socket_->errorString());
                return false;
            }
            continue;
        }

        // 这里读多了不要紧：多出来的留在 buffer 里给下一轮，不能像 ReadInfo 那样丢掉
        buffer.append(socket_->readAll());
    }
    return true;
}

void VideoReader::run()
{
    QByteArray buffer;
    QVector<Frame> frames;
    QString error;
    PerfStats &perf = PerfStats::Get();
    QElapsedTimer stage_timer;
    QElapsedTimer read_timer;

    for (;;)
    {
        read_timer.start();
        if (!FillBuffer(buffer, VideoPacketHeader::kSize, &error))
        {
            if (!stop_.load())
            {
                emit Failed(error);
            }
            emit Ended();
            return;
        }

        VideoPacketHeader header;
        if (!VideoPacketHeader::Parse(buffer, header))
        {
            emit Failed(QStringLiteral("包头解析失败"));
            emit Ended();
            return;
        }

        const int total = VideoPacketHeader::kSize + static_cast<int>(header.size_);
        if (!FillBuffer(buffer, total, &error))
        {
            if (!stop_.load())
            {
                emit Failed(error);
            }
            emit Ended();
            return;
        }

        const QByteArray payload =
            buffer.mid(VideoPacketHeader::kSize, static_cast<int>(header.size_));
        buffer.remove(0, total);
        perf.read_us.fetchAndAddRelaxed(read_timer.nsecsElapsed() / 1000);

        frames.clear();
        stage_timer.start();
        const bool pushed =
            decoder_->Push(payload, header.IsConfig(), header.PtsUs(), frames, &error);
        perf.decode_us.fetchAndAddRelaxed(stage_timer.nsecsElapsed() / 1000);
        if (!pushed)
        {
            emit Failed(error);
            emit Ended();
            return;
        }

        for (const Frame &frame : frames)
        {
            perf.frames.fetchAndAddRelaxed(1);
            emit FrameReady(frame);
        }

        // 每秒把各段耗时打一行，enabled 为 false 时这个函数立刻返回
        if (report_timer_.elapsed() >= 1000)
        {
            perf.ReportAndReset();
            report_timer_.restart();
        }
    }
}

} // namespace qsc::stream
