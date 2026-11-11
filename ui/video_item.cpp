#include "ui/video_item.h"

#include <QDebug>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

VideoItem::VideoItem(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);
    fps_timer_.start();
}

QSize VideoItem::FrameSize() const
{
    QMutexLocker locker(&mutex_);
    return frame_size_;
}

bool VideoItem::KeepAspect() const
{
    return keep_aspect_;
}

int VideoItem::Fps() const
{
    return fps_;
}

void VideoItem::SetKeepAspect(bool keep_aspect)
{
    if (keep_aspect_ == keep_aspect) {
        return;
    }
    keep_aspect_ = keep_aspect;
    emit KeepAspectChanged();
    update();
}

void VideoItem::Present(const qsc::stream::Frame &frame)
{
    if (!frame.IsValid()) {
        return;
    }

    // 帧率：每满一秒结算一次，界面拿 fps 属性显示
    ++frames_in_window_;
    if (fps_timer_.elapsed() >= 1000) {
        fps_ = frames_in_window_;
        frames_in_window_ = 0;
        fps_timer_.restart();
        emit FpsChanged();
    }

    // 转换放在主线程做：720p 大约 3~4 ms，换来渲染线程只做上传、不碰 FFmpeg
    const QImage image = convert_.Convert(frame);
    if (image.isNull()) {
        return;
    }

    bool size_changed = false;
    {
        QMutexLocker locker(&mutex_);
        image_ = image;
        has_image_ = true;
        if (frame_size_ != frame.Size()) {
            frame_size_ = frame.Size();
            size_changed = true;
        }
    }

    if (size_changed) {
        emit FrameSizeChanged();
    }
    update();
}

QRectF VideoItem::TargetRect() const
{
    const QSize frame_size = FrameSize();
    if (!keep_aspect_ || frame_size.isEmpty()) {
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

    // 还没有画面就不要节点。QSGSimpleTextureNode 挂着空纹理会触发 Qt 的
    // "Invalid texture accessed: 0x0"，debug 版直接 abort。
    if (image.isNull()) {
        delete old_node;
        return nullptr;
    }

    QSGTexture *texture = window()->createTextureFromImage(image, QQuickWindow::TextureIsOpaque);
    if (texture == nullptr) {
        // 渲染上下文还没就绪，这张建不出来；丢掉节点，等下一帧再试
        delete old_node;
        return nullptr;
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
    return node;
}
