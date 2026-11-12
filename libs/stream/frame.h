#pragma once

#include <QMetaType>
#include <QSize>
#include <QtGlobal>

#include <memory>

namespace qsc::stream
{

// 一帧解码后的 YUV420P 画面：三个平面指针 + 每行字节跨度。
// 平面数据归 FFmpeg 的 AVFrame 所有，这里用 shared_ptr 一起带着走，最后一个 Frame
// 销毁时才 av_frame_free。跨线程只传这个对象，不传裸指针 —— 裸指针没人负责释放，
// 上一版就是这么每帧漏 1.4 MB 的。
class Frame
{
public:
    Frame() = default;

    // owner 是底层 AVFrame 的持有者，类型擦成 const void，调用方不用认识 FFmpeg
    Frame(QSize size, const quint8 *y, const quint8 *u, const quint8 *v, int stride_y, int stride_u,
          int stride_v, std::shared_ptr<const void> owner)
        : size_(size), y_(y), u_(u), v_(v), stride_y_(stride_y), stride_u_(stride_u),
          stride_v_(stride_v), owner_(std::move(owner))
    {
    }

    bool IsValid() const { return owner_ != nullptr; }
    QSize Size() const { return size_; }

    const quint8 *Y() const { return y_; }                                                      // 亮度平面，尺寸 = Size()
    const quint8 *U() const { return u_; }                                                      // 色度 U，尺寸 = Size()/2
    const quint8 *V() const { return v_; }                                                      // 色度 V，尺寸 = Size()/2

    int StrideY() const { return stride_y_; }                                                   // Y 每行字节数，可能大于宽度（对齐填充）
    int StrideU() const { return stride_u_; }
    int StrideV() const { return stride_v_; }

private:
    QSize size_;
    const quint8 *y_ = nullptr;
    const quint8 *u_ = nullptr;
    const quint8 *v_ = nullptr;
    int stride_y_ = 0;
    int stride_u_ = 0;
    int stride_v_ = 0;
    std::shared_ptr<const void> owner_;                                                         // 持有 AVFrame，Frame 活着数据就有效
};

} // namespace qsc::stream

// 跨线程用信号槽传 Frame 必须先注册这个类型
Q_DECLARE_METATYPE(qsc::stream::Frame)
