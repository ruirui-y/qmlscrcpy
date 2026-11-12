#pragma once

#include <QAtomicInteger>
#include <QtGlobal>

namespace qsc::stream
{

// 链路各段的耗时和帧数累计，每秒结算一次并清零打印。
// 默认打开（QS_PERF=0 关掉），用来回答"卡在哪一段：传输、解码、还是渲染"。
struct PerfStats
{
    static PerfStats &Get();

    bool enabled = true;                                                                        // 启动时按 QS_PERF 决定，默认开
    QAtomicInteger<qint64> read_us{0};                                                          // 等 socket 数据 + 拼包（网络占用）
    QAtomicInteger<qint64> decode_us{0};                                                        // H.264 解码
    QAtomicInteger<qint64> convert_us{0};                                                       // YUV → RGB32
    QAtomicInteger<qint64> upload_us{0};                                                        // 纹理上传 + 场景图提交
    QAtomicInteger<qint64> bytes{0};                                                            // 视频 socket 收到的字节数
    QAtomicInteger<int> frames{0};                                                              // 解出多少帧
    QAtomicInteger<int> keyframes{0};                                                           // 其中关键帧（切窗口时会突然变多）
    QAtomicInteger<int> presented{0};                                                           // 推给渲染多少次
    QAtomicInteger<int> dropped{0};                                                             // 推的时候上帧还没被取走 → 被覆盖
    QAtomicInteger<int> renders{0};                                                             // 渲染线程 synchronize 次数
    QAtomicInteger<int> renders_with_frame{0};                                                  // 其中拿到了新帧的
    QAtomicInteger<qint64> pipeline_us{0};                                                      // 入队到上屏，最近一帧
    QAtomicInteger<qint64> pipeline_max_us{0};                                                  // 入队到上屏，这一秒最大
    QAtomicInteger<qint64> pipeline_sum_us{0};                                                  // 入队到上屏，累计

    void ReportAndReset();                                                                      // 读线程每秒调一次；enabled 为 false 直接返回
};

} // namespace qsc::stream
