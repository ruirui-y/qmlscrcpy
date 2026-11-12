#include "input/keycode_table.h"

namespace qsc::input
{

namespace
{

// 安卓键码，取自 android.view.KeyEvent；只列我们真正会用的那些
constexpr quint32 kKeyHome = 3;
constexpr quint32 kKeyBack = 4;
constexpr quint32 kKey0 = 7;                                                                    // 数字 0..9 是连续的，7 到 16
constexpr quint32 kKeyA = 29;                                                                   // 字母 A..Z 是连续的，29 到 54
constexpr quint32 kKeyDpadUp = 19;
constexpr quint32 kKeyDpadDown = 20;
constexpr quint32 kKeyDpadLeft = 21;
constexpr quint32 kKeyDpadRight = 22;
constexpr quint32 kKeyDpadCenter = 23;
constexpr quint32 kKeyVolumeUp = 24;
constexpr quint32 kKeyVolumeDown = 25;
constexpr quint32 kKeyPower = 26;
constexpr quint32 kKeyAltLeft = 58;
constexpr quint32 kKeyAltRight = 61;
constexpr quint32 kKeyShiftLeft = 59;
constexpr quint32 kKeyShiftRight = 60;
constexpr quint32 kKeyTab = 61;
constexpr quint32 kKeySpace = 62;
constexpr quint32 kKeyEnter = 66;
constexpr quint32 kKeyDel = 67;                                                                 // 退格
constexpr quint32 kKeyGrave = 68;
constexpr quint32 kKeyMinus = 69;
constexpr quint32 kKeyEquals = 70;
constexpr quint32 kKeyLeftBracket = 71;
constexpr quint32 kKeyRightBracket = 72;
constexpr quint32 kKeyBackslash = 73;
constexpr quint32 kKeySemicolon = 74;
constexpr quint32 kKeyApostrophe = 75;
constexpr quint32 kKeySlash = 76;
constexpr quint32 kKeyComma = 55;
constexpr quint32 kKeyPeriod = 56;
constexpr quint32 kKeyMenu = 82;
constexpr quint32 kKeyPageUp = 92;
constexpr quint32 kKeyPageDown = 93;
constexpr quint32 kKeyEscape = 111;
constexpr quint32 kKeyForwardDel = 112;
constexpr quint32 kKeyCtrlLeft = 113;
constexpr quint32 kKeyCtrlRight = 114;
constexpr quint32 kKeyMetaLeft = 117;
constexpr quint32 kKeyMetaRight = 118;
constexpr quint32 kKeyMoveHome = 122;
constexpr quint32 kKeyMoveEnd = 123;
constexpr quint32 kKeyInsert = 124;
constexpr quint32 kKeyF1 = 131;                                                                 // 功能键 F1..F12 是连续的

// metaState 位，同样取自 KeyEvent
constexpr quint32 kMetaShiftOn = 0x1;
constexpr quint32 kMetaAltOn = 0x2;
constexpr quint32 kMetaAltLeftOn = 0x10;
constexpr quint32 kMetaShiftLeftOn = 0x40;
constexpr quint32 kMetaCtrlOn = 0x1000;
constexpr quint32 kMetaCtrlLeftOn = 0x2000;
constexpr quint32 kMetaMetaOn = 0x10000;
constexpr quint32 kMetaMetaLeftOn = 0x20000;

} // namespace

// 字母数字按连续区间算，其余一个一个对
quint32 AndroidKeycode(int qt_key)
{
    if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z)
    {
        return kKeyA + static_cast<quint32>(qt_key - Qt::Key_A);
    }
    if (qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9)
    {
        return kKey0 + static_cast<quint32>(qt_key - Qt::Key_0);
    }
    if (qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F12)
    {
        return kKeyF1 + static_cast<quint32>(qt_key - Qt::Key_F1);
    }

    switch (qt_key)
    {
    // 导航
    case Qt::Key_Up:
        return kKeyDpadUp;
    case Qt::Key_Down:
        return kKeyDpadDown;
    case Qt::Key_Left:
        return kKeyDpadLeft;
    case Qt::Key_Right:
        return kKeyDpadRight;
    case Qt::Key_PageUp:
        return kKeyPageUp;
    case Qt::Key_PageDown:
        return kKeyPageDown;
    case Qt::Key_Home:
        return kKeyMoveHome;
    case Qt::Key_End:
        return kKeyMoveEnd;
    case Qt::Key_Insert:
        return kKeyInsert;

    // 编辑
    case Qt::Key_Backspace:
        return kKeyDel;
    case Qt::Key_Delete:
        return kKeyForwardDel;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return kKeyEnter;
    case Qt::Key_Tab:
        return kKeyTab;
    case Qt::Key_Space:
        return kKeySpace;
    case Qt::Key_Escape:
        return kKeyEscape;

    // 符号
    case Qt::Key_Comma:
        return kKeyComma;
    case Qt::Key_Period:
        return kKeyPeriod;
    case Qt::Key_Slash:
        return kKeySlash;
    case Qt::Key_Semicolon:
        return kKeySemicolon;
    case Qt::Key_Apostrophe:
        return kKeyApostrophe;
    case Qt::Key_BracketLeft:
        return kKeyLeftBracket;
    case Qt::Key_BracketRight:
        return kKeyRightBracket;
    case Qt::Key_Backslash:
        return kKeyBackslash;
    case Qt::Key_QuoteLeft:
        return kKeyGrave;
    case Qt::Key_Minus:
        return kKeyMinus;
    case Qt::Key_Equal:
        return kKeyEquals;

    // 修饰键：左上角的用左键码，设备不区分左右时也够用
    case Qt::Key_Shift:
        return kKeyShiftLeft;
    case Qt::Key_Control:
        return kKeyCtrlLeft;
    case Qt::Key_Alt:
        return kKeyAltLeft;
    case Qt::Key_Meta:
        return kKeyMetaLeft;

    // 系统键
    case Qt::Key_Back:
        return kKeyBack;
    case Qt::Key_HomePage:
        return kKeyHome;
    case Qt::Key_VolumeUp:
        return kKeyVolumeUp;
    case Qt::Key_VolumeDown:
        return kKeyVolumeDown;
    case Qt::Key_PowerOff:
        return kKeyPower;
    case Qt::Key_Menu:
        return kKeyMenu;
    case Qt::Key_Find:
        return kKeyDpadCenter;

    default:
        return 0;
    }
}

// 修饰键是位掩码：通用的位和左右专用的位都要置，设备端两种读法都认
quint32 AndroidMetaState(Qt::KeyboardModifiers modifiers)
{
    quint32 meta = 0;
    if (modifiers & Qt::ShiftModifier)
    {
        meta |= kMetaShiftOn | kMetaShiftLeftOn;
    }
    if (modifiers & Qt::ControlModifier)
    {
        meta |= kMetaCtrlOn | kMetaCtrlLeftOn;
    }
    if (modifiers & Qt::AltModifier)
    {
        meta |= kMetaAltOn | kMetaAltLeftOn;
    }
    if (modifiers & Qt::MetaModifier)
    {
        meta |= kMetaMetaOn | kMetaMetaLeftOn;
    }
    return meta;
}

} // namespace qsc::input
