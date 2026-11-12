#pragma once

#include <QAtomicInt>
#include <QMutex>
#include <QQuickFramebufferObject>
#include <QSize>

#include <atomic>

#include "input/coordinate_mapper.h"
#include "input/key_controller.h"
#include "input/touch_controller.h"
#include "stream/frame.h"

class QTimer;

// QML 里显示一路视频的画布：三张 YUV 平面纹理，着色器在 GPU 上合成 RGB。
// keepAspect 打开时按帧的宽高比居中显示（两边留黑），关掉就拉满整块画布。
//
// 用 QQuickFramebufferObject 而不是自定义 QSGNode：它的 Renderer::render() 是 Qt
// 保证"渲染线程 + GL 上下文当前"的唯一入口，建纹理、传纹理都在这儿做，
// 不会像之前那样栽在 updatePaintNode / 材质里调 GL 的时机上。
//
// 视频线程只往这里丢帧（加锁存最新一帧），不投递任何跨线程事件；
// 重绘由 GUI 线程自己的定时器发起。
class VideoItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(QSize frameSize READ FrameSize NOTIFY FrameSizeChanged)
    Q_PROPERTY(bool keepAspect READ KeepAspect WRITE SetKeepAspect NOTIFY KeepAspectChanged)
    Q_PROPERTY(int fps READ Fps NOTIFY FpsChanged)

public:
    explicit VideoItem(QQuickItem *parent = nullptr);

    Renderer *createRenderer() const override;

    QSize FrameSize() const;                                                                    // 最近一帧的尺寸，QML 里用来显示和算宽高比
    bool KeepAspect() const;
    int Fps() const;

    void SetKeepAspect(bool keep_aspect);
    void SetWheelMode(const QString &mode);                                                     // "scroll" 发滚动事件，"drag" 模拟滑动

    // 视频线程调：把最新一帧放进来（引用计数，不拷像素），旧的直接丢掉
    void Present(const qsc::stream::Frame &frame);

    void Clear();                                                                               // 断开连接时调：把还没渲染的帧丢掉

    // 渲染线程 synchronize() 里调：取走最新一帧，没有就返回无效帧
    qsc::stream::Frame TakePendingFrame();
    QSize NotifiedFrameSize() const;                                                            // 已经报给渲染线程的帧尺寸

signals:
    void FrameSizeChanged();
    void KeepAspectChanged();
    void FpsChanged();
    void ControlMessageReady(const QByteArray &message);                                        // 鼠标键盘翻好的控制消息，交给控制器发出去

protected:
    void geometryChanged(const QRectF &new_geometry, const QRectF &old_geometry) override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    void OnRefreshTimer();                                                                      // GUI 线程：有新帧就请重绘，尺寸变了就通知 QML
    void UpdateFps();                                                                           // GUI 线程：每秒结算一次帧率
    void SyncMapper();                                                                          // 把画布尺寸、帧尺寸、缩放模式同步给坐标换算器
    void EmitControlMessage(const QByteArray &message);                                                 // 空消息直接丢，非空才发信号

    qsc::input::CoordinateMapper mapper_;                                                       // 窗口坐标 → 视频帧坐标
    qsc::input::TouchController touch_{&mapper_};                                               // 鼠标 → 触摸/滚轮消息
    qsc::input::KeyController keys_;                                                            // 键盘 → 按键消息

    mutable QMutex mutex_;                                                                      // 视频线程写 pending_，渲染线程取走
    qsc::stream::Frame pending_;                                                                // 最新一帧，等渲染线程取走
    bool has_pending_ = false;
    QSize frame_size_;                                                                          // 最近一帧的尺寸，旋转时会变
    QSize notified_size_;                                                                       // 已经通知给 QML 的尺寸
    std::atomic<bool> keep_aspect_{false};                                                      // true 保持宽高比居中，false 拉满

    QAtomicInt frames_arrived_;                                                                 // 视频线程每收一帧 +1
    int frames_painted_ = 0;                                                                    // GUI 线程已经画到第几帧
    int fps_ = 0;                                                                               // 上一秒算出来的帧率
    QTimer *refresh_timer_ = nullptr;                                                           // 8ms 一次，把重绘请求从视频线程手里接管过来
    QTimer *fps_timer_ = nullptr;                                                               // 每秒触发一次，负责结算帧率
};
