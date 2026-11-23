#include "ui/frame_probe.h"

#include "stream/video_traffic.h"

#include <QDebug>
#include <QThread>

namespace
{

constexpr qint64 kSecondUs = 1000000;                                                           // 一秒，单位微秒

double ToMs(qint64 us)
{
    return static_cast<double>(us) / 1000.0;
}

// 打印一帧的追踪行：上屏或被顶掉
void PrintTrace(const FrameTracePtr &trace, bool swapped, qint64 alive_us)
{
    if (!trace)
    {
        return;
    }
    if (!swapped)
    {
        qInfo().nospace() << "  帧 " << trace->id << " 丢掉：到达间隔 " << ToMs(trace->gap_us) << " ms，存活 "
                          << ToMs(alive_us) << " ms";
        return;
    }
    qInfo().nospace() << "  帧 " << trace->id << " 上屏：到达间隔 " << ToMs(trace->gap_us) << " ms，上屏间隔 "
                      << ToMs(trace->swap_gap_us) << " ms，synchronize " << ToMs(trace->sync_us) << " ms，绘制 "
                      << ToMs(trace->draw_us) << " ms，提交 " << ToMs(trace->swap_us) << " ms";
}

} // namespace

FrameProbe::FrameProbe()
{
    clock_.start();
    trace_enabled_ = qEnvironmentVariableIntValue("QS_TRACE_FRAMES") == 1;
    qInfo() << "[perf] 逐帧追踪" << (trace_enabled_ ? "开" : "关");
}

FrameProbe::~FrameProbe() = default;

// 建这一帧的追踪记录：没开逐帧追踪时返回空，调用方不用判断
FrameTracePtr FrameProbe::BeginFrame(qint64 arrive_us, qint64 gap_us)
{
    if (!trace_enabled_)
    {
        return FrameTracePtr();
    }
    auto trace = std::make_shared<FrameTrace>();
    trace->id = ++next_frame_id_;
    trace->arrive_us = arrive_us;
    trace->gap_us = gap_us;
    return trace;
}

// 到达间隔：与上一帧的时间差；第一帧返回 0
qint64 FrameProbe::TakeArrivalGap(qint64 now_us)
{
    const qint64 prev = last_present_us_.fetchAndStoreRelaxed(now_us);
    return prev > 0 ? now_us - prev : 0;
}

// 到达间隔分档：和上屏延迟分开统计
void FrameProbe::AccountGap(qint64 gap_us)
{
    if (gap_us <= 0 || gap_us >= kSecondUs)
    {
        return;
    }
    if (gap_us < 8500)
    {
        gap_lt85_.ref();
    }
    else if (gap_us < 17000)
    {
        gap_85_17_.ref();
    }
    else if (gap_us < 34000)
    {
        gap_17_34_.ref();
    }
    else
    {
        gap_gt34_.ref();
    }
}

void FrameProbe::OnReplaced(const FrameTracePtr &trace, qint64 alive_us)
{
    PrintTrace(trace, false, alive_us);
}

void FrameProbe::MarkArrived()
{
    frames_arrived_.ref();
}

void FrameProbe::MarkDropped()
{
    frames_dropped_.ref();
}

// 一轮 synchronize 开始：记起点，顺便算两轮间隔
void FrameProbe::BeginSyncPass()
{
    const qint64 now_us = NowUs();
    sync_start_us_.storeRelaxed(now_us);
}

// render() 画完：把绘制耗时写进这一帧的记录，等 frameSwapped 再结算
void FrameProbe::OnRenderEnd(qint64 draw_start_us, const FrameTracePtr &trace)
{
    if (!trace)
    {
        return;
    }
    const qint64 now_us = NowUs();
    if (now_us > draw_start_us)
    {
        trace->draw_us = now_us - draw_start_us;
    }
    trace->render_end_us = now_us;
    swapped_trace_ = trace;
}

// frameSwapped：上屏间隔、上屏延迟分档、逐帧打印都在这结算
void FrameProbe::OnSwapped()
{
    const qint64 now_us = NowUs();

    FrameTracePtr trace = swapped_trace_;
    swapped_trace_.reset();

    // 上屏间隔
    if (last_swap_us_ > 0 && now_us > last_swap_us_)
    {
        const qint64 swap_gap = now_us - last_swap_us_;
        if (trace)
        {
            trace->swap_gap_us = swap_gap;
        }
        if (swap_gap < 8500)
        {
            swap_lt85_.ref();
        }
        else if (swap_gap < 17000)
        {
            swap_85_17_.ref();
        }
        else if (swap_gap < 34000)
        {
            swap_17_34_.ref();
        }
        else
        {
            swap_gt34_.ref();
        }
    }
    last_swap_us_ = now_us;

    // 上屏延迟 = 现在 - 本轮取走那帧的到达时刻；两条防陈旧条件
    const qint64 arrive_us = pass_arrive_us_.fetchAndStoreRelaxed(0);
    const qint64 pass_sync_us = pass_sync_us_.loadRelaxed();
    const bool fresh = arrive_us > 0 && pass_sync_us >= arrive_us
                       && (pass_sync_us - arrive_us) < 100000
                       && (now_us - pass_sync_us) < 100000;
    if (fresh && now_us > arrive_us)
    {
        const qint64 latency_us = now_us - arrive_us;
        frames_swapped_.ref();
        if (latency_us < 1000)
        {
            lat_lt1_.ref();
        }
        else if (latency_us < 2000)
        {
            lat_1_2_.ref();
        }
        else if (latency_us < 3000)
        {
            lat_2_3_.ref();
        }
        else
        {
            lat_gt3_.ref();
        }
        if (latency_us > lat_max_us_.loadRelaxed())
        {
            lat_max_us_.storeRelaxed(latency_us);
        }
    }

    if (!trace)
    {
        return;
    }
    if (trace->render_end_us > 0 && now_us > trace->render_end_us)
    {
        trace->swap_us = now_us - trace->render_end_us;
    }
    if (!trace->printed)
    {
        trace->printed = true;
        PrintTrace(trace, true, 0);
    }
}

// 每秒一行：帧率、丢帧、延迟与间隔分档
void FrameProbe::ReportSecond()
{
    const int arrived = frames_arrived_.loadRelaxed();
    const int swapped = frames_swapped_.loadRelaxed();
    const int dropped = frames_dropped_.loadRelaxed();

    const int second_arrived = arrived - fps_last_arrived_;
    const int second_swapped = swapped - fps_last_swapped_;
    fps_last_arrived_ = arrived;
    fps_last_swapped_ = swapped;
    fps_ = second_swapped;

    const int dropped_second = dropped;
    frames_dropped_.storeRelaxed(0);

    int lat_lt1 = lat_lt1_.fetchAndStoreRelaxed(0);
    int lat_1_2 = lat_1_2_.fetchAndStoreRelaxed(0);
    int lat_2_3 = lat_2_3_.fetchAndStoreRelaxed(0);
    int lat_gt3 = lat_gt3_.fetchAndStoreRelaxed(0);
    const int lat_total = lat_lt1 + lat_1_2 + lat_2_3 + lat_gt3;
    const qint64 lat_max = lat_max_us_.fetchAndStoreRelaxed(0);

    int gap_lt85 = gap_lt85_.fetchAndStoreRelaxed(0);
    int gap_85_17 = gap_85_17_.fetchAndStoreRelaxed(0);
    int gap_17_34 = gap_17_34_.fetchAndStoreRelaxed(0);
    int gap_gt34 = gap_gt34_.fetchAndStoreRelaxed(0);
    const int gap_total = gap_lt85 + gap_85_17 + gap_17_34 + gap_gt34;

    int swap_lt85 = swap_lt85_.fetchAndStoreRelaxed(0);
    int swap_85_17 = swap_85_17_.fetchAndStoreRelaxed(0);
    int swap_17_34 = swap_17_34_.fetchAndStoreRelaxed(0);
    int swap_gt34 = swap_gt34_.fetchAndStoreRelaxed(0);
    const int swap_total = swap_lt85 + swap_85_17 + swap_17_34 + swap_gt34;

    const auto percent = [](int part, int total) -> int
    {
        return total > 0 ? part * 100 / total : 0;
    };

    const qint64 bytes = qsc::stream::VideoTraffic::Instance().TakeBytes();
    const double mbps = static_cast<double>(bytes) * 8.0 / 1000000.0;

    qInfo().noquote() << QStringLiteral("[perf] 1s 到达 %1 帧，上屏 %2 帧，丢 %3（%4%），实际 %5 Mbps")
                             .arg(second_arrived)
                             .arg(second_swapped)
                             .arg(dropped_second)
                             .arg(lat_total > 0 ? (dropped_second * 100 / qMax(1, second_arrived)) : 0)
                             .arg(mbps, 0, 'f', 1);

    if (lat_total > 0)
    {
        qInfo().noquote() << QStringLiteral("[perf]     上屏延迟 <1.5ms %1% / 1.5~2.5ms %2% / 2.5~3.5ms %3% / >3.5ms %4%，最大 %5 ms")
                                 .arg(percent(lat_lt1, lat_total))
                                 .arg(percent(lat_1_2, lat_total))
                                 .arg(percent(lat_2_3, lat_total))
                                 .arg(percent(lat_gt3, lat_total))
                                 .arg(ToMs(lat_max), 0, 'f', 1);
    }
    if (gap_total > 0)
    {
        qInfo().noquote() << QStringLiteral("[perf]     到达间隔 <8.5ms %1% / 8.5~17ms %2% / 17~34ms %3% / >34ms %4%")
                                 .arg(percent(gap_lt85, gap_total))
                                 .arg(percent(gap_85_17, gap_total))
                                 .arg(percent(gap_17_34, gap_total))
                                 .arg(percent(gap_gt34, gap_total));
    }
    if (swap_total > 0)
    {
        qInfo().noquote() << QStringLiteral("[perf]     上屏间隔 <8.5ms %1% / 8.5~17ms %2% / 17~34ms %3% / >34ms %4%")
                                 .arg(percent(swap_lt85, swap_total))
                                 .arg(percent(swap_85_17, swap_total))
                                 .arg(percent(swap_17_34, swap_total))
                                 .arg(percent(swap_gt34, swap_total));
    }

    Q_UNUSED(lat_lt1)
    Q_UNUSED(lat_1_2)
    Q_UNUSED(lat_2_3)
    Q_UNUSED(lat_gt3)
    Q_UNUSED(gap_lt85)
    Q_UNUSED(gap_85_17)
    Q_UNUSED(gap_17_34)
    Q_UNUSED(gap_gt34)
    Q_UNUSED(swap_lt85)
    Q_UNUSED(swap_85_17)
    Q_UNUSED(swap_17_34)
    Q_UNUSED(swap_gt34)
}
