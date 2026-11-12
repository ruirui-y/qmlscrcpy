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

// 按秒结算一行：收了多少、解了多少、推了多少、丢了多少、渲了多少、积压多少
void PerfStats::ReportAndReset()
{
    if (!enabled)
    {
        return;
    }

    const qint64 byte_count = bytes.fetchAndStoreRelaxed(0);
    const int frame_count = frames.fetchAndStoreRelaxed(0);
    const int keyframe_count = keyframes.fetchAndStoreRelaxed(0);
    const int presented_count = presented.fetchAndStoreRelaxed(0);
    const int dropped_count = dropped.fetchAndStoreRelaxed(0);
    const int render_count = renders.fetchAndStoreRelaxed(0);
    const int render_frame_count = renders_with_frame.fetchAndStoreRelaxed(0);
    const qint64 read_total = read_us.fetchAndStoreRelaxed(0);
    const qint64 decode_total = decode_us.fetchAndStoreRelaxed(0);
    const qint64 convert_total = convert_us.fetchAndStoreRelaxed(0);
    const qint64 upload_total = upload_us.fetchAndStoreRelaxed(0);
    const qint64 pipeline_last = pipeline_us.fetchAndStoreRelaxed(0);
    const qint64 pipeline_max = pipeline_max_us.fetchAndStoreRelaxed(0);
    const qint64 pipeline_sum = pipeline_sum_us.fetchAndStoreRelaxed(0);

    // 一秒钟什么都没发生就不刷屏了；渲染空转也算，所以两个都要为 0
    if (frame_count <= 0 && render_count <= 0)
    {
        qInfo() << "[perf] 这一秒没有新帧，也没有重绘";
        return;
    }

    // 每帧平均耗时按"这一秒总共花了多少 / 帧数"算，单位毫秒
    const double frame_divisor = frame_count > 0 ? frame_count : 1;
    const double pipeline_avg = render_frame_count > 0 ? (pipeline_sum / 1000.0 / render_frame_count) : 0.0;

    qInfo().nospace() << "[perf] 收 " << frame_count << " 帧 " << (byte_count / 1024) << "KB/s"
                      << " 关键帧 " << keyframe_count << " | 推 " << presented_count << " 丢 " << dropped_count
                      << " | 渲 " << render_count << "（带新帧 " << render_frame_count << "）"
                      << " | 等数据 " << (read_total / 1000.0 / frame_divisor) << "ms 解码 "
                      << (decode_total / 1000.0 / frame_divisor) << "ms 转换 "
                      << (convert_total / 1000.0 / frame_divisor) << "ms 上屏 "
                      << (upload_total / 1000.0 / frame_divisor) << "ms"
                      << " | 入队到上屏 现在 " << (pipeline_last / 1000.0) << "ms 平均 " << pipeline_avg << "ms 最大 "
                      << (pipeline_max / 1000.0) << "ms";
}

} // namespace qsc::stream
