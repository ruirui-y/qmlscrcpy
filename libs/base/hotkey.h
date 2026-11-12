#pragma once

#include <QString>
#include <QtGlobal>

namespace qsc::base
{

// 一个全局热键：修饰键掩码 + 虚拟键码。掩码和 Win32 的 MOD_* 对齐，方便直接喂 RegisterHotKey。
struct HotkeySpec
{
    unsigned modifiers_ = 0;                                                                    // MOD_CONTROL | MOD_ALT | ...
    unsigned virtual_key_ = 0;                                                                  // 'B' / VK_F9 / VK_SPACE 之类
};

// Win32 的修饰键掩码。放这里而不是到处 include windows.h，测试也能用。
inline constexpr unsigned kModAlt = 0x0001;
inline constexpr unsigned kModControl = 0x0002;
inline constexpr unsigned kModShift = 0x0004;
inline constexpr unsigned kModWin = 0x0008;

// 解析 "Ctrl+Alt+B" / "ctrl+shift+f9" / "Alt+Space" 这种写法；格式不对返回 false
bool ParseHotkey(const QString &text, HotkeySpec *spec);

// 反过来拼成可读文本，界面上显示用
QString HotkeyText(const HotkeySpec &spec);

} // namespace qsc::base
