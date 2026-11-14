#include "input/touch_controller.h"

#include "protocol/control_message.h"

namespace qsc::input
{

namespace
{

constexpr quint8 kActionDown = 0;                                                               // 对应安卓 MotionEvent.ACTION_DOWN
constexpr quint8 kActionUp = 1;                                                                 // ACTION_UP
constexpr quint8 kActionMove = 2;                                                               // ACTION_MOVE
constexpr quint8 kActionPointerDown = 5;                                                        // ACTION_POINTER_DOWN，第二根手指按下
constexpr quint8 kActionPointerUp = 6;                                                          // ACTION_POINTER_UP，还有手指按着时抬起
constexpr quint64 kMousePointerId = 0xFFFFFFFFFFFFFFFFULL;                                      // 鼠标固定用这个 id，多指才换
constexpr quint64 kPinchPointerA = 1;                                                            // 捏合第一根手指
constexpr quint64 kPinchPointerB = 2;                                                            // 捏合第二根手指
constexpr quint32 kButtonPrimary = 1;                                                           // 安卓 MOUSE_BUTTON_PRIMARY

// 协议层把归一化值除以 16 转定点，所以这里一格滚轮按 16 个单位送
constexpr float kScrollUnitsPerNotch = 16.0f;


} // namespace

TouchController::TouchController(const CoordinateMapper *mapper) : mapper_(mapper) {}

// 左键按下：标记按住状态，发触摸按下
QByteArray TouchController::Press(const QPointF &pos)
{
    pressed_ = true;

    // 记住按下点：拖拽按"起点 + 位移×倍率"算，避免每帧乘一次导致误差累积
    if (mapper_ != nullptr)
    {
        bool inside = false;
        press_frame_pos_ = mapper_->Map(pos, &inside);
    }
    return MakeTouch(kActionDown, pos);
}

// 按住拖动：发触摸移动，倍率不为 1 时按缩放后的位置发
QByteArray TouchController::Move(const QPointF &pos)
{
    if (mapper_ == nullptr || qFuzzyCompare(drag_scale_, 1.0))
    {
        return MakeTouch(kActionMove, pos);
    }

    bool inside = false;
    const QPoint raw = mapper_->Map(pos, &inside);
    if (!inside)
    {
        return MakeTouch(kActionMove, pos);
    }

    // 鼠标位移乘倍率后叠到按下点，再夹进画面：越界的坐标设备会整条丢掉
    const QSize screen = mapper_->FrameSize();
    const int x = press_frame_pos_.x() + qRound((raw.x() - press_frame_pos_.x()) * drag_scale_);
    const int y = press_frame_pos_.y() + qRound((raw.y() - press_frame_pos_.y()) * drag_scale_);
    const QPoint scaled(qBound(0, x, screen.width() - 1), qBound(0, y, screen.height() - 1));
    return MakeTouchAt(kMousePointerId, kActionMove, scaled);
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

// 已经换算好的帧坐标直接组包：拖拽倍率的位移走这里
QByteArray TouchController::MakeTouchAt(quint64 pointer_id, quint8 action, const QPoint &frame_pos)
{
    if (mapper_ == nullptr)
    {
        return QByteArray();
    }

    protocol::TouchEvent event;
    event.action_ = action;
    event.pointer_id_ = pointer_id;
    event.position_ = frame_pos;
    event.screen_size_ = mapper_->FrameSize();
    event.pressure_ = action == kActionUp ? 0.0f : 1.0f;
    event.action_buttons_ = (action == kActionUp || action == kActionDown) ? kButtonPrimary : 0;
    event.buttons_ = kButtonPrimary;
    return protocol::ControlMessage::InjectTouch(event);
}

// 捏合：两根手指以 center 为对称点、相距 2*radius；finger1 在左，finger2 在右
QVector<QByteArray> TouchController::PinchBegin(const QPointF &center, int radius)
{
    QVector<QByteArray> messages;
    if (mapper_ == nullptr)
    {
        return messages;
    }

    bool inside = false;
    const QPoint mapped = mapper_->Map(center, &inside);
    if (!inside)
    {
        return messages;
    }

    const QPoint left = ClampFrame(mapped.x() - radius, mapped.y());
    const QPoint right = ClampFrame(mapped.x() + radius, mapped.y());
    messages.append(MakeTouchAt(kPinchPointerA, kActionDown, left));
    messages.append(MakeTouchAt(kPinchPointerB, kActionPointerDown, right));                       // 第二根手指必须用 POINTER_DOWN，否则会被当成新手势
    return messages;
}

// 捏合移动：只挪两根手指，不重新按
QVector<QByteArray> TouchController::PinchStep(const QPointF &center, int radius)
{
    QVector<QByteArray> messages;
    if (mapper_ == nullptr)
    {
        return messages;
    }

    bool inside = false;
    const QPoint mapped = mapper_->Map(center, &inside);
    if (!inside)
    {
        return messages;
    }

    const QPoint left = ClampFrame(mapped.x() - radius, mapped.y());
    const QPoint right = ClampFrame(mapped.x() + radius, mapped.y());
    messages.append(MakeTouchAt(kPinchPointerA, kActionMove, left));
    messages.append(MakeTouchAt(kPinchPointerB, kActionMove, right));
    return messages;
}

// 捏合收尾：两根手指必须都抬起，否则设备会一直停在按住状态
QVector<QByteArray> TouchController::PinchEnd(const QPointF &center, int radius)
{
    QVector<QByteArray> messages;
    if (mapper_ == nullptr)
    {
        return messages;
    }

    bool inside = false;
    const QPoint mapped = mapper_->Map(center, &inside);
    if (!inside)
    {
        return messages;
    }

    const QPoint left = ClampFrame(mapped.x() - radius, mapped.y());
    const QPoint right = ClampFrame(mapped.x() + radius, mapped.y());
    messages.append(MakeTouchAt(kPinchPointerB, kActionPointerUp, right));                         // 先抬第二根，用 POINTER_UP
    messages.append(MakeTouchAt(kPinchPointerA, kActionUp, left));
    return messages;
}

// 键位映射入口：把 0~1 坐标换算成帧像素，再交给 MakeTouchAt
QByteArray TouchController::NormalizedTouch(quint64 pointer_id, quint8 action, const QPointF &normalized)
{
    if (mapper_ == nullptr)
    {
        return QByteArray();
    }

    const QSize screen = mapper_->FrameSize();
    const int x = qBound(0, qRound(normalized.x() * screen.width()), screen.width() - 1);
    const int y = qBound(0, qRound(normalized.y() * screen.height()), screen.height() - 1);
    return MakeTouchAt(pointer_id, action, QPoint(x, y));
}
// 把帧坐标夹进画面，越界的点设备会整条丢掉
QPoint TouchController::ClampFrame(int x, int y) const
{
    const QSize screen = mapper_->FrameSize();
    return QPoint(qBound(0, x, screen.width() - 1), qBound(0, y, screen.height() - 1));
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
