#include "ui/video_item.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QTimer>

#include "stream/perf_stats.h"

using qsc::stream::Frame;

namespace
{

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

void ResetGlState(QOpenGLFunctions *gl);                                                        // 画完把纹理单元和像素对齐还回默认值

// 渲染器：Qt 只在 render() 里保证 GL 上下文是当前的，所有 GL 调用都塞在这一个函数里。
// synchronize() 在渲染线程但不是 GL 回调，只做"把 GUI 那边的最新一帧拷过来"，
// 拷完就立刻松开对视频线程的依赖 —— 帧本身不跨帧持有，解码器关了也牵连不到这里。
class YuvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    void synchronize(QQuickFramebufferObject *item) override;
    void render() override;

private:
    void EnsureProgram();
    void DrawQuad(QOpenGLFunctions *gl, float width, float height);

    QSize item_size_;                                                                           // 画布（FBO）尺寸，每次 synchronize 更新
    bool keep_aspect_ = false;

    QByteArray planes_[3];                                                                      // 三个平面的紧凑像素副本
    int widths_[3] = {0, 0, 0};
    int heights_[3] = {0, 0, 0};
    QSize frame_size_;                                                                          // 副本对应的帧尺寸
    bool dirty_ = false;                                                                        // 有没上传的新像素

    GLuint textures_[3] = {0, 0, 0};
    QOpenGLShaderProgram program_;
    bool program_ready_ = false;
};

// 同步阶段：取走最新一帧拷进渲染器缓冲，这个阶段没有 GL 上下文
void YuvRenderer::synchronize(QQuickFramebufferObject *item)
{
    auto *video_item = static_cast<VideoItem *>(item);
    item_size_ = QSize(static_cast<int>(video_item->width()), static_cast<int>(video_item->height()));
    keep_aspect_ = video_item->KeepAspect();

    const Frame frame = video_item->TakePendingFrame();
    if (!frame.IsValid())
    {
        return;
    }

    // 拷进渲染器自己的缓冲：帧只活到这一次 synchronize 结束
    const QSize size = frame.Size();
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
        planes_[i].resize(widths[i] * heights[i]);
        for (int row = 0; row < heights[i]; ++row)
        {
            memcpy(planes_[i].data() + row * widths[i],
                   data[i] + static_cast<qsizetype>(row) * strides[i], widths[i]);
        }
        widths_[i] = widths[i];
        heights_[i] = heights[i];
    }
    frame_size_ = size;
    dirty_ = true;
}

// 首次渲染时编译链接 YUV 转 RGB 的着色器
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
        program_ready_ = true;
        return;
    }

    program_.bindAttributeLocation("vertexCoords", 0);
    program_.bindAttributeLocation("texCoords", 1);
    if (!program_.link())
    {
        qWarning() << "[video] 着色器链接失败:" << program_.log();
    }
    program_ready_ = true;
}

// 渲染线程里画一帧：上传三个平面，着色器合成后画进 FBO
void YuvRenderer::render()
{
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

    if (frame_size_.isEmpty() || widths_[0] <= 0)
    {
        return;
    }

    QElapsedTimer upload_timer;
    upload_timer.start();

    // 建纹理 + 上传：只在有像素时才做
    if (dirty_)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (textures_[i] == 0)
            {
                gl->glGenTextures(1, &textures_[i]);
                gl->glBindTexture(GL_TEXTURE_2D, textures_[i]);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                // GL_LUMINANCE 是 GL 2.0 的原生格式，着色器里采样 .r 即可
                gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, widths_[i], heights_[i], 0,
                                 GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
            }
            gl->glBindTexture(GL_TEXTURE_2D, textures_[i]);
            gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, widths_[i], heights_[i], GL_LUMINANCE,
                                GL_UNSIGNED_BYTE, planes_[i].constData());
        }
        dirty_ = false;
        qsc::stream::PerfStats::Get().upload_us.fetchAndAddRelaxed(upload_timer.nsecsElapsed() / 1000);
    }

    if (!program_.isLinked())
    {
        return;
    }

    program_.bind();
    for (int i = 0; i < 3; ++i)
    {
        gl->glActiveTexture(GL_TEXTURE0 + i);
        gl->glBindTexture(GL_TEXTURE_2D, textures_[i]);
    }
    program_.setUniformValue("texY", 0);
    program_.setUniformValue("texU", 1);
    program_.setUniformValue("texV", 2);

    DrawQuad(gl, static_cast<float>(item_size_.width()), static_cast<float>(item_size_.height()));

    program_.release();
    ResetGlState(gl);
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

// 把动过的 GL 状态还原：
// 我们用了 1 号、2 号纹理单元和 UNPACK_ALIGNMENT=1，不还回去的话
// Qt 接着画文字时还停在 2 号单元，采样到的是旧纹理 —— 标题就糊成一团。
void ResetGlState(QOpenGLFunctions *gl)
{
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

} // namespace

VideoItem::VideoItem(QQuickItem *parent) : QQuickFramebufferObject(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);

    // 重绘由 GUI 线程定时器发起：视频线程只存帧，不投递事件
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(8);
    QObject::connect(refresh_timer_, &QTimer::timeout, this, &VideoItem::OnRefreshTimer);
    refresh_timer_->start();

    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(1000);
    QObject::connect(fps_timer_, &QTimer::timeout, this, &VideoItem::UpdateFps);
    fps_timer_->start();
}

QQuickFramebufferObject::Renderer *VideoItem::createRenderer() const
{
    return new YuvRenderer;
}

QSize VideoItem::FrameSize() const
{
    QMutexLocker locker(&mutex_);
    return frame_size_;
}

bool VideoItem::KeepAspect() const
{
    return keep_aspect_.load();
}

int VideoItem::Fps() const
{
    return fps_;
}

// 开关等比缩放，变了要重绘一次
void VideoItem::SetKeepAspect(bool keep_aspect)
{
    if (keep_aspect_.exchange(keep_aspect) == keep_aspect)
    {
        return;
    }
    emit KeepAspectChanged();
    update();
}

// 视频线程调：存下最新一帧，不投递任何跨线程事件
void VideoItem::Present(const Frame &frame)
{
    if (!frame.IsValid())
    {
        return;
    }

    {
        QMutexLocker locker(&mutex_);
        // 只留最新一帧：视频线程比渲染快的时候，旧帧直接覆盖，不排队
        pending_ = frame;
        has_pending_ = true;
        frame_size_ = frame.Size();
    }
    frames_arrived_.ref();

    // 这里故意什么都不投递：重绘交给 OnRefreshTimer 在 GUI 线程里发
}

// 渲染线程调：取走最新一帧，没有就返回无效帧
Frame VideoItem::TakePendingFrame()
{
    QMutexLocker locker(&mutex_);
    if (!has_pending_)
    {
        return Frame();
    }
    const Frame frame = pending_;
    pending_ = Frame();
    has_pending_ = false;
    return frame;
}

QSize VideoItem::NotifiedFrameSize() const
{
    QMutexLocker locker(&mutex_);
    return notified_size_;
}

// 断开连接时把还没渲染的帧丢掉
void VideoItem::Clear()
{
    {
        QMutexLocker locker(&mutex_);
        pending_ = Frame();
        has_pending_ = false;
    }
    update();
}

// GUI 线程每 8ms 看一眼：有新帧就请重绘，尺寸变了就通知 QML
void VideoItem::OnRefreshTimer()
{
    QSize current_size;
    {
        QMutexLocker locker(&mutex_);
        current_size = frame_size_;
    }

    // 尺寸变了（设备旋转）才通知 QML；在 GUI 线程 emit，跨线程的事一点都不做
    if (current_size != notified_size_)
    {
        notified_size_ = current_size;
        emit FrameSizeChanged();
    }

    const int arrived = frames_arrived_.loadAcquire();
    if (arrived != frames_painted_)
    {
        frames_painted_ = arrived;
        update();
    }
}

// 每秒结算一次帧率
void VideoItem::UpdateFps()
{
    static int last_arrived = 0;
    const int arrived = frames_arrived_.loadAcquire();
    const int frames = arrived - last_arrived;
    last_arrived = arrived;
    if (frames == fps_)
    {
        return;
    }
    fps_ = frames;
    emit FpsChanged();
}

// 画布尺寸变了，重绘一次让 FBO 跟上
void VideoItem::geometryChanged(const QRectF &new_geometry, const QRectF &old_geometry)
{
    QQuickItem::geometryChanged(new_geometry, old_geometry);
    // 画布尺寸变了，FBO 也要跟着变，请一次重绘让渲染线程重新拿尺寸
    update();
}
