#pragma once

#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

#include <memory>

#include "session/server_session.h"
#include "stream/h264_decoder.h"
#include "stream/video_reader.h"

class QThread;
class VideoItem;

// 界面上「连接 / 断开」按钮背后的控制器。
//
// 连接过程是阻塞的（push → reverse → execute → 读设备信息，1~2 秒），当前在主线程同步做。
// 连上之后视频 socket 交给 VideoReader 线程，解出来的帧直接送给 VideoItem。
// 状态字符串由 QML 直接绑定，界面不用知道底下发生了什么。
class DeviceController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString status READ Status NOTIFY StatusChanged)
    Q_PROPERTY(QString proposedSerial READ ProposedSerial NOTIFY ProposedSerialChanged)
    Q_PROPERTY(QString wheelMode READ WheelMode NOTIFY WheelModeChanged)
    Q_PROPERTY(int dragPixels READ DragPixels NOTIFY DragPixelsChanged)
    Q_PROPERTY(QString deviceName READ DeviceName NOTIFY DeviceNameChanged)
    Q_PROPERTY(QStringList devices READ Devices NOTIFY DevicesChanged)
    Q_PROPERTY(QStringList deviceLabels READ DeviceLabels NOTIFY DevicesChanged)

public:
    explicit DeviceController(QObject *parent = nullptr);
    ~DeviceController();

    QString Status() const;
    QString ProposedSerial() const;                                                             // 刷新后建议填进输入框的序列号
    QString WheelMode() const;                                                                  // "scroll" 或 "drag"
    int DragPixels() const;                                                                     // 滑动模式下一格滚轮拖多少像素
    QString DeviceName() const;                                                                 // 当前连着的设备串号，标题栏用它
    QStringList Devices() const;                                                                // 扫到的设备地址，和 deviceLabels 下标对应
    QStringList DeviceLabels() const;                                                           // 下拉框显示文字，带 USB / 无线 前缀

    Q_INVOKABLE void SetSurface(QObject *surface);                                              // QML 里把 VideoSurface 传进来

    Q_INVOKABLE void RefreshDevices();                                                          // adb devices，刷新后填 proposedSerial
    Q_INVOKABLE void Connect(const QString &serial);                                            // 同步阻塞，连完才返回
    Q_INVOKABLE void Disconnect();

    Q_INVOKABLE void SetWheelMode(const QString &mode);                                         // 切换滚轮发法，立即生效并记住
    Q_INVOKABLE void AdjustDragPixels(int delta);                                               // 滑动像素加减，立即生效并记住

    // 启动时挑一个串号自动连：优先上次连过的，其次环境变量给的，都要在在线列表里
    Q_INVOKABLE QString PickStartupSerial(const QString &fallback) const;

    // 把当前设备切到无线调试：adb tcpip 5555 → 问出设备 IP → adb connect；需要先插着线
    Q_INVOKABLE void EnableWifi();
    Q_INVOKABLE void SendVolumeUp();                                                            // 音量加一个档
    Q_INVOKABLE void SendVolumeDown();                                                          // 音量减一个档
    Q_INVOKABLE void RotateDevice();                                                            // 让设备转一下屏幕
    Q_INVOKABLE void SaveScreenshot();                                                          // 把当前画面存成 png
    Q_INVOKABLE void SetDisplayPower(bool on);                                                  // true 亮屏，false 息屏
    Q_INVOKABLE void SendBack();                                                                // 返回上一级（顺便能点亮屏幕）
    Q_INVOKABLE void SendRecents();                                                             // 多任务切换（最近任务）

signals:
    void StatusChanged();
    void ProposedSerialChanged();
    void WheelModeChanged();
    void DragPixelsChanged();
    void DeviceNameChanged();
    void DevicesChanged();

private:
    // 切无线三步的结果，工作线程算完交回给主线程
    struct WifiSwitchResult
    {
        bool ok_ = false;                                                                       // 整条流程成没成
        QString endpoint_;                                                                      // 成功时的 IP:端口
        QString message_;                                                                       // 失败时给界面看的原因
    };

    static WifiSwitchResult SwitchToWifiBlocking(const QString &serial);                        // 工作线程：跑完整条切无线流程
    void ApplyWifiResult(const WifiSwitchResult &result);                                       // 主线程：刷新列表、更新状态
    void ApplyConnected(const qsc::session::ConnectResult &result);                             // 连接结果落地：接解码器和读线程
    void SendControl(const QByteArray &message);                                                // 把画布翻好的输入消息写进控制 socket
    void SendKeycode(quint32 keycode);                                                          // 发一次按键（按下+抬起各一条）
    void TearDown();                                                                            // 停线程、关解码器、拆隧道
    void SetStatus(const QString &status);

    VideoItem *surface_ = nullptr;                                                              // QML 那条视频画布，由 main 注入
    QString status_;                                                                            // 给界面绑定的状态文字
    QString proposed_serial_;                                                                   // 刷新出来的第一台设备序列号
    QString wheel_mode_ = QStringLiteral("scroll");                                             // 滚轮发法，从配置里读进来
    int drag_pixels_ = 60;                                                                      // 滑动模式下一格滚轮拖多少像素
    bool wifi_busy_ = false;                                                                    // 正在后台切无线，期间不接受第二次点击
    QString device_name_;                                                                       // 当前连着的设备串号
    QStringList online_serials_;                                                                // 上一次 adb devices 拿到的在线设备
    QStringList devices_;                                                                       // 给 QML 下拉框用的设备地址
    QStringList device_labels_;                                                                 // 下拉框显示文字，和 devices_ 下标对应

    std::unique_ptr<qsc::session::ServerSession> session_;                                      // 连接成功后一直持有
    std::unique_ptr<qsc::stream::H264Decoder> decoder_;                                         // 解码器，和读线程一一对应
    qsc::stream::VideoReader *reader_ = nullptr;                                                // 读线程，连着的时候存在
};
