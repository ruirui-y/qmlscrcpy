#pragma once

#include <QAtomicInteger>
#include <QtGlobal>

namespace qsc::stream
{

// 链路各段的耗时累计（微秒）和帧数，每秒结算一次并清零。
// 设 QS_PERF=1 打开（默认只累加，几乎不影响性能）。
// 用来回答"拖窗口时到底卡在哪一段"：解码、转换、还是上传。
struct PerfStats
{
    static PerfStats &Get();

    bool enabled = false;                                                                       // 启动时读环境变量决定
    QAtomicInteger<qint64> read_us{0};                                                          // 等 socket 数据 + 拼包（网络占用）
    QAtomicInteger<qint64> decode_us{0};                                                        // H.264 解码
    QAtomicInteger<qint64> convert_us{0};                                                       // YUV → RGB32
    QAtomicInteger<qint64> upload_us{0};                                                        // 纹理上传 + 场景图提交
    QAtomicInteger<int> frames{0};                                                              // 这一段里出了多少帧

    void ReportAndReset();                                                                      // 由读线程每秒调一次，enabled 为 false 时直接返回
};

} // namespace qsc::stream
