#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace qsc::protocol {

// 设备端主动发过来的消息，走控制 socket 的反方向。目前只有三种：
//
//   Clipboard    类型 1：4 字节长度 + UTF-8 文本，设备剪贴板变了会推过来
//   AckClipboard 类型 2：8 字节序号，确认它收到了我们发的剪贴板
//   UhidOutput   类型 3：虚拟键鼠的输出事件，P4 之前不处理
struct DeviceMessage
{
    enum class Type : quint8
    {
        Clipboard = 1,
        AckClipboard = 2,
        UhidOutput = 3,
    };

    Type type_ = Type::Clipboard;
    QString text_;          // Type::Clipboard 时有值
    quint64 sequence_ = 0;  // Type::AckClipboard 时有值

    // 从缓冲区头部尝试解一条消息。
    // 返回 >0：解出一条，值是消耗掉的字节数（调用方据此从缓冲区里删掉这段）
    // 返回  0：数据还没收全，等下一次 readyRead
    // 返回 -1：协议错误（类型不认识、长度越界），连接应该断开
    static qint32 TryParse(const QByteArray &buffer, DeviceMessage &message);
};

} // namespace qsc::protocol
