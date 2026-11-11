#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace qsc::protocol {

// 视频流里每个包前面的 12 字节头：8 字节标志位+时间戳，4 字节负载长度，全大端。
// 配置包不是视频帧、不能送解码器，但必须单独留下来，否则后面所有帧都解不出来。
struct VideoPacketHeader
{
    static constexpr int kSize = 12;                                                            // 头长度，不含后面的负载
    static constexpr quint64 kConfigFlag = 1ULL << 63;                                          // 最高位：配置包
    static constexpr quint64 kKeyFrameFlag = 1ULL << 62;                                        // 次高位：关键帧
    static constexpr quint64 kPtsMask = kKeyFrameFlag - 1;                                      // 低 62 位：时间戳

    // 线上布局（大端）：
    //
    //   字节 0                              7 8                 11
    //   ┌────────────────────────────────────┬───────────────────┐
    //   │ 标志位 + 时间戳（64 位）             │ 负载长度（32 位）   │
    //   └────────────────────────────────────┴───────────────────┘
    //     bit63 = 配置包（SPS/PPS），bit62 = 关键帧，bit0..61 = 展示时间戳（微秒）
    quint64 pts_flags_ = 0;                                                                     // 原始 8 字节，标志位和时间戳混在一起
    quint32 size_ = 0;                                                                          // 紧跟其后的 H.264 数据字节数

    bool IsConfig() const { return (pts_flags_ & kConfigFlag) != 0; }
    bool IsKeyFrame() const { return (pts_flags_ & kKeyFrameFlag) != 0; }
    qint64 PtsUs() const { return static_cast<qint64>(pts_flags_ & kPtsMask); }

    // 从缓冲区头部解一个头；不足 12 字节返回 false（数据还没收全）
    static bool Parse(const QByteArray &buffer, VideoPacketHeader &header);

    QByteArray Serialize() const;                                                               // 反向序列化，测试和造假数据时用
};

} // namespace qsc::protocol
