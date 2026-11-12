#pragma once

#include <QByteArray>
#include <QPointF>
#include <QVector>

#include "input/coordinate_mapper.h"

namespace qsc::input
{

// 滚轮两种发法：滚动事件（原生列表最省）和模拟手指拖动（阅读器只认这个）
enum class WheelMode
{
    Scroll = 0,                                                                                 // 发 INJECT_SCROLL
    Drag = 1,                                                                                   // 发按下、移动、抬起三条触摸
};

// 把鼠标动作翻成 scrcpy 的触摸/滚轮控制消息。
// 坐标先经 CoordinateMapper 换算成帧坐标；点落在黑边上时不发消息，免得点到画面外面去。
class TouchController
{
public:
    explicit TouchController(const CoordinateMapper *mapper);

    void SetWheelMode(WheelMode mode) { wheel_mode_ = mode; }
    WheelMode Mode() const { return wheel_mode_; }

    void SetDragPixels(int pixels) { drag_pixels_ = pixels > 0 ? pixels : 60; }                 // 拖动模式一格滚轮拖多远
    int DragPixels() const { return drag_pixels_; }

    QByteArray Press(const QPointF &pos);                                                       // 左键按下
    QByteArray Move(const QPointF &pos);                                                        // 按住拖动
    QByteArray Release(const QPointF &pos);                                                     // 左键抬起

    // 滚轮：正数表示往上滚；拖动模式会返回三条消息，所以用容器
    QVector<QByteArray> Scroll(const QPointF &pos, int notches);

private:
    QByteArray MakeTouch(quint8 action, const QPointF &pos);                                    // 三种动作共用的一套字段填充
    QVector<QByteArray> ScrollByDrag(const QPointF &pos, int notches);                          // 模拟一次短距离滑动
    QByteArray MakeScroll(const QPointF &pos, int notches);                                     // 发原生滚动事件

    const CoordinateMapper *mapper_;                                                            // 不持有，生命周期归调用方
    bool pressed_ = false;                                                                      // 当前是否按着左键，决定 buttons_ 报什么
    WheelMode wheel_mode_ = WheelMode::Scroll;                                                  // 默认发滚动事件，和 scrcpy 客户端一致
    int drag_pixels_ = 60;                                                                      // 拖动模式下一格滚轮手指移动多少像素
};

} // namespace qsc::input
