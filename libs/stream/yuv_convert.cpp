#include "stream/yuv_convert.h"

#include <QtGlobal>

#include <cstring>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace qsc::stream
{

YuvConvert::YuvConvert() = default;

YuvConvert::~YuvConvert()
{
    if (rgb_frame_ != nullptr)
    {
        av_frame_free(&rgb_frame_);
        rgb_frame_ = nullptr;
    }
    if (context_ != nullptr)
    {
        sws_freeContext(context_);
        context_ = nullptr;
    }
}

// 帧尺寸变了才重建转换上下文和目标帧
bool YuvConvert::EnsureBuffers(const QSize &size)
{
    if (context_ != nullptr && size_ == size)
    {
        return true;
    }

    if (rgb_frame_ != nullptr)
    {
        av_frame_free(&rgb_frame_);
        rgb_frame_ = nullptr;
    }
    if (context_ != nullptr)
    {
        sws_freeContext(context_);
        context_ = nullptr;
    }

    context_ = sws_getContext(size.width(), size.height(), AV_PIX_FMT_YUV420P, size.width(),
                              size.height(), AV_PIX_FMT_RGB32, SWS_FAST_BILINEAR, nullptr, nullptr,
                              nullptr);
    if (context_ == nullptr)
    {
        return false;
    }

    // 目标帧像素必须 FFmpeg 自己分配：av_image_alloc 会按 SIMD 留对齐余量
    rgb_frame_ = av_frame_alloc();
    if (rgb_frame_ == nullptr)
    {
        return false;
    }
    rgb_frame_->format = AV_PIX_FMT_RGB32;
    rgb_frame_->width = size.width();
    rgb_frame_->height = size.height();
    if (av_image_alloc(rgb_frame_->data, rgb_frame_->linesize, size.width(), size.height(),
                       AV_PIX_FMT_RGB32, 32) < 0)
                       {
        av_frame_free(&rgb_frame_);
        rgb_frame_ = nullptr;
        return false;
    }

    size_ = size;
    return true;
}

// 把一帧 YUV420P 转成 RGB32 的 QImage
QImage YuvConvert::Convert(const Frame &frame)
{
    if (!frame.IsValid() || !EnsureBuffers(frame.Size()))
    {
        return QImage();
    }

    const uint8_t *src_slice[4] = {frame.Y(), frame.U(), frame.V(), nullptr};
    const int src_stride[4] = {frame.StrideY(), frame.StrideU(), frame.StrideV(), 0};

    sws_scale(context_, src_slice, src_stride, 0, size_.height(), rgb_frame_->data,
              rgb_frame_->linesize);

    // 再按行拷进 QImage：转换结果和 Qt 的像素缓冲彻底解耦，谁也别越界写对方的
    QImage image(size_, QImage::Format_RGB32);
    for (int row = 0; row < size_.height(); ++row)
    {
        std::memcpy(image.scanLine(row), rgb_frame_->data[0] + row * rgb_frame_->linesize[0],
                    static_cast<std::size_t>(size_.width()) * 4);
    }
    return image;
}

} // namespace qsc::stream
