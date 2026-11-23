#include "ui/video_rhi_renderer.h"

#include "ui/video_item.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>

#include <atomic>

namespace
{

// 4 个顶点铺满 NDC：位置 (x,y) + 纹理坐标 (u,v)
const float kQuadVertices[] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};
constexpr int kVertexStride = 4 * static_cast<int>(sizeof(float));

} // namespace

namespace
{
std::atomic<int> g_created{0};                                                                  // 累计创建了几个渲染器
std::atomic<int> g_alive{0};                                                                    // 当前活着几个
std::atomic<int> g_inits{0};                                                                    // initialize 被调了几次
}

VideoRhiRenderer::VideoRhiRenderer(VideoItem *item)
    : item_(item)
{
    qInfo() << "[rhi] 渲染器创建 累计" << ++g_created << "活着" << ++g_alive;
}

VideoRhiRenderer::~VideoRhiRenderer()
{
    ReleaseResources();
    qInfo() << "[rhi] 渲染器销毁 活着" << --g_alive;
}

// 从 exe 旁边的 shaders/ 目录读 QSB（qsb.exe 编译产物，构建时拷过去）
bool VideoRhiRenderer::EnsureShaders()
{
    if (shaders_ready_)
    {
        return true;
    }

    const QString base = QCoreApplication::applicationDirPath() + QStringLiteral("/shaders/");
    QFile vert(base + QStringLiteral("quad.vert.qsb"));
    QFile frag(base + QStringLiteral("nv12.frag.qsb"));
    if (!vert.open(QIODevice::ReadOnly))
    {
        qWarning() << "[rhi] 打不开顶点着色器" << vert.fileName();
        return false;
    }
    if (!frag.open(QIODevice::ReadOnly))
    {
        qWarning() << "[rhi] 打不开片段着色器" << frag.fileName();
        return false;
    }
    vertex_shader_ = QShader::fromSerialized(vert.readAll());
    fragment_shader_ = QShader::fromSerialized(frag.readAll());
    if (!vertex_shader_.isValid() || !fragment_shader_.isValid())
    {
        qWarning() << "[rhi] 着色器反序列化失败";
        return false;
    }
    shaders_ready_ = true;
    return true;
}

// 首次初始化：拿到 RHI、建采样器和顶点缓冲（纹理等尺寸确定后在 render 里建）
void VideoRhiRenderer::initialize(QRhiCommandBuffer *cb)
{
    if (rhi_ != nullptr && sampler_ != nullptr && vertex_buffer_ != nullptr)
    {
        return;                                                                                 // Qt 会重复调 initialize，这里只做一次
    }
    rhi_ = rhi();
    if (rhi_ == nullptr)
    {
        qWarning() << "[rhi] 没有 RHI 实例";
        return;
    }
    qInfo() << "[rhi] 后端" << rhi_->backendName() << "initialize 第" << ++g_inits << "次";

    sampler_ = rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    if (!sampler_->create())
    {
        qWarning() << "[rhi] 采样器创建失败";
    }

    vertex_buffer_ = rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(kQuadVertices));
    if (!vertex_buffer_->create())
    {
        qWarning() << "[rhi] 顶点缓冲创建失败";
        return;
    }
    QRhiResourceUpdateBatch *batch = rhi_->nextResourceUpdateBatch();
    batch->uploadStaticBuffer(vertex_buffer_, kQuadVertices);
    cb->resourceUpdate(batch);
}

// 按帧尺寸建/重建两张平面纹理，以及绑定和管线
bool VideoRhiRenderer::EnsurePipeline(const QSize &size)
{
    const QSize uv_size((size.width() + 1) / 2, (size.height() + 1) / 2);
    if (pipeline_ready_ && texture_y_ != nullptr && texture_y_->pixelSize() == size)
    {
        return true;
    }

    if (!EnsureShaders() || rhi_ == nullptr || sampler_ == nullptr)
    {
        return false;
    }

    delete pipeline_;
    pipeline_ = nullptr;
    delete bindings_;
    bindings_ = nullptr;
    delete texture_y_;
    texture_y_ = nullptr;
    delete texture_uv_;
    texture_uv_ = nullptr;

    texture_y_ = rhi_->newTexture(QRhiTexture::R8, size);
    texture_uv_ = rhi_->newTexture(QRhiTexture::RG8, uv_size);
    if (!texture_y_->create() || !texture_uv_->create())
    {
        qWarning() << "[rhi] 平面纹理创建失败" << size;
        return false;
    }

    bindings_ = rhi_->newShaderResourceBindings();
    bindings_->setBindings({
        QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage, texture_y_, sampler_),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture_uv_, sampler_),
    });
    if (!bindings_->create())
    {
        qWarning() << "[rhi] 资源绑定创建失败";
        return false;
    }

    pipeline_ = rhi_->newGraphicsPipeline();
    pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, vertex_shader_},
        {QRhiShaderStage::Fragment, fragment_shader_},
    });

    QRhiVertexInputLayout layout;
    layout.setBindings({{kVertexStride}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * static_cast<int>(sizeof(float))},
    });
    pipeline_->setVertexInputLayout(layout);
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);                                 // 4 个顶点要按三角带解释，默认 Triangles 只画 1 个
    pipeline_->setShaderResourceBindings(bindings_);
    pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    pipeline_->setSampleCount(renderTarget()->sampleCount());
    if (!pipeline_->create())
    {
        qWarning() << "[rhi] 管线创建失败";
        return false;
    }

    pipeline_ready_ = true;
    qInfo() << "[rhi] 管线就绪" << size;
    return true;
}

void VideoRhiRenderer::ReleaseResources()
{
    delete pipeline_;
    pipeline_ = nullptr;
    delete bindings_;
    bindings_ = nullptr;
    delete texture_y_;
    texture_y_ = nullptr;
    delete texture_uv_;
    texture_uv_ = nullptr;
    delete vertex_buffer_;
    vertex_buffer_ = nullptr;
    delete sampler_;
    sampler_ = nullptr;
}

// Qt 在 GUI 线程调：只做内存拷贝和状态记录，不碰 GPU
void VideoRhiRenderer::synchronize(QQuickRhiItem *item)
{
    item_ = static_cast<VideoItem *>(item);
    if (item_ == nullptr)
    {
        return;
    }

    item_->AdvanceSyncPass();
    const qint64 pass_start_us = item_->SyncStartUs();
    item_->SetPassArriveUs(0);
    item_->SetPassSyncUs(pass_start_us);

    FrameTracePtr trace;
    qint64 arrive_us = 0;
    const qsc::stream::Frame frame = item_->TakePendingFrame(&trace, &arrive_us);
    if (!frame.IsValid())
    {
        return;
    }

    const QSize size = frame.Size();
    const int chroma_height = (size.height() + 1) / 2;
    plane_y_.resize(size.width() * size.height());
    plane_uv_.resize(size.width() * chroma_height);
    for (int row = 0; row < size.height(); ++row)
    {
        memcpy(plane_y_.data() + row * size.width(), frame.Y() + row * frame.StrideY(), size.width());
    }
    for (int row = 0; row < chroma_height; ++row)
    {
        memcpy(plane_uv_.data() + row * size.width(), frame.U() + row * frame.StrideU(), size.width());
    }

    frame_size_ = size;
    has_frame_ = true;
    item_->SetPassArriveUs(arrive_us);
    if (trace)
    {
        trace->sync_us.store(item_->ClockUs() - pass_start_us);
    }
    trace_ = trace;
}

// 渲染线程：上传两平面 → 画一个铺满画布的矩形
void VideoRhiRenderer::render(QRhiCommandBuffer *cb)
{
    if (item_ == nullptr || !has_frame_ || !EnsurePipeline(frame_size_))
    {
        return;
    }

    const QSize target_size_for_log = renderTarget()->pixelSize();
    static QSize last_target;
    if (last_target != target_size_for_log)
    {
        qInfo() << "[rhi] 渲染目标尺寸" << target_size_for_log;
        last_target = target_size_for_log;
    }


    const qint64 draw_start_us = item_->ClockUs();
    // 上传必须发生在 beginPass 之前
    QRhiResourceUpdateBatch *batch = rhi_->nextResourceUpdateBatch();
    const QRhiTextureUploadDescription y_desc(QRhiTextureUploadEntry(
        0, 0, QRhiTextureSubresourceUploadDescription(plane_y_.constData(), plane_y_.size())));
    const QRhiTextureUploadDescription uv_desc(QRhiTextureUploadEntry(
        0, 0, QRhiTextureSubresourceUploadDescription(plane_uv_.constData(), plane_uv_.size())));
    batch->uploadTexture(texture_y_, y_desc);
    batch->uploadTexture(texture_uv_, uv_desc);
    cb->resourceUpdate(batch);
    has_frame_ = false;

    const QRhiCommandBuffer::VertexInput input(vertex_buffer_, 0);
    const QSize target_size = renderTarget()->pixelSize();
    cb->beginPass(renderTarget(), QColor::fromRgbF(0.0f, 0.0f, 0.0f, 1.0f), {1.0f, 0});
    cb->setGraphicsPipeline(pipeline_);
    cb->setViewport({0, 0, static_cast<float>(target_size.width()), static_cast<float>(target_size.height())});
    cb->setShaderResources(bindings_);
    cb->setVertexInput(0, 1, &input);
    cb->draw(4);
    cb->endPass();

    item_->MarkRenderEnd(draw_start_us, trace_);
}
