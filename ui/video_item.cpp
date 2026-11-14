#include "ui/video_item.h"

#include "stream/video_traffic.h"

#include <QDebug>
#include <QGuiApplication>

#include <QThread>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QTimer>
#include <QQuickWindow>
#include <windows.h>

#include "base/log.h"

// Release 里不定义这个宏，下面的每帧追踪代码整段不会被编进去

using qsc::stream::Frame;

namespace
{

// 引擎的动作枚举转成协议那边的 0 按下 / 1 抬起 / 2 移动
quint8 ActionCode(qsc::input::KeyMapAction::Kind kind)
{
    switch (kind)
    {
    case qsc::input::KeyMapAction::Kind::Up:
        return 1;
    case qsc::input::KeyMapAction::Kind::Move:
        return 2;
    case qsc::input::KeyMapAction::Kind::Down:
    default:
        return 0;
    }
}

// 这个键在系统里是否还按着：自动重复会成对发 Press/Release，靠它把假的 Release 挡掉
bool KeyPhysicallyDown(int qt_key)
{
    const int vk = static_cast<int>(qt_key);
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}
// Qt 的键值转成配置里写的文本：Q → "Q"，空格 → "Space"
QString KeyName(int qt_key)
{
    return QKeySequence(qt_key).toString(QKeySequence::PortableText);
}

// 直接问系统修饰键是否按下：Qt 的 keyboardModifiers() 靠事件更新，没新事件时会读到旧值
bool ModifierDownNow(const QString &modifier)
{
    const auto down = [](int vk) { return (::GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (modifier == QStringLiteral("none"))
    {
        return false;
    }
    if (modifier == QStringLiteral("alt"))
    {
        return down(VK_MENU);
    }
    if (modifier == QStringLiteral("shift"))
    {
        return down(VK_SHIFT);
    }
    if (modifier == QStringLiteral("ctrl_alt"))
    {
        return down(VK_CONTROL) && down(VK_MENU);
    }
    return down(VK_CONTROL);
}

// 一帧的完整事件记录：只在 Debug 编进来，发布版里连这段代码都不存在
void PrintTrace(const FrameTracePtr& trace, bool swapped, qint64 alive_us)
{
    if (!trace)
    {
        return;
    }

    const auto ms = [](qint64 us)
        {
            return us / 1000.0;
        };

    if (!swapped)
    {
        // 还没画就被后面的帧顶掉：只报到达到被覆盖活了多久
        qInfo().nospace() << "  帧 " << trace->id << " 丢掉：到达间隔 " << ms(trace->gap_us) << " ms，存活 "
            << ms(alive_us) << " ms";
        return;
    }


    qInfo().nospace() << "  帧 " << trace->id << " 上屏：到达间隔 " << ms(trace->gap_us) << "，上屏间隔 "
                      << ms(trace->swap_gap_us) << "，GUI 排队 "
        << ms(trace->queue_us.load()) << "，GUI 到渲染线程 " << ms(trace->schedule_us.load())
        << "，唤醒 " << ms(trace->schedule_us.load()) << "，预处理 " << ms(trace->preprocess_us.load()) << "，取帧 " << ms(trace->sync_us.load())
        << "，画一帧 " << ms(trace->draw_us.load()) << "，交换阻塞 " << ms(trace->swap_us.load())
        << "，从到达到上屏 "
        << ms(trace->render_end_us.load() + trace->swap_us.load() - trace->arrive_us) << " ms";
}


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
    FrameTracePtr trace_;                                                                       // 当前这一帧的事件记录，render() 回报耗时用
    VideoItem *item_ = nullptr;                                                                 // synchronize 时记住画布指针，render() 回报耗时要它
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

    // 这一轮 synchronize 自己花了多久（取帧 + 拷三平面）
    if (trace)
    {
        trace->sync_us.store(video_item->ClockUs() - pass_start_us);
    }
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

    if (frame_size_.isEmpty() || widths_[0] <= 0)
    {
        return;
    }


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

    // 重绘由 Present 逐帧投递，尺寸变化也在 Present 里通知，GUI 线程不再有 8ms 轮询定时器
    trace_enabled_ = qEnvironmentVariableIntValue("QS_TRACE_FRAMES") == 1;
    qInfo() << "[perf] 逐帧追踪" << (trace_enabled_ ? "开" : "关");

    // 每秒结算一次：到达 / 上屏 / 丢帧率 / 延迟分布
    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(1000);
    QObject::connect(fps_timer_, &QTimer::timeout, this, &VideoItem::ReportSecond);
    fps_timer_->start();

    // 摇杆心跳：部分设备会把完全静止的触摸判成别的状态，每 100ms 补一条同样的 MOVE
    keymap_tick_timer_ = new QTimer(this);
    keymap_tick_timer_->setInterval(100);
    QObject::connect(keymap_tick_timer_, &QTimer::timeout, this, [this]()
    {
        if (map_mode_ != 1)
        {
            return;
        }
        for (const qsc::input::KeyMapAction &action : key_map_engine_.Tick())
        {
            EmitControlMessage(touch_.NormalizedTouch(action.pointer_id_, ActionCode(action.kind_), action.pos_));
        }
    });
    keymap_tick_timer_->start();

    // 映射键用轮询判断按下/松开：Qt 的自动重复事件在 Windows 上不可信，成对的 Press/Release 会互相干扰
    keymap_poll_timer_ = new QTimer(this);
    keymap_poll_timer_->setInterval(16);
    QObject::connect(keymap_poll_timer_, &QTimer::timeout, this, &VideoItem::SlotKeyMapPoll);
    keymap_poll_timer_->start();

    // 松开修饰键时要抬两指，但窗口没焦点时收不到 keyRelease，所以每 250ms 问一次键盘状态兜底
    pinch_watch_timer_ = new QTimer(this);
    pinch_watch_timer_->setInterval(250);
    QObject::connect(pinch_watch_timer_, &QTimer::timeout, this, [this]()
    {
        if (pinch_active_ && !ModifierDownNow(pinch_modifier_))
        {
            EndPinch();
        }
    });
    pinch_watch_timer_->start();
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

// 左键拖拽倍率：鼠标走 1 像素，设备上的手指走 scale 像素
void VideoItem::SetDragScale(double scale)
{
    touch_.SetDragScale(scale);
}

// 视频线程调：存下最新一帧，等渲染线程来取
void VideoItem::Present(const Frame &frame)
{
    if (!frame.IsValid())
    {
        return;
    }

    const qint64 enqueue_us = clock_.nsecsElapsed() / 1000;

    // 到达间隔分档：和上屏延迟分开统计，一个看源多快、一个看我们多慢
    const qint64 prev_present_us = last_present_us_.fetchAndStoreRelaxed(enqueue_us);
    const qint64 gap_us = prev_present_us > 0 ? (enqueue_us - prev_present_us) : 0;
    if (gap_us > 0 && gap_us < 1000000)
    {
        if (gap_us < 8500)
        {
            gap_lt85_.ref();
        }
        else if (gap_us < 17000)
        {
            gap_85_17_.ref();
        }
        else if (gap_us < 34000)
        {
            gap_17_34_.ref();
        }
        else
        {
            gap_gt34_.ref();
        }
    }

    // 逐帧追踪才建 Trace；平时只有计数和两个时间戳，开销可以忽略
    FrameTracePtr trace;
    if (trace_enabled_)
    {
        trace = std::make_shared<FrameTrace>();
        trace->id = ++next_frame_id_;
        trace->arrive_us = enqueue_us;
        trace->gap_us = gap_us;
    }

    FrameTracePtr replaced_trace;
    qint64 replaced_alive_us = 0;
    bool replaced = false;
    bool size_changed = false;
    {
        QMutexLocker locker(&mutex_);
        // 只留最新一帧：视频线程比渲染快的时候，旧帧直接覆盖，不排队
        replaced = has_pending_;
        if (replaced && trace_enabled_)
        {
            replaced_trace = pending_trace_;
            replaced_alive_us = enqueue_us - pending_trace_->arrive_us;
        }
        size_changed = frame_size_ != frame.Size();
        pending_ = frame;
        pending_arrive_us_ = enqueue_us;
        if (trace_enabled_)
        {
            pending_trace_ = trace;
        }
        has_pending_ = true;
        frame_size_ = frame.Size();
    }
    frames_arrived_.ref();
    if (replaced)
    {
        frames_dropped_.ref();
    }

    // 尺寸变了（设备旋转）才通知 QML；emit 只能在 GUI 线程做，投递一次
    if (size_changed)
    {
        QMetaObject::invokeMethod(this, [this]() { NotifyFrameSize(); }, Qt::QueuedConnection);
    }

    // 被顶掉的帧当场结算：它从到达到被覆盖活了多久
    if (replaced_trace)
    {
        PrintTrace(replaced_trace, false, replaced_alive_us);
    }

    // 到帧就排队请求一次重绘：视频线程直接调 update() 不安全，投递到 GUI 线程执行
    QMetaObject::invokeMethod(this, [this, trace, enqueue_us]()
    {
        const qint64 start_us = clock_.nsecsElapsed() / 1000;
        if (trace)
        {
            trace->queue_us.store(start_us - trace->arrive_us);
        }
        update();
        if (trace)
        {
            trace->update_done_us.store(clock_.nsecsElapsed() / 1000);
        }

        // 交换缓冲发生在我们看不见的地方，接 frameSwapped 才能量到它阻塞了多久
        if (!swap_hooked_ && window() != nullptr)
        {
            QObject::connect(window(), &QQuickWindow::frameSwapped, this, [this]() { MarkSwapped(); },
                             Qt::UniqueConnection);

            // 渲染线程刚开始这一轮的时刻：它到 synchronize 之间的差就是 Qt 自己的准备工作
            QObject::connect(window(), &QQuickWindow::beforeSynchronizing, this, [this]()
            {
                before_sync_us_.storeRelaxed(clock_.nsecsElapsed() / 1000);
            }, Qt::DirectConnection);

            swap_hooked_ = true;
        }

    }, Qt::QueuedConnection);
}


// 渲染线程调：取走最新一帧（连它的到达时刻和事件记录），没有就返回无效帧
Frame VideoItem::TakePendingFrame(FrameTracePtr *trace_out, qint64 *arrive_us_out)
{
    QMutexLocker locker(&mutex_);
    if (!has_pending_)
    {
        return Frame();
    }
    const Frame frame = pending_;
    if (trace_out != nullptr)
    {
        *trace_out = pending_trace_;
    }
    if (arrive_us_out != nullptr)
    {
        *arrive_us_out = pending_arrive_us_;
    }
    pending_ = Frame();
    pending_trace_.reset();
    has_pending_ = false;
    return frame;
}

QSize VideoItem::NotifiedFrameSize() const
{
    QMutexLocker locker(&mutex_);
    return notified_size_;
}

// 记下本轮 synchronize 的起点，返回上一轮起点：两者相减就是渲染线程的轮次间隔
qint64 VideoItem::AdvanceSyncPass()
{
    const qint64 now_us = clock_.nsecsElapsed() / 1000;
    return sync_start_us_.fetchAndStoreRelaxed(now_us);
}

// 本轮 synchronize 的起始时刻
qint64 VideoItem::SyncStartUs() const
{
    return sync_start_us_.loadRelaxed();
}

// 这一帧真正提交给系统了：先记统计（永远开），开启追踪时再打整条明细
void VideoItem::MarkSwapped()
{
    const qint64 now_us = clock_.nsecsElapsed() / 1000;

    // 先把这一轮的事件记录取出来，统计和逐帧打印都用它
    FrameTracePtr trace;
    {
        QMutexLocker locker(&mutex_);
        trace = swapped_trace_;
        swapped_trace_.reset();
    }

    // 上屏间隔：相邻两次 frameSwapped 的间距，用来判断"没丢帧但看着卡"是不是抖动
    if (last_swap_us_ > 0 && now_us > last_swap_us_)
    {
        const qint64 swap_gap = now_us - last_swap_us_;
        if (trace)
        {
            trace->swap_gap_us = swap_gap;
        }
        if (swap_gap < 8500)
        {
            swap_lt85_.ref();
        }
        else if (swap_gap < 17000)
        {
            swap_85_17_.ref();
        }
        else if (swap_gap < 34000)
        {
            swap_17_34_.ref();
        }
        else
        {
            swap_gt34_.ref();
        }
    }
    last_swap_us_ = now_us;

    // 上屏延迟 = 现在 - 本轮取走那一帧的到达时刻；取完清零。
    // 两条防陈旧：帧不早于本轮 sync（真属于这一轮），且 swap 离 sync 不超过 100ms
    const qint64 arrive_us = pass_arrive_us_.fetchAndStoreRelaxed(0);
    const qint64 pass_sync_us = pass_sync_us_.loadRelaxed();
    const bool fresh = arrive_us > 0 && pass_sync_us >= arrive_us
                       && (pass_sync_us - arrive_us) < 100000
                       && (now_us - pass_sync_us) < 100000;
    if (fresh && now_us > arrive_us)
    {
        const qint64 latency_us = now_us - arrive_us;
        frames_swapped_.ref();
        if (latency_us < 1000)
        {
            lat_lt1_.ref();
        }
        else if (latency_us < 2000)
        {
            lat_1_2_.ref();
        }
        else if (latency_us < 3000)
        {
            lat_2_3_.ref();
        }
        else
        {
            lat_gt3_.ref();
        }
        if (latency_us > lat_max_us_.loadRelaxed())
        {
            lat_max_us_.storeRelaxed(latency_us);
        }
    }

    if (!trace)
    {
        return;
    }

    const qint64 render_end_us = trace->render_end_us.load();
    if (render_end_us > 0 && now_us > render_end_us)
    {
        trace->swap_us.store(now_us - render_end_us);
    }

    // 同一帧可能因为窗口重绘多跑一轮，只打第一次
    if (!trace->printed.exchange(true))
    {
        PrintTrace(trace, true, 0);
    }
}

// render() 画完时回报：整个函数跑了多久，记在这一帧自己的记录里
void VideoItem::MarkRenderEnd(qint64 start_us, const FrameTracePtr &trace)
{
    if (!trace)
    {
        return;
    }

    const qint64 end_us = clock_.nsecsElapsed() / 1000;
    trace->draw_us.store(end_us - start_us);
    trace->render_end_us.store(end_us);

    // 交给 GUI 线程的 frameSwapped：那时候才知道交换缓冲阻塞了多久
    QMutexLocker locker(&mutex_);
    swapped_trace_ = trace;
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
    update();
}

// GUI 线程：帧尺寸变了才通知 QML（由 Present 投递过来，不再轮询）
void VideoItem::NotifyFrameSize()
{
    QSize current_size;
    {
        QMutexLocker locker(&mutex_);
        current_size = frame_size_;
    }

    if (current_size != notified_size_)
    {
        notified_size_ = current_size;
        emit SigFrameSizeChanged();
    }
}

// 每秒结算一次：帧数 / 丢帧率一行，上屏延迟与到达间隔各一行
void VideoItem::ReportSecond()
{
    const int arrived = frames_arrived_.loadAcquire();
    const int frames = arrived - fps_last_arrived_;
    fps_last_arrived_ = arrived;

    const int swapped = frames_swapped_.fetchAndStoreOrdered(0);
    const int dropped = frames_dropped_.fetchAndStoreOrdered(0);
    const int l_lt1 = lat_lt1_.fetchAndStoreOrdered(0);
    const int l_12 = lat_1_2_.fetchAndStoreOrdered(0);
    const int l_23 = lat_2_3_.fetchAndStoreOrdered(0);
    const int l_gt3 = lat_gt3_.fetchAndStoreOrdered(0);
    const qint64 max_us = lat_max_us_.fetchAndStoreOrdered(0);
    const int g_lt85 = gap_lt85_.fetchAndStoreOrdered(0);
    const int g_8517 = gap_85_17_.fetchAndStoreOrdered(0);
    const int g_1734 = gap_17_34_.fetchAndStoreOrdered(0);
    const int g_gt34 = gap_gt34_.fetchAndStoreOrdered(0);
    const int w_lt85 = swap_lt85_.fetchAndStoreOrdered(0);
    const int w_8517 = swap_85_17_.fetchAndStoreOrdered(0);
    const int w_1734 = swap_17_34_.fetchAndStoreOrdered(0);
    const int w_gt34 = swap_gt34_.fetchAndStoreOrdered(0);

    if (frames != fps_)
    {
        fps_ = frames;
        emit SigFpsChanged();
    }

    // 窗口还没显示时全是"丢"，这种秒不报，避免启动噪音
    if (swapped <= 0)
    {
        return;
    }

    const double drop_rate = frames > 0 ? (100.0 * dropped / frames) : 0.0;
    const int lat_total = l_lt1 + l_12 + l_23 + l_gt3;
    const int gap_total = g_lt85 + g_8517 + g_1734 + g_gt34;

    const auto percent = [](int count, int total)
    {
        return total > 0 ? (100.0 * count / total) : 0.0;
    };
    const auto pcts = [&percent](int count, int total)
    {
        return QString::number(percent(count, total), 'f', 0);
    };

    // 实际链路流量：字节数 × 8 / 1000000 就是这一秒跑出来的 Mbps
    const qint64 bytes = qsc::stream::VideoTraffic::Instance().TakeBytes();
    const double actual_mbps = bytes * 8.0 / 1000000.0;

    qInfo().noquote() << QStringLiteral("[perf] 1s 到达 %1 帧，上屏 %2 帧，丢 %3（%4%），实际 %5 KB/s（%6 Mbps）")
                             .arg(frames)
                             .arg(swapped)
                             .arg(dropped)
                             .arg(QString::number(drop_rate, 'f', 1))
                             .arg(bytes / 1024)
                             .arg(QString::number(actual_mbps, 'f', 1));

    qInfo().noquote() << QStringLiteral("[perf]     上屏延迟 <1.5ms %1% / 1.5~2.5ms %2% / 2.5~3.5ms %3% / >3.5ms %4%，最大 %5 ms")
                             .arg(pcts(l_lt1, lat_total))
                             .arg(pcts(l_12, lat_total))
                             .arg(pcts(l_23, lat_total))
                             .arg(pcts(l_gt3, lat_total))
                             .arg(QString::number(max_us / 1000.0, 'f', 1));

    qInfo().noquote() << QStringLiteral("[perf]     到达间隔 <8.5ms %1% / 8.5~17ms %2% / 17~34ms %3% / >34ms %4%")
                             .arg(pcts(g_lt85, gap_total))
                             .arg(pcts(g_8517, gap_total))
                             .arg(pcts(g_1734, gap_total))
                             .arg(pcts(g_gt34, gap_total));

    const int swap_total = w_lt85 + w_8517 + w_1734 + w_gt34;
    qInfo().noquote() << QStringLiteral("[perf]     上屏间隔 <8.5ms %1% / 8.5~17ms %2% / 17~34ms %3% / >34ms %4%")
                             .arg(pcts(w_lt85, swap_total))
                             .arg(pcts(w_8517, swap_total))
                             .arg(pcts(w_1734, swap_total))
                             .arg(pcts(w_gt34, swap_total));
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

    // 按住修饰键滚轮 = 双指捏合：上滚放大、下滚缩小，中心跟着鼠标
    if (PinchModifierHeld(event->modifiers()))
    {
        PinchBy(notches, event->position());
        event->accept();
        return;
    }
    // 拖动模式一次会出三条消息，逐条发

    // 没按修饰键就滚：先把两指抬起，否则设备会把滚动当成继续缩放
    if (pinch_active_)
    {
        const QVector<QByteArray> ending = touch_.PinchEnd(pinch_center_, pinch_radius_);
        for (const QByteArray &message : ending)
        {
            EmitControlMessage(message);
        }
        pinch_active_ = false;
    }
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
    // 玩模式：已映射的键转成触摸，不再作为按键转发
    if (map_mode_ == 1 && key_map_engine_.Handles(KeyName(event->key())))
    {
        // 映射键全部交给 16ms 轮询处理，这里只负责吞掉，避免 Qt 的自动重复事件干扰
        event->accept();
        return;

    }
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
    // 松开后捏合键已经不在按下状态就抬两指；其它按键的松开不影响捏合
    if (pinch_active_ && !ModifierDownNow(pinch_modifier_))
    {
        EndPinch();
    }

    if (map_mode_ == 1 && key_map_engine_.Handles(KeyName(event->key())))
    {
        // 映射键的抬起同样交给轮询，这里只吞掉
        event->accept();
        return;
    }
    const QByteArray message = keys_.Release(event->key(), event->modifiers());
    if (message.isEmpty())
    {
        QQuickFramebufferObject::keyReleaseEvent(event);
        return;
    }
    EmitControlMessage(message);
    event->accept();
}

// 当前按下的修饰键是不是配置里那个捏合键
bool VideoItem::PinchModifierHeld(Qt::KeyboardModifiers modifiers) const
{
    if (pinch_modifier_ == QStringLiteral("none"))
    {
        return false;
    }
    if (pinch_modifier_ == QStringLiteral("alt"))
    {
        return modifiers.testFlag(Qt::AltModifier);
    }
    if (pinch_modifier_ == QStringLiteral("shift"))
    {
        return modifiers.testFlag(Qt::ShiftModifier);
    }
    if (pinch_modifier_ == QStringLiteral("ctrl_alt"))
    {
        return modifiers.testFlag(Qt::ControlModifier) && modifiers.testFlag(Qt::AltModifier);
    }
    return modifiers.testFlag(Qt::ControlModifier);
}

// 一格滚轮把两指间距放大或缩小 12%，第一次滚先把两指按下去
void VideoItem::PinchBy(int notches, const QPointF &pos)
{
    pinch_center_ = pos;

    // 半径上限是画面短边的 45%，再大会把第二根手指顶出屏幕
    const QSize frame = FrameSize();
    const int limit = qMax(20, qMin(frame.width(), frame.height()) * 50 / 100);

    if (!pinch_active_)
    {
        // 半径跨手势保留：滚得慢时每格都会新开一次手势，若每次重置回 40，
        // 上一格放大的效果就丢了，看起来像"每一格都在放大"
        const qint64 now_us = clock_.nsecsElapsed() / 1000;
        if (pinch_last_us_ == 0 || (now_us - pinch_last_us_) > 2000000)
        {
            pinch_radius_ = 40;
        }
        pinch_last_us_ = now_us;
        pinch_active_ = true;
        emit SigPinchingChanged();
        const QVector<QByteArray> messages = touch_.PinchBegin(pinch_center_, pinch_radius_);
        for (const QByteArray &message : messages)
        {
            EmitControlMessage(message);
        }
    }

    // 一格 15%：12% 感觉滚几下就没变化了
    const double scaled = static_cast<double>(pinch_radius_) * std::pow(1.15, notches);
    pinch_radius_ = qBound(20, static_cast<int>(scaled), limit);

    const QVector<QByteArray> messages = touch_.PinchStep(pinch_center_, pinch_radius_);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }
}

// 捏合修饰键：ctrl / alt / shift / ctrl_alt / none
void VideoItem::SetPinchModifier(const QString &modifier)
{
    pinch_modifier_ = modifier.trimmed().toLower();
}

// 抬起两指：修饰键松开时调用
void VideoItem::EndPinch()
{
    if (!pinch_active_)
    {
        return;
    }

    const QVector<QByteArray> messages = touch_.PinchEnd(pinch_center_, pinch_radius_);
    for (const QByteArray &message : messages)
    {
        EmitControlMessage(message);
    }
    pinch_active_ = false;
    emit SigPinchingChanged();
}

// 切映射模式：从"玩/编辑"退出时要把所有按住的手指抬起来，否则设备会一直按着
void VideoItem::SetMapMode(int mode)
{
    const int clamped = qBound(0, mode, 2);
    if (clamped == map_mode_)
    {
        return;
    }

    if (map_mode_ != 0)
    {
        for (const qsc::input::KeyMapAction &action : key_map_engine_.Reset())
        {
            EmitControlMessage(touch_.NormalizedTouch(action.pointer_id_, ActionCode(action.kind_), action.pos_));
        }
    }

    map_mode_ = clamped;
    if (map_mode_ == 2)
    {
        emit SigKeyMapChanged();
    }
    emit SigMapModeChanged();
}

// 换设备：读这台设备自己的键位文件，读不到就是空配置
void VideoItem::SetKeyMapPath(const QString &path)
{
    key_map_path_ = path;
    QString error;
    key_map_ = qsc::input::KeyMap::Load(path, &error);
    if (!error.isEmpty())
    {
        qWarning() << "[keymap]" << error;
    }
    key_map_engine_.SetMap(key_map_);
    qInfo() << "[keymap] 已加载" << key_map_.items_.size() << "个键位：" << path;
    emit SigKeyMapChanged();
}

// 给 QML 叠加层用的键位快照：位置已经换算成 0~1，界面直接按比例摆
QVariantList VideoItem::KeyMapItems() const
{
    QVariantList list;
    for (int i = 0; i < key_map_.items_.size(); ++i)
    {
        const qsc::input::KeyMapItem &item = key_map_.items_.at(i);
        QVariantMap entry;
        entry.insert(QStringLiteral("index"), i);
        entry.insert(QStringLiteral("type"), static_cast<int>(item.type_));
        entry.insert(QStringLiteral("label"), item.label_);
        entry.insert(QStringLiteral("key"), item.key_);
        entry.insert(QStringLiteral("x"), item.pos_.x());
        entry.insert(QStringLiteral("y"), item.pos_.y());
        entry.insert(QStringLiteral("radius"), item.radius_);
        entry.insert(QStringLiteral("up"), item.up_key_);
        entry.insert(QStringLiteral("down"), item.down_key_);
        entry.insert(QStringLiteral("left"), item.left_key_);
        entry.insert(QStringLiteral("right"), item.right_key_);
        list.append(entry);
    }
    return list;
}

// 新增一条键位，返回它的下标；type 0 点按 1 按住 2 摇杆
int VideoItem::AddKeyMapItem(int type, const QString &key, double x, double y, const QString &label)
{
    qsc::input::KeyMapItem item;
    item.type_ = static_cast<qsc::input::KeyMapItemType>(qBound(0, type, 2));
    item.key_ = key;
    item.label_ = label;
    item.pos_ = QPointF(x, y);
    key_map_.Add(item);
    key_map_engine_.SetMap(key_map_);

    const int index = key_map_.items_.size() - 1;
    emit SigKeyMapChanged();
    return index;
}

void VideoItem::MoveKeyMapItem(int index, double x, double y)
{
    key_map_.Move(index, QPointF(x, y));
    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

void VideoItem::RemoveKeyMapItem(int index)
{
    key_map_.Remove(index);
    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

// 写回 JSON：目录不存在时先建
void VideoItem::SaveKeyMap()
{
    if (key_map_path_.isEmpty())
    {
        return;
    }
    const QFileInfo info(key_map_path_);
    QDir().mkpath(info.absolutePath());
    QString error;
    if (!key_map_.Save(key_map_path_, &error))
    {
        qWarning() << "[keymap]" << error;
    }
}

// 改摇杆的四个方向键：空的填回默认值，免得某个方向永远推不动
void VideoItem::SetSteerKeys(int index, const QString &up, const QString &down, const QString &left,
                             const QString &right)
{
    if (index < 0 || index >= key_map_.items_.size() || key_map_.items_.at(index).type_ != qsc::input::KeyMapItemType::Steer)
    {
        return;
    }

    qsc::input::KeyMapItem &item = key_map_.items_[index];
    item.up_key_ = up.isEmpty() ? item.up_key_ : up;
    item.down_key_ = down.isEmpty() ? item.down_key_ : down;
    item.left_key_ = left.isEmpty() ? item.left_key_ : left;
    item.right_key_ = right.isEmpty() ? item.right_key_ : right;

    key_map_engine_.SetMap(key_map_);
    emit SigKeyMapChanged();
}

// 键名字符串转成 Windows 虚拟键码：字母数字用 ASCII，其余查小表
int VkForName(const QString &name)
{
    if (name.size() == 1)
    {
        const QChar ch = name.at(0).toUpper();
        return ch.unicode();
    }
    // 表里的键名用小写：配置里可能存成 space / Space，统一按小写查
    static const QHash<QString, int> kExtra = {
        { QStringLiteral("space"), VK_SPACE },   { QStringLiteral("tab"), VK_TAB },
        { QStringLiteral("esc"), VK_ESCAPE },    { QStringLiteral("enter"), VK_RETURN },
        { QStringLiteral("up"), VK_UP },         { QStringLiteral("down"), VK_DOWN },
        { QStringLiteral("left"), VK_LEFT },     { QStringLiteral("right"), VK_RIGHT },
        { QStringLiteral("shift"), VK_SHIFT },   { QStringLiteral("ctrl"), VK_CONTROL },
        { QStringLiteral("alt"), VK_MENU },
    };
    return kExtra.value(name.toLower(), 0);
}

// 16ms 轮询：只有"按下状态"发生变化才发触摸，自动重复事件完全不影响这里
void VideoItem::SlotKeyMapPoll()
{
    if (map_mode_ != 1)
    {
        if (!polled_keys_.isEmpty())
        {
            DispatchKeyMapActions(key_map_engine_.Reset());
            polled_keys_.clear();
        }
        return;
    }

    for (const QString &name : key_map_engine_.TrackedKeys())
    {
        const int vk = VkForName(name);
        if (vk == 0)
        {
            continue;
        }
        const bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool was_down = polled_keys_.contains(name);
        if (down == was_down)
        {
            continue;
        }

        if (down)
        {
            polled_keys_.insert(name);
            qInfo() << "[keymap] poll down" << name;
            DispatchKeyMapActions(key_map_engine_.Press(name));
        }
        else
        {
            polled_keys_.remove(name);
            qInfo() << "[keymap] poll up" << name;
            DispatchKeyMapActions(key_map_engine_.Release(name));
        }
    }
}

// 动作翻成控制消息：点按类先按下，40ms 后补一个抬起
void VideoItem::DispatchKeyMapActions(const QVector<qsc::input::KeyMapAction> &actions)
{
    const bool tap_pair = actions.size() == 2
                          && actions.at(0).kind_ == qsc::input::KeyMapAction::Kind::Down
                          && actions.at(1).kind_ == qsc::input::KeyMapAction::Kind::Up;
    for (const qsc::input::KeyMapAction &action : actions)
    {
        if (tap_pair && action.kind_ == qsc::input::KeyMapAction::Kind::Up)
        {
            const quint64 pointer = action.pointer_id_;
            const QPointF pos = action.pos_;
            QTimer::singleShot(40, this, [this, pointer, pos]()
            {
                EmitControlMessage(touch_.NormalizedTouch(pointer, 1, pos));
            });
            continue;
        }
        EmitControlMessage(touch_.NormalizedTouch(action.pointer_id_, ActionCode(action.kind_), action.pos_));
    }
}

// 进入录制：点圆点后调它，下一次按键就绑到这个键位；Esc 取消
void VideoItem::BeginKeyRecord(int index)
{
    if (index < 0 || index >= key_map_.items_.size())
    {
        return;
    }
    record_index_ = index;
    forceActiveFocus();                                                                          // 叠加层不拿焦点，不抢就收不到按键
    qInfo() << "[keymap] record start index" << index;
    emit SigRecordingChanged();
}

// QML 收到按键后调它：把键绑到当前录制目标上
void VideoItem::BindRecordedKey(int qt_key)
{
    if (record_index_ < 0 || record_index_ >= key_map_.items_.size())
    {
        return;
    }
    const int index = record_index_;
    record_index_ = -1;
    if (key_map_.items_.at(index).type_ != qsc::input::KeyMapItemType::Steer)
    {
        key_map_.items_[index].key_ = KeyName(qt_key);
        key_map_.items_[index].label_ = KeyName(qt_key);
        key_map_engine_.SetMap(key_map_);
        qInfo() << "[keymap] record bound" << KeyName(qt_key) << "to index" << index;
        emit SigKeyMapChanged();
    }
    emit SigRecordingChanged();
}