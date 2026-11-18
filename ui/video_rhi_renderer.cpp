#include "ui/video_rhi_renderer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>

#include <d3d11.h>

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

// 渲染器析构在渲染线程：RHI 资源必须在这里删，否则退出会报 unreleased resources
VideoRhiRenderer::~VideoRhiRenderer()
{
    delete texture_y_;
    delete texture_uv_;
    delete pipeline_;
    delete bindings_;
    delete sampler_;
    delete vertex_buffer_;
    ReleaseNativeWraps();
    delete rgba_pipeline_;
    delete rgba_bindings_;
}

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

    // D3D11 后端才有的握手：把 Qt 的设备交给解码器，硬解纹理才能被直接引用
    if (source_ != nullptr)
    {
        auto *handles = const_cast<QRhiD3D11NativeHandles *>(
            static_cast<const QRhiD3D11NativeHandles *>(rhi_->nativeHandles()));
        if (handles != nullptr && handles->dev != nullptr)
        {
            source_->ProvideRhiDevice(handles->dev, handles->context);
        }
    }
}

// 建原生纹理的直通管线：一张纹理，着色器只采样输出

// 建渲染线程独占的 VideoProcessor：设备从 RHI 借，不和视频线程共用上下文
bool VideoRhiRenderer::EnsureNativeConverter(const QSize &size)
{
    if (processor_ != nullptr && converter_size_ == size)
    {
        return true;
    }

    auto *handles = const_cast<QRhiD3D11NativeHandles *>(
        static_cast<const QRhiD3D11NativeHandles *>(rhi_->nativeHandles()));
    if (handles == nullptr || handles->dev == nullptr)
    {
        return false;
    }

    auto *device = static_cast<ID3D11Device *>(handles->dev);
    auto *context = static_cast<ID3D11DeviceContext *>(handles->context);

    ID3D11VideoDevice *video_device = nullptr;
    ID3D11VideoContext *video_context = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(&video_device)))
        || FAILED(context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void **>(&video_context))))
    {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = static_cast<UINT>(size.width());
    content.InputHeight = static_cast<UINT>(size.height());
    content.OutputWidth = static_cast<UINT>(size.width());
    content.OutputHeight = static_cast<UINT>(size.height());
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    ID3D11VideoProcessorEnumerator *processor_enum = nullptr;
    ID3D11VideoProcessor *processor = nullptr;
    if (FAILED(video_device->CreateVideoProcessorEnumerator(&content, &processor_enum))
        || FAILED(video_device->CreateVideoProcessor(processor_enum, 0, &processor)))
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = static_cast<UINT>(size.width());
    texture_desc.Height = static_cast<UINT>(size.height());
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D *output = nullptr;
    if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &output)))
    {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_desc.Texture2D.MipSlice = 0;

    ID3D11VideoProcessorOutputView *output_view = nullptr;
    if (FAILED(video_device->CreateVideoProcessorOutputView(output, processor_enum, &output_desc, &output_view)))
    {
        return false;
    }

    video_device_ = video_device;
    video_context_ = video_context;
    processor_enum_ = processor_enum;
    processor_ = processor;
    output_texture_ = output;
    output_view_ = output_view;
    converter_size_ = size;
    qInfo() << "[rhi] 视频处理器就绪" << size;
    return true;
}

// 把解码器的 NV12 纹理（按切片）转成自己的 RGBA 纹理，返回它的指针
void *VideoRhiRenderer::ConvertNativeFrame()
{
    if (processor_ == nullptr || native_ptr_ == nullptr)
    {
        return nullptr;
    }

    auto *video_device = static_cast<ID3D11VideoDevice *>(video_device_);
    auto *video_context = static_cast<ID3D11VideoContext *>(video_context_);
    auto *texture = static_cast<ID3D11Texture2D *>(native_ptr_);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.MipSlice = 0;
    input_desc.Texture2D.ArraySlice = static_cast<UINT>(native_slice_);

    ID3D11VideoProcessorInputView *input_view = nullptr;
    if (FAILED(video_device->CreateVideoProcessorInputView(
            texture, static_cast<ID3D11VideoProcessorEnumerator *>(processor_enum_), &input_desc, &input_view)))
    {
        return nullptr;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.pInputSurface = input_view;

    const HRESULT blt = video_context->VideoProcessorBlt(
        static_cast<ID3D11VideoProcessor *>(processor_),
        static_cast<ID3D11VideoProcessorOutputView *>(output_view_), 0, 1, &stream);
    input_view->Release();

    if (FAILED(blt))
    {
        return nullptr;
    }
    return output_texture_;
}

bool VideoRhiRenderer::EnsureRgbaPipeline()
{
    if (rgba_pipeline_ != nullptr)
    {
        return true;
    }

    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/shaders/rgba.frag.qsb");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        qWarning() << "[rhi] 打不开着色器" << path;
        return false;
    }
    fragment_rgba_ = QShader::fromSerialized(file.readAll());

    rgba_bindings_ = rhi_->newShaderResourceBindings();
    rgba_bindings_->create();

    rgba_pipeline_ = rhi_->newGraphicsPipeline();
    rgba_pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, vertex_shader_},
        {QRhiShaderStage::Fragment, fragment_rgba_},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    rgba_pipeline_->setVertexInputLayout(layout);
    rgba_pipeline_->setShaderResourceBindings(rgba_bindings_);
    rgba_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    rgba_pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    rgba_pipeline_->setCullMode(QRhiGraphicsPipeline::None);

    if (!rgba_pipeline_->create())
    {
        qWarning() << "[rhi] 直通管线创建失败";
        delete rgba_pipeline_;
        rgba_pipeline_ = nullptr;
        return false;
    }
    return true;
}

// 拿某个解码纹理对应的 QRhiTexture：指针没变就复用，变了才新建
QRhiTexture *VideoRhiRenderer::NativeWrapFor(void *source, const QSize &size)
{
    for (NativeWrap &entry : native_wraps_)
    {
        if (entry.source == source && entry.texture != nullptr)
        {
            return entry.texture;
        }
    }
    for (NativeWrap &entry : native_wraps_)
    {
        if (entry.texture == nullptr)
        {
            QRhiTexture *texture = rhi_->newTexture(QRhiTexture::RGBA8, size);
            QRhiTexture::NativeTexture native_src;
            native_src.object = reinterpret_cast<quint64>(source);
            native_src.layout = 0;
            if (!texture->createFrom(native_src))
            {
                delete texture;
                qWarning() << "[rhi] 原生纹理包装失败" << source;
                return nullptr;
            }
            entry.source = source;
            entry.texture = texture;
            return texture;
        }
    }
    ReleaseNativeWraps();
    return NativeWrapFor(source, size);
}

void VideoRhiRenderer::ReleaseNativeWraps()
{
    for (NativeWrap &entry : native_wraps_)
    {
        delete entry.texture;
        entry = {};
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

    // 原生纹理：解码器给的是 NV12 纹理 + 切片号，这里只记指针，转色在 render 里做
    if (frame.HasNative())
    {
        native_ptr_ = frame.Native().texture;
        native_slice_ = frame.Native().slice;
        native_size_ = frame.Size();
        has_native_ = true;
        frame_size_ = native_size_;
        return;
    }
    has_native_ = false;

    const QSize size = frame.Size();

    plane_y_.resize(size.width() * size.height());
    for (int row = 0; row < size.height(); ++row)
    {
        memcpy(plane_y_.data() + row * size.width(),
               frame.Y() + static_cast<qsizetype>(row) * frame.StrideY(), size.width());
    }

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
    else
    {
        for (int row = 0; row < chroma_height; ++row)
        {
            const quint8 *u_row = frame.U() + static_cast<qsizetype>(row) * frame.StrideU();
            const quint8 *v_row = frame.V() + static_cast<qsizetype>(row) * frame.StrideV();
            quint8 *dst = reinterpret_cast<quint8 *>(plane_uv_.data()) + row * size.width();
            for (int col = 0; col < chroma_width; ++col)
            {
                dst[col * 2] = u_row[col];
                dst[col * 2 + 1] = v_row[col];
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

    // 原生纹理分支：解码器给的是它池子里的 NV12 纹理，包成 QRhiTexture 后直接画
    if (has_native_ && native_ptr_ != nullptr && EnsureRgbaPipeline())
    {
        // 转色在渲染线程用 VideoProcessor 做：NV12 + 切片 → 自己的 RGBA 纹理
        void *rgba = EnsureNativeConverter(native_size_) ? ConvertNativeFrame() : nullptr;
        QRhiTexture *wrapped = rgba != nullptr ? NativeWrapFor(rgba, native_size_) : nullptr;
        if (wrapped != nullptr)
        {
            rgba_bindings_->setBindings({QRhiShaderResourceBinding::sampledTexture(
                0, QRhiShaderResourceBinding::FragmentStage, wrapped, sampler_)});
            rgba_bindings_->create();
            rgba_pipeline_->setShaderResourceBindings(rgba_bindings_);
            rgba_pipeline_->create();

            const QSize native_target = renderTarget()->pixelSize();
            cb->beginPass(renderTarget(), Qt::black, {1.0f, 0});
            cb->setViewport({0.0f, 0.0f, static_cast<float>(native_target.width()),
                             static_cast<float>(native_target.height())});
            const QRhiCommandBuffer::VertexInput native_inputs[] = {{vertex_buffer_, 0}};
            cb->setGraphicsPipeline(rgba_pipeline_);
            cb->setShaderResources(rgba_bindings_);
            cb->setVertexInput(0, 1, native_inputs);
            cb->draw(4);
            cb->endPass();

            source_->NotifyRenderEnd(source_->NowUs());
            return;
        }
        has_native_ = false;
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
