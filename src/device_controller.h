#pragma once

#include <QObject>
#include <QSize>
#include <QPointer>
#include <QList>
#include <QUrl>
#include <QString>
#include <QStringList>
#include <memory>
#include "control_writer.h"
#include "wifi_status.h"
#include "video_controller.h"
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
class VideoController;

class DeviceController : public QObject, public ControlWriter
{
    Q_OBJECT
    Q_PROPERTY(QString status READ Status NOTIFY SigStatusChanged)
    Q_PROPERTY(QString proposedSerial READ ProposedSerial NOTIFY SigProposedSerialChanged)
    Q_PROPERTY(QString deviceName READ DeviceName NOTIFY SigDeviceNameChanged)
    Q_PROPERTY(QStringList devices READ Devices NOTIFY SigDevicesChanged)
    Q_PROPERTY(QStringList deviceLabels READ DeviceLabels NOTIFY SigDevicesChanged)
    Q_PROPERTY(bool showConsole READ ShowConsole NOTIFY SigShowConsoleChanged)
    Q_PROPERTY(int videoBitRate READ VideoBitRate NOTIFY SigVideoSettingsChanged)
    Q_PROPERTY(int videoMaxFps READ VideoMaxFps NOTIFY SigVideoSettingsChanged)
    Q_PROPERTY(QString videoProfile READ VideoProfile NOTIFY SigVideoSettingsChanged)
    Q_PROPERTY(QObject *wifiStatus READ WifiStatusObject CONSTANT)                                   // WiFi 健康状态，只有无线连接时 available 为 true
    Q_PROPERTY(bool connected READ IsConnected NOTIFY SigConnectedChanged)
    Q_PROPERTY(QString bossKey READ BossKey NOTIFY SigBossKeyChanged)
    Q_PROPERTY(QString shortcutBack READ ShortcutBack NOTIFY SigShortcutsChanged)
    Q_PROPERTY(QString shortcutHome READ ShortcutHome NOTIFY SigShortcutsChanged)
    Q_PROPERTY(QString shortcutScreenshot READ ShortcutScreenshot NOTIFY SigShortcutsChanged)
    Q_PROPERTY(QString shortcutFullscreen READ ShortcutFullscreen NOTIFY SigShortcutsChanged)
    Q_PROPERTY(QString uploadDir READ UploadDir NOTIFY SigUploadDirChanged)

public:
    explicit DeviceController(QObject *parent = nullptr);
    ~DeviceController();

    QString Status() const;
    QString ProposedSerial() const;                                                             // 刷新后建议填进输入框的序列号
    QString DeviceName() const;                                                                 // 当前连着的设备串号，标题栏用它
    QStringList Devices() const;                                                                // 扫到的设备地址，和 deviceLabels 下标对应
    QStringList DeviceLabels() const;                                                           // 下拉框显示文字，带 USB / 无线 前缀
    bool ShowConsole() const;                                                                   // 控制台窗口当前显示着没有
    int VideoBitRate() const;                                                                   // 当前生效的码率（bps）
    int VideoMaxFps() const;                                                                    // 当前生效的帧率上限，0 = 不限
    QString VideoProfile() const;
    QObject *WifiStatusObject() { return &wifi_status_; }                                          // WiFi 状态对象，QML 通过它读 summary / level
    bool IsConnected() const override;                                                                   // 会话建起来没有，投屏窗靠它决定要不要显示
    QString BossKey() const;                                                                    // 当前老板键文本，例如 Ctrl+Alt+B
    QString ShortcutBack() const;                                                               // 返回键的快捷键
    QString ShortcutHome() const;                                                               // 回桌面的快捷键
    QString ShortcutScreenshot() const;                                                         // 截图的快捷键
    QString ShortcutFullscreen() const;                                                         // 全屏的快捷键
    QString UploadDir() const;                                                                  // 拖文件上传到设备的哪个目录                                                                    // 当前老板键文本，例如 Ctrl+Alt+B


    Q_INVOKABLE void RefreshDevices();                                                          // adb devices，刷新后填 proposedSerial
    Q_INVOKABLE void Connect(const QString &serial);                                            // 同步阻塞，连完才返回
    Q_INVOKABLE void Disconnect();

    Q_INVOKABLE void SetShowConsole(bool show);                                                 // 显示或隐藏命令行窗口，并记住
    Q_INVOKABLE void ApplyVideoSettings(int bit_rate, int max_fps);                             // 改码率/帧率并重连生效
    Q_INVOKABLE void ResetAllConfig();                                                          // 恢复默认配置，提示重启
    Q_INVOKABLE void SetBossKeyHost(QObject *host);                                             // main 把 BossKey 对象交进来
    Q_INVOKABLE bool SetBossKey(const QString &text);                                           // 界面改老板键：解析 → 重注册 → 记住                                  
    Q_INVOKABLE bool SetShortcut(const QString &name, const QString &sequence);                 // name: back/home/screenshot/fullscreen
    Q_INVOKABLE bool SetUploadDir(const QString &dir);                                          // 改上传目录并记住                                                            
    Q_INVOKABLE void RegisterWindow(QObject *window);                                           // QML 把自己的窗口登记进来，老板键要藏它们
    Q_INVOKABLE void ToggleBossKey();                                                           // 一键隐藏/恢复所有窗口

    // ControlWriter：视频模块发消息、报状态、问连接情况的入口
    bool WriteControl(const QByteArray &message) override;
    void ReportStatus(const QString &text) override;
    QString DeviceSerial() const override;

    // main 把视频控制器交进来：接帧信号、取画布都要用它
    void AttachVideoController(VideoController *video);

    // 启动时挑一个串号自动连：优先上次连过的，其次环境变量给的，都要在在线列表里
    Q_INVOKABLE QString PickStartupSerial(const QString &fallback) const;

    // 把当前设备切到无线调试：adb tcpip 5555 → 问出设备 IP → adb connect；需要先插着线
    Q_INVOKABLE void EnableWifi();

signals:
    void SigStatusChanged();
    void SigProposedSerialChanged();
    void SigDeviceNameChanged();
    void SigDevicesChanged();
    void SigShowConsoleChanged();
    void SigVideoSettingsChanged();
    void SigConnectedChanged();
    void SigBossKeyChanged();
    void SigShortcutsChanged();
    void SigUploadDirChanged();

private:
    void OnControlData();                                                                       // 控制 socket 有数据：解析设备消息（剪贴板推送、Ack）
    void OnLocalClipboardChanged();                                                             // 电脑剪贴板变了（开了自动同步才接）

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
    void TearDown();                                                                            // 停线程、关解码器、拆隧道
    void SetStatus(const QString &status);

    QString status_;                                                                            // 给界面绑定的状态文字
    QString proposed_serial_;                                                                   // 刷新出来的第一台设备序列号
    bool wifi_busy_ = false;                                                                    // 正在后台切无线，期间不接受第二次点击
    QString device_name_;                                                                       // 当前连着的设备串号
    QStringList online_serials_;                                                                // 上一次 adb devices 拿到的在线设备
    QStringList devices_;                                                                       // 给 QML 下拉框用的设备地址
    VideoController *video_ = nullptr;                                                          // 视频模块，接帧和取画布要用
    QTcpSocket *control_reader_ = nullptr;                                                      // 控制 socket，读设备消息用
    QByteArray control_buffer_;                                                                 // 控制 socket 的半条消息缓存
    quint64 clipboard_sequence_ = 0;                                                            // SetClipboard 的序号，设备 Ack 会带回
    bool suppress_clipboard_ = false;                                                           // 正在写电脑剪贴板，别把它再推回设备
    bool boss_hidden_ = false;                                                                  // 老板键是否已经把窗口藏起来了
    QPointer<QObject> boss_key_host_;                                                           // BossKey 对象，改键时要重新注册
    QList<QPointer<QWindow>> app_windows_;                                                      // QML 登记进来的窗口，老板键只藏这些
    QList<QPointer<QWindow>> hidden_windows_;                                                   // 藏起来的窗口，恢复时逐个 show
    QMetaObject::Connection clipboard_connection_;                                              // 自动同步的剪贴板信号连接
    bool connected_ = false;                                                                    // 会话活着就是 true
    QString video_profile_ = QStringLiteral("wired");                                           // 当前连接方式的画质档位
    qsc::app::WifiStatus wifi_status_;                                                          // 无线时每 3 秒采一次信号/速率/重传
    bool show_console_ = false;                                                                 // 控制台窗口显示状态
    QStringList device_labels_;                                                                 // 下拉框显示文字，和 devices_ 下标对应

    std::unique_ptr<qsc::session::ServerSession> session_;                                      // 连接成功后一直持有
    std::unique_ptr<qsc::stream::H264Decoder> decoder_;                                         // 解码器，和读线程一一对应
    qsc::stream::VideoReader *reader_ = nullptr;                                                // 读线程，连着的时候存在
};
