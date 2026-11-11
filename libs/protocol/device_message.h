#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace qsc::protocol {

// 设备端主动发过来的消息，走控制 socket 的反方向：剪贴板推送、剪贴板确认、虚拟键鼠输出。
// 解析是增量的——同一个 TCP 包里可能连着好几条，调用方按返回的消耗字节数往下切。
struct DeviceMessage
{
    enum class Type : quint8
    {
        Clipboard = 1,                                                                          // 设备剪贴板变了，内容一起推过来
        AckClipboard = 2,                                                                       // 确认收到了我们发过去的剪贴板
        UhidOutput = 3,                                                                         // 虚拟键鼠的输出事件，P4 之前不支持
    };

    Type type_ = Type::Clipboard;                                                               // 刚解出来的这条是哪一种
    QString text_ = QString();                                                                  // Clipboard 时有值，UTF-8 解出来的文本
    quint64 sequence_ = 0;                                                                      // AckClipboard 时有值，对应我们发出去的序号

    // 从缓冲区头部尝试解一条消息。
    // 返回 >0：解出一条，值是消耗掉的字节数（调用方据此从缓冲区里删掉这段）
    // 返回  0：数据还没收全，等下一次 readyRead
    // 返回 -1：协议错误（类型不认识、长度越界），连接应该断开
    static qint32 TryParse(const QByteArray &buffer, DeviceMessage &message);
};

} // namespace qsc::protocol
