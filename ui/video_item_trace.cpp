// 视频统计与逐帧追踪：从 video_item.cpp 拆出来，避免统计打印逻辑占住画布主文件。

#include "ui/video_item.h"

#include "stream/video_traffic.h"

#include <QDebug>

#include <cstring>


// 一帧的完整事件记录：只在 Debug 编进来，发布版里连这段代码都不存在
void PrintTrace(const FrameTracePtr& trace, bool swapped, qint64 alive_us)
{
    if (!trace)
    {
        return;
    }

    const auto ms = [](qint64 us)
        {
            return us / 1000.0;
        };

    if (!swapped)
    {
        // 还没画就被后面的帧顶掉：只报到达到被覆盖活了多久
        qInfo().nospace() << "  帧 " << trace->id << " 丢掉：到达间隔 " << ms(trace->gap_us) << " ms，存活 "
            << ms(alive_us) << " ms";
        return;
    }


    qInfo().nospace() << "  帧 " << trace->id << " 上屏：到达间隔 " << ms(trace->gap_us) << "，上屏间隔 "
                      << ms(trace->swap_gap_us) << "，GUI 排队 "
        << ms(trace->queue_us.load()) << "，GUI 到渲染线程 " << ms(trace->schedule_us.load())
        << "，唤醒 " << ms(trace->schedule_us.load()) << "，预处理 " << ms(trace->preprocess_us.load()) << "，取帧 " << ms(trace->sync_us.load())
        << "，画一帧 " << ms(trace->draw_us.load()) << "，交换阻塞 " << ms(trace->swap_us.load())
        << "，从到达到上屏 "
        << ms(trace->render_end_us.load() + trace->swap_us.load() - trace->arrive_us) << " ms";
}




// 这一帧真正提交给系统了：先记统计（永远开），开启追踪时再打整条明细
void VideoItem::MarkSwapped()
{
    const qint64 now_us = clock_.nsecsElapsed() / 1000;

    // 先把这一轮的事件记录取出来，统计和逐帧打印都用它
    FrameTracePtr trace;
    {
        QMutexLocker locker(&mutex_);
        trace = swapped_trace_;
        swapped_trace_.reset();
    }

    // 上屏间隔：相邻两次 frameSwapped 的间距，用来判断"没丢帧但看着卡"是不是抖动
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

    // 上屏延迟 = 现在 - 本轮取走那一帧的到达时刻；取完清零。
    // 两条防陈旧：帧不早于本轮 sync（真属于这一轮），且 swap 离 sync 不超过 100ms
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

    const qint64 render_end_us = trace->render_end_us.load();
    if (render_end_us > 0 && now_us > render_end_us)
    {
        trace->swap_us.store(now_us - render_end_us);
    }

    // 同一帧可能因为窗口重绘多跑一轮，只打第一次
    if (!trace->printed.exchange(true))
    {
        PrintTrace(trace, true, 0);
    }
}

// render() 画完时回报：整个函数跑了多久，记在这一帧自己的记录里
void VideoItem::MarkRenderEnd(qint64 start_us, const FrameTracePtr &trace)
{
    if (!trace)
    {
        return;
    }

    const qint64 end_us = clock_.nsecsElapsed() / 1000;
    trace->draw_us.store(end_us - start_us);
    trace->render_end_us.store(end_us);

    // 交给 GUI 线程的 frameSwapped：那时候才知道交换缓冲阻塞了多久
    QMutexLocker locker(&mutex_);
    swapped_trace_ = trace;
}

// 断开连接时把还没渲染的帧丢掉

// 每秒结算一次：帧数 / 丢帧率一行，上屏延迟与到达间隔各一行
void VideoItem::ReportSecond()
{
    const int arrived = frames_arrived_.loadAcquire();
    const int frames = arrived - fps_last_arrived_;
    fps_last_arrived_ = arrived;

    const int swapped = frames_swapped_.fetchAndStoreOrdered(0);
    const int dropped = frames_dropped_.fetchAndStoreOrdered(0);
    const int l_lt1 = lat_lt1_.fetchAndStoreOrdered(0);
    const int l_12 = lat_1_2_.fetchAndStoreOrdered(0);
    const int l_23 = lat_2_3_.fetchAndStoreOrdered(0);
    const int l_gt3 = lat_gt3_.fetchAndStoreOrdered(0);
    const qint64 max_us = lat_max_us_.fetchAndStoreOrdered(0);
    const int g_lt85 = gap_lt85_.fetchAndStoreOrdered(0);
    const int g_8517 = gap_85_17_.fetchAndStoreOrdered(0);
    const int g_1734 = gap_17_34_.fetchAndStoreOrdered(0);
    const int g_gt34 = gap_gt34_.fetchAndStoreOrdered(0);
    const int w_lt85 = swap_lt85_.fetchAndStoreOrdered(0);
    const int w_8517 = swap_85_17_.fetchAndStoreOrdered(0);
    const int w_1734 = swap_17_34_.fetchAndStoreOrdered(0);
    const int w_gt34 = swap_gt34_.fetchAndStoreOrdered(0);

    if (frames != fps_)
    {
        fps_ = frames;
        emit SigFpsChanged();
    }

    // 窗口还没显示时全是"丢"，这种秒不报，避免启动噪音
    if (swapped <= 0)
    {
        return;
    }

    const double drop_rate = frames > 0 ? (100.0 * dropped / frames) : 0.0;
    const int lat_total = l_lt1 + l_12 + l_23 + l_gt3;
    const int gap_total = g_lt85 + g_8517 + g_1734 + g_gt34;

    const auto percent = [](int count, int total)
    {
        return total > 0 ? (100.0 * count / total) : 0.0;
    };
    const auto pcts = [&percent](int count, int total)
    {
        return QString::number(percent(count, total), 'f', 0);
    };

    // 实际链路流量：字节数 × 8 / 1000000 就是这一秒跑出来的 Mbps
    const qint64 bytes = qsc::stream::VideoTraffic::Instance().TakeBytes();
    const double actual_mbps = bytes * 8.0 / 1000000.0;

    qInfo().noquote() << QStringLiteral("[perf] 1s 到达 %1 帧，上屏 %2 帧，丢 %3（%4%），实际 %5 KB/s（%6 Mbps）")
                             .arg(frames)
                             .arg(swapped)
                             .arg(dropped)
                             .arg(QString::number(drop_rate, 'f', 1))
                             .arg(bytes / 1024)
                             .arg(QString::number(actual_mbps, 'f', 1));

    qInfo().noquote() << QStringLiteral("[perf]     上屏延迟 <1.5ms %1% / 1.5~2.5ms %2% / 2.5~3.5ms %3% / >3.5ms %4%，最大 %5 ms")
                             .arg(pcts(l_lt1, lat_total))
                             .arg(pcts(l_12, lat_total))
                             .arg(pcts(l_23, lat_total))
                             .arg(pcts(l_gt3, lat_total))
                             .arg(QString::number(max_us / 1000.0, 'f', 1));

    qInfo().noquote() << QStringLiteral("[perf]     到达间隔 <8.5ms %1% / 8.5~17ms %2% / 17~34ms %3% / >34ms %4%")
                             .arg(pcts(g_lt85, gap_total))
                             .arg(pcts(g_8517, gap_total))
                             .arg(pcts(g_1734, gap_total))
                             .arg(pcts(g_gt34, gap_total));

    const int swap_total = w_lt85 + w_8517 + w_1734 + w_gt34;
    qInfo().noquote() << QStringLiteral("[perf]     上屏间隔 <8.5ms %1% / 8.5~17ms %2% / 17~34ms %3% / >34ms %4%")
                             .arg(pcts(w_lt85, swap_total))
                             .arg(pcts(w_8517, swap_total))
                             .arg(pcts(w_1734, swap_total))
                             .arg(pcts(w_gt34, swap_total));
}

// 画布尺寸变了，重绘一次让 FBO 跟上（Qt6 起钩子名是 geometryChange）
// 到达间隔分档：只在统计里用，和上屏延迟分开看
void VideoItem::AccountGap(qint64 gap_us)
{
    if (gap_us <= 0 || gap_us >= 1000000)
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

// 被顶掉的帧当场结算：它从到达到被覆盖活了多久
void VideoItem::ReportReplacedFrame(const FrameTracePtr &trace, qint64 alive_us)
{
    PrintTrace(trace, false, alive_us);
}