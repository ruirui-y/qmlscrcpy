#pragma once

#include <QAtomicInt>
#include <QImage>
#include <QMutex>
#include <QQuickItem>
#include <QSize>

#include <atomic>

class QTimer;

// QML 里显示一路视频的画布：收图、交给一张纹理上屏。
// keepAspect 打开时按帧的宽高比居中显示（两边留黑），关掉就拉满整块画布。
//
// 视频线程只往这里丢图（加锁存最新一张），**不投递任何跨线程事件**：
// 拖窗口时 GUI 线程卡在系统模态循环里，投递排队调用会把视频线程一起堵住。
// 重绘由 GUI 线程自己的定时器发起。
class VideoItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QSize frameSize READ FrameSize NOTIFY FrameSizeChanged)
    Q_PROPERTY(bool keepAspect READ KeepAspect WRITE SetKeepAspect NOTIFY KeepAspectChanged)
    Q_PROPERTY(int fps READ Fps NOTIFY FpsChanged)

public:
    explicit VideoItem(QQuickItem *parent = nullptr);

    QSize FrameSize() const;                                                                    // 最近一帧的尺寸，QML 里用来显示和算宽高比
    bool KeepAspect() const;
    int Fps() const;

    void SetKeepAspect(bool keep_aspect);

    // 视频线程调：把最新一张图放进来，旧的直接丢掉，不排队也不补帧
    void Present(const QImage &image);

    void Clear();                                                                               // 断开连接时调：把还没渲染的图丢掉

signals:
    void FrameSizeChanged();
    void KeepAspectChanged();
    void FpsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *data) override;
    void geometryChanged(const QRectF &new_geometry, const QRectF &old_geometry) override;

private:
    void OnRefreshTimer();                                                                      // GUI 线程：有新图就请重绘，尺寸变了就通知 QML
    void UpdateFps();                                                                           // GUI 线程：每秒结算一次帧率
    QRectF TargetRect() const;                                                                  // 按 keepAspect 算出画面应该铺在哪块矩形里

    mutable QMutex mutex_;                                                                      // 视频线程写 image_，GUI/渲染线程读
    QImage image_;                                                                              // 已经转好的 RGB32 画面，等渲染线程取走
    bool has_image_ = false;
    QSize frame_size_;                                                                          // 最近一帧的尺寸，旋转时会变
    QSize notified_size_;                                                                       // 已经通知给 QML 的尺寸
    std::atomic<bool> keep_aspect_{false};                                                      // true 保持宽高比居中，false 拉满

    QAtomicInt frames_arrived_;                                                                 // 视频线程每收一帧 +1
    int frames_painted_ = 0;                                                                    // GUI 线程已经画到第几帧
    int fps_ = 0;                                                                               // 上一秒算出来的帧率
    QTimer *refresh_timer_ = nullptr;                                                           // 8ms 一次，把重绘请求从视频线程手里接管过来
    QTimer *fps_timer_ = nullptr;                                                               // 每秒触发一次，负责结算帧率
};
