#pragma once

#include <QByteArray>
#include <QPointF>

#include "input/coordinate_mapper.h"

namespace qsc::input
{

// 把鼠标动作翻成 scrcpy 的触摸/滚轮控制消息。
// 坐标先经 CoordinateMapper 换算成帧坐标；点落在黑边上时不发消息，免得点到画面外面去。
class TouchController
{
public:
    explicit TouchController(const CoordinateMapper *mapper);

    QByteArray Press(const QPointF &pos);                                                       // 左键按下
    QByteArray Move(const QPointF &pos);                                                        // 按住拖动
    QByteArray Release(const QPointF &pos);                                                     // 左键抬起
    QByteArray Scroll(const QPointF &pos, int notches);                                         // 滚轮，正数表示往上滚

private:
    QByteArray MakeTouch(quint8 action, const QPointF &pos);                                    // 三种动作共用的一套字段填充

    const CoordinateMapper *mapper_;                                                            // 不持有，生命周期归调用方
    bool pressed_ = false;                                                                      // 当前是否按着左键，决定 buttons_ 报什么
};

} // namespace qsc::input
