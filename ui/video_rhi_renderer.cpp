#include "ui/video_rhi_renderer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>

namespace qsc::ui
{

namespace
{

// 全屏四边形：4 个顶点画 TRIANGLE_STRIP，位置给裁剪空间、UV 给纹理坐标
constexpr float kQuadVertices[] = {
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
};

} // namespace

// 从 exe 旁边的 shaders/ 目录加载 QSB 着色器（构建时用 qsb.exe 编译并拷贝过去）
bool VideoRhiRenderer::EnsureShaders()
{
    if (vertex_shader_.isValid() && fragment_shader_.isValid())
    {
        return true;
    }

    const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/shaders/");
    QFile vert(base + QStringLiteral("quad.vert.qsb"));
    QFile frag(base + QStringLiteral("nv12.frag.qsb"));
    if (!vert.open(QIODevice::ReadOnly) || !frag.open(QIODevice::ReadOnly))
    {
        qWarning() << "[rhi] 打不开着色器：" << base;
        return false;
    }

    vertex_shader_ = QShader::fromSerialized(vert.readAll());
    fragment_shader_ = QShader::fromSerialized(frag.readAll());
    return vertex_shader_.isValid() && fragment_shader_.isValid();
}

// 建采样器、资源绑定、顶点缓冲和管线：只在第一帧做一次
bool VideoRhiRenderer::EnsurePipeline()
{
    if (pipeline_ != nullptr)
    {
        return true;
    }
    if (!EnsureShaders())
    {
        return false;
    }

    sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    sampler_->create();

    bindings_ = rhi_->newShaderResourceBindings();
    bindings_->create();                                                                        // 具体绑定在纹理建好后设置

    // 全屏四边形：位置 + UV，4 个顶点画 TRIANGLE_STRIP
    vertex_buffer_ = rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuadVertices));
    vertex_buffer_->create();

    pipeline_ = rhi_->newGraphicsPipeline();
    pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, vertex_shader_},
        {QRhiShaderStage::Fragment, fragment_shader_},
    });

    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    pipeline_->setVertexInputLayout(layout);
    pipeline_->setShaderResourceBindings(bindings_);
    pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setCullMode(QRhiGraphicsPipeline::None);

    if (!pipeline_->create())
    {
        qWarning() << "[rhi] 管线创建失败";
        delete pipeline_;
        pipeline_ = nullptr;
        return false;
    }
    return true;
}

// 帧尺寸变了就重建两张平面纹理：Y 是 R8，UV 是 RG8
bool VideoRhiRenderer::EnsureTextures(const QSize &size)
{
    if (texture_y_ != nullptr && texture_size_ == size)
    {
        return true;
    }

    delete texture_y_;
    delete texture_uv_;
    texture_y_ = nullptr;
    texture_uv_ = nullptr;

    texture_y_ = rhi_->newTexture(QRhiTexture::R8, size);
    const bool y_ok = texture_y_->create();

    // NV12 的 UV 是交错的：一个纹素装一对 U/V，所以宽高都是一半
    texture_uv_ = rhi_->newTexture(QRhiTexture::RG8, QSize((size.width() + 1) / 2, (size.height() + 1) / 2));
    const bool uv_ok = texture_uv_->create();

    if (!y_ok || !uv_ok)
    {
        qWarning() << "[rhi] 纹理创建失败" << size;
        return false;
    }

    bindings_->setBindings({
        QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage, texture_y_, sampler_),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture_uv_, sampler_),
    });
    bindings_->create();
    pipeline_->setShaderResourceBindings(bindings_);
    pipeline_->create();                                                                        // 绑定变了要重建管线

    texture_size_ = size;
    return true;
}

VideoRhiRenderer::VideoRhiRenderer() = default;

VideoRhiRenderer::~VideoRhiRenderer() = default;

void VideoRhiRenderer::SetSource(VideoFrameSource *source)
{
    source_ = source;
}

void VideoRhiRenderer::initialize(QRhiCommandBuffer *cb)
{
    Q_UNUSED(cb)

    rhi_ = rhi();
    // 渲染器会被重建很多次（缩放、场景图重建），这行只在第一次打，避免刷屏
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        qInfo() << "[rhi] 渲染器初始化，后端" << rhi_->backendName();
    }
}

void VideoRhiRenderer::synchronize(QQuickRhiItem *)
{
    if (source_ == nullptr)
    {
        return;
    }

    source_->NotifySyncPass();
    RenderFrameTiming timing;
    timing.sync_start_us = source_->SyncPassStartUs();

    const qsc::stream::Frame frame = source_->TakePendingFrame(&timing);
    if (!frame.IsValid())
    {
        return;
    }

    const QSize size = frame.Size();

    // Y 平面：两种像素格式都是连续的 w×h，逐行按源步长拷成紧凑排列
    plane_y_.resize(size.width() * size.height());
    for (int row = 0; row < size.height(); ++row)
    {
        memcpy(plane_y_.data() + row * size.width(),
               frame.Y() + static_cast<qsizetype>(row) * frame.StrideY(), size.width());
    }

    // UV 平面：着色器只认 NV12（U/V 交错），软解的 YUV420P 在这里合并成交错格式。
    // 这一步是纯 CPU 拼接，比换一套三平面管线简单得多，代价也只是每帧一次行拷贝。
    const int chroma_height = (size.height() + 1) / 2;
    const int chroma_width = (size.width() + 1) / 2;
    plane_uv_.resize(size.width() * chroma_height);

    if (frame.Format() == qsc::stream::PixelFormat::Nv12)
    {
        for (int row = 0; row < chroma_height; ++row)
        {
            memcpy(plane_uv_.data() + row * size.width(),
                   frame.U() + static_cast<qsizetype>(row) * frame.StrideU(), size.width());
        }
    }
    // 软解：U/V 分开存，按 2 字节一组拼成交错
    else
    {
        for (int row = 0; row < chroma_height; ++row)
        {
            const quint8 *u_row = frame.U() + static_cast<qsizetype>(row) * frame.StrideU();
            const quint8 *v_row = frame.V() + static_cast<qsizetype>(row) * frame.StrideV();
            quint8 *dst = reinterpret_cast<quint8 *>(plane_uv_.data()) + row * size.width();
            for (int col = 0; col < chroma_width; ++col)
            {
                dst[col * 2] = u_row[col];                                                      // U 在前
                dst[col * 2 + 1] = v_row[col];                                                  // V 在后
            }
        }
    }

    frame_size_ = size;
    has_frame_ = true;
}

void VideoRhiRenderer::render(QRhiCommandBuffer *cb)
{
    if (!EnsurePipeline() || source_ == nullptr)
    {
        return;
    }

    if (!vertex_uploaded_ && vertex_buffer_ != nullptr)
    {
        // beginPass 之前才能传资源：这里还没进 pass
        QRhiResourceUpdateBatch *batch = rhi_->nextResourceUpdateBatch();
        batch->uploadStaticBuffer(vertex_buffer_, kQuadVertices);
        cb->resourceUpdate(batch);
        vertex_uploaded_ = true;
    }

    if (has_frame_ && EnsureTextures(frame_size_))
    {
        QRhiResourceUpdateBatch *batch = rhi_->nextResourceUpdateBatch();
        // 紧凑平面没有行填充，整块直接传；一项 entry 对应一个 mip 层
        const QRhiTextureUploadDescription y_desc(QRhiTextureUploadEntry(
            0, 0, QRhiTextureSubresourceUploadDescription(plane_y_.constData(), plane_y_.size())));
        const QRhiTextureUploadDescription uv_desc(QRhiTextureUploadEntry(
            0, 0, QRhiTextureSubresourceUploadDescription(plane_uv_.constData(), plane_uv_.size())));
        batch->uploadTexture(texture_y_, y_desc);
        batch->uploadTexture(texture_uv_, uv_desc);
        cb->resourceUpdate(batch);
        has_frame_ = false;
    }

    const QSize target_size = renderTarget()->pixelSize();
    cb->beginPass(renderTarget(), Qt::black, {1.0f, 0});
    cb->setViewport({0.0f, 0.0f, static_cast<float>(target_size.width()),
                     static_cast<float>(target_size.height())});

    const QRhiCommandBuffer::VertexInput inputs[] = {{vertex_buffer_, 0}};
    cb->setGraphicsPipeline(pipeline_);
    cb->setShaderResources(bindings_);
    cb->setVertexInput(0, 1, inputs);
    cb->draw(4);

    cb->endPass();

    source_->NotifyRenderEnd(source_->NowUs());
}

} // namespace qsc::ui
