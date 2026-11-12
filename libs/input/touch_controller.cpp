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

// 左键按下：标记按住状态，发触摸按下
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

// 左键抬起：清按住状态、压力归零
QByteArray TouchController::Release(const QPointF &pos)
{
    pressed_ = false;
    return MakeTouch(kActionUp, pos);
}

// 滚轮入口：按当前模式分派，两种模式返回的消息条数不一样
QVector<QByteArray> TouchController::Scroll(const QPointF &pos, int notches)
{
    QVector<QByteArray> messages;
    if (notches == 0)
    {
        return messages;
    }

    if (wheel_mode_ == WheelMode::Drag)
    {
        return ScrollByDrag(pos, notches);
    }

    const QByteArray message = MakeScroll(pos, notches);
    if (!message.isEmpty())
    {
        messages.append(message);
    }
    return messages;
}

// 原生滚动事件：微信这类列表认这个，最省流量
QByteArray TouchController::MakeScroll(const QPointF &pos, int notches)
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

    // 实测：安卓收到负值反而是往下滚，所以往上拨给正值
    protocol::ScrollEvent event;
    event.position_ = frame_pos;
    event.screen_size_ = mapper_->FrameSize();
    event.v_scroll_ = static_cast<float>(notches) * kScrollUnitsPerNotch;
    event.buttons_ = pressed_ ? kButtonPrimary : 0;
    return protocol::ControlMessage::InjectScroll(event);
}

// 模拟手指拖动：阅读器这类只认拖动的应用靠它。
// 往上滚一格 = 手指往下拖 drag_pixels_ 像素（内容跟着手指走，视觉上就是往上翻）
QVector<QByteArray> TouchController::ScrollByDrag(const QPointF &pos, int notches)
{
    QVector<QByteArray> messages;
    if (mapper_ == nullptr)
    {
        return messages;
    }

    bool inside = false;
    const QPoint frame_pos = mapper_->Map(pos, &inside);
    if (!inside)
    {
        return messages;
    }

    // 位置按帧坐标算位移，避免窗口缩放时手感变来变去
    const int delta = (notches > 0 ? 1 : -1) * drag_pixels_ * qAbs(notches);
    const QPoint start = frame_pos;
    const QPoint end = QPoint(frame_pos.x(), frame_pos.y() + delta);
    const QSize screen = mapper_->FrameSize();

    const quint32 clamped_end_y = static_cast<quint32>(qBound(0, end.y(), screen.height() - 1));

    // 三条消息：按下、移动、抬起；action_buttons 只在按下和抬起时报主键
    protocol::TouchEvent down;
    down.action_ = kActionDown;
    down.pointer_id_ = kMousePointerId;
    down.position_ = start;
    down.screen_size_ = screen;
    down.pressure_ = 1.0f;
    down.action_buttons_ = kButtonPrimary;
    down.buttons_ = kButtonPrimary;

    protocol::TouchEvent move = down;
    move.action_ = kActionMove;
    move.position_ = QPoint(start.x(), static_cast<int>(clamped_end_y));
    move.action_buttons_ = 0;

    protocol::TouchEvent up = move;
    up.action_ = kActionUp;
    up.pressure_ = 0.0f;
    up.action_buttons_ = kButtonPrimary;
    up.buttons_ = 0;

    messages.append(protocol::ControlMessage::InjectTouch(down));
    messages.append(protocol::ControlMessage::InjectTouch(move));
    messages.append(protocol::ControlMessage::InjectTouch(up));
    return messages;
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
