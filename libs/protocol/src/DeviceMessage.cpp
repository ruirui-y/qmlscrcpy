#include "qsc/protocol/DeviceMessage.h"

#include "qsc/protocol/ControlMessage.h"
#include "qsc/protocol/Wire.h"

namespace qsc::protocol {

qint32 DeviceMessage::TryParse(const QByteArray &buffer, DeviceMessage &message)
{
    if (buffer.isEmpty()) {
        return 0;
    }

    WireReader reader(buffer);
    quint8 raw_type = 0;
    if (!reader.ReadU8(raw_type)) {
        return 0;
    }

    switch (static_cast<Type>(raw_type)) {
    case Type::Clipboard: {
        quint32 length = 0;
        // 长度本身没读全，或者正文还没收全，都算"再等等"
        if (!reader.ReadU32(length)) {
            return 0;
        }
        // 离谱的长度说明字节流已经错位，继续读下去只会解出垃圾
        if (length > static_cast<quint32>(kMaxMessageBytes)) {
            return -1;
        }
        if (static_cast<quint32>(reader.Remaining()) < length) {
            return 0;
        }

        QByteArray utf8;
        if (!reader.ReadBytes(static_cast<int>(length), utf8)) {
            return 0;
        }

        message.type_ = Type::Clipboard;
        message.text_ = QString::fromUtf8(utf8);
        return static_cast<qint32>(1 + 4 + length);
    }

    case Type::AckClipboard: {
        quint64 sequence = 0;
        if (!reader.ReadU64(sequence)) {
            return 0;
        }

        message.type_ = Type::AckClipboard;
        message.sequence_ = sequence;
        return static_cast<qint32>(1 + 8);
    }

    case Type::UhidOutput:
        // 明确不支持：虚拟键鼠的输出事件。收到就断开，别继续读错位的流
        return -1;
    }

    return -1;
}

} // namespace qsc::protocol
