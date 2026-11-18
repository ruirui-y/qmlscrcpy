#pragma once

// QRhi 版渲染器：D3D11/OpenGL/Vulkan 后端都能用，不再直接调 GL。
// 第一阶段只处理 NV12（硬解 B 方案的产物）；YUV420P 三平面随后补。
// 渲染器只依赖 VideoFrameSource 接口，换画布基类时它不用改。

#include <QByteArray>
#include <QSize>

#include <QQuickRhiItem>

#include <rhi/qrhi.h>

#include "ui/video_frame_source.h"

namespace qsc::ui
{

class VideoRhiRenderer : public QQuickRhiItemRenderer
{
public:
    VideoRhiRenderer();
    ~VideoRhiRenderer() override;

    void SetSource(VideoFrameSource *source);                                                   // 画布实现，渲染线程里用

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void synchronize(QQuickRhiItem *item) override;
    void render(QRhiCommandBuffer *cb) override;

private:
    bool EnsureShaders();                                                                       // 加载 QSB 着色器
    bool EnsurePipeline();                                                                      // 建管线、采样器、顶点缓冲
    bool EnsureTextures(const QSize &size);                                                     // 建/重建两张平面纹理
    bool EnsureRgbaPipeline();
    bool EnsureNativeConverter(const QSize &size);
    void *ConvertNativeFrame();
    QRhiTexture *NativeWrapFor(void *source, const QSize &size);
    void ReleaseNativeWraps();

    VideoFrameSource *source_ = nullptr;

    QRhi *rhi_ = nullptr;
    QShader vertex_shader_;
    QShader fragment_shader_;
    QShader fragment_rgba_;
    QRhiShaderResourceBindings *bindings_ = nullptr;
    QRhiSampler *sampler_ = nullptr;
    QRhiBuffer *vertex_buffer_ = nullptr;
    QRhiGraphicsPipeline *pipeline_ = nullptr;

    QRhiTexture *texture_y_ = nullptr;                                                          // R8
    QRhiTexture *texture_uv_ = nullptr;                                                         // RG8
    QRhiTexture *native_texture_ = nullptr;
    struct NativeWrap
    {
        void *source = nullptr;
        QRhiTexture *texture = nullptr;
    };
    NativeWrap native_wraps_[4];
    void *video_device_ = nullptr;
    void *video_context_ = nullptr;
    void *processor_enum_ = nullptr;
    void *processor_ = nullptr;
    void *output_texture_ = nullptr;
    void *output_view_ = nullptr;
    QSize converter_size_;
    int native_slice_ = 0;
    QRhiGraphicsPipeline *rgba_pipeline_ = nullptr;
    QRhiShaderResourceBindings *rgba_bindings_ = nullptr;
    void *native_ptr_ = nullptr;
    QSize native_size_;
    bool has_native_ = false;
    QSize texture_size_;
    bool vertex_uploaded_ = false;                                                              // 顶点缓冲是否已上传（render 里、beginPass 之前做）

    QByteArray plane_y_;                                                                        // 紧凑的 Y 平面
    QByteArray plane_uv_;                                                                       // 紧凑的 UV 平面
    QSize frame_size_;
    bool has_frame_ = false;                                                                    // 有新帧待上传
};

} // namespace qsc::ui
