#include "protocol/control_message.h"

#include "protocol/wire.h"

namespace qsc::protocol
{
namespace
{

// 位置 = 4 字节 x + 4 字节 y + 2 字节宽 + 2 字节高，单位都是像素
void AppendPosition(QByteArray &out, const QPoint &position, const QSize &screen_size)
{
    const quint32 x = static_cast<quint32>(qMax(0, position.x()));
    const quint32 y = static_cast<quint32>(qMax(0, position.y()));
    const quint16 width = static_cast<quint16>(qBound(0, screen_size.width(), 0xFFFF));
    const quint16 height = static_cast<quint16>(qBound(0, screen_size.height(), 0xFFFF));
    AppendU32(out, x);
    AppendU32(out, y);
    AppendU16(out, width);
    AppendU16(out, height);
}

// 0..1 映射成 16 位无符号定点，设备端除以 65535 还原
quint16 ToU16Fixed(float value)
{
    const float clamped = qBound(0.0f, value, 1.0f);
    const quint32 fixed = static_cast<quint32>(clamped * 65536.0f);
    return static_cast<quint16>(qMin<quint32>(fixed, 0xFFFF));
}

// -1..1 映射成 16 位有符号定点，设备端除以 32768 还原
qint16 ToI16Fixed(float value)
{
    const float clamped = qBound(-1.0f, value, 1.0f);
    qint32 fixed = static_cast<qint32>(clamped * 32768.0f);
    // 1.0 * 32768 会溢出到 0x8000，得按正数上限截回去
    fixed = qBound<qint32>(-0x8000, fixed, 0x7FFF);
    return static_cast<qint16>(fixed);
}

// 按字节数截断 UTF-8，并且不能把一个多字节字符劈成两半。
// 上一版按 QString::length()（字符数）截断、却按字节数写长度，中文一次就超限。
QByteArray TruncateUtf8(const QString &text, int max_bytes)
{
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= max_bytes)
    {
        return utf8;
    }

    int cut = max_bytes;
    // UTF-8 的续字节都是 10xxxxxx，往回退到字符起始字节
    while (cut > 0 && (static_cast<quint8>(utf8.at(cut)) & 0xC0) == 0x80)
    {
        --cut;
    }
    return utf8.left(cut);
}

QByteArray MakeHeader(ControlType type)
{
    QByteArray out;
    AppendU8(out, static_cast<quint8>(type));
    return out;
}

} // namespace

QByteArray ControlMessage::InjectKeycode(const KeycodeEvent &event)
{
    QByteArray out = MakeHeader(ControlType::InjectKeycode);
    AppendU8(out, static_cast<quint8>(event.action_));
    AppendU32(out, event.keycode_);
    AppendU32(out, event.repeat_);
    AppendU32(out, event.meta_state_);
    return out;
}

QByteArray ControlMessage::InjectText(const QString &text)
{
    const QByteArray utf8 = TruncateUtf8(text, kMaxInjectTextBytes);

    QByteArray out = MakeHeader(ControlType::InjectText);
    AppendU32(out, static_cast<quint32>(utf8.size()));
    out.append(utf8);
    return out;
}

QByteArray ControlMessage::InjectTouch(const TouchEvent &event)
{
    QByteArray out = MakeHeader(ControlType::InjectTouch);
    AppendU8(out, event.action_);
    AppendU64(out, event.pointer_id_);
    AppendPosition(out, event.position_, event.screen_size_);
    AppendU16(out, ToU16Fixed(event.pressure_));
    AppendU32(out, event.action_buttons_);
    AppendU32(out, event.buttons_);
    return out;
}

QByteArray ControlMessage::InjectScroll(const ScrollEvent &event)
{
    // 协议里的滚动量是 [-1, 1] 定点；Qt 一次滚轮是 120 的整数倍，约定 120 算一格
    const float h_norm = event.h_scroll_ / 16.0f;
    const float v_norm = event.v_scroll_ / 16.0f;

    QByteArray out = MakeHeader(ControlType::InjectScroll);
    AppendPosition(out, event.position_, event.screen_size_);
    AppendU16(out, static_cast<quint16>(ToI16Fixed(h_norm)));
    AppendU16(out, static_cast<quint16>(ToI16Fixed(v_norm)));
    AppendU32(out, event.buttons_);
    return out;
}

QByteArray ControlMessage::BackOrScreenOn(bool down)
{
    QByteArray out = MakeHeader(ControlType::BackOrScreenOn);
    AppendU8(out, down ? 0 : 1);                                                                // 0 = ACTION_DOWN
    return out;
}

QByteArray ControlMessage::ExpandNotificationPanel()
{
    return MakeHeader(ControlType::ExpandNotificationPanel);
}

QByteArray ControlMessage::ExpandSettingsPanel()
{
    return MakeHeader(ControlType::ExpandSettingsPanel);
}

QByteArray ControlMessage::CollapsePanels()
{
    return MakeHeader(ControlType::CollapsePanels);
}

QByteArray ControlMessage::GetClipboard(ClipboardAction action)
{
    QByteArray out = MakeHeader(ControlType::GetClipboard);
    AppendU8(out, static_cast<quint8>(action));
    return out;
}

QByteArray ControlMessage::SetClipboard(quint64 sequence, bool paste, const QString &text)
{
    const QByteArray utf8 = TruncateUtf8(text, kMaxClipboardBytes);

    QByteArray out = MakeHeader(ControlType::SetClipboard);
    AppendU64(out, sequence);
    AppendU8(out, paste ? 1 : 0);
    AppendU32(out, static_cast<quint32>(utf8.size()));
    out.append(utf8);
    return out;
}

QByteArray ControlMessage::SetDisplayPower(bool on)
{
    QByteArray out = MakeHeader(ControlType::SetDisplayPower);
    AppendU8(out, on ? 1 : 0);
    return out;
}

QByteArray ControlMessage::RotateDevice()
{
    return MakeHeader(ControlType::RotateDevice);
}

} // namespace qsc::protocol
