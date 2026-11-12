#pragma once

#include <QByteArray>
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

    // 送一个包，解出来的帧追加到 frames（通常 0 或 1 个）。返回 false 时 error 有原因。
    // 配置包（SPS/PPS）不收帧：先存起来，跟下一个数据包拼在一起送。
    // 单独送参数集时 FFmpeg 会报 "no frame!" 并返回 INVALIDDATA；拼上切片就没这个问题，
    // 设备旋转后换新参数集也走同一条路。
    bool Push(const QByteArray &data, bool is_config, qint64 pts_us, QVector<Frame> &frames,
              QString *error);

private:
    bool DrainFrames(QVector<Frame> &frames, QString *error);                                   // 把解码器里积压的帧全取出来

    AVCodecContext *context_ = nullptr;                                                         // 解码器上下文，Close 后置空
    AVPacket *packet_ = nullptr;                                                                // 复用一个包，避免每帧都分配
    QByteArray pending_config_;                                                                 // 还没拼出去的那份 SPS/PPS
};

} // namespace qsc::stream
