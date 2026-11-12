#include "input/key_controller.h"

#include "input/keycode_table.h"
#include "protocol/control_message.h"

namespace qsc::input
{

namespace
{

constexpr quint32 kActionDown = 0;                                                              // 对应安卓 KeyEvent.ACTION_DOWN
constexpr quint32 kActionUp = 1;                                                                // ACTION_UP

} // namespace

// 按下：第一次 repeat 是 0，系统自动重复时每次加一
QByteArray KeyController::Press(int qt_key, Qt::KeyboardModifiers modifiers, bool auto_repeat)
{
    const quint32 keycode = AndroidKeycode(qt_key);
    if (keycode == 0)
    {
        return QByteArray();
    }

    int repeat = 0;
    if (auto_repeat)
    {
        repeat = repeats_.value(qt_key, 0) + 1;
    }
    repeats_.insert(qt_key, repeat);

    protocol::KeycodeEvent event;
    event.action_ = kActionDown;
    event.keycode_ = keycode;
    event.repeat_ = static_cast<quint32>(repeat);
    event.meta_state_ = AndroidMetaState(modifiers);
    return protocol::ControlMessage::InjectKeycode(event);
}

// 抬起：顺手把重复计数清掉，下次按下又是第 0 次
QByteArray KeyController::Release(int qt_key, Qt::KeyboardModifiers modifiers)
{
    const quint32 keycode = AndroidKeycode(qt_key);
    if (keycode == 0)
    {
        return QByteArray();
    }

    repeats_.remove(qt_key);

    protocol::KeycodeEvent event;
    event.action_ = kActionUp;
    event.keycode_ = keycode;
    event.repeat_ = 0;
    event.meta_state_ = AndroidMetaState(modifiers);
    return protocol::ControlMessage::InjectKeycode(event);
}

// 清掉所有按键的重复计数
void KeyController::Reset()
{
    repeats_.clear();
}

} // namespace qsc::input
