#include "ui/video_renderer.h"

#include "ui/video_item.h"

#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>

#include <GL/gl.h>

namespace qsc::ui
{

using qsc::stream::Frame;
using qsc::stream::PixelFormat;

// 顶点着色器：顶点是裁剪空间坐标，直接用
const char *const kVertexShader = R"(
attribute highp vec4 vertexCoords;
attribute highp vec2 texCoords;
varying highp vec2 uv;
void main()
{
    gl_Position = vertexCoords;
    uv = texCoords;
}
)";

// 片段着色器：三张平面纹理在 GPU 上合成 RGB（BT.601 有限范围，Android 屏幕采集的默认）
const char *const kFragmentShader = R"(
varying highp vec2 uv;
uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;
void main()
{
    mediump vec3 yuv;
    yuv.x = texture2D(texY, uv).r - 0.0625;
    yuv.y = texture2D(texU, uv).r - 0.5;
    yuv.z = texture2D(texV, uv).r - 0.5;
    mediump vec3 rgb;
    rgb.r = dot(yuv, vec3(1.164, 0.000, 1.596));
    rgb.g = dot(yuv, vec3(1.164, -0.391, -0.813));
    rgb.b = dot(yuv, vec3(1.164, 2.018, 0.000));
    gl_FragColor = vec4(rgb, 1.0);
}
)";

// 片段着色器：NV12（硬解下载下来的排布），Y 一个平面，UV 交错在另一个平面里
const char *const kNv12FragmentShader = R"(
varying highp vec2 uv;
uniform sampler2D texY;
uniform sampler2D texUV;
void main()
{
    mediump vec3 yuv;
    yuv.x = texture2D(texY, uv).r - 0.0625;
    yuv.y = texture2D(texUV, uv).r - 0.5;
    yuv.z = texture2D(texUV, uv).a - 0.5;
    mediump vec3 rgb;
    rgb.r = dot(yuv, vec3(1.164, 0.000, 1.596));
    rgb.g = dot(yuv, vec3(1.164, -0.391, -0.813));
    rgb.b = dot(yuv, vec3(1.164, 2.018, 0.000));
    gl_FragColor = vec4(rgb, 1.0);
}
)";

// 片段着色器：原生 BGRA 纹理。DXGI 的 B8G8R8A8 在 GL 里被当成 RGBA8 读，通道是反的
const char *const kBgraFragmentShader = R"(
varying highp vec2 uv;
uniform sampler2D texBgra;
void main()
{
    mediump vec4 color = texture2D(texBgra, uv);
    gl_FragColor = vec4(color.b, color.g, color.r, 1.0);
}
)";

void ResetGlState(QOpenGLFunctions *gl);                                                        // 画完把纹理单元和像素对齐还回默认值

// 把一个平面按行拷成紧凑缓冲：源每行可能有对齐填充，目标不留
void CopyPlane(QByteArray &target, const quint8 *source, int width, int height, int stride)
{
    target.resize(width * height);
    for (int row = 0; row < height; ++row)
    {
        memcpy(target.data() + row * width, source + static_cast<qsizetype>(row) * stride, width);
    }
}

// 渲染器：Qt 只在 render() 里保证 GL 上下文是当前的，所有 GL 调用都塞在这一个函数里。
// synchronize() 在渲染线程但不是 GL 回调，只做"把 GUI 那边的最新一帧拷过来"，
// 拷完就立刻松开对视频线程的依赖 —— 帧本身不跨帧持有，解码器关了也牵连不到这里。
// 同步阶段：取走最新一帧拷进渲染器缓冲，这个阶段没有 GL 上下文
void YuvRenderer::synchronize(QQuickFramebufferObject *item)
{
    auto *video_item = static_cast<VideoItem *>(item);
    item_ = video_item;
    item_size_ = QSize(static_cast<int>(video_item->width()), static_cast<int>(video_item->height()));
    keep_aspect_ = video_item->KeepAspect();

    // 两轮 synchronize 之间隔了多久，渲染线程的节奏就靠这个看
    video_item->AdvanceSyncPass();
    const qint64 pass_start_us = video_item->SyncStartUs();

    FrameTracePtr trace;
    qint64 arrive_us = 0;

    // 先清掉上一轮的到达时刻：这一轮没取到帧时，MarkSwapped 就不会把旧值算成延迟
    video_item->SetPassArriveUs(0);
    video_item->SetPassSyncUs(pass_start_us);
    const Frame frame = video_item->TakePendingFrame(&trace, &arrive_us);
    if (!frame.IsValid())
    {
        return;
    }
    video_item->SetPassArriveUs(arrive_us);

    // 零拷贝帧：只把纹理指针带到渲染线程，像素一个字节都不拷
    if (frame.HasNative())
    {
        native_pending_ = true;
        native_device_ = frame.Native().device;
        native_texture_ = frame.Native().texture;
        native_share_ = frame.Native().share_handle;
        native_size_ = frame.Size();
        frame_size_ = frame.Size();
        dirty_ = false;
        if (trace)
        {
            trace->sync_us.store(video_item->ClockUs() - pass_start_us);
        }
        return;
    }

    // 这一帧等渲染线程等了多久：拆成 update() → 渲染线程被唤醒 → Qt 同步前准备 → synchronize
    const qint64 update_done_us = trace ? trace->update_done_us.load() : 0;
    const qint64 before_sync_us = video_item->BeforeSyncUs();
    if (trace && update_done_us > 0)
    {
        if (before_sync_us > update_done_us)
        {
            trace->schedule_us.store(before_sync_us - update_done_us);
            if (pass_start_us > before_sync_us)
            {
                trace->preprocess_us.store(pass_start_us - before_sync_us);
            }
        }
        else if (pass_start_us > update_done_us)
        {
            // 没抓到信号时退回整段计时，免得这条链断掉
            trace->wake_us.store(pass_start_us - update_done_us);
        }
    }
    trace_ = trace;

    // 拷进渲染器自己的缓冲：帧只活到这一次 synchronize 结束
    const QSize size = frame.Size();
    nv12_ = frame.Format() == qsc::stream::PixelFormat::Nv12;

    if (nv12_)
    {
        // NV12：Y 整张；UV 是交错的两个通道，一个像素装一对 U/V，所以宽高都只有一半
        if (frame.Y() == nullptr || frame.U() == nullptr || size.width() <= 0 || size.height() <= 0)
        {
            return;
        }
        CopyPlane(planes_[0], frame.Y(), size.width(), size.height(), frame.StrideY());
        // 一行 UV 的字节数等于帧宽（每两个字节一对 U/V），行数是高度的一半
        CopyPlane(planes_[1], frame.U(), size.width(), (size.height() + 1) / 2, frame.StrideU());
        widths_[0] = size.width();
        heights_[0] = size.height();
        widths_[1] = (size.width() + 1) / 2;                                                    // LA 纹理：一个纹素装一对 U/V
        heights_[1] = (size.height() + 1) / 2;
        widths_[2] = 0;
        heights_[2] = 0;
    }
    else
    {
        const quint8 *data[3] = {frame.Y(), frame.U(), frame.V()};
        const int strides[3] = {frame.StrideY(), frame.StrideU(), frame.StrideV()};
        const int widths[3] = {size.width(), (size.width() + 1) / 2, (size.width() + 1) / 2};
        const int heights[3] = {size.height(), (size.height() + 1) / 2, (size.height() + 1) / 2};
        for (int i = 0; i < 3; ++i)
        {
            if (data[i] == nullptr || widths[i] <= 0 || heights[i] <= 0)
            {
                return;
            }
            CopyPlane(planes_[i], data[i], widths[i], heights[i], strides[i]);
            widths_[i] = widths[i];
            heights_[i] = heights[i];
        }
    }

    frame_size_ = size;
    dirty_ = true;

    // 这一轮 synchronize 自己花了多久（取帧 + 拷三平面）
    if (trace)
    {
        trace->sync_us.store(video_item->ClockUs() - pass_start_us);
    }
}

// 首次渲染时编译链接两套着色器：三平面 YUV420P 和两平面 NV12
void YuvRenderer::EnsureProgram()
{
    if (program_ready_)
    {
        return;
    }

    if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader))
    {
        qWarning() << "[video] 着色器编译失败:" << program_.log();
        // 失败也标记成 ready：每帧重试编译只会刷屏

    // 原生 BGRA 纹理那一套：和上面互不影响，编不过只影响零拷贝
    if (!bgra_program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !bgra_program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kBgraFragmentShader))
    {
        qWarning() << "[video] BGRA 着色器编译失败:" << bgra_program_.log();
    }
    else
    {
        bgra_program_.bindAttributeLocation("vertexCoords", 0);
        bgra_program_.bindAttributeLocation("texCoords", 1);
        if (!bgra_program_.link())
        {
            qWarning() << "[video] BGRA 着色器链接失败:" << bgra_program_.log();
        }
    }
        program_ready_ = true;
        return;
    }

    program_.bindAttributeLocation("vertexCoords", 0);
    program_.bindAttributeLocation("texCoords", 1);
    if (!program_.link())
    {
        qWarning() << "[video] 着色器链接失败:" << program_.log();
    }

    // NV12 这套只在硬解时用得上，编不过只影响硬解，不该拖累软解
    if (!nv12_program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !nv12_program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kNv12FragmentShader))
    {
        qWarning() << "[video] NV12 着色器编译失败:" << nv12_program_.log();
    }
    else
    {
        nv12_program_.bindAttributeLocation("vertexCoords", 0);
        nv12_program_.bindAttributeLocation("texCoords", 1);
        if (!nv12_program_.link())
        {
            qWarning() << "[video] NV12 着色器链接失败:" << nv12_program_.log();
        }
    }

    program_ready_ = true;
}

// 渲染线程里画一帧：上传三个平面，着色器合成后画进 FBO
void YuvRenderer::render()
{
    const qint64 draw_start_us = item_ != nullptr ? item_->ClockUs() : 0;
    QOpenGLFunctions *gl = QOpenGLContext::currentContext() != nullptr
                               ? QOpenGLContext::currentContext()->functions()
                               : nullptr;
    if (gl == nullptr)
    {
        return;
    }

    EnsureProgram();

    const QSize fbo_size = framebufferObject() != nullptr ? framebufferObject()->size() : item_size_;
    gl->glViewport(0, 0, qMax(1, fbo_size.width()), qMax(1, fbo_size.height()));
    gl->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);

    // 零拷贝：锁住 GL 注册对象 → 直接画 BGRA 纹理 → 解锁
    if (native_pending_)
    {
        native_pending_ = false;
        EnsureInterop();
        if (RegisterNative())
        {
            int slot = -1;
            for (int i = 0; i < registered_count_; ++i)
            {
                if (registered_[i].texture == native_texture_)
                {
                    slot = i;
                    break;
                }
            }
            if (slot >= 0)
            {
                void *object = registered_[slot].object;
                if (dx_lock_(interop_device_, 1, &object))
                {
                    gl->glActiveTexture(GL_TEXTURE0);
                    gl->glBindTexture(GL_TEXTURE_2D, registered_[slot].gl_texture);
                    bgra_program_.bind();
                    bgra_program_.setUniformValue("texBgra", 0);
                    draw_program_ = &bgra_program_;
                    DrawQuadRaw(gl, static_cast<float>(item_size_.width()), static_cast<float>(item_size_.height()));
                    bgra_program_.release();
                    dx_unlock_(interop_device_, 1, &object);
                    draw_program_ = &program_;
                    ResetGlState(gl);
                    if (item_ != nullptr && draw_start_us > 0)
                    {
                        item_->MarkRenderEnd(draw_start_us, trace_);
                    }
                    return;
                }
            }
        }
    }

    if (frame_size_.isEmpty() || widths_[0] <= 0)
    {
        return;
    }


    // 建纹理 + 上传：只在有像素时才做。NV12 两平面，YUV420P 三平面
    const int plane_count = nv12_ ? 2 : 3;
    if (dirty_)
    {
        for (int i = 0; i < plane_count; ++i)
        {
            // 尺寸变了（横竖屏切换）必须重建纹理，否则 glTexSubImage2D 会越界，画面直接卡住
            const bool size_changed = texture_sizes_[i] != QSize(widths_[i], heights_[i]);
            if (textures_[i] == 0 || size_changed)
            {
                if (textures_[i] != 0)
                {
                    gl->glDeleteTextures(1, &textures_[i]);
                }
                gl->glGenTextures(1, &textures_[i]);
                gl->glBindTexture(GL_TEXTURE_2D, textures_[i]);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                // NV12 的 UV 平面是交错的两个通道，用 LUMINANCE_ALPHA 一次装进 r 和 a
                const GLint internal = nv12_ && i == 1 ? GL_LUMINANCE_ALPHA : GL_LUMINANCE;
                gl->glTexImage2D(GL_TEXTURE_2D, 0, internal, widths_[i], heights_[i], 0, internal,
                                 GL_UNSIGNED_BYTE, nullptr);
                texture_sizes_[i] = QSize(widths_[i], heights_[i]);
            }
            gl->glBindTexture(GL_TEXTURE_2D, textures_[i]);
            gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            const GLenum internal = nv12_ && i == 1 ? GL_LUMINANCE_ALPHA : GL_LUMINANCE;
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, widths_[i], heights_[i], internal,
                                GL_UNSIGNED_BYTE, planes_[i].constData());
        }
        dirty_ = false;
    }

    QOpenGLShaderProgram &active = nv12_ ? nv12_program_ : program_;
    if (!active.isLinked())
    {
        return;
    }

    draw_program_ = &program_;
    active.bind();
    for (int i = 0; i < plane_count; ++i)
    {
        gl->glActiveTexture(GL_TEXTURE0 + i);
        gl->glBindTexture(GL_TEXTURE_2D, textures_[i]);
    }
    active.setUniformValue("texY", 0);
    if (nv12_)
    {
        active.setUniformValue("texUV", 1);
    }
    else
    {
        active.setUniformValue("texU", 1);
        active.setUniformValue("texV", 2);
    }

    DrawQuad(gl, static_cast<float>(item_size_.width()), static_cast<float>(item_size_.height()));

    active.release();
    ResetGlState(gl);

    // 画完立刻记一笔：缓冲交换发生在 render() 之后，那段时间由 synchronize 侧的"交换+等待"统计
    if (item_ != nullptr && draw_start_us > 0)
    {
        item_->MarkRenderEnd(draw_start_us, trace_);
    }
}

// 画一个四边形铺满画布；keepAspect 打开时按帧的宽高比留黑边
void YuvRenderer::DrawQuad(QOpenGLFunctions *gl, float width, float height)
{
    float left = -1.0f;
    float right = 1.0f;
    float top = 1.0f;
    float bottom = -1.0f;

    if (keep_aspect_ && !frame_size_.isEmpty() && width > 0.0f && height > 0.0f)
    {
        const float item_aspect = width / height;
        const float frame_aspect = static_cast<float>(frame_size_.width()) / frame_size_.height();
        if (frame_aspect > item_aspect)
        {
            // 画面比窗口更宽：上下各留一条黑边
            const float scale = item_aspect / frame_aspect;
            top = scale;
            bottom = -scale;
        }
        else
        {
            // 画面比窗口更高：左右各留一条黑边
            const float scale = frame_aspect / item_aspect;
            left = -scale;
            right = scale;
        }
    }

    // 顶点顺序（TRIANGLE_STRIP）：左上、右上、左下、右下
    const GLfloat coords[] = {
        left,  top,    0.0f, 1.0f,
        right, top,    1.0f, 1.0f,
        left,  bottom, 0.0f, 0.0f,
        right, bottom, 1.0f, 0.0f,
    };

    program_.enableAttributeArray(0);
    program_.setAttributeArray(0, coords, 2, 4 * sizeof(GLfloat));
    program_.enableAttributeArray(1);
    program_.setAttributeArray(1, coords + 2, 2, 4 * sizeof(GLfloat));

    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    program_.disableAttributeArray(0);
    program_.disableAttributeArray(1);
}

// 画原生纹理用的四边形：属性指针直接走 GL，不经过 QOpenGLShaderProgram 的封装
// （Qt6 里那层封装要求内部 QOpenGLFunctions 已初始化，程序创建失败时会直接断言）
void YuvRenderer::DrawQuadRaw(QOpenGLFunctions *gl, float width, float height)
{
    float left = -1.0f;
    float right = 1.0f;
    float top = 1.0f;
    float bottom = -1.0f;

    if (keep_aspect_ && !frame_size_.isEmpty() && width > 0.0f && height > 0.0f)
    {
        const float item_aspect = width / height;
        const float frame_aspect = static_cast<float>(frame_size_.width()) / frame_size_.height();
        if (frame_aspect > item_aspect)
        {
            const float scale = item_aspect / frame_aspect;
            top = scale;
            bottom = -scale;
        }
        else
        {
            const float scale = frame_aspect / item_aspect;
            left = -scale;
            right = scale;
        }
    }

    const GLfloat coords[] = {
        left,  top,    0.0f, 1.0f,
        right, top,    1.0f, 1.0f,
        left,  bottom, 0.0f, 0.0f,
        right, bottom, 1.0f, 0.0f,
    };

    // 属性位置在 link 前用 bindAttributeLocation 固定成 0 和 1
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), coords);
    gl->glEnableVertexAttribArray(1);
    gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), coords + 2);
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl->glDisableVertexAttribArray(0);
    gl->glDisableVertexAttribArray(1);
}

// 加载 wglDX* 扩展并打开 interop 设备：只在渲染线程调用
void YuvRenderer::EnsureInterop()
{
    if (interop_ready_ || interop_tried_)
    {
        return;
    }
    interop_tried_ = true;

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr || native_device_ == nullptr)
    {
        return;
    }

    dx_open_device_ = reinterpret_cast<PfnWglDXOpenDeviceNV>(context->getProcAddress("wglDXOpenDeviceNV"));
    dx_close_device_ = reinterpret_cast<PfnWglDXCloseDeviceNV>(context->getProcAddress("wglDXCloseDeviceNV"));
    dx_set_share_handle_ = reinterpret_cast<PfnWglDXSetResourceShareHandleNV>(
        context->getProcAddress("wglDXSetResourceShareHandleNV"));
    dx_register_ = reinterpret_cast<PfnWglDXRegisterObjectNV>(context->getProcAddress("wglDXRegisterObjectNV"));
    dx_unregister_ = reinterpret_cast<PfnWglDXUnregisterObjectNV>(context->getProcAddress("wglDXUnregisterObjectNV"));
    dx_lock_ = reinterpret_cast<PfnWglDXLockObjectsNV>(context->getProcAddress("wglDXLockObjectsNV"));
    dx_unlock_ = reinterpret_cast<PfnWglDXUnlockObjectsNV>(context->getProcAddress("wglDXUnlockObjectsNV"));

    if (dx_open_device_ == nullptr || dx_register_ == nullptr || dx_lock_ == nullptr || dx_unlock_ == nullptr)
    {
        qWarning() << "[video] 显卡驱动没提供 WGL_NV_DX_interop2，零拷贝不可用";
        return;
    }

    interop_device_ = dx_open_device_(native_device_);
    if (interop_device_ == nullptr)
    {
        qWarning() << "[video] wglDXOpenDeviceNV 失败，零拷贝不可用";
        return;
    }

    interop_ready_ = true;
    qInfo() << "[video] GL interop 已打开";
}

// 把当前帧的 D3D11 纹理注册成 GL 纹理：按纹理指针缓存，双缓冲只会注册两次
bool YuvRenderer::RegisterNative()
{
    if (!interop_ready_ || native_texture_ == nullptr || dx_set_share_handle_ == nullptr)
    {
        return false;
    }
    for (int i = 0; i < registered_count_; ++i)
    {
        if (registered_[i].texture == native_texture_)
        {
            return true;
        }
    }
    if (registered_count_ >= 4)
    {
        return false;
    }

    QOpenGLFunctions *gl = QOpenGLContext::currentContext() != nullptr
                               ? QOpenGLContext::currentContext()->functions()
                               : nullptr;
    if (gl == nullptr)
    {
        return false;
    }

    GLuint gl_texture = 0;
    gl->glGenTextures(1, &gl_texture);
    gl->glBindTexture(GL_TEXTURE_2D, gl_texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    dx_set_share_handle_(native_texture_, reinterpret_cast<HANDLE>(native_share_));
    void *object = dx_register_(interop_device_, native_texture_, gl_texture, GL_TEXTURE_2D, 0x0000);
    if (object == nullptr)
    {
        qWarning() << "[video] wglDXRegisterObjectNV 失败，纹理没法交给 GL";
        gl->glDeleteTextures(1, &gl_texture);
        return false;
    }

    registered_[registered_count_].texture = native_texture_;
    registered_[registered_count_].gl_texture = gl_texture;
    registered_[registered_count_].object = object;
    ++registered_count_;
    qInfo() << "[video] BGRA 纹理已注册给 GL，共" << registered_count_ << "张";
    return true;
}

// 把动过的 GL 状态还原：
// 我们用了 1 号、2 号纹理单元和 UNPACK_ALIGNMENT=1，不还回去的话
// Qt 接着画文字时还停在 2 号单元，采样到的是旧纹理 —— 标题就糊成一团。
void ResetGlState(QOpenGLFunctions *gl)
{
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}


} // namespace qsc::ui
