#include "base/hotkey.h"

#include <QStringList>

namespace qsc::base
{

namespace
{

// 单键名转虚拟键码。只认常见的那批，够配置用；返回 0 表示不认识。
unsigned VirtualKeyFromName(const QString &name)
{
    const QString upper = name.trimmed().toUpper();

    if (upper.size() == 1)
    {
        const QChar ch = upper.at(0);
        if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
        {
            return static_cast<unsigned>(ch.unicode());                                         // 字母键的虚拟键码就是大写 ASCII
        }
        if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
        {
            return static_cast<unsigned>(ch.unicode());
        }
    }

    if (upper.startsWith(QLatin1Char('F')))
    {
        bool ok = false;
        const int number = upper.mid(1).toInt(&ok);
        if (ok && number >= 1 && number <= 24)
        {
            return static_cast<unsigned>(0x70 + number - 1);                                    // VK_F1 = 0x70
        }
    }

    // 少数特殊键，写全名比写单字符直观
    if (upper == QLatin1String("SPACE"))
    {
        return 0x20;
    }
    if (upper == QLatin1String("TAB"))
    {
        return 0x09;
    }
    if (upper == QLatin1String("ESC") || upper == QLatin1String("ESCAPE"))
    {
        return 0x1B;
    }
    if (upper == QLatin1String("INSERT"))
    {
        return 0x2D;
    }
    if (upper == QLatin1String("DELETE"))
    {
        return 0x2E;
    }
    if (upper == QLatin1String("HOME"))
    {
        return 0x24;
    }
    if (upper == QLatin1String("END"))
    {
        return 0x23;
    }
    if (upper == QLatin1String("PAGEUP"))
    {
        return 0x21;
    }
    if (upper == QLatin1String("PAGEDOWN"))
    {
        return 0x22;
    }
    return 0;
}

// 修饰键名转掩码
unsigned ModifierFromName(const QString &name)
{
    const QString upper = name.trimmed().toUpper();
    if (upper == QLatin1String("CTRL") || upper == QLatin1String("CONTROL"))
    {
        return kModControl;
    }
    if (upper == QLatin1String("ALT"))
    {
        return kModAlt;
    }
    if (upper == QLatin1String("SHIFT"))
    {
        return kModShift;
    }
    if (upper == QLatin1String("WIN") || upper == QLatin1String("META") || upper == QLatin1String("SUPER"))
    {
        return kModWin;
    }
    return 0;
}

} // namespace

// 解析 "Ctrl+Alt+B" 这类写法：最后一段是主键，前面的都当修饰键
bool ParseHotkey(const QString &text, HotkeySpec *spec)
{
    if (spec == nullptr)
    {
        return false;
    }

    const QStringList parts = text.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
    {
        return false;
    }

    unsigned modifiers = 0;
    for (int index = 0; index + 1 < parts.size(); ++index)
    {
        const unsigned modifier = ModifierFromName(parts.at(index));
        if (modifier == 0)
        {
            return false;                                                                       // 前几段必须是修饰键
        }
        modifiers |= modifier;
    }

    const unsigned key = VirtualKeyFromName(parts.last());
    if (key == 0)
    {
        return false;
    }

    // 单键热键会抢全局输入，要求至少带一个修饰键
    if (modifiers == 0)
    {
        return false;
    }

    spec->modifiers_ = modifiers;
    spec->virtual_key_ = key;
    return true;
}

// 拼回 "Ctrl+Alt+B" 方便显示
QString HotkeyText(const HotkeySpec &spec)
{
    QStringList parts;
    if ((spec.modifiers_ & kModControl) != 0)
    {
        parts << QStringLiteral("Ctrl");
    }
    if ((spec.modifiers_ & kModAlt) != 0)
    {
        parts << QStringLiteral("Alt");
    }
    if ((spec.modifiers_ & kModShift) != 0)
    {
        parts << QStringLiteral("Shift");
    }
    if ((spec.modifiers_ & kModWin) != 0)
    {
        parts << QStringLiteral("Win");
    }

    const unsigned key = spec.virtual_key_;
    if (key >= 0x41 && key <= 0x5A)
    {
        parts << QString(static_cast<QChar>(key));                                              // 字母
    }
    else if (key >= 0x30 && key <= 0x39)
    {
        parts << QString(static_cast<QChar>(key));                                              // 数字
    }
    else if (key >= 0x70 && key <= 0x87)
    {
        parts << QStringLiteral("F%1").arg(key - 0x70 + 1);                                     // 功能键
    }
    else
    {
        parts << QStringLiteral("VK_%1").arg(key, 0, 16);                                       // 其它键直接显示码值
    }
    return parts.join(QLatin1Char('+'));
}

} // namespace qsc::base
