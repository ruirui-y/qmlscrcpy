#pragma once

#include <QByteArray>
#include <QHash>
#include <Qt>

namespace qsc::input
{

// 把键盘事件翻成 scrcpy 的按键消息。
// 重点在两件事：自己数重复次数（Qt 只给布尔），以及把修饰键折算成 metaState。
class KeyController
{
public:
    QByteArray Press(int qt_key, Qt::KeyboardModifiers modifiers, bool auto_repeat);
    QByteArray Release(int qt_key, Qt::KeyboardModifiers modifiers);

    void Reset();                                                                               // 断开连接时清掉所有重复计数

private:
    QHash<int, int> repeats_;                                                                   // 每个键当前的第几次重复，按下时从 0 开始
};

} // namespace qsc::input
