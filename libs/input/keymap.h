#pragma once

#include <QPointF>
#include <QString>
#include <QVector>

namespace qsc::input
{

// 键位类型：点一下、按住不放、摇杆四向
enum class KeyMapItemType
{
    Tap = 0,                                                                                    // 点一下就抬起，技能和平A用
    Hold = 1,                                                                                   // 按住不放，松开才抬起
    Steer = 2,                                                                                  // 摇杆：按下中心，按方向键推出去
};

// 一个键位：坐标一律归一化到 0~1，换分辨率或横竖屏都不用改配置
struct KeyMapItem
{
    KeyMapItemType type_ = KeyMapItemType::Tap;
    QString label_;                                                                             // 界面上显示的短名，比如"技能一"
    QString key_;                                                                               // tap / hold 绑定的键盘键
    QPointF pos_;                                                                               // 归一化中心坐标（0~1）
    double radius_ = 0.06;                                                                      // steer 的推动半径（归一化）
    QString up_key_;                                                                            // steer 四个方向键，默认 W/S/A/D
    QString down_key_;
    QString left_key_;
    QString right_key_;
};

// 一套键位配置：一个设备一份，存成 JSON
class KeyMap
{
public:
    QString name_ = QStringLiteral("默认");
    QVector<KeyMapItem> items_;                                                                 // 所有键位，顺序即叠加层绘制顺序

    // 读失败时返回空配置，error 里带原因；文件不存在不算错
    static KeyMap Load(const QString &path, QString *error);
    bool Save(const QString &path, QString *error) const;

    void Add(const KeyMapItem &item);
    void Remove(int index);
    void Move(int index, const QPointF &pos);                                                   // 改位置，坐标超界会被夹回 0~1
    int FindByKey(const QString &key) const;                                                    // 按键盘键找 tap/hold 下标，找不到返回 -1
    int FindBySteerKey(const QString &key, KeyMapItem *item_out) const;                         // 按方向键找 steer，找到返回下标

private:
    static QString TypeToString(KeyMapItemType type);
    static KeyMapItemType TypeFromString(const QString &text);
};

} // namespace qsc::input
