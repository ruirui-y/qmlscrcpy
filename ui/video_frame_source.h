#pragma once

// 渲染器需要的"帧来源"接口：把画布和渲染器解耦。
// VideoItem 之后会实现它，渲染器就不再依赖 VideoItem 的具体类型
// （这样从 QQuickFramebufferObject 换成 QQuickRhiItem 时，渲染器一行不用改）。

#include <QPointF>
#include <QSize>

#include "stream/frame.h"

namespace qsc::ui
{

// 帧事件记录，统计用；渲染器只透传，不解释
struct RenderFrameTiming
{
    qint64 arrive_us = 0;                                                                       // 帧到达画布的时刻
    qint64 sync_start_us = 0;                                                                   // 本帧 synchronize 起始时刻
};

class VideoFrameSource
{
public:
    virtual ~VideoFrameSource() = default;

    // 取最新一帧；没有就返回无效帧
    virtual qsc::stream::Frame TakePendingFrame(RenderFrameTiming *timing_out) = 0;

    // 渲染开始/结束的时刻记录，统计用
    virtual void NotifySyncPass() = 0;                                                          // 新的一轮同步开始（统计用）
    virtual qint64 SyncPassStartUs() const = 0;
    virtual qint64 NowUs() const = 0;
    virtual void NotifyRenderEnd(qint64 draw_start_us) = 0;                                     // 一帧画完

    // 画布属性
    virtual bool KeepAspectEnabled() const = 0;
    virtual QSize CanvasSize() const = 0;
};

} // namespace qsc::ui
