#include "ui/video_item.h"

#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QTimer>

#include "base/log.h"
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
    QSize texture_sizes_[3];                                                                    // 每张纹理当前对应的平面尺寸，变了就重建
    QOpenGLShaderProgram program_;
    bool program_ready_ = false;
};

// 同步阶段：取走最新一帧拷进渲染器缓冲，这个阶段没有 GL 上下文
void YuvRenderer::synchronize(QQuickFramebufferObject *item)
{
    auto *video_item = static_cast<VideoItem *>(item);
    item_size_ = QSize(static_cast<int>(video_item->width()), static_cast<int>(video_item->height()));
    keep_aspect_ = video_item->KeepAspect();

    qsc::stream::PerfStats &perf = qsc::stream::PerfStats::Get();
    perf.renders.fetchAndAddRelaxed(1);

    const qint64 enqueue_us = video_item->PendingEnqueueUs();
    const Frame frame = video_item->TakePendingFrame();
    if (frame.IsValid())
    {
        perf.renders_with_frame.fetchAndAddRelaxed(1);

        // 帧从进进程到真正上屏花了多久：这个数和设备时钟无关，有线和无线可以直接比
        if (enqueue_us > 0)
        {
            const qint64 pipeline_us = video_item->ClockUs() - enqueue_us;
            perf.pipeline_us.storeRelaxed(pipeline_us);
            perf.pipeline_sum_us.fetchAndAddRelaxed(pipeline_us);
            if (pipeline_us > perf.pipeline_max_us.loadRelaxed())
            {
                perf.pipeline_max_us.storeRelaxed(pipeline_us);
            }
        }
    }
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
                // GL_LUMINANCE 是 GL 2.0 的原生格式，着色器里采样 .r 即可
                gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, widths_[i], heights_[i], 0,
                                 GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
                texture_sizes_[i] = QSize(widths_[i], heights_[i]);
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
    clock_.start();
    setFlag(QQuickItem::ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);                                                    // 只认左键：右键留给后面做菜单

    // 重绘由 Present 逐帧投递（到帧就画）；这个 8ms 定时器只负责尺寸通知和兜底统计
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
    emit SigKeepAspectChanged();
    update();
}

// 视频线程调：存下最新一帧，不投递任何跨线程事件
// 切换滚轮模式：scroll 发原生滚动事件，drag 模拟手指滑动
void VideoItem::SetWheelMode(const QString &mode)
{
    touch_.SetWheelMode(mode == QStringLiteral("drag") ? qsc::input::WheelMode::Drag
                                                       : qsc::input::WheelMode::Scroll);
}

// 视频线程调：存下最新一帧，等渲染线程来取
// 滑动模式的手感：一格滚轮拖多少像素
void VideoItem::SetDragPixels(int pixels)
{
    touch_.SetDragPixels(pixels);
}

// 视频线程调：存下最新一帧，等渲染线程来取
void VideoItem::Present(const Frame &frame)
{
    if (!frame.IsValid())
    {
        return;
    }

    bool replaced = false;
    {
        QMutexLocker locker(&mutex_);
        // 只留最新一帧：视频线程比渲染快的时候，旧帧直接覆盖，不排队
        replaced = has_pending_;
        pending_ = frame;
        has_pending_ = true;
        frame_size_ = frame.Size();
        pending_enqueue_us_ = clock_.nsecsElapsed() / 1000;
    }
    frames_arrived_.ref();

    // 记一笔"没被渲染就被覆盖"的帧数，这是判断渲染跟不跟得上的关键指标
    qsc::stream::PerfStats &perf = qsc::stream::PerfStats::Get();
    perf.presented.fetchAndAddRelaxed(1);
    if (replaced)
    {
        perf.dropped.fetchAndAddRelaxed(1);
    }

    // 到帧就排队请求一次重绘：视频线程直接调 update() 不安全，投递到 GUI 线程执行。
    // 以前靠 8ms 定时器轮询，120fps 时大量帧落在两个 tick 之间被覆盖。
    QMetaObject::invokeMethod(this, [this]()
    {
        update();
    }, Qt::QueuedConnection);

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

// 待渲染帧是什么时候到进程的（微秒），渲染线程用它算管道延迟
qint64 VideoItem::PendingEnqueueUs() const
{
    QMutexLocker locker(&mutex_);
    return pending_enqueue_us_;
}

// 断开连接时把还没渲染的帧丢掉
void VideoItem::Clear()
{
    {
        QMutexLocker locker(&mutex_);
        pending_ = Frame();
        has_pending_ = false;
    }
    // 断开时把按键重复计数一起清掉，下次连接不会从第 3 次重复开始
    keys_.Reset();
    fps_last_arrived_ = 0;
    frames_painted_ = 0;
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
        emit SigFrameSizeChanged();
    }

    // 重绘已经由 Present 逐帧投递，这里只兜一次"有帧但没触发"的极端情况
    const int arrived = frames_arrived_.loadAcquire();
    if (arrived != frames_painted_)
    {
        frames_painted_ = arrived;
    }
}

// 每秒结算一次"收到多少帧"（渲染帧数看 [perf] 行里的「渲」）
void VideoItem::UpdateFps()
{
    const int arrived = frames_arrived_.loadAcquire();
    // 用成员变量而不是函数内 static：断线重连时计数器归零，static 会算出一个巨大的负数
    const int frames = arrived - fps_last_arrived_;
    fps_last_arrived_ = arrived;
    if (frames == fps_)
    {
        return;
    }
    fps_ = frames;
    emit SigFpsChanged();
}

// 画布尺寸变了，重绘一次让 FBO 跟上
void VideoItem::geometryChanged(const QRectF &new_geometry, const QRectF &old_geometry)
{
    QQuickItem::geometryChanged(new_geometry, old_geometry);
    // 画布尺寸变了，FBO 也要跟着变，请一次重绘让渲染线程重新拿尺寸
    update();
}

// 坐标换算器要用最新的画布尺寸和帧尺寸，每次事件前同步一次（都是几个赋值，开销可忽略）
void VideoItem::SyncMapper()
{
    mapper_.SetFrameSize(FrameSize());
    mapper_.SetItemSize(QSizeF(width(), height()));
    mapper_.SetKeepAspect(KeepAspect());
}

// 消息为空说明点在黑边上或者键没映射，不发
void VideoItem::EmitControlMessage(const QByteArray &message)
{
    if (message.isEmpty())
    {
        qCDebug(qsc::base::lcInput) << "消息为空，丢掉（点在黑边上或键没映射）";
        return;
    }
    qCDebug(qsc::base::lcInput) << "发出消息" << message.size() << "字节" << message.left(2).toHex();
    emit SigControlMessageReady(message);
}

// 鼠标按下：先拿焦点，再换算坐标发触摸按下
void VideoItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QQuickFramebufferObject::mousePressEvent(event);
        return;
    }

    // 点了才要键盘焦点，免得一启动就把焦点抢过来
    forceActiveFocus();
    SyncMapper();
    EmitControlMessage(touch_.Press(event->localPos()));
    event->accept();
}

// 按住移动才发，否则设备会当成悬停
void VideoItem::mouseMoveEvent(QMouseEvent *event)
{
    // 只有按着左键才发移动，否则设备会当成悬停乱跳
    if ((event->buttons() & Qt::LeftButton) == 0)
    {
        QQuickFramebufferObject::mouseMoveEvent(event);
        return;
    }

    SyncMapper();
    EmitControlMessage(touch_.Move(event->localPos()));
    event->accept();
}

// 鼠标抬起：发触摸抬起
void VideoItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QQuickFramebufferObject::mouseReleaseEvent(event);
        return;
    }

    SyncMapper();
    EmitControlMessage(touch_.Release(event->localPos()));
    event->accept();
}

// 滚轮按下 120 为一格，换算成滚动消息
void VideoItem::wheelEvent(QWheelEvent *event)
{
    // 一格的 delta 是 120，除下来就是滚了几格
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0)
    {
        QQuickFramebufferObject::wheelEvent(event);
        return;
    }

    SyncMapper();
    // 拖动模式一次会出三条消息，逐条发
    const QVector<QByteArray> messages = touch_.Scroll(event->position(), notches);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }
    event->accept();
}

// 按键按下：没映射的键交回基类处理
void VideoItem::keyPressEvent(QKeyEvent *event)
{
    const QByteArray message = keys_.Press(event->key(), event->modifiers(), event->isAutoRepeat());
    if (message.isEmpty())
    {
        QQuickFramebufferObject::keyPressEvent(event);
        return;
    }
    EmitControlMessage(message);
    event->accept();
}

// 按键抬起：同时清掉这个键的重复计数
void VideoItem::keyReleaseEvent(QKeyEvent *event)
{
    const QByteArray message = keys_.Release(event->key(), event->modifiers());
    if (message.isEmpty())
    {
        QQuickFramebufferObject::keyReleaseEvent(event);
        return;
    }
    EmitControlMessage(message);
    event->accept();
}
