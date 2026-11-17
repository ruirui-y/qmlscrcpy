#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QVector>

#include "stream/frame.h"

struct AVCodecContext;
struct AVPacket;

namespace qsc::stream
{

// H.264 软解。输入是裸 Annex-B 数据：配置包（SPS/PPS）和数据包都往里送，
// FFmpeg 自己按 in-band 参数集解析，不用我们手工拼 extradata。
class H264Decoder
{
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;

    bool Open(QString *error);                                                                  // 找 H.264 解码器并打开上下文
    void Close();                                                                               // 释放上下文，可以重复调

    // 送一个包，解出的帧追加到 frames（通常 0 或 1 个）；失败时 error 有原因
    bool Push(const QByteArray &data, bool is_config, qint64 pts_us, QVector<Frame> &frames,
              QString *error);

private:
    bool DrainFrames(QVector<Frame> &frames, QString *error);                                   // 把解码器里积压的帧全取出来
    // void ReportDecodeTime(qint64 pack_ns, qint64 send_ns, qint64 drain_ns);                  // 临时探针（已关）：每秒打一行解码耗时

    AVCodecContext *context_ = nullptr;                                                         // 解码器上下文，Close 后置空
    AVPacket *packet_ = nullptr;                                                                // 复用一个包，避免每帧都分配
    QByteArray pending_config_;                                                                 // 还没拼出去的那份 SPS/PPS

    // 临时探针（已关）：量单次 Push 三段耗时 + 按秒结算
    // QElapsedTimer timer_;                                                                    // 量单次 Push 里三段各花多久
    // QElapsedTimer window_timer_;                                                             // 满一秒就结算一次
    // int stat_frames_ = 0;                                                                    // 本秒解了多少帧
    // qint64 stat_pack_ns_ = 0;                                                                // 本秒拼包累计
    // qint64 stat_send_ns_ = 0;                                                                // 本秒送包累计
    // qint64 stat_drain_ns_ = 0;                                                               // 本秒收帧累计
    // qint64 stat_max_total_ns_ = 0;                                                           // 本秒单帧最慢
};

} // namespace qsc::stream
