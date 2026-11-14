#pragma once

#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>

#include "input/keymap.h"

namespace qsc::input
{

// 引擎只产出"第几个手指、做什么动作、落在哪个归一化坐标"，
// 具体编码成协议字节由调用方负责，这样它可以脱机单测。
struct KeyMapAction
{
    enum class Kind
    {
        Down,
        Move,
        Up,
    };

    Kind kind_ = Kind::Down;
    quint64 pointer_id_ = 0;                                                                    // 每个键位一个固定 id，互不干扰
    QPointF pos_;                                                                               // 归一化 0~1
};

// 把键盘按键翻译成触摸动作：tap 点一下、hold 按住、steer 摇杆推方向
class KeyMapEngine
{
public:
    void SetMap(const KeyMap &map);
    const KeyMap &Map() const { return map_; }

    bool Handles(const QString &key) const;                                                     // 这个键是否被映射，决定要不要拦下来

    QVector<KeyMapAction> Press(const QString &key);                                            // 按下某个键，返回要发的动作
    QVector<KeyMapAction> Release(const QString &key);                                          // 松开，返回要发的动作
    QVector<KeyMapAction> Reset();                                                              // 全部抬起（断线、退出映射模式时用）

private:
    quint64 PointerFor(int index) const { return kFirstPointerId + static_cast<quint64>(index); }
    QPointF SteerPos(const KeyMapItem &item, int index) const;                                  // 按当前按着的方向算摇杆位置

    static constexpr quint64 kFirstPointerId = 10;                                              // 0~9 留给鼠标和捏合，映射从 10 开始

    KeyMap map_;
    QSet<QString> held_keys_;                                                                   // 当前按着的 tap / hold 键
    QSet<QString> steer_keys_;                                                                  // 当前按着的摇杆方向键
};

} // namespace qsc::input
