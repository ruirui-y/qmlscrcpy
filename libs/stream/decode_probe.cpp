#include "decode_probe.h"

#include <QDebug>
#include <QtGlobal>

namespace qsc::stream
{

DecodeProbe::DecodeProbe()
{
    enabled_ = qEnvironmentVariableIntValue("QS_TRACE_DECODE") == 1;                            // 默认关闭，探针代码不进热路径
}

void DecodeProbe::BeginPush()
{
    if (!enabled_)
    {
        return;
    }
    timer_.start();
    pack_ns_ = 0;
    send_ns_ = 0;
    mark_us_ = 0;
}

void DecodeProbe::MarkPack()
{
    if (!enabled_)
    {
        return;
    }
    mark_us_ = timer_.nsecsElapsed();
    pack_ns_ = mark_us_;                                                                        // 从 BeginPush 到这里的差就是拼包
}

void DecodeProbe::MarkSend()
{
    if (!enabled_)
    {
        return;
    }
    const qint64 now_us = timer_.nsecsElapsed();
    send_ns_ = now_us - mark_us_;                                                               // 上一个打点到这儿的差
    mark_us_ = now_us;
}

void DecodeProbe::EndFrame(bool hardware)
{
    if (!enabled_)
    {
        return;
    }

    const qint64 drain_ns = timer_.nsecsElapsed() - mark_us_;                                   // 收帧：送包之后到这里的差

    ++frames_;
    stat_pack_ns_ += pack_ns_;
    stat_send_ns_ += send_ns_;
    stat_drain_ns_ += drain_ns;

    const qint64 total_ns = pack_ns_ + send_ns_ + drain_ns;
    if (total_ns > stat_max_total_ns_)
    {
        stat_max_total_ns_ = total_ns;
    }

    if (!window_.isValid())
    {
        window_.start();
    }
    if (window_.elapsed() < 1000)
    {
        return;
    }

    const int n = frames_;
    const qint64 total_avg_ns = (stat_pack_ns_ + stat_send_ns_ + stat_drain_ns_) / n;
    qInfo().nospace() << "[decode] 秒结 " << n << " 帧" << (hardware ? "（硬解）" : "（软解）")
                      << "：拼包 " << stat_pack_ns_ / n / 1000 << "us / 送包 " << stat_send_ns_ / n / 1000
                      << "us / 收帧 " << stat_drain_ns_ / n / 1000 << "us / 合计 "
                      << total_avg_ns / 1000 << "us / 最慢 " << stat_max_total_ns_ / 1000 << "us";

    Reset();
    window_.restart();
}

void DecodeProbe::Reset()
{
    frames_ = 0;
    stat_pack_ns_ = 0;
    stat_send_ns_ = 0;
    stat_drain_ns_ = 0;
    stat_max_total_ns_ = 0;
}

} // namespace qsc::stream

