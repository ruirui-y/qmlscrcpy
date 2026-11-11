#pragma once

#include <QImage>
#include <QSize>

#include "stream/frame.h"

struct AVFrame;
struct SwsContext;

namespace qsc::stream {

// YUV420P 转 RGB32（CPU 转）。
//
// 为什么用 CPU：GPU 三平面方案（三张纹理 + 自定义材质）在 Qt5 场景图里没有安全的
// QSGTexture 删除点 —— 材质被渲染器缓存着，删早了下一次 bind() 就是野指针。
// 先用这条路把画面跑出来，后面要省这个拷贝再换 QSGDynamicTexture 持久纹理。
class YuvConvert
{
public:
    YuvConvert();
    ~YuvConvert();

    YuvConvert(const YuvConvert &) = delete;
    YuvConvert &operator=(const YuvConvert &) = delete;

    QImage Convert(const Frame &frame);                                                         // 转一帧，返回的 QImage 自己持有像素内存

private:
    bool EnsureBuffers(const QSize &size);                                                      // 尺寸变了就重建转换上下文和目标帧

    SwsContext *context_ = nullptr;                                                             // 转换上下文，帧尺寸变了才重建
    AVFrame *rgb_frame_ = nullptr;                                                              // 目标帧，像素由 FFmpeg 自己分配（见下面注释）
    QSize size_;                                                                                // context_ 和 rgb_frame_ 对应的帧尺寸
};

} // namespace qsc::stream
