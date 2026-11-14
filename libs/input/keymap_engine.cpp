#include "input/keymap_engine.h"

#include <cmath>

namespace qsc::input
{

namespace
{

// 方向键映射成单位向量，四向同时按会自然叠加成斜向
QPointF DirectionVector(const QString &key, const KeyMapItem &item)
{
    if (!item.up_key_.isEmpty() && key == item.up_key_)
    {
        return QPointF(0.0, -1.0);
    }
    if (!item.down_key_.isEmpty() && key == item.down_key_)
    {
        return QPointF(0.0, 1.0);
    }
    if (!item.left_key_.isEmpty() && key == item.left_key_)
    {
        return QPointF(-1.0, 0.0);
    }
    if (!item.right_key_.isEmpty() && key == item.right_key_)
    {
        return QPointF(1.0, 0.0);
    }
    return QPointF(0.0, 0.0);
}

} // namespace

void KeyMapEngine::SetMap(const KeyMap &map)
{
    map_ = map;
    held_keys_.clear();
    steer_keys_.clear();
}

// 被映射的键才拦下来，其余照旧转发给设备
bool KeyMapEngine::Handles(const QString &key) const
{
    if (map_.FindByKey(key) >= 0)
    {
        return true;
    }
    return map_.FindBySteerKey(key, nullptr) >= 0;
}

// 按下：tap 直接按下再抬起由调用方延时抬，hold 只按下，steer 先按中心再推方向
QVector<KeyMapAction> KeyMapEngine::Press(const QString &key)
{
    QVector<KeyMapAction> actions;

    const int tap_index = map_.FindByKey(key);
    if (tap_index >= 0)
    {
        const KeyMapItem &item = map_.items_.at(tap_index);
        actions.append({ KeyMapAction::Kind::Down, PointerFor(tap_index), item.pos_ });

        if (item.type_ == KeyMapItemType::Tap)
        {
            // 点按：立刻抬起，中间不留按住时间，避免游戏当成蓄力
            actions.append({ KeyMapAction::Kind::Up, PointerFor(tap_index), item.pos_ });
        }
        else
        {
            held_keys_.insert(key);
        }
        return actions;
    }

    KeyMapItem steer_item;
    const int steer_index = map_.FindBySteerKey(key, &steer_item);
    if (steer_index < 0)
    {
        return actions;
    }

    // 摇杆第一次按下要先在中心落指，后续方向变化只发 MOVE
    const bool first_direction = steer_keys_.isEmpty();
    steer_keys_.insert(key);
    if (first_direction)
    {
        actions.append({ KeyMapAction::Kind::Down, PointerFor(steer_index), steer_item.pos_ });
    }
    actions.append({ KeyMapAction::Kind::Move, PointerFor(steer_index), SteerPos(steer_item, steer_index) });
    return actions;
}

// 松开：tap 早就抬过了，hold 现在抬，steer 回中心，方向键全松才抬手指
QVector<KeyMapAction> KeyMapEngine::Release(const QString &key)
{
    QVector<KeyMapAction> actions;

    const int tap_index = map_.FindByKey(key);
    if (tap_index >= 0)
    {
        if (!held_keys_.remove(key))
        {
            return actions;
        }
        const KeyMapItem &item = map_.items_.at(tap_index);
        actions.append({ KeyMapAction::Kind::Up, PointerFor(tap_index), item.pos_ });
        return actions;
    }

    KeyMapItem steer_item;
    const int steer_index = map_.FindBySteerKey(key, &steer_item);
    if (steer_index < 0)
    {
        return actions;
    }

    if (!steer_keys_.remove(key))
    {
        return actions;
    }

    if (steer_keys_.isEmpty())
    {
        // 最后一个方向也松了：先回中心再抬起，避免手指停在偏移位置
        actions.append({ KeyMapAction::Kind::Move, PointerFor(steer_index), steer_item.pos_ });
        actions.append({ KeyMapAction::Kind::Up, PointerFor(steer_index), steer_item.pos_ });
    }
    else
    {
        actions.append({ KeyMapAction::Kind::Move, PointerFor(steer_index), SteerPos(steer_item, steer_index) });
    }
    return actions;
}

// 把所有还按着的键抬起来：断线、切回打字模式时调，否则设备会一直按住
QVector<KeyMapAction> KeyMapEngine::Reset()
{
    QVector<KeyMapAction> actions;

    for (const QString &key : held_keys_)
    {
        const int index = map_.FindByKey(key);
        if (index >= 0)
        {
            actions.append({ KeyMapAction::Kind::Up, PointerFor(index), map_.items_.at(index).pos_ });
        }
    }
    held_keys_.clear();

    for (const KeyMapItem &item : map_.items_)
    {
        if (item.type_ != KeyMapItemType::Steer)
        {
            continue;
        }
        const int index = map_.FindBySteerKey(item.up_key_.isEmpty() ? item.right_key_ : item.up_key_, nullptr);
        if (index < 0)
        {
            continue;
        }
        actions.append({ KeyMapAction::Kind::Up, PointerFor(index), item.pos_ });
    }
    steer_keys_.clear();
    return actions;
}

// 摇杆位置 = 中心 + 方向单位向量 × 半径；斜向会叠加后归一化，不会推到画面外
QPointF KeyMapEngine::SteerPos(const KeyMapItem &item, int index) const
{
    Q_UNUSED(index)

    QPointF direction(0.0, 0.0);
    for (const QString &key : steer_keys_)
    {
        direction += DirectionVector(key, item);
    }

    const double length = std::hypot(direction.x(), direction.y());
    if (length < 1e-6)
    {
        return item.pos_;
    }

    direction /= length;
    QPointF pos = item.pos_ + direction * item.radius_;
    pos.setX(qBound(0.0, pos.x(), 1.0));
    pos.setY(qBound(0.0, pos.y(), 1.0));
    return pos;
}

} // namespace qsc::input
