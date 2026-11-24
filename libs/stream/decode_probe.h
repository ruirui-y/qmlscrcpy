#pragma once

// 解码耗时探针：QS_TRACE_DECODE=1 时按秒统计「拼包 / 送包 / 收帧」。
// 从 H264Decoder 里搬出来，主流程只留 BeginPush / MarkXxx / EndFrame 四行，
// 读解码逻辑时不用在业务代码里翻计时器和累加器。

#include <QElapsedTimer>

namespace qsc::stream
{

class DecodeProbe
{
public:
    DecodeProbe();                                                                              // 读环境变量决定是否启用

    bool Enabled() const { return enabled_; }

    void BeginPush();                                                                           // 一次 Push 开始计时
    void MarkPack();                                                                            // 拼包完成
    void MarkSend();                                                                            // 送包完成
    void EndFrame(bool hardware);                                                               // 收帧完成后结算；满一秒打印

private:
    bool enabled_ = false;                                                                      // 环境变量开关
    QElapsedTimer timer_;                                                                       // 量单次 Push 的三段
    QElapsedTimer window_;                                                                      // 满一秒结算一次

    qint64 pack_ns_ = 0;                                                                        // 本次 Push 各段
    qint64 send_ns_ = 0;
    qint64 mark_us_ = 0;                                                                        // 上一个打点的时刻，算段间差

    int frames_ = 0;                                                                            // 本秒帧数
    qint64 stat_pack_ns_ = 0;                                                                   // 本秒累计
    qint64 stat_send_ns_ = 0;
    qint64 stat_drain_ns_ = 0;
    qint64 stat_max_total_ns_ = 0;                                                              // 本秒单帧最慢

    void Reset();                                                                               // 清零本秒累计
};

} // namespace qsc::stream

