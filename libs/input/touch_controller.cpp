#include "input/touch_controller.h"

#include "protocol/control_message.h"

namespace qsc::input
{

namespace
{

constexpr quint8 kActionDown = 0;                                                               // 对应安卓 MotionEvent.ACTION_DOWN
constexpr quint8 kActionUp = 1;                                                                 // ACTION_UP
constexpr quint8 kActionMove = 2;                                                               // ACTION_MOVE
constexpr quint64 kMousePointerId = 0xFFFFFFFFFFFFFFFFULL;                                      // 鼠标固定用这个 id，多指才换
constexpr quint32 kButtonPrimary = 1;                                                           // 安卓 MOUSE_BUTTON_PRIMARY

// 协议层把归一化值除以 16 转定点，所以这里一格滚轮按 16 个单位送
constexpr float kScrollUnitsPerNotch = 16.0f;

} // namespace

TouchController::TouchController(const CoordinateMapper *mapper) : mapper_(mapper) {}

// 左键按下：换算坐标后发触摸按下
QByteArray TouchController::Press(const QPointF &pos)
{
    pressed_ = true;
    return MakeTouch(kActionDown, pos);
}

// 按住拖动：发触摸移动
QByteArray TouchController::Move(const QPointF &pos)
{
    return MakeTouch(kActionMove, pos);
}

// 左键抬起：压力归零、清按键状态
QByteArray TouchController::Release(const QPointF &pos)
{
    pressed_ = false;
    return MakeTouch(kActionUp, pos);
}

// 滚轮：正数表示往上滚
QByteArray TouchController::Scroll(const QPointF &pos, int notches)
{
    if (mapper_ == nullptr)
    {
        return QByteArray();
    }

    bool inside = false;
    const QPoint frame_pos = mapper_->Map(pos, &inside);
    if (!inside)
    {
        return QByteArray();
    }

    // 滚轮往上拨时协议里的 v_scroll 是负数，和安卓 AXIS_VSCROLL 的方向一致
    protocol::ScrollEvent event;
    event.position_ = frame_pos;
    event.screen_size_ = mapper_->FrameSize();
    event.v_scroll_ = -static_cast<float>(notches) * kScrollUnitsPerNotch;
    event.buttons_ = pressed_ ? kButtonPrimary : 0;
    return protocol::ControlMessage::InjectScroll(event);
}

// 填一次触摸事件的所有字段，三种动作只是 action 和按键状态不同
QByteArray TouchController::MakeTouch(quint8 action, const QPointF &pos)
{
    if (mapper_ == nullptr)
    {
        return QByteArray();
    }

    bool inside = false;
    const QPoint frame_pos = mapper_->Map(pos, &inside);
    if (!inside)
    {
        return QByteArray();
    }

    protocol::TouchEvent event;
    event.action_ = action;
    event.pointer_id_ = kMousePointerId;
    event.position_ = frame_pos;
    event.screen_size_ = mapper_->FrameSize();
    event.pressure_ = action == kActionUp ? 0.0f : 1.0f;
    event.action_buttons_ = kButtonPrimary;
    event.buttons_ = action == kActionUp ? 0 : kButtonPrimary;
    return protocol::ControlMessage::InjectTouch(event);
}

} // namespace qsc::input
