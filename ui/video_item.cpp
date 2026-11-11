#include "ui/video_item.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QTimer>

#include "stream/perf_stats.h"

VideoItem::VideoItem(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);

    // 重绘请求由 GUI 线程的定时器发起：视频线程只负责存图，
    // 一旦从视频线程投递事件，拖窗口时就会被 GUI 线程反过来堵死。
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(8);
    QObject::connect(refresh_timer_, &QTimer::timeout, this, &VideoItem::OnRefreshTimer);
    refresh_timer_->start();

    // 帧率统计同样在 GUI 线程
    fps_timer_ = new QTimer(this);
    fps_timer_->setInterval(1000);
    QObject::connect(fps_timer_, &QTimer::timeout, this, &VideoItem::UpdateFps);
    fps_timer_->start();
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

void VideoItem::SetKeepAspect(bool keep_aspect)
{
    if (keep_aspect_.exchange(keep_aspect) == keep_aspect) {
        return;
    }
    emit KeepAspectChanged();
    update();
}

void VideoItem::Present(const QImage &image)
{
    if (image.isNull()) {
        return;
    }

    {
        QMutexLocker locker(&mutex_);
        // 只留最新一张：视频线程比渲染快的时候，旧帧直接覆盖，不排队
        image_ = image;
        has_image_ = true;
        frame_size_ = image.size();
    }
    frames_arrived_.ref();

    // 这里故意什么都不投递：重绘交给 OnRefreshTimer 在 GUI 线程里发
}

void VideoItem::Clear()
{
    {
        QMutexLocker locker(&mutex_);
        image_ = QImage();
        has_image_ = false;
    }
    update();
}

void VideoItem::OnRefreshTimer()
{
    QSize current_size;
    {
        QMutexLocker locker(&mutex_);
        current_size = frame_size_;
    }

    // 尺寸变了（设备旋转）才通知 QML；在 GUI 线程 emit，跨线程的事一点都不做
    if (current_size != notified_size_) {
        notified_size_ = current_size;
        emit FrameSizeChanged();
    }

    // 有新图就请一次重绘
    const int arrived = frames_arrived_.loadAcquire();
    if (arrived != frames_painted_) {
        frames_painted_ = arrived;
        update();
    }
}

void VideoItem::UpdateFps()
{
    static int last_arrived = 0;
    const int arrived = frames_arrived_.loadAcquire();
    const int frames = arrived - last_arrived;
    last_arrived = arrived;
    if (frames == fps_) {
        return;
    }
    fps_ = frames;
    emit FpsChanged();
}

QRectF VideoItem::TargetRect() const
{
    const QSize frame_size = FrameSize();
    if (!KeepAspect() || frame_size.isEmpty()) {
        return QRectF(0, 0, width(), height());
    }

    // 等比缩放到刚好塞进画布，然后居中；两边多出来的地方就是黑边
    const qreal scale = qMin(width() / frame_size.width(), height() / frame_size.height());
    const qreal target_width = frame_size.width() * scale;
    const qreal target_height = frame_size.height() * scale;
    return QRectF((width() - target_width) / 2.0, (height() - target_height) / 2.0, target_width,
                  target_height);
}

void VideoItem::geometryChanged(const QRectF &new_geometry, const QRectF &old_geometry)
{
    QQuickItem::geometryChanged(new_geometry, old_geometry);
    // 窗口大小变了，画面要重新算位置和缩放
    update();
}

QSGNode *VideoItem::updatePaintNode(QSGNode *old_node, UpdatePaintNodeData *)
{
    QImage image;
    {
        QMutexLocker locker(&mutex_);
        if (has_image_) {
            image = image_;
            image_ = QImage();
            has_image_ = false;
        }
    }

    // 没有新画面时保留上一帧，但矩形还是要重算：
    // 窗口正在缩放时，画面得立刻跟着变形，不能等下一帧（那看起来就是"不跟手"）。
    if (image.isNull()) {
        if (old_node != nullptr) {
            static_cast<QSGSimpleTextureNode *>(old_node)->setRect(TargetRect());
        }
        return old_node;
    }

    QElapsedTimer upload_timer;
    upload_timer.start();

    QSGTexture *texture = window()->createTextureFromImage(image, QQuickWindow::TextureIsOpaque);
    if (texture == nullptr) {
        // 渲染上下文还没就绪，这张建不出来；保留旧节点，等下一帧再试
        return old_node;
    }

    auto *node = static_cast<QSGSimpleTextureNode *>(old_node);
    if (node == nullptr) {
        node = new QSGSimpleTextureNode;
        // 交给节点管纹理生命周期：换新纹理时它会按场景图的时机释放旧的那张，
        // 自己 delete 就会删在渲染器还拿着指针的时候，一炸一个准
        node->setOwnsTexture(true);
    }

    node->setTexture(texture);
    node->setRect(TargetRect());
    node->markDirty(QSGNode::DirtyMaterial);

    qsc::stream::PerfStats::Get().upload_us.fetchAndAddRelaxed(upload_timer.nsecsElapsed() / 1000);
    return node;
}
