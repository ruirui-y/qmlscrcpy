#pragma once

#include <QtGlobal>
#include <Qt>

namespace qsc::input
{

// Qt 按键 → 安卓键码。不支持的键返回 0，调用方据此跳过。
quint32 AndroidKeycode(int qt_key);

// Qt 修饰键 → 安卓 metaState 位；左右两个位都要置，设备和 scrcpy 都认这一套。
quint32 AndroidMetaState(Qt::KeyboardModifiers modifiers);

} // namespace qsc::input
