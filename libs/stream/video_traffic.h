#pragma once

#include <QAtomicInteger>

namespace qsc::stream
{

// 视频链路的实际流量计数器：读线程每收一包加字节数，界面每秒取一次并清零。
// 只用来回答「到底跑在多少 Mbps」，不参与任何逻辑判断。
class VideoTraffic
{
public:
    static VideoTraffic &Instance();

    void AddBytes(qint64 bytes);                                                                    // 视频线程：一包（12 字节头 + 负载）收完加一次
    qint64 TakeBytes();                                                                             // GUI 线程：取走并清零，得到这一秒的字节数

private:
    QAtomicInteger<qint64> bytes_{0};
};

} // namespace qsc::stream
