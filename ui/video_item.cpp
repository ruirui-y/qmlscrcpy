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

// 这两个和 video_item_input.cpp 各留一份：匿名命名空间是内部链接，两个 TU 互不影响
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

    // 到达间隔：和上屏延迟分开统计，一个看源多快、一个看我们多慢
    const qint64 prev_present_us = last_present_us_.fetchAndStoreRelaxed(enqueue_us);
    const qint64 gap_us = prev_present_us > 0 ? (enqueue_us - prev_present_us) : 0;
    AccountGap(gap_us);                                                                       // 分档计数在统计文件里

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
        ReportReplacedFrame(replaced_trace, replaced_alive_us);
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
