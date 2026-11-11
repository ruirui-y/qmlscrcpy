#include "ui/video_item.h"

#include <QDebug>
#include <QMutexLocker>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

VideoItem::VideoItem(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(QQuickItem::ItemHasContents, true);
}

QSize VideoItem::FrameSize() const
{
    QMutexLocker locker(&mutex_);
    return frame_size_;
}

void VideoItem::Present(const qsc::stream::Frame &frame)
{
    if (!frame.IsValid()) {
        return;
    }

    // 转换放在主线程做：720p 大约 2~3 ms，换来的好处是渲染线程只做上传，不碰 FFmpeg
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
    node->setRect(0, 0, width(), height());
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
}
