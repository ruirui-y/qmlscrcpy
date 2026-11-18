#include "ui/video_item.h"

#include "stream/video_traffic.h"
#include "ui/video_renderer.h"

#include <QDebug>
#include <QGuiApplication>

#include <QThread>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QCursor>
#include <QTimer>
#include <QQuickWindow>
#include <windows.h>

#include <cstring>

#include "base/log.h"

// Release 里不定义这个宏，下面的每帧追踪代码整段不会被编进去

using qsc::stream::Frame;

namespace
{

constexpr quint64 kClickerPointerId = 99;                                                       // 连点专用手指，避开鼠标和映射键

// 引擎的动作枚举转成协议那边的 0 按下 / 1 抬起 / 2 移动
quint8 ActionCode(qsc::input::KeyMapAction::Kind kind)
{
    switch (kind)
    {
    case qsc::input::KeyMapAction::Kind::Up:
        return 1;
    case qsc::input::KeyMapAction::Kind::Move:
        return 2;
    case qsc::input::KeyMapAction::Kind::Down:
    default:
        return 0;
    }
}

// 这个键在系统里是否还按着：自动重复会成对发 Press/Release，靠它把假的 Release 挡掉
bool KeyPhysicallyDown(int qt_key)
{
    const int vk = static_cast<int>(qt_key);
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}
// Qt 的键值转成配置里写的文本：Q → "Q"，空格 → "Space"
QString KeyName(int qt_key)
{
    return QKeySequence(qt_key).toString(QKeySequence::PortableText);
}

// 直接问系统修饰键是否按下：Qt 的 keyboardModifiers() 靠事件更新，没新事件时会读到旧值
bool ModifierDownNow(const QString &modifier)
{
    const auto down = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (modifier == QStringLiteral("none"))
    {
        return false;
    }
    if (modifier == QStringLiteral("alt"))
    {
        return down(VK_MENU);
    }
    if (modifier == QStringLiteral("shift"))
    {
        return down(VK_SHIFT);
    }
    if (modifier == QStringLiteral("ctrl_alt"))
    {
        return down(VK_CONTROL) && down(VK_MENU);
    }
    return down(VK_CONTROL);
}

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



} // namespace

VideoItem::VideoItem(QQuickItem *parent) : QQuickFramebufferObject(parent)
{
    clock_.start();
    setFlag(QQuickItem::ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);                                                    // 只认左键：右键留给后面做菜单

    // 重绘由 Present 逐帧投递，尺寸变化也在 Present 里通知，GUI 线程不再有 8ms 轮询定时器
    trace_enabled_ = qEnvironmentVariableIntValue("QS_TRACE_FRAMES") == 1;
    qInfo() << "[perf] 逐帧追踪" << (trace_enabled_ ? "开" : "关");

    // 每秒结算一次：到达 / 上屏 / 丢帧率 / 延迟分布
    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(1000);
    QObject::connect(fps_timer_, &QTimer::timeout, this, &VideoItem::ReportSecond);
    fps_timer_->start();

    // 摇杆心跳：部分设备会把完全静止的触摸判成别的状态，每 100ms 补一条同样的 MOVE
    keymap_tick_timer_ = new QTimer(this);
    keymap_tick_timer_->setInterval(100);
    QObject::connect(keymap_tick_timer_, &QTimer::timeout, this, [this]()
    {
        if (map_mode_ != 1)
        {
            return;
        }
        for (const qsc::input::KeyMapAction &action : key_map_engine_.Tick())
        {
            EmitControlMessage(touch_.NormalizedTouch(action.pointer_id_, ActionCode(action.kind_), action.pos_));
        }
    });
    keymap_tick_timer_->start();

    // 映射键用轮询判断按下/松开：Qt 的自动重复事件在 Windows 上不可信，成对的 Press/Release 会互相干扰
    keymap_poll_timer_ = new QTimer(this);
    keymap_poll_timer_->setInterval(16);
    QObject::connect(keymap_poll_timer_, &QTimer::timeout, this, &VideoItem::SlotKeyMapPoll);
    keymap_poll_timer_->start();

    // 连点：按住空格时按设定间隔在鼠标位置连续点击
    clicker_timer_ = new QTimer(this);
    clicker_timer_->setInterval(10);
    QObject::connect(clicker_timer_, &QTimer::timeout, this, [this]()
    {
        if (clicker_interval_ms_ <= 0 || map_mode_ == 2 || width() <= 0 || height() <= 0)
        {
            return;
        }

        // 位置直接查全局光标：不做 hover 跟踪，避免影响滚轮和拖动
        const QPointF local = mapFromGlobal(QCursor::pos());
        if (local.x() < 0 || local.y() < 0 || local.x() >= width() || local.y() >= height())
        {
            return;
        }
        clicker_pos_ = QPointF(local.x() / width(), local.y() / height());

        // 空格没按着就什么都不做，按下去的瞬间重新计时
        if ((::GetAsyncKeyState(VK_SPACE) & 0x8000) == 0)
        {
            clicker_next_us_ = 0;
            return;
        }

        const qint64 now_us = clock_.nsecsElapsed() / 1000;
        if (clicker_next_us_ != 0 && now_us < clicker_next_us_)
        {
            return;
        }
        clicker_next_us_ = now_us + static_cast<qint64>(clicker_interval_ms_) * 1000;

        // 用和鼠标点击相同的通道发：按下 + 20ms 后抬起
        EmitControlMessage(touch_.NormalizedTouch(kClickerPointerId, 0, clicker_pos_));
        const QPointF pos = clicker_pos_;
        QTimer::singleShot(20, this, [this, pos]()
        {
            EmitControlMessage(touch_.NormalizedTouch(kClickerPointerId, 1, pos));
        });
    });
    clicker_timer_->start();

    // 松开修饰键时要抬两指，但窗口没焦点时收不到 keyRelease，所以每 250ms 问一次键盘状态兜底
    pinch_watch_timer_ = new QTimer(this);
    pinch_watch_timer_->setInterval(250);
    QObject::connect(pinch_watch_timer_, &QTimer::timeout, this, [this]()
    {
        if (pinch_active_ && !ModifierDownNow(pinch_modifier_))
        {
            EndPinch();
        }
    });
    pinch_watch_timer_->start();
}

QQuickFramebufferObject::Renderer *VideoItem::createRenderer() const
{
    return new qsc::ui::YuvRenderer;
}

QSize VideoItem::FrameSize() const
{
    QMutexLocker locker(&mutex_);
    return frame_size_;
}

bool VideoItem::KeepAspect() const
{
    return keep_aspect_.load();
}

int VideoItem::Fps() const
{
    return fps_;
}

// 开关等比缩放，变了要重绘一次
void VideoItem::SetKeepAspect(bool keep_aspect)
{
    if (keep_aspect_.exchange(keep_aspect) == keep_aspect)
    {
        return;
    }
    emit SigKeepAspectChanged();
    update();
}

// 视频线程调：存下最新一帧，不投递任何跨线程事件
// 切换滚轮模式：scroll 发原生滚动事件，drag 模拟手指滑动
void VideoItem::SetWheelMode(const QString &mode)
{
    touch_.SetWheelMode(mode == QStringLiteral("drag") ? qsc::input::WheelMode::Drag
                                                       : qsc::input::WheelMode::Scroll);
}

// 视频线程调：存下最新一帧，等渲染线程来取
// 滑动模式的手感：一格滚轮拖多少像素
void VideoItem::SetDragPixels(int pixels)
{
    touch_.SetDragPixels(pixels);
}

// 左键拖拽倍率：鼠标走 1 像素，设备上的手指走 scale 像素
void VideoItem::SetDragScale(double scale)
{
    touch_.SetDragScale(scale);
}

// 视频线程调：存下最新一帧，等渲染线程来取
void VideoItem::Present(const Frame &frame)
{
    if (!frame.IsValid())
    {
        return;
    }

    const qint64 enqueue_us = clock_.nsecsElapsed() / 1000;

    // 到达间隔分档：和上屏延迟分开统计，一个看源多快、一个看我们多慢
    const qint64 prev_present_us = last_present_us_.fetchAndStoreRelaxed(enqueue_us);
    const qint64 gap_us = prev_present_us > 0 ? (enqueue_us - prev_present_us) : 0;
    if (gap_us > 0 && gap_us < 1000000)
    {
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

    // 逐帧追踪才建 Trace；平时只有计数和两个时间戳，开销可以忽略
    FrameTracePtr trace;
    if (trace_enabled_)
    {
        trace = std::make_shared<FrameTrace>();
        trace->id = ++next_frame_id_;
        trace->arrive_us = enqueue_us;
        trace->gap_us = gap_us;
    }

    FrameTracePtr replaced_trace;
    qint64 replaced_alive_us = 0;
    bool replaced = false;
    bool size_changed = false;
    {
        QMutexLocker locker(&mutex_);
        // 只留最新一帧：视频线程比渲染快的时候，旧帧直接覆盖，不排队
        replaced = has_pending_;
        if (replaced && trace_enabled_)
        {
            replaced_trace = pending_trace_;
            replaced_alive_us = enqueue_us - pending_trace_->arrive_us;
        }
        size_changed = frame_size_ != frame.Size();
        pending_ = frame;
        pending_arrive_us_ = enqueue_us;
        if (trace_enabled_)
        {
            pending_trace_ = trace;
        }
        has_pending_ = true;
        frame_size_ = frame.Size();
    }
    frames_arrived_.ref();
    if (replaced)
    {
        frames_dropped_.ref();
    }

    // 尺寸变了（设备旋转）才通知 QML；emit 只能在 GUI 线程做，投递一次
    if (size_changed)
    {
        QMetaObject::invokeMethod(this, [this]() { NotifyFrameSize(); }, Qt::QueuedConnection);
    }

    // 被顶掉的帧当场结算：它从到达到被覆盖活了多久
    if (replaced_trace)
    {
        PrintTrace(replaced_trace, false, replaced_alive_us);
    }

    // 到帧就排队请求一次重绘：视频线程直接调 update() 不安全，投递到 GUI 线程执行
    QMetaObject::invokeMethod(this, [this, trace, enqueue_us]()
    {
        const qint64 start_us = clock_.nsecsElapsed() / 1000;
        if (trace)
        {
            trace->queue_us.store(start_us - trace->arrive_us);
        }
        update();
        if (trace)
        {
            trace->update_done_us.store(clock_.nsecsElapsed() / 1000);
        }
    }, Qt::QueuedConnection);
}


// 渲染线程调：取走最新一帧（连它的到达时刻和事件记录），没有就返回无效帧
Frame VideoItem::TakePendingFrame(FrameTracePtr *trace_out, qint64 *arrive_us_out)
{
    QMutexLocker locker(&mutex_);
    if (!has_pending_)
    {
        return Frame();
    }
    const Frame frame = pending_;
    if (trace_out != nullptr)
    {
        *trace_out = pending_trace_;
    }
    if (arrive_us_out != nullptr)
    {
        *arrive_us_out = pending_arrive_us_;
    }
    pending_ = Frame();
    pending_trace_.reset();
    has_pending_ = false;
    return frame;
}

QSize VideoItem::NotifiedFrameSize() const
{
    QMutexLocker locker(&mutex_);
    return notified_size_;
}

// 记下本轮 synchronize 的起点，返回上一轮起点：两者相减就是渲染线程的轮次间隔
qint64 VideoItem::AdvanceSyncPass()
{
    const qint64 now_us = clock_.nsecsElapsed() / 1000;
    return sync_start_us_.fetchAndStoreRelaxed(now_us);
}

// 本轮 synchronize 的起始时刻
qint64 VideoItem::SyncStartUs() const
{
    return sync_start_us_.loadRelaxed();
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
void VideoItem::Clear()
{
    {
        QMutexLocker locker(&mutex_);
        pending_ = Frame();
        has_pending_ = false;
    }
    // 断开时把按键重复计数一起清掉，下次连接不会从第 3 次重复开始
    keys_.Reset();
    fps_last_arrived_ = 0;
    update();
}

// GUI 线程：帧尺寸变了才通知 QML（由 Present 投递过来，不再轮询）
void VideoItem::NotifyFrameSize()
{
    QSize current_size;
    {
        QMutexLocker locker(&mutex_);
        current_size = frame_size_;
    }

    if (current_size != notified_size_)
    {
        notified_size_ = current_size;
        emit SigFrameSizeChanged();
    }
}

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
void VideoItem::geometryChange(const QRectF &new_geometry, const QRectF &old_geometry)
{
    QQuickItem::geometryChange(new_geometry, old_geometry);
    // 画布尺寸变了，FBO 也要跟着变，请一次重绘让渲染线程重新拿尺寸
    update();
}

// 组件树建好后窗口才一定有：交换缓冲和同步起点的两个信号只在这里接一次。
// 之前放在每帧的队列回调里，Qt6 下 update() 会同步进渲染循环再回到这里，栈直接爆掉
void VideoItem::componentComplete()
{
    QQuickFramebufferObject::componentComplete();

    if (window() == nullptr)
    {
        return;
    }

    // 交换缓冲发生在我们看不见的地方，接 frameSwapped 才能量到它阻塞了多久。
    // Qt6 里 Qt::UniqueConnection 配 lambda 会直接断言（Qt5 是静默忽略），这里靠 swap_hooked_ 保证只接一次
    QObject::connect(window(), &QQuickWindow::frameSwapped, this, [this]() { MarkSwapped(); });

    // 渲染线程刚开始这一轮的时刻：它到 synchronize 之间的差就是 Qt 自己的准备工作
    QObject::connect(window(), &QQuickWindow::beforeSynchronizing, this, [this]()
    {
        before_sync_us_.storeRelaxed(clock_.nsecsElapsed() / 1000);
    }, Qt::DirectConnection);

    swap_hooked_ = true;
}

// 坐标换算器要用最新的画布尺寸和帧尺寸，每次事件前同步一次（都是几个赋值，开销可忽略）
void VideoItem::SyncMapper()
{
    mapper_.SetFrameSize(FrameSize());
    mapper_.SetItemSize(QSizeF(width(), height()));
    mapper_.SetKeepAspect(KeepAspect());
}

// 消息为空说明点在黑边上或者键没映射，不发
void VideoItem::EmitControlMessage(const QByteArray &message)
{
    if (message.isEmpty())
    {
        qCDebug(qsc::base::lcInput) << "消息为空，丢掉（点在黑边上或键没映射）";
        return;
    }
    qCDebug(qsc::base::lcInput) << "发出消息" << message.size() << "字节" << message.left(2).toHex();
    emit SigControlMessageReady(message);
}

// 鼠标按下：先拿焦点，再换算坐标发触摸按下
void VideoItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QQuickFramebufferObject::mousePressEvent(event);
        return;
    }

    // 点了才要键盘焦点，免得一启动就把焦点抢过来
    forceActiveFocus();
    SyncMapper();
    EmitControlMessage(touch_.Press(event->position()));
    event->accept();
}

// 按住移动才发，否则设备会当成悬停
void VideoItem::mouseMoveEvent(QMouseEvent *event)
{
    // 只有按着左键才发移动，否则设备会当成悬停乱跳
    if ((event->buttons() & Qt::LeftButton) == 0)
    {
        QQuickFramebufferObject::mouseMoveEvent(event);
        return;
    }

    SyncMapper();
    EmitControlMessage(touch_.Move(event->position()));
    event->accept();
}

// 鼠标抬起：发触摸抬起
void VideoItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QQuickFramebufferObject::mouseReleaseEvent(event);
        return;
    }

    SyncMapper();
    EmitControlMessage(touch_.Release(event->position()));
    event->accept();
}

// 滚轮按下 120 为一格，换算成滚动消息
void VideoItem::wheelEvent(QWheelEvent *event)
{
    // 一格的 delta 是 120，除下来就是滚了几格
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0)
    {
        QQuickFramebufferObject::wheelEvent(event);
        return;
    }

    SyncMapper();

    // 按住修饰键滚轮 = 双指捏合：上滚放大、下滚缩小，中心跟着鼠标
    if (PinchModifierHeld(event->modifiers()))
    {
        PinchBy(notches, event->position());
        event->accept();
        return;
    }
    // 拖动模式一次会出三条消息，逐条发

    // 没按修饰键就滚：先把两指抬起，否则设备会把滚动当成继续缩放
    if (pinch_active_)
    {
        const QVector<QByteArray> ending = touch_.PinchEnd(pinch_center_, pinch_radius_);
        for (const QByteArray &message : ending)
        {
            EmitControlMessage(message);
        }
        pinch_active_ = false;
    }
    const QVector<QByteArray> messages = touch_.Scroll(event->position(), notches);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }

    // 拖动模式：抬起延后 120ms，让手指在终点停稳；安卓取最后 100ms 的速度，停住就不会甩动
    if (touch_.Mode() == qsc::input::WheelMode::Drag)
    {
        const QPointF pos = event->position();
        const int count = notches;
        QTimer::singleShot(120, this, [this, pos, count]()
        {
            EmitControlMessage(touch_.ScrollDragEnd(pos, count));
        });
    }
}

// 按键按下：没映射的键交回基类处理
void VideoItem::keyPressEvent(QKeyEvent *event)
{
    // 玩模式：已映射的键转成触摸，不再作为按键转发
    if (map_mode_ == 1 && key_map_engine_.Handles(KeyName(event->key())))
    {
        // 映射键全部交给 16ms 轮询处理，这里只负责吞掉，避免 Qt 的自动重复事件干扰
        event->accept();
        return;

    }
    const QByteArray message = keys_.Press(event->key(), event->modifiers(), event->isAutoRepeat());
    if (message.isEmpty())
    {
        QQuickFramebufferObject::keyPressEvent(event);
        return;
    }
    EmitControlMessage(message);
    event->accept();
}

// 按键抬起：同时清掉这个键的重复计数
void VideoItem::keyReleaseEvent(QKeyEvent *event)
{
    // 松开后捏合键已经不在按下状态就抬两指；其它按键的松开不影响捏合
    if (pinch_active_ && !ModifierDownNow(pinch_modifier_))
    {
        EndPinch();
    }

    if (map_mode_ == 1 && key_map_engine_.Handles(KeyName(event->key())))
    {
        // 映射键的抬起同样交给轮询，这里只吞掉
        event->accept();
        return;
    }
    const QByteArray message = keys_.Release(event->key(), event->modifiers());
    if (message.isEmpty())
    {
        QQuickFramebufferObject::keyReleaseEvent(event);
        return;
    }
    EmitControlMessage(message);
    event->accept();
}

// 当前按下的修饰键是不是配置里那个捏合键
bool VideoItem::PinchModifierHeld(Qt::KeyboardModifiers modifiers) const
{
    if (pinch_modifier_ == QStringLiteral("none"))
    {
        return false;
    }
    if (pinch_modifier_ == QStringLiteral("alt"))
    {
        return modifiers.testFlag(Qt::AltModifier);
    }
    if (pinch_modifier_ == QStringLiteral("shift"))
    {
        return modifiers.testFlag(Qt::ShiftModifier);
    }
    if (pinch_modifier_ == QStringLiteral("ctrl_alt"))
    {
        return modifiers.testFlag(Qt::ControlModifier) && modifiers.testFlag(Qt::AltModifier);
    }
    return modifiers.testFlag(Qt::ControlModifier);
}

// 一格滚轮把两指间距放大或缩小 12%，第一次滚先把两指按下去
void VideoItem::PinchBy(int notches, const QPointF &pos)
{
    pinch_center_ = pos;

    // 半径上限是画面短边的 45%，再大会把第二根手指顶出屏幕
    const QSize frame = FrameSize();
    const int limit = qMax(20, qMin(frame.width(), frame.height()) * 50 / 100);

    if (!pinch_active_)
    {
        // 半径跨手势保留：滚得慢时每格都会新开一次手势，若每次重置回 40，
        // 上一格放大的效果就丢了，看起来像"每一格都在放大"
        const qint64 now_us = clock_.nsecsElapsed() / 1000;
        if (pinch_last_us_ == 0 || (now_us - pinch_last_us_) > 2000000)
        {
            pinch_radius_ = 40;
        }
        pinch_last_us_ = now_us;
        pinch_active_ = true;
        emit SigPinchingChanged();
        const QVector<QByteArray> messages = touch_.PinchBegin(pinch_center_, pinch_radius_);
        for (const QByteArray &message : messages)
        {
            EmitControlMessage(message);
        }
    }

    // 一格 15%：12% 感觉滚几下就没变化了
    const double scaled = static_cast<double>(pinch_radius_) * std::pow(1.15, notches);
    pinch_radius_ = qBound(20, static_cast<int>(scaled), limit);

    const QVector<QByteArray> messages = touch_.PinchStep(pinch_center_, pinch_radius_);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }
}

// 捏合修饰键：ctrl / alt / shift / ctrl_alt / none
void VideoItem::SetPinchModifier(const QString &modifier)
{
    pinch_modifier_ = modifier.trimmed().toLower();
}

// 抬起两指：修饰键松开时调用
void VideoItem::EndPinch()
{
    if (!pinch_active_)
    {
        return;
    }

    const QVector<QByteArray> messages = touch_.PinchEnd(pinch_center_, pinch_radius_);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }
    pinch_active_ = false;
    emit SigPinchingChanged();
}

// 切映射模式：从"玩/编辑"退出时要把所有按住的手指抬起来，否则设备会一直按着
void VideoItem::SetMapMode(int mode)
{
    const int clamped = qBound(0, mode, 2);
    if (clamped == map_mode_)
    {
        return;
    }

    if (map_mode_ != 0)
    {
        for (const qsc::input::KeyMapAction &action : key_map_engine_.Reset())
        {
            EmitControlMessage(touch_.NormalizedTouch(action.pointer_id_, ActionCode(action.kind_), action.pos_));
        }
    }

    map_mode_ = clamped;
    if (map_mode_ == 2)
    {
        emit SigKeyMapChanged();
    }
    emit SigMapModeChanged();
}

// 换设备：读这台设备自己的键位文件，读不到就是空配置
void VideoItem::SetKeyMapPath(const QString &path)
{
    key_map_path_ = path;
    QString error;
    key_map_ = qsc::input::KeyMap::Load(path, &error);
    if (!error.isEmpty())
    {
        qWarning() << "[keymap]" << error;
    }
    key_map_engine_.SetMap(key_map_);
    qInfo() << "[keymap] 已加载" << key_map_.items_.size() << "个键位：" << path;
    emit SigKeyMapChanged();
}

// 给 QML 叠加层用的键位快照：位置已经换算成 0~1，界面直接按比例摆
QVariantList VideoItem::KeyMapItems() const
{
    QVariantList list;
    for (int i = 0; i < key_map_.items_.size(); ++i)
    {
        const qsc::input::KeyMapItem &item = key_map_.items_.at(i);
        QVariantMap entry;
        entry.insert(QStringLiteral("index"), i);
        entry.insert(QStringLiteral("type"), static_cast<int>(item.type_));
        entry.insert(QStringLiteral("label"), item.label_);
        entry.insert(QStringLiteral("key"), item.key_);
        entry.insert(QStringLiteral("x"), item.pos_.x());
        entry.insert(QStringLiteral("y"), item.pos_.y());
        entry.insert(QStringLiteral("radius"), item.radius_);
        entry.insert(QStringLiteral("up"), item.up_key_);
        entry.insert(QStringLiteral("down"), item.down_key_);
        entry.insert(QStringLiteral("left"), item.left_key_);
        entry.insert(QStringLiteral("right"), item.right_key_);
        list.append(entry);
    }
    return list;
}

// 新增一条键位，返回它的下标；type 0 点按 1 按住 2 摇杆
int VideoItem::AddKeyMapItem(int type, const QString &key, double x, double y, const QString &label)
{
    qsc::input::KeyMapItem item;
    item.type_ = static_cast<qsc::input::KeyMapItemType>(qBound(0, type, 2));
    item.key_ = key;
    item.label_ = label;
    item.pos_ = QPointF(x, y);
    key_map_.Add(item);
    key_map_engine_.SetMap(key_map_);

    const int index = key_map_.items_.size() - 1;
    emit SigKeyMapChanged();
    return index;
}

void VideoItem::MoveKeyMapItem(int index, double x, double y)
{
    key_map_.Move(index, QPointF(x, y));
    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

void VideoItem::RemoveKeyMapItem(int index)
{
    key_map_.Remove(index);
    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

// 写回 JSON：目录不存在时先建
void VideoItem::SaveKeyMap()
{
    if (key_map_path_.isEmpty())
    {
        return;
    }
    const QFileInfo info(key_map_path_);
    QDir().mkpath(info.absolutePath());
    QString error;
    if (!key_map_.Save(key_map_path_, &error))
    {
        qWarning() << "[keymap]" << error;
    }
}

// 改摇杆的四个方向键：空的填回默认值，免得某个方向永远推不动
void VideoItem::SetSteerKeys(int index, const QString &up, const QString &down, const QString &left,
                             const QString &right)
{
    if (index < 0 || index >= key_map_.items_.size() || key_map_.items_.at(index).type_ != qsc::input::KeyMapItemType::Steer)
    {
        return;
    }

    qsc::input::KeyMapItem &item = key_map_.items_[index];
    item.up_key_ = up.isEmpty() ? item.up_key_ : up;
    item.down_key_ = down.isEmpty() ? item.down_key_ : down;
    item.left_key_ = left.isEmpty() ? item.left_key_ : left;
    item.right_key_ = right.isEmpty() ? item.right_key_ : right;

    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

// 键名字符串转成 Windows 虚拟键码：字母数字用 ASCII，其余查小表
int VkForName(const QString &name)
{
    if (name.size() == 1)
    {
        const QChar ch = name.at(0).toUpper();
        return ch.unicode();
    }
    // 表里的键名用小写：配置里可能存成 space / Space，统一按小写查
    static const QHash<QString, int> kExtra = {
        { QStringLiteral("space"), VK_SPACE },   { QStringLiteral("tab"), VK_TAB },
        { QStringLiteral("esc"), VK_ESCAPE },    { QStringLiteral("enter"), VK_RETURN },
        { QStringLiteral("up"), VK_UP },         { QStringLiteral("down"), VK_DOWN },
        { QStringLiteral("left"), VK_LEFT },     { QStringLiteral("right"), VK_RIGHT },
        { QStringLiteral("shift"), VK_SHIFT },   { QStringLiteral("ctrl"), VK_CONTROL },
        { QStringLiteral("alt"), VK_MENU },
    };
    return kExtra.value(name.toLower(), 0);
}

// 16ms 轮询：只有"按下状态"发生变化才发触摸，自动重复事件完全不影响这里
void VideoItem::SlotKeyMapPoll()
{
    if (map_mode_ != 1)
    {
        if (!polled_keys_.isEmpty())
        {
            DispatchKeyMapActions(key_map_engine_.Reset());
            polled_keys_.clear();
        }
        return;
    }

    for (const QString &name : key_map_engine_.TrackedKeys())
    {
        const int vk = VkForName(name);
        if (vk == 0)
        {
            continue;
        }
        const bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool was_down = polled_keys_.contains(name);
        if (down == was_down)
        {
            continue;
        }

        if (down)
        {
            polled_keys_.insert(name);
            qInfo() << "[keymap] poll down" << name;
            DispatchKeyMapActions(key_map_engine_.Press(name));
        }
        else
        {
            polled_keys_.remove(name);
            qInfo() << "[keymap] poll up" << name;
            DispatchKeyMapActions(key_map_engine_.Release(name));
        }
    }
}

// 动作翻成控制消息：点按类先按下，40ms 后补一个抬起
void VideoItem::DispatchKeyMapActions(const QVector<qsc::input::KeyMapAction> &actions)
{
    const bool tap_pair = actions.size() == 2
                          && actions.at(0).kind_ == qsc::input::KeyMapAction::Kind::Down
                          && actions.at(1).kind_ == qsc::input::KeyMapAction::Kind::Up;
    for (const qsc::input::KeyMapAction &action : actions)
    {
        if (tap_pair && action.kind_ == qsc::input::KeyMapAction::Kind::Up)
        {
            const quint64 pointer = action.pointer_id_;
            const QPointF pos = action.pos_;
            QTimer::singleShot(40, this, [this, pointer, pos]()
            {
                EmitControlMessage(touch_.NormalizedTouch(pointer, 1, pos));
            });
            continue;
        }
        EmitControlMessage(touch_.NormalizedTouch(action.pointer_id_, ActionCode(action.kind_), action.pos_));
    }
}

// 进入录制：点圆点后调它，下一次按键就绑到这个键位；Esc 取消
void VideoItem::BeginKeyRecord(int index)
{
    if (index < 0 || index >= key_map_.items_.size())
    {
        return;
    }
    record_index_ = index;
    forceActiveFocus();                                                                          // 叠加层不拿焦点，不抢就收不到按键
    qInfo() << "[keymap] record start index" << index;
    emit SigRecordingChanged();
}

// QML 收到按键后调它：把键绑到当前录制目标上
void VideoItem::BindRecordedKey(int qt_key)
{
    if (record_index_ < 0 || record_index_ >= key_map_.items_.size())
    {
        return;
    }
    const int index = record_index_;
    record_index_ = -1;
    if (key_map_.items_.at(index).type_ != qsc::input::KeyMapItemType::Steer)
    {
        key_map_.items_[index].key_ = KeyName(qt_key);
        key_map_.items_[index].label_ = KeyName(qt_key);
        key_map_engine_.SetMap(key_map_);
        qInfo() << "[keymap] record bound" << KeyName(qt_key) << "to index" << index;
        emit SigKeyMapChanged();
    }
    emit SigRecordingChanged();
}

// 连点间隔：0 关，其它值按毫秒连点；同时把开关状态记进配置
void VideoItem::SetClickerInterval(int ms)
{
    clicker_interval_ms_ = qBound(0, ms, 1000);
    clicker_next_us_ = 0;
}
