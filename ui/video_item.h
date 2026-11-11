#pragma once

#include <QImage>
#include <QMutex>
#include <QQuickItem>
#include <QSize>

#include "stream/frame.h"
#include "stream/yuv_convert.h"

// QML 里显示一路视频的画布：收帧、转 RGB32、交给一张纹理上屏。
class VideoItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QSize frameSize READ FrameSize NOTIFY FrameSizeChanged)

public:
    explicit VideoItem(QQuickItem *parent = nullptr);

    QSize FrameSize() const;                                                                    // 最近一帧的尺寸，QML 里用来显示和算宽高比
    void Present(const qsc::stream::Frame &frame);                                              // 主线程调：转好后排队等渲染

signals:
    void FrameSizeChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *data) override;

private:
    mutable QMutex mutex_;                                                                      // 主线程写 image_、渲染线程读；FrameSize 是 const 方法也要加锁
    QImage image_;                                                                              // 已经转好的 RGB32 画面，等渲染线程取走
    bool has_image_ = false;
    QSize frame_size_;                                                                          // 最近一帧的尺寸，旋转时会变
    qsc::stream::YuvConvert convert_;                                                           // 只在主线程用，不用加锁
};
