#pragma once

#include <QAtomicInt>
#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QEvent>
#include <QMutex>
#include <QQuickRhiItem>
#include <QSize>

#include <atomic>
#include <memory>

#include "input/coordinate_mapper.h"
#include "input/key_controller.h"
#include <QSet>
#include <QVariantList>

#include "input/keymap.h"
#include "input/keymap_engine.h"
#include "input/touch_controller.h"
#include "stream/frame.h"


class QTimer;

#include "ui/frame_probe.h"

class VideoItem : public QQuickRhiItem
{
    Q_OBJECT
    Q_PROPERTY(QSize frameSize READ FrameSize NOTIFY SigFrameSizeChanged)
    Q_PROPERTY(bool keepAspect READ KeepAspect WRITE SetKeepAspect NOTIFY SigKeepAspectChanged)
    Q_PROPERTY(int fps READ Fps NOTIFY SigFpsChanged)

public:
    explicit VideoItem(QQuickItem *parent = nullptr);

    QQuickRhiItemRenderer *createRenderer() override;

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
    void SetSteerKeys(int index, const QString &up, const QString &down, const QString &left,
                      const QString &right);                                                     // 改摇杆四个方向键
    void SetClickerInterval(int ms);                                                             // 连点间隔，0 = 关闭
    int ClickerInterval() const { return clicker_interval_ms_; }
    void BeginKeyRecord(int index);                                                              // 进入录制：下一个按下的键绑到这个键位
    bool Recording() const { return record_index_ >= 0; }
    void BindRecordedKey(int qt_key);                                                            // QML 收到按键后调它完成绑定
    void SaveKeyMap();                                                                           // 写回 JSON，编辑退出时调

    // 视频线程调：把最新一帧放进来（引用计数，不拷像素），旧的直接丢掉
    void Present(const qsc::stream::Frame &frame);

    void Clear();                                                                               // 断开连接时调：把还没渲染的帧丢掉

    // 渲染线程 synchronize() 里调：取走最新一帧，没有就返回无效帧
    qsc::stream::Frame TakePendingFrame(FrameTracePtr *trace_out, qint64 *arrive_us_out);
    QSize NotifiedFrameSize() const;                                                            // 已经报给渲染线程的帧尺寸
    void BeginSyncPass() { probe_->BeginSyncPass(); }                                                // 一轮 synchronize 开始
    qint64 SyncStartUs() const { return probe_->SyncStartUs(); }
    void MarkRenderEnd(qint64 draw_start_us, const FrameTracePtr &trace) { probe_->OnRenderEnd(draw_start_us, trace); }
    void MarkSwapped() { probe_->OnSwapped(); }
    qint64 ClockUs() const { return probe_->NowUs(); }
    void SetPassArriveUs(qint64 arrive_us) { probe_->SetPassArrive(arrive_us); }
    void SetPassSyncUs(qint64 sync_us) { probe_->SetPassSync(sync_us); }

signals:
    void SigFrameSizeChanged();
    void SigKeepAspectChanged();
    void SigFpsChanged();
    void SigControlMessageReady(const QByteArray &message);                                     // 鼠标键盘翻好的控制消息，交给控制器发出去
    void SigPinchingChanged();                                                                  // 捏合开始或结束，界面用它显示双手状态
    void SigMapModeChanged();                                                                   // 映射模式变了，工具栏和叠加层跟着切
    void SigKeyMapChanged();                                                                    // 键位列表变了，叠加层重画
    void SigRecordingChanged();                                                                 // 进入或退出按键录制

protected:
    // Qt5 叫 geometryChanged，Qt6 改名成 geometryChange，参数含义没变
    void geometryChange(const QRectF &new_geometry, const QRectF &old_geometry) override;

    // 挂在窗口上的两个信号只在这里接一次：放到每帧的队列回调里会在 Qt6 下重入
    void componentComplete() override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    bool PinchModifierHeld(Qt::KeyboardModifiers modifiers) const;                              // 当前修饰键是不是捏合键
    void PinchBy(int notches, const QPointF &pos);                                              // 按一格滚轮做一次捏合
    void SlotKeyMapPoll();                                                                       // 轮询物理按键，状态变化才发触摸
    void DispatchKeyMapActions(const QVector<qsc::input::KeyMapAction> &actions);                // 把动作翻成控制消息
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
    QSize frame_size_;                                                                          // 最近一帧的尺寸，旋转时会变
    QSize notified_size_;                                                                       // 已经通知给 QML 的尺寸
    std::atomic<bool> keep_aspect_{false};                                                      // true 保持宽高比居中，false 拉满
    FrameProbe *probe_ = nullptr;                                                                // 埋点：帧序号、分档、每秒统计

    bool swap_hooked_ = false;                                                                  // frameSwapped 只接一次
    QTimer *fps_timer_ = nullptr;                                                               // 每秒触发一次，负责结算统计
    QTimer *keymap_tick_timer_ = nullptr;                                                        // 摇杆心跳：按住期间每 100ms 补一条 MOVE
    QTimer *pinch_watch_timer_ = nullptr;                                                        // 100ms 轮询修饰键，兜住松手时窗口没焦点
    QString pinch_modifier_ = QStringLiteral("ctrl");                                           // 当前捏合修饰键
    bool pinch_active_ = false;                                                                 // 两根手指是否按着
    QPointF pinch_center_;                                                                      // 捏合中心（画布坐标）
    int pinch_radius_ = 40;                                                                     // 两指间距的一半，单位帧像素，跨手势保留
    qsc::input::KeyMap key_map_;                                                                 // 当前设备的键位配置
    qsc::input::KeyMapEngine key_map_engine_;                                                    // 按键 → 触摸动作
    QString key_map_path_;                                                                       // config/keymap/<串号>.json
    QTimer *clicker_timer_ = nullptr;                                                            // 连点：按住空格时按间隔在鼠标位置连点
    int clicker_interval_ms_ = 0;                                                                // 0 = 关闭
    qint64 clicker_next_us_ = 0;                                                                 // 下一次该点的时刻
    QPointF clicker_pos_;                                                                        // 最近一次算出的连点位置（归一化）
    int record_index_ = -1;                                                                      // 正在录制的键位下标，-1 表示没在录
    int map_mode_ = 0;                                                                           // 0 关 / 1 玩 / 2 编辑
    QSet<QString> polled_keys_;                                                                  // 轮询到的"当前按着"的键名
    QTimer *keymap_poll_timer_ = nullptr;                                                        // 16ms 轮询物理按键，绕开 Qt 的重复事件
    qint64 pinch_last_us_ = 0;                                                                  // 上一次捏合的时刻，用来判断要不要重置半径
};
