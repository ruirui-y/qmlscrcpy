#include "qsc/protocol/VideoPacket.h"

#include "qsc/protocol/Wire.h"

namespace qsc::protocol {

bool VideoPacketHeader::Parse(const QByteArray &buffer, VideoPacketHeader &header)
{
    if (buffer.size() < kSize) {
        return false;
    }

    WireReader reader(buffer);
    VideoPacketHeader parsed;
    // 顺序固定：先 8 字节标志位+时间戳，再 4 字节长度
    if (!reader.ReadU64(parsed.pts_flags_) || !reader.ReadU32(parsed.size_)) {
        return false;
    }

    header = parsed;
    return true;
}

QByteArray VideoPacketHeader::Serialize() const
{
    QByteArray out;
    out.reserve(kSize);
    AppendU64(out, pts_flags_);
    AppendU32(out, size_);
    return out;
}

} // namespace qsc::protocol
