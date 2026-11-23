#pragma once

// 渲染侧埋点：帧序号、逐帧追踪、延迟/间隔分档、每秒统计。
// 从 VideoItem 剥离出来，画布只负责"交帧"，统计与打印全在这里。
//
// 开关：环境变量 QS_TRACE_FRAMES=1 才逐帧打印。

#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QSize>
#include <QString>

#include <memory>

class QMutex;

// 一帧在管道里的事件记录；只在开了逐帧追踪时创建
struct FrameTrace
{
    quint64 id = 0;                                                                             // 帧序号
    qint64 arrive_us = 0;                                                                       // 到达画布的时刻
    qint64 gap_us = 0;                                                                          // 与上一帧的到达间隔
    qint64 sync_us = 0;                                                                         // synchronize 耗时
    qint64 draw_us = 0;                                                                         // render 里绘制耗时
    qint64 swap_us = 0;                                                                         // 提交到 frameSwapped 的间隔
    qint64 render_end_us = 0;                                                                   // render 结束时刻
    qint64 swap_gap_us = 0;                                                                     // 上屏间隔
    bool printed = false;                                                                       // 同一帧只打一次
};

using FrameTracePtr = std::shared_ptr<FrameTrace>;

class FrameProbe
{
public:
    FrameProbe();
    ~FrameProbe();

    bool Enabled() const { return trace_enabled_; }
    qint64 NowUs() const { return clock_.nsecsElapsed() / 1000; }

    // ---- 视频线程 ----
    FrameTracePtr BeginFrame(qint64 arrive_us, qint64 gap_us);                                  // 建追踪记录（未开追踪返回空）
    qint64 TakeArrivalGap(qint64 now_us);                                                       // 算与上一帧的到达间隔
    void AccountGap(qint64 gap_us);                                                             // 到达间隔分档
    void OnReplaced(const FrameTracePtr &trace, qint64 alive_us);                               // 被新帧顶掉的旧帧
    void MarkArrived();
    void MarkDropped();

    // ---- 渲染线程 ----
    void BeginSyncPass();                                                                       // 一轮 synchronize 开始
    qint64 SyncStartUs() const { return sync_start_us_; }
    void SetPassArrive(qint64 arrive_us) { pass_arrive_us_.storeRelaxed(arrive_us); }
    void SetPassSync(qint64 sync_us) { pass_sync_us_.storeRelaxed(sync_us); }
    void OnRenderEnd(qint64 draw_start_us, const FrameTracePtr &trace);
    void OnSwapped();                                                                           // frameSwapped

    // ---- 每秒结算（GUI 线程）----
    void ReportSecond();

    // 帧率展示：上一秒算出来的值
    int Fps() const { return fps_; }

private:
    QElapsedTimer clock_;

    bool trace_enabled_ = false;
    quint64 next_frame_id_ = 0;

    FrameTracePtr swapped_trace_;                                                               // 刚提交完、等着打印的那一帧

    QAtomicInt frames_arrived_;
    QAtomicInt frames_swapped_{0};
    QAtomicInt frames_dropped_{0};
    QAtomicInt lat_lt1_{0};
    QAtomicInt lat_1_2_{0};
    QAtomicInt lat_2_3_{0};
    QAtomicInt lat_gt3_{0};
    QAtomicInt gap_lt85_{0};
    QAtomicInt gap_85_17_{0};
    QAtomicInt gap_17_34_{0};
    QAtomicInt gap_gt34_{0};
    QAtomicInt swap_lt85_{0};
    QAtomicInt swap_85_17_{0};
    QAtomicInt swap_17_34_{0};
    QAtomicInt swap_gt34_{0};

    qint64 last_swap_us_ = 0;
    QAtomicInteger<qint64> lat_max_us_{0};
    QAtomicInteger<qint64> pass_arrive_us_{0};
    QAtomicInteger<qint64> pass_sync_us_{0};
    QAtomicInteger<qint64> sync_start_us_{0};
    QAtomicInteger<qint64> last_present_us_{0};

    int fps_ = 0;
    int fps_last_swapped_ = 0;
    int fps_last_arrived_ = 0;
};
