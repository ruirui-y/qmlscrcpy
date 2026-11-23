#pragma once

// 输入处理：鼠标/滚轮/键盘/捏合/键位映射/连点器。
// 状态和方法都在这个类里，画布只把事件转进来，并把控制消息转发给控制器。

#include <QByteArray>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QVariantList>

#include "input/coordinate_mapper.h"
#include "input/key_controller.h"
#include "input/keymap.h"
#include "input/keymap_engine.h"
#include "input/touch_controller.h"

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class VideoItem;

class VideoInput : public QObject
{
    Q_OBJECT

public:
    explicit VideoInput(VideoItem *owner);

    void SetWheelMode(const QString &mode);                                                     // "scroll" 发滚动事件，"drag" 模拟滑动
    void SetDragPixels(int pixels);                                                             // 滑动模式下一格滚轮拖多少像素
    void SetDragScale(double scale);                                                            // 左键拖拽倍率
    void SetPinchModifier(const QString &modifier);                                             // ctrl / alt / shift / ctrl_alt / none
    bool IsPinching() const { return pinch_active_; }

    void SetMapMode(int mode);                                                                  // 0 关 / 1 玩 / 2 编辑
    int MapMode() const { return map_mode_; }
    void SetKeyMapPath(const QString &path);
    QVariantList KeyMapItems() const;
    int AddKeyMapItem(int type, const QString &key, double x, double y, const QString &label);
    void MoveKeyMapItem(int index, double x, double y);
    void RemoveKeyMapItem(int index);
    void SetSteerKeys(int index, const QString &up, const QString &down, const QString &left, const QString &right);
    void SetClickerInterval(int ms);
    int ClickerInterval() const { return clicker_interval_ms_; }
    void BeginKeyRecord(int index);
    bool Recording() const { return record_index_ >= 0; }
    void BindRecordedKey(int qt_key);
    void SaveKeyMap();
    void ResetKeys() { keys_.Reset(); }                                                         // 断开连接时清按键重复计数

    // 事件入口（由画布转发）
    void OnMousePress(QMouseEvent *event);
    void OnMouseMove(QMouseEvent *event);
    void OnMouseRelease(QMouseEvent *event);
    void OnWheel(QWheelEvent *event);
    void OnKeyPress(QKeyEvent *event);
    void OnKeyRelease(QKeyEvent *event);

    // 定时器回调（定时器本体在画布里，逻辑在这里）
    void TickKeymap();
    void TickKeymapPoll();
    void TickClicker();
    void TickPinchWatch();

signals:
    void SigControlMessage(const QByteArray &message);                                          // 翻好的控制消息，交给控制器发出去
    void SigPinchingChanged();
    void SigMapModeChanged();
    void SigKeyMapChanged();
    void SigRecordingChanged();

private:
    bool PinchModifierHeld(Qt::KeyboardModifiers modifiers) const;
    void PinchBy(int notches, const QPointF &pos);
    void EndPinch();
    void DispatchKeyMapActions(const QVector<qsc::input::KeyMapAction> &actions);
    void SlotKeyMapPoll();                                                                      // 16ms 轮询物理按键，状态变化才发触摸
    void SyncMapper();                                                                          // 把画布尺寸、帧尺寸、缩放模式同步给坐标换算器
    void EmitControlMessage(const QByteArray &message);                                         // 空消息直接丢

    VideoItem *owner_ = nullptr;                                                               // 画布：取尺寸、发起重绘

    qsc::input::CoordinateMapper mapper_;                                                       // 窗口坐标 → 视频帧坐标
    qsc::input::TouchController touch_{&mapper_};                                               // 鼠标 → 触摸/滚轮消息
    qsc::input::KeyController keys_;                                                            // 键盘 → 按键消息

    QString pinch_modifier_ = QStringLiteral("ctrl");                                           // 当前捏合修饰键
    bool pinch_active_ = false;                                                                 // 两根手指是否按着
    QPointF pinch_center_;                                                                      // 捏合中心（画布坐标）
    int pinch_radius_ = 40;                                                                     // 两指间距的一半，单位帧像素
    qint64 pinch_last_us_ = 0;                                                                  // 上一次捏合的时刻

    qsc::input::KeyMap key_map_;                                                                // 当前设备的键位配置
    qsc::input::KeyMapEngine key_map_engine_;                                                   // 按键 → 触摸动作
    QString key_map_path_;                                                                      // config/keymap/<串号>.json
    int record_index_ = -1;                                                                     // 正在录制的键位下标
    int map_mode_ = 0;                                                                          // 0 关 / 1 玩 / 2 编辑
    QSet<QString> polled_keys_;                                                                 // 轮询到的"当前按着"的键名

    int clicker_interval_ms_ = 0;                                                               // 连点间隔，0 = 关闭
    qint64 clicker_next_us_ = 0;                                                                // 下一次该点的时刻
    QPointF clicker_pos_;                                                                       // 最近一次算出的连点位置（归一化）
};
