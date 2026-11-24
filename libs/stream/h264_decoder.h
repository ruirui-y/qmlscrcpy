#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include "stream/decode_probe.h"
#include "stream/frame.h"

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVD3D12VASyncContext;

namespace qsc::stream
{

// H.264 解码。输入是裸 Annex-B 数据：配置包（SPS/PPS）和数据包都往里送，
// FFmpeg 自己按 in-band 参数集解析，不用我们手工拼 extradata。
// 硬解只走 d3d12va（用画布给的 D3D12 设备），起不来就退回软解。
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
    void SetSoftThreads(int threads) { soft_threads_ = threads; }                               // 软解线程数（1~8）
    void SetExternalDevice(void *device) { external_device_ = device; }                         // 画布给的 D3D12 设备，Open 之前设
    bool Hardware() const { return hardware_; }                                                 // 这轮是不是硬解（日志用）

    // 送一个包，解出的帧追加到 frames（通常 0 或 1 个）；失败时 error 有原因
    bool Push(const QByteArray &data, bool is_config, qint64 pts_us, QVector<Frame> &frames,
              QString *error);

private:
    bool DrainFrames(QVector<Frame> &frames, QString *error);                                   // 把解码器里积压的帧全取出来
    bool OpenHardware(QString *error);                                                          // 只试 d3d12va：用画布的设备，起不来就软解
    bool OpenD3d12(QString *error);                                                             // 把画布给的设备挂到上下文上，走 d3d12va
    bool WrapHardwareFrame(AVFrame *hardware_frame, Frame &frame, QString *error);              // 硬解：把显存纹理和围栏包成 Frame
    bool WaitForDecodeFence(const AVD3D12VASyncContext &sync);                                  // 解码线程里等 GPU 写完

    AVCodecContext *context_ = nullptr;                                                         // 解码器上下文，Close 后置空
    AVPacket *packet_ = nullptr;                                                                // 复用一个包，避免每帧都分配
    AVBufferRef *hw_device_ = nullptr;                                                          // 硬解设备引用，软解时为 nullptr
    bool hardware_allowed_ = true;                                                              // 配置开关，false 时直接走软解
    bool hardware_ = false;                                                                     // 下面 Push 的分支要看它
    void *external_device_ = nullptr;                                                           // 画布给的 ID3D12Device，不持有引用
    void *fence_event_ = nullptr;                                                               // 等解码围栏用的事件对象
    QByteArray pending_config_;                                                                 // 还没拼出去的那份 SPS/PPS

    int soft_threads_ = 4;                                                                      // 软解线程数，0/1 时退化成单线程
    DecodeProbe probe_;                                                                         // 解码耗时探针（拼包/送包/收帧）
};

} // namespace qsc::stream

