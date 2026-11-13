#pragma once

#include <QByteArray>
#include <QString>

// 视频那一侧要往外发消息、报状态、问连接情况；用接口隔开，避免 VideoController 反向依赖 DeviceController。
// 实现方是 DeviceController：它管着 session，知道现在连没连、串号是什么。
class ControlWriter
{
public:
    virtual ~ControlWriter() = default;

    virtual bool WriteControl(const QByteArray &message) = 0;                                   // 写进控制 socket，没连接返回 false
    virtual void ReportStatus(const QString &text) = 0;                                         // 更新主窗状态栏
    virtual bool IsConnected() const = 0;                                                       // 会话建起来没有
    virtual QString DeviceSerial() const = 0;                                                   // 当前设备串号，adb 命令要用
};
