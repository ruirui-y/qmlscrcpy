#include "stream/video_reader.h"

#include <QDebug>
#include <QTcpSocket>

#include "protocol/video_packet.h"
#include "stream/h264_decoder.h"

namespace qsc::stream {

using protocol::VideoPacketHeader;

VideoReader::VideoReader(QTcpSocket *socket, H264Decoder *decoder, QObject *parent)
    : QThread(parent), socket_(socket), decoder_(decoder)
{
}

void VideoReader::Stop()
{
    stop_.store(true);
    wait(1000);
}

bool VideoReader::FillBuffer(QByteArray &buffer, int want, QString *reason)
{
    while (buffer.size() < want) {
        if (stop_.load()) {
            *reason = QStringLiteral("已停止");
            return false;
        }

        // 200ms 一跳：既能及时响应 Stop，也不会白占 CPU
        if (!socket_->waitForReadyRead(200)) {
            if (socket_->state() != QAbstractSocket::ConnectedState) {
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

    for (;;) {
        if (!FillBuffer(buffer, VideoPacketHeader::kSize, &error)) {
            if (!stop_.load()) {
                emit Failed(error);
            }
            emit Ended();
            return;
        }

        VideoPacketHeader header;
        if (!VideoPacketHeader::Parse(buffer, header)) {
            emit Failed(QStringLiteral("包头解析失败"));
            emit Ended();
            return;
        }

        const int total = VideoPacketHeader::kSize + static_cast<int>(header.size_);
        if (!FillBuffer(buffer, total, &error)) {
            if (!stop_.load()) {
                emit Failed(error);
            }
            emit Ended();
            return;
        }

        const QByteArray payload =
            buffer.mid(VideoPacketHeader::kSize, static_cast<int>(header.size_));
        buffer.remove(0, total);

        frames.clear();
        if (!decoder_->Push(payload, header.IsConfig(), header.PtsUs(), frames, &error)) {
            emit Failed(error);
            emit Ended();
            return;
        }

        for (const Frame &frame : frames) {
            emit FrameReady(frame);
        }
    }
}

} // namespace qsc::stream
