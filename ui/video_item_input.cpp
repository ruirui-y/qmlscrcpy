// 输入处理：鼠标、滚轮、键盘、捏合、键位映射、连点器。
// 从 video_item.cpp 拆出来，画布主文件只留帧生命周期和尺寸。

#include "ui/video_item.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include <windows.h>

namespace
{

// 引擎的动作枚举转成协议那边的 0 按下 / 1 抬起 / 2 移动
quint8 ActionCode(qsc::input::KeyMapAction::Kind kind)
{
    switch (kind)
    {
    case qsc::input::KeyMapAction::Kind::Up:
        return 1;
    case qsc::input::KeyMapAction::Kind::Move:
        return 2;
    case qsc::input::KeyMapAction::Kind::Down:
    default:
        return 0;
    }
}

// 这个键在系统里是否还按着：自动重复会成对发 Press/Release，靠它把假的 Release 挡掉
bool KeyPhysicallyDown(int qt_key)
{
    const int vk = static_cast<int>(qt_key);
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}
// Qt 的键值转成配置里写的文本：Q → "Q"，空格 → "Space"
QString KeyName(int qt_key)
{
    return QKeySequence(qt_key).toString(QKeySequence::PortableText);
}

// 直接问系统修饰键是否按下：Qt 的 keyboardModifiers() 靠事件更新，没新事件时会读到旧值
bool ModifierDownNow(const QString &modifier)
{
    const auto down = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (modifier == QStringLiteral("none"))
    {
        return false;
    }
    if (modifier == QStringLiteral("alt"))
    {
        return down(VK_MENU);
    }
    if (modifier == QStringLiteral("shift"))
    {
        return down(VK_SHIFT);
    }
    if (modifier == QStringLiteral("ctrl_alt"))
    {
        return down(VK_CONTROL) && down(VK_MENU);
    }
    return down(VK_CONTROL);
}

} // namespace

void VideoItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QQuickFramebufferObject::mousePressEvent(event);
        return;
    }

    // 点了才要键盘焦点，免得一启动就把焦点抢过来
    forceActiveFocus();
    SyncMapper();
    EmitControlMessage(touch_.Press(event->position()));
    event->accept();
}

// 按住移动才发，否则设备会当成悬停
void VideoItem::mouseMoveEvent(QMouseEvent *event)
{
    // 只有按着左键才发移动，否则设备会当成悬停乱跳
    if ((event->buttons() & Qt::LeftButton) == 0)
    {
        QQuickFramebufferObject::mouseMoveEvent(event);
        return;
    }

    SyncMapper();
    EmitControlMessage(touch_.Move(event->position()));
    event->accept();
}

// 鼠标抬起：发触摸抬起
void VideoItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QQuickFramebufferObject::mouseReleaseEvent(event);
        return;
    }

    SyncMapper();
    EmitControlMessage(touch_.Release(event->position()));
    event->accept();
}

// 滚轮按下 120 为一格，换算成滚动消息
void VideoItem::wheelEvent(QWheelEvent *event)
{
    // 一格的 delta 是 120，除下来就是滚了几格
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0)
    {
        QQuickFramebufferObject::wheelEvent(event);
        return;
    }

    SyncMapper();

    // 按住修饰键滚轮 = 双指捏合：上滚放大、下滚缩小，中心跟着鼠标
    if (PinchModifierHeld(event->modifiers()))
    {
        PinchBy(notches, event->position());
        event->accept();
        return;
    }
    // 拖动模式一次会出三条消息，逐条发

    // 没按修饰键就滚：先把两指抬起，否则设备会把滚动当成继续缩放
    if (pinch_active_)
    {
        const QVector<QByteArray> ending = touch_.PinchEnd(pinch_center_, pinch_radius_);
        for (const QByteArray &message : ending)
        {
            EmitControlMessage(message);
        }
        pinch_active_ = false;
    }
    const QVector<QByteArray> messages = touch_.Scroll(event->position(), notches);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }

    // 拖动模式：抬起延后 120ms，让手指在终点停稳；安卓取最后 100ms 的速度，停住就不会甩动
    if (touch_.Mode() == qsc::input::WheelMode::Drag)
    {
        const QPointF pos = event->position();
        const int count = notches;
        QTimer::singleShot(120, this, [this, pos, count]()
        {
            EmitControlMessage(touch_.ScrollDragEnd(pos, count));
        });
    }
}

// 按键按下：没映射的键交回基类处理
void VideoItem::keyPressEvent(QKeyEvent *event)
{
    // 玩模式：已映射的键转成触摸，不再作为按键转发
    if (map_mode_ == 1 && key_map_engine_.Handles(KeyName(event->key())))
    {
        // 映射键全部交给 16ms 轮询处理，这里只负责吞掉，避免 Qt 的自动重复事件干扰
        event->accept();
        return;

    }
    const QByteArray message = keys_.Press(event->key(), event->modifiers(), event->isAutoRepeat());
    if (message.isEmpty())
    {
        QQuickFramebufferObject::keyPressEvent(event);
        return;
    }
    EmitControlMessage(message);
    event->accept();
}

// 按键抬起：同时清掉这个键的重复计数
void VideoItem::keyReleaseEvent(QKeyEvent *event)
{
    // 松开后捏合键已经不在按下状态就抬两指；其它按键的松开不影响捏合
    if (pinch_active_ && !ModifierDownNow(pinch_modifier_))
    {
        EndPinch();
    }

    if (map_mode_ == 1 && key_map_engine_.Handles(KeyName(event->key())))
    {
        // 映射键的抬起同样交给轮询，这里只吞掉
        event->accept();
        return;
    }
    const QByteArray message = keys_.Release(event->key(), event->modifiers());
    if (message.isEmpty())
    {
        QQuickFramebufferObject::keyReleaseEvent(event);
        return;
    }
    EmitControlMessage(message);
    event->accept();
}

// 当前按下的修饰键是不是配置里那个捏合键
bool VideoItem::PinchModifierHeld(Qt::KeyboardModifiers modifiers) const
{
    if (pinch_modifier_ == QStringLiteral("none"))
    {
        return false;
    }
    if (pinch_modifier_ == QStringLiteral("alt"))
    {
        return modifiers.testFlag(Qt::AltModifier);
    }
    if (pinch_modifier_ == QStringLiteral("shift"))
    {
        return modifiers.testFlag(Qt::ShiftModifier);
    }
    if (pinch_modifier_ == QStringLiteral("ctrl_alt"))
    {
        return modifiers.testFlag(Qt::ControlModifier) && modifiers.testFlag(Qt::AltModifier);
    }
    return modifiers.testFlag(Qt::ControlModifier);
}

// 一格滚轮把两指间距放大或缩小 12%，第一次滚先把两指按下去
void VideoItem::PinchBy(int notches, const QPointF &pos)
{
    pinch_center_ = pos;

    // 半径上限是画面短边的 45%，再大会把第二根手指顶出屏幕
    const QSize frame = FrameSize();
    const int limit = qMax(20, qMin(frame.width(), frame.height()) * 50 / 100);

    if (!pinch_active_)
    {
        // 半径跨手势保留：滚得慢时每格都会新开一次手势，若每次重置回 40，
        // 上一格放大的效果就丢了，看起来像"每一格都在放大"
        const qint64 now_us = clock_.nsecsElapsed() / 1000;
        if (pinch_last_us_ == 0 || (now_us - pinch_last_us_) > 2000000)
        {
            pinch_radius_ = 40;
        }
        pinch_last_us_ = now_us;
        pinch_active_ = true;
        emit SigPinchingChanged();
        const QVector<QByteArray> messages = touch_.PinchBegin(pinch_center_, pinch_radius_);
        for (const QByteArray &message : messages)
        {
            EmitControlMessage(message);
        }
    }

    // 一格 15%：12% 感觉滚几下就没变化了
    const double scaled = static_cast<double>(pinch_radius_) * std::pow(1.15, notches);
    pinch_radius_ = qBound(20, static_cast<int>(scaled), limit);

    const QVector<QByteArray> messages = touch_.PinchStep(pinch_center_, pinch_radius_);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }
}

// 捏合修饰键：ctrl / alt / shift / ctrl_alt / none
void VideoItem::SetPinchModifier(const QString &modifier)
{
    pinch_modifier_ = modifier.trimmed().toLower();
}

// 抬起两指：修饰键松开时调用
void VideoItem::EndPinch()
{
    if (!pinch_active_)
    {
        return;
    }

    const QVector<QByteArray> messages = touch_.PinchEnd(pinch_center_, pinch_radius_);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }
    pinch_active_ = false;
    emit SigPinchingChanged();
}

// 切映射模式：从"玩/编辑"退出时要把所有按住的手指抬起来，否则设备会一直按着
void VideoItem::SetMapMode(int mode)
{
    const int clamped = qBound(0, mode, 2);
    if (clamped == map_mode_)
    {
        return;
    }

    if (map_mode_ != 0)
    {
        for (const qsc::input::KeyMapAction &action : key_map_engine_.Reset())
        {
            EmitControlMessage(touch_.NormalizedTouch(action.pointer_id_, ActionCode(action.kind_), action.pos_));
        }
    }

    map_mode_ = clamped;
    if (map_mode_ == 2)
    {
        emit SigKeyMapChanged();
    }
    emit SigMapModeChanged();
}

// 换设备：读这台设备自己的键位文件，读不到就是空配置
void VideoItem::SetKeyMapPath(const QString &path)
{
    key_map_path_ = path;
    QString error;
    key_map_ = qsc::input::KeyMap::Load(path, &error);
    if (!error.isEmpty())
    {
        qWarning() << "[keymap]" << error;
    }
    key_map_engine_.SetMap(key_map_);
    qInfo() << "[keymap] 已加载" << key_map_.items_.size() << "个键位：" << path;
    emit SigKeyMapChanged();
}

// 给 QML 叠加层用的键位快照：位置已经换算成 0~1，界面直接按比例摆
QVariantList VideoItem::KeyMapItems() const
{
    QVariantList list;
    for (int i = 0; i < key_map_.items_.size(); ++i)
    {
        const qsc::input::KeyMapItem &item = key_map_.items_.at(i);
        QVariantMap entry;
        entry.insert(QStringLiteral("index"), i);
        entry.insert(QStringLiteral("type"), static_cast<int>(item.type_));
        entry.insert(QStringLiteral("label"), item.label_);
        entry.insert(QStringLiteral("key"), item.key_);
        entry.insert(QStringLiteral("x"), item.pos_.x());
        entry.insert(QStringLiteral("y"), item.pos_.y());
        entry.insert(QStringLiteral("radius"), item.radius_);
        entry.insert(QStringLiteral("up"), item.up_key_);
        entry.insert(QStringLiteral("down"), item.down_key_);
        entry.insert(QStringLiteral("left"), item.left_key_);
        entry.insert(QStringLiteral("right"), item.right_key_);
        list.append(entry);
    }
    return list;
}

// 新增一条键位，返回它的下标；type 0 点按 1 按住 2 摇杆
int VideoItem::AddKeyMapItem(int type, const QString &key, double x, double y, const QString &label)
{
    qsc::input::KeyMapItem item;
    item.type_ = static_cast<qsc::input::KeyMapItemType>(qBound(0, type, 2));
    item.key_ = key;
    item.label_ = label;
    item.pos_ = QPointF(x, y);
    key_map_.Add(item);
    key_map_engine_.SetMap(key_map_);

    const int index = key_map_.items_.size() - 1;
    emit SigKeyMapChanged();
    return index;
}

void VideoItem::MoveKeyMapItem(int index, double x, double y)
{
    key_map_.Move(index, QPointF(x, y));
    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

void VideoItem::RemoveKeyMapItem(int index)
{
    key_map_.Remove(index);
    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

// 写回 JSON：目录不存在时先建
void VideoItem::SaveKeyMap()
{
    if (key_map_path_.isEmpty())
    {
        return;
    }
    const QFileInfo info(key_map_path_);
    QDir().mkpath(info.absolutePath());
    QString error;
    if (!key_map_.Save(key_map_path_, &error))
    {
        qWarning() << "[keymap]" << error;
    }
}

// 改摇杆的四个方向键：空的填回默认值，免得某个方向永远推不动
void VideoItem::SetSteerKeys(int index, const QString &up, const QString &down, const QString &left,
                             const QString &right)
{
    if (index < 0 || index >= key_map_.items_.size() || key_map_.items_.at(index).type_ != qsc::input::KeyMapItemType::Steer)
    {
        return;
    }

    qsc::input::KeyMapItem &item = key_map_.items_[index];
    item.up_key_ = up.isEmpty() ? item.up_key_ : up;
    item.down_key_ = down.isEmpty() ? item.down_key_ : down;
    item.left_key_ = left.isEmpty() ? item.left_key_ : left;
    item.right_key_ = right.isEmpty() ? item.right_key_ : right;

    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

// 键名字符串转成 Windows 虚拟键码：字母数字用 ASCII，其余查小表
int VkForName(const QString &name)
{
    if (name.size() == 1)
    {
        const QChar ch = name.at(0).toUpper();
        return ch.unicode();
    }
    // 表里的键名用小写：配置里可能存成 space / Space，统一按小写查
    static const QHash<QString, int> kExtra = {
        { QStringLiteral("space"), VK_SPACE },   { QStringLiteral("tab"), VK_TAB },
        { QStringLiteral("esc"), VK_ESCAPE },    { QStringLiteral("enter"), VK_RETURN },
        { QStringLiteral("up"), VK_UP },         { QStringLiteral("down"), VK_DOWN },
        { QStringLiteral("left"), VK_LEFT },     { QStringLiteral("right"), VK_RIGHT },
        { QStringLiteral("shift"), VK_SHIFT },   { QStringLiteral("ctrl"), VK_CONTROL },
        { QStringLiteral("alt"), VK_MENU },
    };
    return kExtra.value(name.toLower(), 0);
}

// 16ms 轮询：只有"按下状态"发生变化才发触摸，自动重复事件完全不影响这里
void VideoItem::SlotKeyMapPoll()
{
    if (map_mode_ != 1)
    {
        if (!polled_keys_.isEmpty())
        {
            DispatchKeyMapActions(key_map_engine_.Reset());
            polled_keys_.clear();
        }
        return;
    }

    for (const QString &name : key_map_engine_.TrackedKeys())
    {
        const int vk = VkForName(name);
        if (vk == 0)
        {
            continue;
        }
        const bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool was_down = polled_keys_.contains(name);
        if (down == was_down)
        {
            continue;
        }

        if (down)
        {
            polled_keys_.insert(name);
            qInfo() << "[keymap] poll down" << name;
            DispatchKeyMapActions(key_map_engine_.Press(name));
        }
        else
        {
            polled_keys_.remove(name);
            qInfo() << "[keymap] poll up" << name;
            DispatchKeyMapActions(key_map_engine_.Release(name));
        }
    }
}

// 动作翻成控制消息：点按类先按下，40ms 后补一个抬起
void VideoItem::DispatchKeyMapActions(const QVector<qsc::input::KeyMapAction> &actions)
{
    const bool tap_pair = actions.size() == 2
                          && actions.at(0).kind_ == qsc::input::KeyMapAction::Kind::Down
                          && actions.at(1).kind_ == qsc::input::KeyMapAction::Kind::Up;
    for (const qsc::input::KeyMapAction &action : actions)
    {
        if (tap_pair && action.kind_ == qsc::input::KeyMapAction::Kind::Up)
        {
            const quint64 pointer = action.pointer_id_;
            const QPointF pos = action.pos_;
            QTimer::singleShot(40, this, [this, pointer, pos]()
            {
                EmitControlMessage(touch_.NormalizedTouch(pointer, 1, pos));
            });
            continue;
        }
        EmitControlMessage(touch_.NormalizedTouch(action.pointer_id_, ActionCode(action.kind_), action.pos_));
    }
}

// 进入录制：点圆点后调它，下一次按键就绑到这个键位；Esc 取消
void VideoItem::BeginKeyRecord(int index)
{
    if (index < 0 || index >= key_map_.items_.size())
    {
        return;
    }
    record_index_ = index;
    forceActiveFocus();                                                                          // 叠加层不拿焦点，不抢就收不到按键
    qInfo() << "[keymap] record start index" << index;
    emit SigRecordingChanged();
}

// QML 收到按键后调它：把键绑到当前录制目标上
void VideoItem::BindRecordedKey(int qt_key)
{
    if (record_index_ < 0 || record_index_ >= key_map_.items_.size())
    {
        return;
    }
    const int index = record_index_;
    record_index_ = -1;
    if (key_map_.items_.at(index).type_ != qsc::input::KeyMapItemType::Steer)
    {
        key_map_.items_[index].key_ = KeyName(qt_key);
        key_map_.items_[index].label_ = KeyName(qt_key);
        key_map_engine_.SetMap(key_map_);
        qInfo() << "[keymap] record bound" << KeyName(qt_key) << "to index" << index;
        emit SigKeyMapChanged();
    }
    emit SigRecordingChanged();
}

// 连点间隔：0 关，其它值按毫秒连点；同时把开关状态记进配置
void VideoItem::SetClickerInterval(int ms)
{
    clicker_interval_ms_ = qBound(0, ms, 1000);
    clicker_next_us_ = 0;
}
