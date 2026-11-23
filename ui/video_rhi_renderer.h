#pragma once

// RHI 版视频渲染器：把一帧 NV12 用两张平面纹理（R8 + RG8）上传，着色器里转 RGB。
// 只保留一条路径（CPU 读回 + 上传），不碰 VideoProcessor / 原生纹理包装。

#include <QByteArray>
#include <QSize>

#include <memory>

#include <QQuickRhiItem>
#include <QtGui/private/qrhi_p.h>

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

    QShader vertex_shader_;
    QShader fragment_shader_;
    bool shaders_ready_ = false;

    QByteArray plane_y_;                                                                        // 紧凑拷贝（无行填充）
    QByteArray plane_uv_;
    QSize frame_size_;
    bool has_frame_ = false;                                                                    // 有新像素要上传
    bool pipeline_ready_ = false;
};
