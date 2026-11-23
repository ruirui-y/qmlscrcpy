#pragma once

#include <QMetaType>
#include <QSize>
#include <QtGlobal>

#include <memory>

namespace qsc::stream
{

// 解码输出的像素排布：软解给 YUV420P（三个平面），硬解读回是 NV12（UV 交错成一个平面），
// 零拷贝时没有 CPU 平面，只有原生纹理
enum class PixelFormat
{
    Yuv420P,                                                                                    // Y / U / V 各一个平面，色度宽高各一半
    Nv12,                                                                                       // Y 一个平面，UV 交错成第二个平面
};

// 硬解零拷贝帧的原生纹理：同一台 D3D12 设备上的 NV12 纹理，加上「解码写完了」的围栏。
// 纹理归 FFmpeg 的 DPB 池，Frame 用 owner_ 持有 AVFrame，保证它在渲染期间不被复用。
struct NativeTexture
{
    void *texture = nullptr;                                                                    // ID3D12Resource*，格式 NV12
    int subresource = 0;                                                                        // 数组层号
    void *fence = nullptr;                                                                      // ID3D12Fence*，解码什么时候写完
    quint64 fence_value = 0;                                                                    // 要等的围栏值
    int width = 0;                                                                              // 纹理宽
    int height = 0;                                                                             // 纹理高
};

// 一帧解码后的画面：平面指针 + 每行字节跨度，或者一张原生纹理。
// 平面数据归 FFmpeg 的 AVFrame 所有，这里用 shared_ptr 一起带着走，最后一个 Frame
// 销毁时才 av_frame_free。跨线程只传这个对象，不传裸指针 —— 裸指针没人负责释放，
// 上一版就是这么每帧漏 1.4 MB 的。
class Frame
{
public:
    Frame() = default;

    // owner 是底层 AVFrame 的持有者，类型擦成 const void，调用方不用认识 FFmpeg
    Frame(QSize size, PixelFormat format, const quint8 *y, const quint8 *u, const quint8 *v, int stride_y,
          int stride_u, int stride_v, std::shared_ptr<const void> owner)
        : size_(size), format_(format), y_(y), u_(u), v_(v), stride_y_(stride_y), stride_u_(stride_u),
          stride_v_(stride_v), owner_(std::move(owner))
    {
    }

    // 硬解零拷贝帧：只有原生纹理，没有 CPU 平面数据；owner 仍然要带，否则纹理会被解码器复用
    Frame(QSize size, const NativeTexture &native, std::shared_ptr<const void> owner)
        : size_(size), native_(native), owner_(std::move(owner))
    {
    }

    bool IsValid() const { return owner_ != nullptr || native_.texture != nullptr; }
    bool HasNative() const { return native_.texture != nullptr; }
    const NativeTexture &Native() const { return native_; }
    QSize Size() const { return size_; }
    PixelFormat Format() const { return format_; }

    const quint8 *Y() const { return y_; }                                                      // 亮度平面，尺寸 = Size()
    const quint8 *U() const { return u_; }                                                      // YUV420P 是 U 平面，NV12 是 UV 交错平面
    const quint8 *V() const { return v_; }                                                      // YUV420P 才用；NV12 为 nullptr

    int StrideY() const { return stride_y_; }                                                   // Y 每行字节数，可能大于宽度（对齐填充）
    int StrideU() const { return stride_u_; }
    int StrideV() const { return stride_v_; }

private:
    QSize size_;
    PixelFormat format_ = PixelFormat::Yuv420P;
    const quint8 *y_ = nullptr;
    const quint8 *u_ = nullptr;
    const quint8 *v_ = nullptr;
    int stride_y_ = 0;
    int stride_u_ = 0;
    int stride_v_ = 0;
    std::shared_ptr<const void> owner_;                                                         // 持有 AVFrame，Frame 活着数据就有效
    NativeTexture native_;                                                                      // 硬解零拷贝时这里是 NV12 纹理 + 围栏
};

} // namespace qsc::stream

// 跨线程用信号槽传 Frame 必须先注册这个类型
Q_DECLARE_METATYPE(qsc::stream::Frame)
