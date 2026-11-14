#include "stream/video_traffic.h"

namespace qsc::stream
{

VideoTraffic &VideoTraffic::Instance()
{
    static VideoTraffic instance;
    return instance;
}

// 视频线程每收一包调用，只做一次原子加
void VideoTraffic::AddBytes(qint64 bytes)
{
    bytes_.fetchAndAddRelaxed(bytes);
}

// 每秒取一次并清零；返回这一秒收到的视频字节总数
qint64 VideoTraffic::TakeBytes()
{
    return bytes_.fetchAndStoreRelaxed(0);
}

} // namespace qsc::stream
