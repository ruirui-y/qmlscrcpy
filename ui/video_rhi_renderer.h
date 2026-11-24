#pragma once

// RHI 版视频渲染器：把一帧 NV12 用两张平面纹理（R8 + RG8）上传，着色器里转 RGB。
// 只保留一条路径（CPU 读回 + 上传），不碰 VideoProcessor / 原生纹理包装。

#include <QByteArray>
#include <QSize>

#include <memory>

#include <QQuickRhiItem>
#include <QtGui/private/qrhi_p.h>

#include "stream/frame.h"
#include "ui/d3d12_nv12_convert.h"

class VideoItem;
struct FrameTrace;
using FrameTracePtr = std::shared_ptr<FrameTrace>;

class VideoRhiRenderer : public QQuickRhiItemRenderer
{
public:
    explicit VideoRhiRenderer(VideoItem *item);
    ~VideoRhiRenderer() override;

    void initialize(QRhiCommandBuffer *cb) override;
    void synchronize(QQuickRhiItem *item) override;
    void render(QRhiCommandBuffer *cb) override;

private:
    bool EnsureShaders();
    bool EnsurePipeline(const QSize &size);
    bool EnsureRgbaPipeline(const QSize &size);                                                 // 零拷贝：采样转色后的 RGBA 纹理
    void SyncNativeFrame(const qsc::stream::Frame &frame);                                       // 硬解：只存纹理结构体
    void SyncSoftwareFrame(const qsc::stream::Frame &frame);                                     // 软解：逐行拷 Y/UV 两平面
    void RenderNativeFrame(QRhiCommandBuffer *cb);                                               // 零拷贝那一趟：转色 + 画
    void RenderSoftwareFrame(QRhiCommandBuffer *cb);                                             // 软解那一趟：上传两平面 + 画
    void ReleaseResources();

    VideoItem *item_ = nullptr;                                                                 // 画布，render 里回报埋点用
    FrameTracePtr trace_;                                                                       // 本轮取走那帧的追踪记录

    QRhi *rhi_ = nullptr;
    QRhiBuffer *vertex_buffer_ = nullptr;
    QRhiSampler *sampler_ = nullptr;
    QRhiGraphicsPipeline *pipeline_ = nullptr;
    QRhiShaderResourceBindings *bindings_ = nullptr;
    QRhiTexture *texture_y_ = nullptr;                                                          // R8
    QRhiTexture *texture_uv_ = nullptr;                                                         // RG8

    // 零拷贝路径：我们自己把 NV12 转成 RGBA 写进这张 RHI 纹理，再用 rgba 管线采样它
    qsc::ui::D3d12Nv12Convert convert_;
    QRhiTexture *rgba_texture_ = nullptr;
    QRhiGraphicsPipeline *rgba_pipeline_ = nullptr;
    QRhiShaderResourceBindings *rgba_bindings_ = nullptr;
    QShader rgba_fragment_shader_;
    bool frame_has_native_ = false;
    qsc::stream::Frame native_frame_;                                                        // 本轮的零拷贝帧，owner 撑着纹理

    QShader vertex_shader_;
    QShader fragment_shader_;
    bool shaders_ready_ = false;

    QByteArray plane_y_;                                                                        // 紧凑拷贝（无行填充）
    QByteArray plane_uv_;
    QSize frame_size_;
    bool has_frame_ = false;                                                                    // 有新像素要上传
    bool pipeline_ready_ = false;
};
