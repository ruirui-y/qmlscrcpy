#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QVector>

#include "stream/decode_probe.h"
#include "stream/frame.h"

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace qsc::stream
{

struct D3d11State;                                                                              // 设备、上下文和暂存纹理，定义在 cpp 里
struct FrameBufferPool;                                                                         // 帧缓冲池：复用 5MB 的 QByteArray

// H.264 解码。输入是裸 Annex-B 数据：配置包（SPS/PPS）和数据包都往里送，
// FFmpeg 自己按 in-band 参数集解析，不用我们手工拼 extradata。
// 优先走 D3D11VA 硬解，解出的 NV12 纹理拷进自己的暂存纹理再读回内存；设备建不起来就退回软解。
class H264Decoder
{
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;

    bool Open(QString *error);                                                                  // 找 H.264 解码器并打开上下文
    void Close();                                                                               // 释放上下文，可以重复调
    void SetHardwareAllowed(bool allowed) { hardware_allowed_ = allowed; }                      // 配置里关掉硬解时传 false
    void SetAsyncReadback(bool enabled) { async_readback_ = enabled; }                          // 硬解读回：异步（双 staging）还是同步
    void SetSoftThreads(int threads) { soft_threads_ = threads; }                               // 软解线程数（1~8）
    bool Hardware() const { return hardware_; }                                                 // 这轮是不是硬解（日志用）

    // 送一个包，解出的帧追加到 frames（通常 0 或 1 个）；失败时 error 有原因
    bool Push(const QByteArray &data, bool is_config, qint64 pts_us, QVector<Frame> &frames,
              QString *error);

private:
    bool DrainFrames(QVector<Frame> &frames, QString *error);                                   // 把解码器里积压的帧全取出来
    bool OpenHardware(QString *error);                                                          // 建 D3D11 设备并挂到上下文上
    bool DownloadFrame(AVFrame *hardware_frame, Frame &frame, QString *error);                  // 拷进暂存纹理再读回内存，组装成 NV12 帧
    bool ReadStaging(int slot, unsigned int width, unsigned int height, Frame &frame, QString *error);

    AVCodecContext *context_ = nullptr;                                                         // 解码器上下文，Close 后置空
    AVPacket *packet_ = nullptr;                                                                // 复用一个包，避免每帧都分配
    AVBufferRef *hw_device_ = nullptr;                                                          // D3D11 设备引用，软解时为 nullptr
    bool hardware_allowed_ = true;                                                              // 配置开关，false 时直接走软解
    bool hardware_ = false;                                                                     // 下面 Push 的分支要看它
    QByteArray pending_config_;                                                                 // 还没拼出去的那份 SPS/PPS
    std::unique_ptr<D3d11State> d3d_;                                                           // 自己建的设备 + 复用的暂存纹理

    std::shared_ptr<FrameBufferPool> pool_;                                                     // 缓冲池（跨线程引用计数归还）
    bool async_readback_ = false;                                                               // 配置 decode/async_readback
    int soft_threads_ = 4;                                                                      // 软解线程数，0/1 时退化成单线程
    DecodeProbe probe_;                                                                         // 解码耗时探针（拼包/送包/收帧）
};

} // namespace qsc::stream
