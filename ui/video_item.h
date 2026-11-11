#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QQuickItem>
#include <QSize>

#include "stream/frame.h"
#include "stream/yuv_convert.h"

// QML 里显示一路视频的画布：收帧、转 RGB32、交给一张纹理上屏。
// keepAspect 打开时按帧的宽高比居中显示（两边留黑），关掉就拉满整块画布。
class VideoItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QSize frameSize READ FrameSize NOTIFY FrameSizeChanged)
    Q_PROPERTY(bool keepAspect READ KeepAspect WRITE SetKeepAspect NOTIFY KeepAspectChanged)
    Q_PROPERTY(int fps READ Fps NOTIFY FpsChanged)

public:
    explicit VideoItem(QQuickItem *parent = nullptr);

    QSize FrameSize() const;    // 最近一帧的尺寸，QML 里用来显示和算宽高比
    bool KeepAspect() const;
    int Fps() const;

    void SetKeepAspect(bool keep_aspect);
    void Present(const qsc::stream::Frame &frame);    // 主线程调：转好后排队等渲染

signals:
    void FrameSizeChanged();
    void KeepAspectChanged();
    void FpsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *data) override;
    void geometryChanged(const QRectF &new_geometry, const QRectF &old_geometry) override;

private:
    QRectF TargetRect() const;    // 按 keepAspect 算出画面应该铺在哪块矩形里

    mutable QMutex mutex_;    // 主线程写 image_、渲染线程读；FrameSize 是 const 方法也要加锁
    QImage image_;    // 已经转好的 RGB32 画面，等渲染线程取走
    bool has_image_ = false;
    QSize frame_size_;    // 最近一帧的尺寸，旋转时会变
    bool keep_aspect_ = false;    // 默认拉满窗口；窗口本身会按帧比例调，所以不会变形
    qsc::stream::YuvConvert convert_;    // 只在主线程用，不用加锁

    QElapsedTimer fps_timer_;    // 统计窗口的起点
    int frames_in_window_ = 0;    // 这个统计窗口里收了多少帧
    int fps_ = 0;    // 上一秒算出来的帧率
};
