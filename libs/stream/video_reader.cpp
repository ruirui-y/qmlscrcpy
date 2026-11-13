#include "stream/video_reader.h"

#include <QDebug>
#include <QTcpSocket>

#include "protocol/video_packet.h"
#include "stream/h264_decoder.h"

namespace qsc::stream
{

using protocol::VideoPacketHeader;

VideoReader::VideoReader(QTcpSocket *socket, H264Decoder *decoder, QObject *parent)
    : QThread(parent), socket_(socket), decoder_(decoder)
{
}

// 置停止标志并等线程退出，最多等 1 秒
void VideoReader::Stop()
{
    stop_.store(true);
    // 线程可能正卡在 socket 上，1 秒是给它醒过来的余量；超时就不等了，免得界面卡死
    wait(1000);
}

// 把缓冲补到 want 字节；停机或断线返回 false 并给出原因
bool VideoReader::FillBuffer(QByteArray &buffer, int want, QString *reason)
{
    // 一直补到够数为止。注意只往尾部追加，已经存在 buffer 里的字节不动
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
            // 超时未必是断线，可能只是设备这会儿没画面
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

// 读包、解码、发帧，循环到停机或断线
void VideoReader::run()
{
    QByteArray buffer;                                                                          // 还没处理完的零碎字节，半帧就存在这儿，跨循环保留
    QVector<Frame> frames;                                                                      // 一次 Push 可能吐出多帧，所以用容器接
    QString error;                                                                              // 出错原因，往上传给界面

    // 死循环，只有出错或停机才靠 return 跳出
    for (;;)
    {

        // TCP 是字节流，帧边界得自己数：先凑够 12 字节包头，才知道这一帧有多长
        if (!FillBuffer(buffer, VideoPacketHeader::kSize, &error))
        {
            // 用户主动断开不算错误，只有真出事才往界面上抛
            if (!stop_.load())
            {
                emit SigFailed(error);
            }
            emit SigEnded();
            return;
        }

        // 解析只读不消费：buffer 里那 12 字节原地不动，等整帧到齐再一起删
        VideoPacketHeader header;
        if (!VideoPacketHeader::Parse(buffer, header))
        {
            emit SigFailed(QStringLiteral("包头解析失败"));
            emit SigEnded();
            return;
        }

        // 第二次补的是整帧的长度：包头已经在 buffer 里，这里只是把负载等齐
        const int total = VideoPacketHeader::kSize + static_cast<int>(header.size_);
        if (!FillBuffer(buffer, total, &error))
        {
            if (!stop_.load())
            {
                emit SigFailed(error);
            }
            emit SigEnded();
            return;
        }

        // 切出负载并把整帧从缓冲区删掉，不删会反复解同一帧
        const QByteArray payload = buffer.mid(VideoPacketHeader::kSize, static_cast<int>(header.size_));
        buffer.remove(0, total);

        // 配置包只存不放画面；容器复用，clear 只清大小不释放容量
        frames.clear();
        const bool pushed = decoder_->Push(payload, header.IsConfig(), header.PtsUs(), frames, &error);
        if (!pushed)
        {
            emit SigFailed(error);
            emit SigEnded();
            return;
        }

        // 直连：这里等于就地调 VideoItem::Present，不进 GUI 线程队列
        for (const Frame &frame : frames)
        {
            emit SigFrameReady(frame);
        }

    }
}

} // namespace qsc::stream
