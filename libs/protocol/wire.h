#pragma once

#include <QByteArray>
#include <QtGlobal>

namespace qsc::protocol {

// 设备端按"整条消息不超过 256 KiB"分配接收缓冲区，超了直接断连接。
// 控制消息和设备消息都受这个上限约束，所以放在最底层这个头里。
inline constexpr int kMaxMessageBytes = 1 << 18;

// 协议里的整数全是大端，这里手动移位写出去。
// 不用 QDataStream：它的字节序是可改的成员状态，读代码看不出到底按哪种写的。
inline void AppendU8(QByteArray &out, quint8 value)
{
    out.append(static_cast<char>(value));
}

inline void AppendU16(QByteArray &out, quint16 value)
{
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

inline void AppendU32(QByteArray &out, quint32 value)
{
    out.append(static_cast<char>((value >> 24) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

inline void AppendU64(QByteArray &out, quint64 value)
{
    AppendU32(out, static_cast<quint32>(value >> 32));
    AppendU32(out, static_cast<quint32>(value & 0xFFFFFFFFULL));
}

// 只往前走的大端读取器。越界一次就把 ok_ 置 false，之后所有读都直接失败，
// 调用方只在最后查一次 Ok()，不用每行判错。
class WireReader
{
public:
    explicit WireReader(const QByteArray &data) : data_(data) {}

    bool ReadU8(quint8 &value)
    {
        if (!Ensure(1)) {
            return false;
        }
        value = static_cast<quint8>(data_.at(pos_));
        pos_ += 1;
        return true;
    }

    bool ReadU16(quint16 &value)
    {
        if (!Ensure(2)) {
            return false;
        }
        const auto *raw = reinterpret_cast<const quint8 *>(data_.constData() + pos_);
        value = static_cast<quint16>((raw[0] << 8) | raw[1]);
        pos_ += 2;
        return true;
    }

    bool ReadU32(quint32 &value)
    {
        if (!Ensure(4)) {
            return false;
        }
        const auto *raw = reinterpret_cast<const quint8 *>(data_.constData() + pos_);
        value = (static_cast<quint32>(raw[0]) << 24) | (static_cast<quint32>(raw[1]) << 16)
                | (static_cast<quint32>(raw[2]) << 8) | static_cast<quint32>(raw[3]);
        pos_ += 4;
        return true;
    }

    bool ReadU64(quint64 &value)
    {
        quint32 high = 0;
        quint32 low = 0;
        if (!ReadU32(high) || !ReadU32(low)) {
            return false;
        }
        value = (static_cast<quint64>(high) << 32) | low;
        return true;
    }

    bool ReadBytes(int count, QByteArray &out)
    {
        if (count < 0 || !Ensure(count)) {
            return false;
        }
        out = data_.mid(pos_, count);
        pos_ += count;
        return true;
    }

    bool Skip(int count)                                                                        // 跳过不关心的字节，比如设备信息里那 64 字节设备名
    {
        if (count < 0 || !Ensure(count)) {
            return false;
        }
        pos_ += count;
        return true;
    }

    int Remaining() const { return data_.size() - pos_; }
    int Position() const { return pos_; }
    bool Ok() const { return ok_; }

private:
    bool Ensure(int count)
    {
        if (!ok_ || count < 0 || Remaining() < count) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const QByteArray &data_;                                                                    // 被读的原始缓冲区，调用方保证它活得比本对象长
    int pos_ = 0;                                                                               // 下一个要读的字节下标
    bool ok_ = true;                                                                            // 是否已经读越界；置 false 后所有读都失败
};

} // namespace qsc::protocol
