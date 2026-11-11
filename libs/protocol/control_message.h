#pragma once

#include <QByteArray>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include "protocol/wire.h"

namespace qsc::protocol {

// 客户端发给设备的控制消息，走控制 socket。每条 = 1 字节类型 + 负载，整数全是 32 位大端。
// 类型号就是线上协议里的数字，对齐 scrcpy 3.x 的 ControlMessage.Type，顺序不能动。
enum class ControlType : quint8
{
    InjectKeycode = 0,              // 按键按下或抬起
    InjectText = 1,                 // 注一段文本
    InjectTouch = 2,                // 触摸点按下、移动、抬起
    InjectScroll = 3,               // 滚轮
    BackOrScreenOn = 4,             // 返回键；屏幕熄着时改为点亮
    ExpandNotificationPanel = 5,    // 下拉通知栏
    ExpandSettingsPanel = 6,        // 下拉快捷设置
    CollapsePanels = 7,             // 收起面板
    GetClipboard = 8,               // 向设备要剪贴板
    SetClipboard = 9,               // 设置设备剪贴板
    SetDisplayPower = 10,           // 熄屏或亮屏
    RotateDevice = 11,              // 让设备转一次画面
};

// 注文本一次最多 300 字节。注文本很慢，超长的打字体验反而更差。
inline constexpr int kMaxInjectTextBytes = 300;

// 剪贴板文本上限 = 整条消息上限 - 固定头（类型 1 + 序号 8 + paste 1 + 长度 4）
inline constexpr int kMaxClipboardBytes = kMaxMessageBytes - 14;

// 一次按键动作。action 取 0 按下、1 抬起（对应安卓 KeyEvent.ACTION_DOWN / ACTION_UP）。
struct KeycodeEvent
{
    quint32 action_ = 0;                                                                        // 0 按下，1 抬起
    quint32 keycode_ = 0;                                                                       // AndroidKeycode 数值，P4 才引入语义枚举
    quint32 repeat_ = 0;                                                                        // 长按重复次数；Qt 只给布尔，必须自己数
    quint32 meta_state_ = 0;                                                                    // 修饰键位掩码：Shift / Ctrl / Alt / Meta
};

// 一次触摸。位置是"视频帧坐标系里的像素 + 视频帧尺寸"，设备端自己换算到真实屏幕。
// 不能直接送 0..1 归一化值：设备端拿到的是 sc_position，换算全靠 screen_size。
struct TouchEvent
{
    quint8 action_ = 0;                                                                         // 0 按下，1 抬起，2 移动
    quint64 pointer_id_ = 0;                                                                    // 触摸点 id；鼠标固定用一个大值，多指才换
    QPoint position_ = QPoint(0, 0);                                                            // 视频帧坐标系里的像素位置
    QSize screen_size_ = QSize(0, 0);                                                           // 视频帧尺寸，设备端靠它换算
    float pressure_ = 0.0f;                                                                     // 0..1，按下给 1.0
    quint32 action_buttons_ = 0;                                                                // 触发本次动作的那个按键
    quint32 buttons_ = 0;                                                                       // 当前所有按键状态
};

// 一次滚轮。滚动量单位是"格"，协议里按 [-16, 16] 归一化后转定点数。
struct ScrollEvent
{
    QPoint position_ = QPoint(0, 0);                                                            // 视频帧坐标系里的像素位置
    QSize screen_size_ = QSize(0, 0);                                                           // 视频帧尺寸
    float h_scroll_ = 0.0f;                                                                     // 水平滚动量，单位"格"
    float v_scroll_ = 0.0f;                                                                     // 垂直滚动量，单位"格"
    quint32 buttons_ = 0;                                                                       // 当前按下的鼠标键
};

// 向设备取剪贴板时，是否顺手把设备上的内容"剪切"掉
enum class ClipboardAction : quint8
{
    None = 0,      // 不动设备剪贴板
    Copy = 1,      // 复制出来
    Cut = 2,       // 剪切出来
};

// 纯静态工厂：不留对象、不 new，调用方拿到的就是一段可以直接写进 socket 的字节。
// 上一版是 new 一个事件对象再 postEvent，整条链路都在处理所有权。
class ControlMessage
{
public:
    static QByteArray InjectKeycode(const KeycodeEvent &event);
    static QByteArray InjectText(const QString &text);
    static QByteArray InjectTouch(const TouchEvent &event);
    static QByteArray InjectScroll(const ScrollEvent &event);
    static QByteArray ExpandNotificationPanel();                                                // 无负载，只有 1 字节类型
    static QByteArray ExpandSettingsPanel();                                                    // 无负载
    static QByteArray CollapsePanels();                                                         // 无负载
    static QByteArray RotateDevice();                                                           // 无负载，相当于点一下旋转按钮

    // down=true 表示按下。屏幕熄灭时设备会顺手点亮，而不是执行返回
    static QByteArray BackOrScreenOn(bool down);

    static QByteArray GetClipboard(ClipboardAction action = ClipboardAction::Copy);             // 默认复制出来

    // sequence 由客户端自增，设备回 AckClipboard 时原样带回，用来确认是哪一次设置
    static QByteArray SetClipboard(quint64 sequence, bool paste, const QString &text);

    static QByteArray SetDisplayPower(bool on);                                                 // true 亮屏，false 熄屏
};

} // namespace qsc::protocol
