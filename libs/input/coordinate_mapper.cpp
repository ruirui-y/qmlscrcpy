#include "input/coordinate_mapper.h"

namespace qsc::input
{

// 换算一个点：先减掉黑边偏移，再除以"每个帧像素占多少画布像素"，最后夹在边界内
QPoint CoordinateMapper::Map(const QPointF &item_pos, bool *inside) const
{
    if (frame_size_.isEmpty() || item_size_.isEmpty())
    {
        *inside = false;
        return QPoint();
    }

    // fit_x = 一个帧像素在画布上占几个像素；拉满模式下两个方向各自算
    const qreal fit_x = item_size_.width() / frame_size_.width();
    const qreal fit_y = item_size_.height() / frame_size_.height();
    qreal scale_x = fit_x;
    qreal scale_y = fit_y;

    // 等比模式：取较小的那个，画面整体缩到塞得下，多出来的居中留黑
    if (keep_aspect_)
    {
        const qreal scale = qMin(fit_x, fit_y);
        scale_x = scale;
        scale_y = scale;
    }

    const qreal offset_x = (item_size_.width() - frame_size_.width() * scale_x) / 2.0;
    const qreal offset_y = (item_size_.height() - frame_size_.height() * scale_y) / 2.0;
    const qreal local_x = item_pos.x() - offset_x;
    const qreal local_y = item_pos.y() - offset_y;
    const qreal visible_width = frame_size_.width() * scale_x;
    const qreal visible_height = frame_size_.height() * scale_y;

    const bool hit = local_x >= 0.0 && local_y >= 0.0 && local_x < visible_width && local_y < visible_height;
    *inside = hit;
    if (!hit)
    {
        return QPoint();
    }

    // 贴着右下边缘时算出来可能正好等于宽高，这里夹到 width-1 / height-1
    const int x = qBound(0, static_cast<int>(local_x / scale_x), frame_size_.width() - 1);
    const int y = qBound(0, static_cast<int>(local_y / scale_y), frame_size_.height() - 1);
    return QPoint(x, y);
}

} // namespace qsc::input
