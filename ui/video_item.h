#pragma once

#include <QAtomicInt>
#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QEvent>
#include <QMutex>
#include <QQuickFramebufferObject>
#include <QSize>

#include <atomic>
#include <memory>

#include "input/coordinate_mapper.h"
#include "input/key_controller.h"
#include <QVariantList>

#include "input/keymap.h"
#include "input/keymap_engine.h"
#include "input/touch_controller.h"
#include "stream/frame.h"


class QTimer;

// 一帧从到达到上屏（或被顶掉）的完整事件记录。跟着帧一起传递，谁的事件谁来填。
struct FrameTrace
{
    quint64 id = 0;                                                                             // 帧序号，从 1 开始
    qint64 arrive_us = 0;                                                                       // 到进程的时刻
    qint64 gap_us = 0;                                                                          // 距上一帧到达的间隔
    qint64 swap_gap_us = 0;                                                                     // 距上一帧上屏的间隔
    std::atomic<qint64> queue_us{-1};                                                           // 事件在 GUI 队列里等多久
    std::atomic<qint64> update_done_us{0};                                                      // update() 执行完的时刻
    std::atomic<qint64> schedule_us{-1};                                                        // update 完到渲染线程被唤醒（beforeSynchronizing）
    std::atomic<qint64> preprocess_us{-1};                                                      // Qt 同步前的准备工作耗时
    std::atomic<qint64> wake_us{-1};                                                            // 唤醒到 synchronize 真正开跑
    std::atomic<bool> printed{false};                                                            // 已经打印过就不再重复
    std::atomic<qint64> sync_us{-1};                                                            // synchronize 全程
    std::atomic<qint64> draw_us{-1};                                                            // render 全程
    std::atomic<qint64> render_end_us{0};                                                       // 画完的时刻
    std::atomic<qint64> swap_us{-1};                                                            // 画完到提交给系统
};

using FrameTracePtr = std::shared_ptr<FrameTrace>;

// QML 里显示一路视频的画布：三张 YUV 平面纹理，着色器在 GPU 上合成 RGB。
// keepAspect 打开时按帧的宽高比居中显示（两边留黑），关掉就拉满整块画布。
//
// 用 QQuickFramebufferObject 而不是自定义 QSGNode：它的 Renderer::render() 是 Qt
// 保证"渲染线程 + GL 上下文当前"的唯一入口，建纹理、传纹理都在这儿做。
//
// 视频线程只往这里丢帧（加锁存最新一帧，附带该帧的 FrameTrace），不投递任何跨线程事件；
// 重绘请求通过 QueuedConnection 交给 GUI 线程发 update()。
class VideoItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(QSize frameSize READ FrameSize NOTIFY SigFrameSizeChanged)
    Q_PROPERTY(bool keepAspect READ KeepAspect WRITE SetKeepAspect NOTIFY SigKeepAspectChanged)
    Q_PROPERTY(int fps READ Fps NOTIFY SigFpsChanged)

public:
    explicit VideoItem(QQuickItem *parent = nullptr);

    Renderer *createRenderer() const override;

    QSize FrameSize() const;                                                                    // 最近一帧的尺寸，QML 里用来显示和算宽高比
    bool KeepAspect() const;
    int Fps() const;

    void SetKeepAspect(bool keep_aspect);
    void SetWheelMode(const QString &mode);                                                     // "scroll" 发滚动事件，"drag" 模拟滑动
    void SetDragPixels(int pixels);                                                             // 滑动模式下一格滚轮拖多少像素
    void SetDragScale(double scale);                                                            // 左键拖拽倍率，1.0 = 原样
    void SetPinchModifier(const QString &modifier);                                             // 捏合缩放的修饰键：ctrl / alt / shift / ctrl_alt / none
    bool IsPinching() const { return pinch_active_; }                                           // 现在是不是两指按着

    // 映射模式：0 关（键盘照旧转发）/ 1 玩（映射生效）/ 2 编辑（叠加层，键盘不转发）
    void SetMapMode(int mode);
    int MapMode() const { return map_mode_; }
    void SetKeyMapPath(const QString &path);                                                    // 换设备时给新的键位文件路径
    QVariantList KeyMapItems() const;                                                            // 给 QML 叠加层读键位列表
    int AddKeyMapItem(int type, const QString &key, double x, double y, const QString &label);   // 新增键位，返回下标
    void MoveKeyMapItem(int index, double x, double y);                                          // 拖动键位
    void RemoveKeyMapItem(int index);                                                            // 删除键位
    void SaveKeyMap();                                                                           // 写回 JSON，编辑退出时调

    // 视频线程调：把最新一帧放进来（引用计数，不拷像素），旧的直接丢掉
    void Present(const qsc::stream::Frame &frame);

    void Clear();                                                                               // 断开连接时调：把还没渲染的帧丢掉

    // 渲染线程 synchronize() 里调：取走最新一帧，没有就返回无效帧
    qsc::stream::Frame TakePendingFrame(FrameTracePtr *trace_out, qint64 *arrive_us_out);
    QSize NotifiedFrameSize() const;                                                            // 已经报给渲染线程的帧尺寸
    qint64 AdvanceSyncPass();                                                                   // 记下本轮起点，返回上一轮起点（算两轮间隔用）
    qint64 SyncStartUs() const;                                                                 // 本轮 synchronize 的起始时刻
    void MarkRenderEnd(qint64 start_us, const FrameTracePtr &trace);                            // render() 画完时回报，算绘制耗时
    void MarkSwapped();                                                                         // 这一帧真正提交给系统了
    qint64 ClockUs() const { return clock_.nsecsElapsed() / 1000; }                             // 当前单调时刻（微秒），渲染线程算延迟用
    void SetPassArriveUs(qint64 arrive_us) { pass_arrive_us_.storeRelaxed(arrive_us); }         // 渲染线程：记下本轮取走那一帧的到达时刻
    void SetPassSyncUs(qint64 sync_us) { pass_sync_us_.storeRelaxed(sync_us); }                 // 渲染线程：记下本轮 synchronize 的开始时刻
    qint64 BeforeSyncUs() const { return before_sync_us_.loadRelaxed(); }                       // Qt 发出 beforeSynchronizing 的时刻

signals:
    void SigFrameSizeChanged();
    void SigKeepAspectChanged();
    void SigFpsChanged();
    void SigControlMessageReady(const QByteArray &message);                                     // 鼠标键盘翻好的控制消息，交给控制器发出去
    void SigPinchingChanged();                                                                  // 捏合开始或结束，界面用它显示双手状态
    void SigMapModeChanged();                                                                   // 映射模式变了，工具栏和叠加层跟着切
    void SigKeyMapChanged();                                                                    // 键位列表变了，叠加层重画

protected:
    void geometryChanged(const QRectF &new_geometry, const QRectF &old_geometry) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    void ReportSecond();
    bool PinchModifierHeld(Qt::KeyboardModifiers modifiers) const;                              // 当前修饰键是不是捏合键
    void PinchBy(int notches, const QPointF &pos);                                              // 按一格滚轮做一次捏合
    void EndPinch();                                                                             // 抬起两指，结束捏合
    void NotifyFrameSize();                                                                     // GUI 线程：帧尺寸变了才发信号
    void SyncMapper();                                                                          // 把画布尺寸、帧尺寸、缩放模式同步给坐标换算器
    void EmitControlMessage(const QByteArray &message);                                         // 空消息直接丢，非空才发信号

    qsc::input::CoordinateMapper mapper_;                                                       // 窗口坐标 → 视频帧坐标
    qsc::input::TouchController touch_{&mapper_};                                               // 鼠标 → 触摸/滚轮消息
    qsc::input::KeyController keys_;                                                            // 键盘 → 按键消息

    mutable QMutex mutex_;                                                                      // 视频线程写 pending_，渲染线程取走
    qsc::stream::Frame pending_;                                                                // 最新一帧，等渲染线程取走
    qint64 pending_arrive_us_ = 0;                                                              // 待渲染帧的到达时刻
    bool has_pending_ = false;
    FrameTracePtr pending_trace_;                                                               // 待渲染帧的事件记录
    FrameTracePtr swapped_trace_;                                                               // 刚提交完、等着打印的那一帧
    quint64 next_frame_id_ = 0;                                                                 // 帧序号自增，只在视频线程里改
    QSize frame_size_;                                                                          // 最近一帧的尺寸，旋转时会变
    QSize notified_size_;                                                                       // 已经通知给 QML 的尺寸
    std::atomic<bool> keep_aspect_{false};                                                      // true 保持宽高比居中，false 拉满

    QAtomicInt frames_arrived_;                                                                 // 视频线程每收一帧 +1
    QAtomicInt frames_swapped_{0};                                                              // 真正提交给系统的帧数
    QAtomicInt frames_dropped_{0};                                                              // 还没画就被新帧顶掉的帧数
    QAtomicInt lat_lt1_{0};                                                                     // 上屏延迟 < 1ms 的帧数
    QAtomicInt lat_1_2_{0};                                                                     // 1~2ms
    QAtomicInt lat_2_3_{0};                                                                     // 2~3ms
    QAtomicInt lat_gt3_{0};                                                                     // > 3ms
    QAtomicInt gap_lt85_{0};                                                                    // 到达间隔 < 8.5ms（约 120fps 以上）
    QAtomicInt gap_85_17_{0};                                                                   // 8.5~17ms（约 60fps）
    QAtomicInt gap_17_34_{0};                                                                   // 17~34ms（约 30fps）
    QAtomicInt gap_gt34_{0};                                                                    // > 34ms（低于 30fps）
    QAtomicInt swap_lt85_{0};                                                                   // 上屏间隔 < 8.5ms
    QAtomicInt swap_85_17_{0};                                                                  // 8.5~17ms
    QAtomicInt swap_17_34_{0};                                                                  // 17~34ms
    QAtomicInt swap_gt34_{0};                                                                   // > 34ms
    qint64 last_swap_us_ = 0;                                                                   // 上一帧上屏（frameSwapped）的时刻
    QAtomicInteger<qint64> lat_max_us_{0};                                                      // 这一秒最大上屏延迟
    QAtomicInteger<qint64> pass_arrive_us_{0};                                                  // 本轮取走那一帧的到达时刻
    QAtomicInteger<qint64> pass_sync_us_{0};                                                    // 本轮 synchronize 的开始时刻
    bool trace_enabled_ = false;                                                                // QS_TRACE_FRAMES=1 才逐帧追踪
    int fps_ = 0;                                                                               // 上一秒算出来的帧率
    int fps_last_arrived_ = 0;                                                                  // 上一秒结算时的累计帧数，用来算差值
    QElapsedTimer clock_;                                                                       // 进程启动后的单调时钟，用来算帧在管道里的时间
    QAtomicInteger<qint64> sync_start_us_{0};                                                   // 本轮 synchronize 的开始时刻
    QAtomicInteger<qint64> last_present_us_{0};                                                 // 上一帧到达（Present 入口）的时刻
    bool swap_hooked_ = false;                                                                  // frameSwapped 只接一次
    QAtomicInteger<qint64> before_sync_us_{0};                                                  // Qt 发出 beforeSynchronizing 的时刻（渲染线程）
    QTimer *fps_timer_ = nullptr;                                                               // 每秒触发一次，负责结算统计
    QTimer *pinch_watch_timer_ = nullptr;                                                        // 100ms 轮询修饰键，兜住松手时窗口没焦点
    QString pinch_modifier_ = QStringLiteral("ctrl");                                           // 当前捏合修饰键
    bool pinch_active_ = false;                                                                 // 两根手指是否按着
    QPointF pinch_center_;                                                                      // 捏合中心（画布坐标）
    int pinch_radius_ = 40;                                                                     // 两指间距的一半，单位帧像素，跨手势保留
    qsc::input::KeyMap key_map_;                                                                 // 当前设备的键位配置
    qsc::input::KeyMapEngine key_map_engine_;                                                    // 按键 → 触摸动作
    QString key_map_path_;                                                                       // config/keymap/<串号>.json
    int map_mode_ = 0;                                                                           // 0 关 / 1 玩 / 2 编辑
    qint64 pinch_last_us_ = 0;                                                                  // 上一次捏合的时刻，用来判断要不要重置半径
};
