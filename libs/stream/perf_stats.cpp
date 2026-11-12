#include "stream/perf_stats.h"

#include <QDebug>

namespace qsc::stream
{

// 全局单例，是否打印由启动时的环境变量决定
PerfStats &PerfStats::Get()
{
    static PerfStats stats;
    return stats;
}

// 按秒结算四段耗时打印一行，然后清零
void PerfStats::ReportAndReset()
{
    if (!enabled)
    {
        return;
    }

    const int frame_count = frames.fetchAndStoreRelaxed(0);
    const qint64 read_total = read_us.fetchAndStoreRelaxed(0);
    const qint64 decode_total = decode_us.fetchAndStoreRelaxed(0);
    const qint64 convert_total = convert_us.fetchAndStoreRelaxed(0);
    const qint64 upload_total = upload_us.fetchAndStoreRelaxed(0);

    if (frame_count <= 0)
    {
        qInfo() << "[perf] 这一秒没有新帧";
        return;
    }

    // 平均值按"每帧多少毫秒"报，秒里有多少帧就是这一秒的实际帧率
    qInfo() << "[perf] fps" << frame_count << "等数据" << (read_total / 1000.0 / frame_count) << "ms" << "解码" << (decode_total / 1000.0 / frame_count) << "ms"
            << "转换" << (convert_total / 1000.0 / frame_count) << "ms" << "上屏"
            << (upload_total / 1000.0 / frame_count) << "ms";
}

} // namespace qsc::stream
