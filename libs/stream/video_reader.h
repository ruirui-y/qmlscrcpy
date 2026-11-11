#pragma once

#include <QByteArray>
#include <QString>
#include <QThread>
#include <atomic>

#include "stream/frame.h"

class QTcpSocket;

namespace qsc::stream {

class H264Decoder;

// 视频线程：从 socket 阻塞读「12 字节头 + 负载」，边读边解码，把帧发给主线程。
//
// 读和解码放同一个线程里做——中间不再过一层队列，延迟更低。边界还是那条：
// 这个线程只产 YUV 帧，主线程只负责上屏。
class VideoReader : public QThread
{
    Q_OBJECT

public:
    VideoReader(QTcpSocket *socket, H264Decoder *decoder, QObject *parent = nullptr);

    void Stop();                                                                                // 置停止标志，最多等 1 秒

signals:
    void FrameReady(qsc::stream::Frame frame);                                                  // 每解出一帧发一次，跨线程自动排队
    void Failed(QString reason);                                                                // 读或解码出错，连接实质上不可用了
    void Ended();                                                                               // 线程正常退出（收到 Stop 或者对端断开）

protected:
    void run() override;

private:
    // 把 buffer 补到 want 字节；返回 false 表示已停止或连接断开，reason 里有原因
    bool FillBuffer(QByteArray &buffer, int want, QString *reason);

    QTcpSocket *socket_;                                                                        // 已建好连接的视频 socket，本对象只读
    H264Decoder *decoder_;                                                                      // 解码器，和本线程一一对应，别跨线程共用
    std::atomic<bool> stop_{false};
};

} // namespace qsc::stream
