#pragma once

#include <QAtomicInt>
#include <QAtomicInteger>
#include <QElapsedTimer>
#include <QEvent>
#include <QMutex>
#include <QQuickRhiItem>
#include <QSize>

#include <atomic>
#include <memory>

#include <QSet>
#include <QVariantList>

#include "stream/frame.h"

namespace qsc::stream
{
class H264Decoder;
}


class QTimer;

#include "ui/frame_probe.h"
#include "ui/video_input.h"

class VideoItem : public QQuickRhiItem
{
    Q_OBJECT
    Q_PROPERTY(QSize frameSize READ FrameSize NOTIFY SigFrameSizeChanged)
    Q_PROPERTY(bool keepAspect READ KeepAspect WRITE SetKeepAspect NOTIFY SigKeepAspectChanged)
    Q_PROPERTY(int fps READ Fps NOTIFY SigFpsChanged)

public:
    explicit VideoItem(QQuickItem *parent = nullptr);
    ~VideoItem() override;                                                                      // 放掉设备那份引用

    QQuickRhiItemRenderer *createRenderer() override;

    QSize FrameSize() const;                                                                    // 最近一帧的尺寸，QML 里用来显示和算宽高比
    bool KeepAspect() const;
    int Fps() const;

    void SetKeepAspect(bool keep_aspect);
#pragma region input // 输入：全部转发到 VideoInput，别在这里加逻辑
    void SetWheelMode(const QString &mode) { input_->SetWheelMode(mode); }
    void SetDragPixels(int pixels) { input_->SetDragPixels(pixels); }
    void SetDragScale(double scale) { input_->SetDragScale(scale); }
    void SetPinchModifier(const QString &modifier) { input_->SetPinchModifier(modifier); }
    bool IsPinching() const { return input_->IsPinching(); }
    void SetMapMode(int mode) { input_->SetMapMode(mode); }
    int MapMode() const { return input_->MapMode(); }
    void SetKeyMapPath(const QString &path) { input_->SetKeyMapPath(path); }
    QVariantList KeyMapItems() const { return input_->KeyMapItems(); }
    int AddKeyMapItem(int type, const QString &key, double x, double y, const QString &label)
    {
        return input_->AddKeyMapItem(type, key, x, y, label);
    }
    void MoveKeyMapItem(int index, double x, double y) { input_->MoveKeyMapItem(index, x, y); }
    void RemoveKeyMapItem(int index) { input_->RemoveKeyMapItem(index); }
    void SetSteerKeys(int index, const QString &up, const QString &down, const QString &left, const QString &right)
    {
        input_->SetSteerKeys(index, up, down, left, right);
    }
    void SetClickerInterval(int ms) { input_->SetClickerInterval(ms); }
    int ClickerInterval() const { return input_->ClickerInterval(); }
    void BeginKeyRecord(int index) { input_->BeginKeyRecord(index); }
    bool Recording() const { return input_->Recording(); }
    void BindRecordedKey(int qt_key) { input_->BindRecordedKey(qt_key); }
    void SaveKeyMap() { input_->SaveKeyMap(); }
    // 基类事件转发：VideoInput 不是 QQuickItem 的子类，只能通过画布调回基类实现
    void BaseMousePress(QMouseEvent *event) { QQuickRhiItem::mousePressEvent(event); }
    void BaseMouseMove(QMouseEvent *event) { QQuickRhiItem::mouseMoveEvent(event); }
    void BaseMouseRelease(QMouseEvent *event) { QQuickRhiItem::mouseReleaseEvent(event); }
    void BaseWheel(QWheelEvent *event) { QQuickRhiItem::wheelEvent(event); }
    void BaseKeyPress(QKeyEvent *event) { QQuickRhiItem::keyPressEvent(event); }
    void BaseKeyRelease(QKeyEvent *event) { QQuickRhiItem::keyReleaseEvent(event); }
    void TakeFocus() { forceActiveFocus(); }
#pragma endregion

    // 视频线程调：把最新一帧放进来（引用计数，不拷像素），旧的直接丢掉
    void Present(const qsc::stream::Frame &frame);

    void Clear();                                                                               // 断开连接时调：把还没渲染的帧丢掉

    // 渲染线程 synchronize() 里调：取走最新一帧，没有就返回无效帧
    qsc::stream::Frame TakePendingFrame(FrameTracePtr *trace_out, qint64 *arrive_us_out);
    void BeginSyncPass() { probe_->BeginSyncPass(); }                                                // 一轮 synchronize 开始
    qint64 SyncStartUs() const { return probe_->SyncStartUs(); }
    void EndSyncPass(qint64 sync_start_us, qint64 arrive_us, const FrameTracePtr &trace) { probe_->EndSyncPass(sync_start_us, arrive_us, trace); }
    void MarkRenderEnd(qint64 draw_start_us, const FrameTracePtr &trace) { probe_->OnRenderEnd(draw_start_us, trace); }
    void MarkSwapped() { probe_->OnSwapped(); }
    void RecordConvert(qint64 record_us, qint64 wait_us) { probe_->RecordConvert(record_us, wait_us); } // 转色耗时交给探针统计
    qint64 ClockUs() const { return probe_->NowUs(); }
    void SetPassArriveUs(qint64 arrive_us) { probe_->SetPassArrive(arrive_us); }
    void SetPassSyncUs(qint64 sync_us) { probe_->SetPassSync(sync_us); }

    // 渲染线程调：把 Qt RHI 的 D3D12 设备交过来（内部 AddRef，画布持有一份）
    void ProvideDevice(void *device);
    void *D3dDevice() const;                                                                     // 渲染线程与解码线程共用：没就绪返回空

    // 解码器归画布：一个窗口一条视频管线，窗口没了解码器才没
    qsc::stream::H264Decoder *Decoder(bool hardware, int soft_threads);                          // 懒创建，参数由控制器传，不负责 Open
    void CloseDecoder();                                                                        // 断开连接时关掉，画布还在

signals:
    void SigFrameSizeChanged();
    void SigKeepAspectChanged();
    void SigFpsChanged();
    void SigControlMessageReady(const QByteArray &message);                                     // 鼠标键盘翻好的控制消息，交给控制器发出去
    void SigPinchingChanged();                                                                  // 捏合开始或结束，界面用它显示双手状态
    void SigMapModeChanged();                                                                   // 映射模式变了，工具栏和叠加层跟着切
    void SigKeyMapChanged();                                                                    // 键位列表变了，叠加层重画
    void SigRecordingChanged();                                                                 // 进入或退出按键录制

protected:
    // Qt5 叫 geometryChanged，Qt6 改名成 geometryChange，参数含义没变
    void geometryChange(const QRectF &new_geometry, const QRectF &old_geometry) override;

    // 挂在窗口上的两个信号只在这里接一次：放到每帧的队列回调里会在 Qt6 下重入
    void componentComplete() override;

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    void NotifyFrameSize();                                                                     // GUI 线程：帧尺寸变了才发信号


    mutable QMutex mutex_;                                                                      // 视频线程写 pending_，渲染线程取走
    qsc::stream::Frame pending_;                                                                // 最新一帧，等渲染线程取走
    qint64 pending_arrive_us_ = 0;                                                              // 待渲染帧的到达时刻
    bool has_pending_ = false;
    FrameTracePtr pending_trace_;                                                               // 待渲染帧的事件记录
    QSize frame_size_;                                                                          // 最近一帧的尺寸，旋转时会变
    QSize notified_size_;                                                                       // 已经通知给 QML 的尺寸
    std::atomic<bool> keep_aspect_{false};                                                      // true 保持宽高比居中，false 拉满
    FrameProbe *probe_ = nullptr;                                                                // 埋点：帧序号、分档、每秒统计
    VideoInput *input_ = nullptr;                                                                // 输入：鼠标/键盘/键位映射/连点

    std::unique_ptr<qsc::stream::H264Decoder> decoder_;                                          // 本窗口的解码器

    mutable QMutex device_mutex_;                                                                // d3d_device_ 跨线程读写
    void *d3d_device_ = nullptr;                                                                 // Qt RHI 的 D3D12 设备，我们自己持有一份引用

    bool swap_hooked_ = false;                                                                  // frameSwapped 只接一次
    QTimer *fps_timer_ = nullptr;                                                               // 每秒结算统计
    QTimer *keymap_tick_timer_ = nullptr;                                                       // 摇杆心跳，100ms
    QTimer *keymap_poll_timer_ = nullptr;                                                       // 16ms 轮询物理按键
    QTimer *clicker_timer_ = nullptr;                                                           // 连点，10ms
    QTimer *pinch_watch_timer_ = nullptr;                                                       // 250ms 捏合兜底
};
