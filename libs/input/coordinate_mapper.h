#pragma once

#include <QPoint>
#include <QPointF>
#include <QSize>
#include <QSizeF>

namespace qsc::input
{

// 把 QML 画布上的坐标换算成"视频帧坐标系"的像素，scrcpy 只认这套坐标。
// 两种模式：拉满（宽高各自缩放）和等比（居中留黑边）。
class CoordinateMapper
{
public:
    void SetFrameSize(QSize frame_size) { frame_size_ = frame_size; }                            // 视频帧尺寸，设备旋转时会变
    void SetItemSize(QSizeF item_size) { item_size_ = item_size; }                               // QML 画布尺寸
    void SetKeepAspect(bool keep_aspect) { keep_aspect_ = keep_aspect; }                         // true 等比居中，false 拉满

    // inside 表示点是否落在画面里；落在黑边上时坐标无意义，返回 (0, 0)
    QPoint Map(const QPointF &item_pos, bool *inside) const;

    QSize FrameSize() const { return frame_size_; }

private:
    QSize frame_size_;
    QSizeF item_size_;
    bool keep_aspect_ = false;
};

} // namespace qsc::input
