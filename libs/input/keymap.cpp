#include "input/keymap.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace qsc::input
{

namespace
{

constexpr const char *kTypeTap = "tap";
constexpr const char *kTypeHold = "hold";
constexpr const char *kTypeSteer = "steer";

// 归一化坐标夹到 0~1，越界的点设备那边会整条丢掉
double Clamp01(double value)
{
    if (value < 0.0)
    {
        return 0.0;
    }
    if (value > 1.0)
    {
        return 1.0;
    }
    return value;
}

} // namespace

// 读一份键位配置；文件不存在返回空配置，解析失败把原因写进 error
KeyMap KeyMap::Load(const QString &path, QString *error)
{
    KeyMap map;
    QFile file(path);
    if (!file.exists())
    {
        return map;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("打不开键位文件：%1").arg(file.errorString());
        }
        return map;
    }

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("键位 JSON 解析失败：%1").arg(parse_error.errorString());
        }
        return map;
    }

    const QJsonObject root = document.object();
    map.name_ = root.value(QStringLiteral("name")).toString(QStringLiteral("默认"));

    const QJsonArray items = root.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &value : items)
    {
        const QJsonObject object = value.toObject();
        KeyMapItem item;
        item.type_ = TypeFromString(object.value(QStringLiteral("type")).toString());
        item.label_ = object.value(QStringLiteral("label")).toString();
        item.key_ = object.value(QStringLiteral("key")).toString();
        item.pos_ = QPointF(object.value(QStringLiteral("x")).toDouble(0.5),
                            object.value(QStringLiteral("y")).toDouble(0.5));
        item.radius_ = object.value(QStringLiteral("radius")).toDouble(0.06);

        const QJsonObject keys = object.value(QStringLiteral("keys")).toObject();
        item.up_key_ = keys.value(QStringLiteral("up")).toString(QStringLiteral("W"));
        item.down_key_ = keys.value(QStringLiteral("down")).toString(QStringLiteral("S"));
        item.left_key_ = keys.value(QStringLiteral("left")).toString(QStringLiteral("A"));
        item.right_key_ = keys.value(QStringLiteral("right")).toString(QStringLiteral("D"));

        // tap / steer 至少要有一个能触发的键，否则这条配置是废的
        if (item.type_ == KeyMapItemType::Steer)
        {
            if (item.up_key_.isEmpty() && item.down_key_.isEmpty() && item.left_key_.isEmpty()
                && item.right_key_.isEmpty())
            {
                continue;
            }
        }
        else if (item.key_.isEmpty())
        {
            continue;
        }
        map.items_.append(item);
    }
    return map;
}

// 写一份键位配置；目录不存在时由调用方先建好
bool KeyMap::Save(const QString &path, QString *error) const
{
    QJsonArray items;
    for (const KeyMapItem &item : items_)
    {
        QJsonObject object;
        object.insert(QStringLiteral("type"), TypeToString(item.type_));
        object.insert(QStringLiteral("label"), item.label_);
        object.insert(QStringLiteral("x"), item.pos_.x());
        object.insert(QStringLiteral("y"), item.pos_.y());

        if (item.type_ == KeyMapItemType::Steer)
        {
            object.insert(QStringLiteral("radius"), item.radius_);
            QJsonObject keys;
            keys.insert(QStringLiteral("up"), item.up_key_);
            keys.insert(QStringLiteral("down"), item.down_key_);
            keys.insert(QStringLiteral("left"), item.left_key_);
            keys.insert(QStringLiteral("right"), item.right_key_);
            object.insert(QStringLiteral("keys"), keys);
        }
        else
        {
            object.insert(QStringLiteral("key"), item.key_);
        }
        items.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("name"), name_);
    root.insert(QStringLiteral("items"), items);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("写不了键位文件：%1").arg(file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

// 加一条键位；同一键盘键已经绑过就覆盖位置，避免一个键对应两个点
void KeyMap::Add(const KeyMapItem &item)
{
    for (KeyMapItem &existing : items_)
    {
        if (existing.type_ == item.type_ && existing.type_ != KeyMapItemType::Steer
            && existing.key_ == item.key_)
        {
            existing = item;
            existing.pos_ = QPointF(Clamp01(item.pos_.x()), Clamp01(item.pos_.y()));
            return;
        }
    }
    KeyMapItem copy = item;
    copy.pos_ = QPointF(Clamp01(item.pos_.x()), Clamp01(item.pos_.y()));
    items_.append(copy);
}

void KeyMap::Remove(int index)
{
    if (index >= 0 && index < items_.size())
    {
        items_.removeAt(index);
    }
}

// 拖动键位：坐标夹回 0~1，免得点跑到画面外
void KeyMap::Move(int index, const QPointF &pos)
{
    if (index < 0 || index >= items_.size())
    {
        return;
    }
    items_[index].pos_ = QPointF(Clamp01(pos.x()), Clamp01(pos.y()));
}

// 按键盘键找 tap / hold
int KeyMap::FindByKey(const QString &key) const
{
    for (int i = 0; i < items_.size(); ++i)
    {
        if (items_[i].type_ != KeyMapItemType::Steer && items_[i].key_ == key)
        {
            return i;
        }
    }
    return -1;
}

// 按方向键找摇杆：把命中的方向写进 item_out，调用方据此算推动方向
int KeyMap::FindBySteerKey(const QString &key, KeyMapItem *item_out) const
{
    for (int i = 0; i < items_.size(); ++i)
    {
        const KeyMapItem &item = items_[i];
        if (item.type_ != KeyMapItemType::Steer)
        {
            continue;
        }
        if (item.up_key_ == key || item.down_key_ == key || item.left_key_ == key || item.right_key_ == key)
        {
            if (item_out != nullptr)
            {
                *item_out = item;
            }
            return i;
        }
    }
    return -1;
}

QString KeyMap::TypeToString(KeyMapItemType type)
{
    switch (type)
    {
    case KeyMapItemType::Hold:
        return QString::fromLatin1(kTypeHold);
    case KeyMapItemType::Steer:
        return QString::fromLatin1(kTypeSteer);
    case KeyMapItemType::Tap:
    default:
        return QString::fromLatin1(kTypeTap);
    }
}

KeyMapItemType KeyMap::TypeFromString(const QString &text)
{
    if (text == QString::fromLatin1(kTypeHold))
    {
        return KeyMapItemType::Hold;
    }
    if (text == QString::fromLatin1(kTypeSteer))
    {
        return KeyMapItemType::Steer;
    }
    return KeyMapItemType::Tap;
}

} // namespace qsc::input
