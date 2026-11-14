#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class ControlWriter;
class VideoItem;

// 投屏窗那一侧的所有行为：输入注入、滚轮语义、剪贴板、文件传输、截图。
// 主窗的连接/设备列表/设置不在这里；要发消息就走 ControlWriter 接口。
class VideoController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString wheelMode READ WheelMode NOTIFY SigWheelModeChanged)
    Q_PROPERTY(int dragPixels READ DragPixels NOTIFY SigDragPixelsChanged)
    Q_PROPERTY(double dragScale READ DragScale NOTIFY SigDragScaleChanged)
    Q_PROPERTY(QString pinchModifier READ PinchModifier NOTIFY SigPinchModifierChanged)          // 捏合修饰键
    Q_PROPERTY(bool pinching READ Pinching NOTIFY SigPinchingChanged)                           // 现在是否两指按着
    Q_PROPERTY(int mapMode READ MapMode NOTIFY SigMapModeChanged)                               // 键位映射：0 关 / 1 玩 / 2 编辑
    Q_PROPERTY(QVariantList keyMapItems READ KeyMapItems NOTIFY SigKeyMapChanged)               // 当前设备的键位列表

public:
    explicit VideoController(ControlWriter *writer, QObject *parent = nullptr);

    QString WheelMode() const;                                                                  // "scroll" 或 "drag"
    int DragPixels() const;                                                                     // 滑动模式下一格滚轮拖多少像素
    double DragScale() const;                                                                   // 左键拖拽倍率
    QString PinchModifier() const;                                                              // 捏合修饰键
    bool Pinching() const;                                                                      // 两指是否按着
    int MapMode() const;                                                                        // 键位映射模式
    QVariantList KeyMapItems() const;                                                           // 键位列表，叠加层用

    VideoItem *Surface() const;                                                                 // 画布指针，DeviceController 接帧信号时要用

    Q_INVOKABLE void SetSurface(QObject *surface);                                              // QML 把 VideoSurface 传进来
    Q_INVOKABLE void SetWheelMode(const QString &mode);                                         // 切换滚轮发法，立即生效并记住
    Q_INVOKABLE void AdjustDragPixels(int delta);                                               // 滑动像素加减，立即生效并记住
    Q_INVOKABLE void AdjustDragScale(int delta);                                                // 拖拽倍率加减（delta 以 0.1 为单位）
    Q_INVOKABLE void CyclePinchModifier();                                                      // 捏合修饰键：Ctrl → Alt → Shift → Ctrl+Alt → 关
    Q_INVOKABLE void CycleMapMode();                                                            // 映射模式循环：关 → 玩 → 编辑
    Q_INVOKABLE void LoadKeyMapFor(const QString &serial);                                       // 连上设备时按串号读键位文件
    Q_INVOKABLE int AddKeyMapItem(int type, const QString &key, double x, double y, const QString &label);
    Q_INVOKABLE void MoveKeyMapItem(int index, double x, double y);
    Q_INVOKABLE void RemoveKeyMapItem(int index);
    Q_INVOKABLE void SetSteerKeys(int index, const QString &up, const QString &down, const QString &left, const QString &right);

    Q_INVOKABLE void SendBack();                                                                // 返回上一级（顺便能点亮屏幕）
    Q_INVOKABLE void SendHome();                                                                // 回桌面（KEYCODE_HOME）
    Q_INVOKABLE void SendPower();                                                               // 按一下电源键（锁屏/点亮）
    Q_INVOKABLE void SendRecents();                                                             // 多任务切换（最近任务）
    Q_INVOKABLE void SendVolumeUp();                                                            // 音量加一个档
    Q_INVOKABLE void SendVolumeDown();                                                          // 音量减一个档
    Q_INVOKABLE void RotateDevice();                                                            // 让设备转一下屏幕
    Q_INVOKABLE void SetDisplayPower(bool on);                                                  // true 亮屏，false 息屏

    Q_INVOKABLE void SendClipboardToDevice();                                                   // 电脑剪贴板 → 设备，写完直接粘贴
    Q_INVOKABLE void PullClipboardFromDevice();                                                 // 设备剪贴板 → 电脑
    Q_INVOKABLE void PushFiles(const QList<QUrl> &urls);                                        // 拖进投屏窗的文件：推到手机
    Q_INVOKABLE void OpenUploadDir();                                                           // 在手机上打开上传目录
    Q_INVOKABLE void SaveScreenshot();                                                          // 把当前画面存成 png

signals:
    void SigWheelModeChanged();
    void SigDragPixelsChanged();
    void SigDragScaleChanged();
    void SigPinchModifierChanged();
    void SigPinchingChanged();                                                                  // 捏合状态变了
    void SigMapModeChanged();                                                                   // 映射模式变了
    void SigKeyMapChanged();                                                                    // 键位列表变了                                                             // 捏合修饰键变了

private:
    void OnSurfaceControlMessage(const QByteArray &message);                                    // 画布翻好的输入消息，转手写进 socket
    void SendKeycode(quint32 keycode);                                                          // 发一次按键（按下+抬起各一条）

    ControlWriter *writer_ = nullptr;                                                           // 发消息/报状态的出口，不持有
    VideoItem *surface_ = nullptr;                                                              // QML 那条视频画布，由 QML 注入
    QString wheel_mode_;                                                                        // 滚轮发法，构造时从配置读进来
    int drag_pixels_ = 60;                                                                      // 滑动模式下一格滚轮拖多少像素
    double drag_scale_ = 1.0;                                                                   // 左键拖拽倍率
    QString pinch_modifier_ = QStringLiteral("ctrl");                                           // 捏合修饰键
    quint64 clipboard_sequence_ = 0;                                                            // SetClipboard 的序号，设备 Ack 会带回
};
