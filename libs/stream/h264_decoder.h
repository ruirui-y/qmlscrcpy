#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QVector>

#include "stream/frame.h"

struct AVBufferRef;
struct AVCodecContext;
struct AVPacket;

namespace qsc::stream
{

// H.264 解码。输入是裸 Annex-B 数据：配置包（SPS/PPS）和数据包都往里送，
// FFmpeg 自己按 in-band 参数集解析，不用我们手工拼 extradata。
// 优先走 D3D11VA 硬解，帧下载成 NV12 交给渲染层；设备建不起来就退回软解。
class H264Decoder
{
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;

    bool Open(QString *error);                                                                  // 找 H.264 解码器并打开上下文
    void Close();                                                                               // 释放上下文，可以重复调
    bool Hardware() const { return hardware_; }                                                 // 这轮是不是硬解（日志用）

    // 送一个包，解出的帧追加到 frames（通常 0 或 1 个）；失败时 error 有原因
    bool Push(const QByteArray &data, bool is_config, qint64 pts_us, QVector<Frame> &frames,
              QString *error);

private:
    bool DrainFrames(QVector<Frame> &frames, QString *error);                                   // 把解码器里积压的帧全取出来
    bool OpenHardware(QString *error);                                                          // 建 D3D11 设备并挂到上下文上
    void ReportDecodeTime(qint64 pack_ns, qint64 send_ns, qint64 drain_ns);                     // 每秒一行：拼包/送包/收帧/下载/合计

    AVCodecContext *context_ = nullptr;                                                         // 解码器上下文，Close 后置空
    AVPacket *packet_ = nullptr;                                                                // 复用一个包，避免每帧都分配
    AVBufferRef *hw_device_ = nullptr;                                                          // D3D11 设备引用，软解时为 nullptr
    bool hardware_ = false;                                                                     // 下面 Push 的分支要看它
    QByteArray pending_config_;                                                                 // 还没拼出去的那份 SPS/PPS

    // 解码耗时探针：QS_TRACE_DECODE=1 才记，按秒结算。硬解时 drain 里还含 GPU→CPU 的下载
    bool trace_enabled_ = false;                                                                // 环境变量开的开关
    QElapsedTimer timer_;                                                                       // 量单次 Push
    QElapsedTimer window_timer_;                                                                // 满一秒结算一次
    int stat_frames_ = 0;                                                                       // 本秒解了多少帧
    qint64 stat_pack_ns_ = 0;                                                                   // 本秒拼包累计
    qint64 stat_send_ns_ = 0;                                                                   // 本秒送包累计
    qint64 stat_drain_ns_ = 0;                                                                  // 本秒收帧累计
    qint64 stat_download_ns_ = 0;                                                               // 本秒下载累计（只有硬解有）
    qint64 stat_max_total_ns_ = 0;                                                              // 本秒单帧最慢
    qint64 drain_download_ns_ = 0;                                                              // 本次 Push 里下载花了多少
};

} // namespace qsc::stream
