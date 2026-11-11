#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QThread>
#include <atomic>

#include "stream/frame.h"
#include "stream/yuv_convert.h"

class QTcpSocket;

namespace qsc::stream {

class H264Decoder;

// 视频线程：从 socket 阻塞读「12 字节头 + 负载」，解码、转成 RGB32，
// 把图直接交给 VideoItem（调用方用直连，不经过 GUI 线程的事件循环）。
//
// 转换放在这个线程：720p 一帧 3~4 ms，放 GUI 线程会拖累拖窗口这类操作，
// 而且拖窗口时 GUI 线程会被系统模态循环卡住，帧在队列里越堆越多。
class VideoReader : public QThread
{
    Q_OBJECT

public:
    VideoReader(QTcpSocket *socket, H264Decoder *decoder, QObject *parent = nullptr);

    void Stop();                                                                                // 置停止标志，最多等 1 秒

signals:
    void ImageReady(QImage image);                                                              // 每解出一帧发一次；接收方用直连，跑在本线程
    void Failed(QString reason);                                                                // 读或解码出错，连接实质上不可用了
    void Ended();                                                                               // 线程正常退出（收到 Stop 或者对端断开）

protected:
    void run() override;

private:
    // 把 buffer 补到 want 字节；返回 false 表示已停止或连接断开，reason 里有原因
    bool FillBuffer(QByteArray &buffer, int want, QString *reason);

    QTcpSocket *socket_;                                                                        // 已建好连接的视频 socket，本对象只读
    H264Decoder *decoder_;                                                                      // 解码器，和本线程一一对应，别跨线程共用
    YuvConvert convert_;                                                                        // YUV 转 RGB，只在本线程用
    QElapsedTimer report_timer_;                                                                // 每秒结算一次耗时统计
    std::atomic<bool> stop_{false};
};

} // namespace qsc::stream
