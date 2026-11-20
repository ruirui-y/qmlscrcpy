#include "device_controller.h"

#include "app_settings.h"
#include "playback_settings.h"
#include "device_scanner.h"
#include "wifi_bridge.h"
#include "window_keeper.h"
#include "base/hotkey.h"
#include "base/task_runner.h"
#include "boss_key.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QClipboard>
#include <QKeySequence>
#include <QRegularExpression>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQuickWindow>
#include "base/log.h"
#include "adb/adb_args.h"
#include "adb/net_address.h"
#include "base/console_window.h"

#include <QGuiApplication>
#include <QHostAddress>
#include <QNetworkInterface>
#include "adb/adb_runner.h"
#include "protocol/control_message.h"
#include "protocol/device_message.h"

#include <QDebug>
#include <QTcpSocket>
#include "ui/video_item.h"

DeviceController::DeviceController(QObject *parent) : QObject(parent)
{
    // 后台预热 adb server：第一次调用可能要把别的版本的 server 换掉，实测 5 秒，
    // 别让用户点"刷新"时正好卡在这一下（startDetached 不阻塞，用完自己退）
    QProcess::startDetached(qsc::adb::AdbPath(), QStringList{QStringLiteral("start-server")});

    // 滚轮发法跟配置走：界面里选过的优先，没选过就用 config.ini 的默认值
    AppSettings settings;
    show_console_ = settings.ShowConsole();
    status_ = QStringLiteral("未连接");

    keeper_ = new qsc::app::WindowKeeper(this);
    QObject::connect(keeper_, &qsc::app::WindowKeeper::SigBossKeyChanged, this,
                     [this]() { emit SigBossKeyChanged(); });
    QObject::connect(keeper_, &qsc::app::WindowKeeper::SigShortcutsChanged, this,
                     [this]() { emit SigShortcutsChanged(); });
    QObject::connect(keeper_, &qsc::app::WindowKeeper::SigStatus, this,
                     [this](const QString &text) { SetStatus(text); });

    scanner_ = new qsc::app::DeviceScanner(this);
    QObject::connect(scanner_, &qsc::app::DeviceScanner::SigDevicesChanged, this,
                     [this]() { emit SigDevicesChanged(); });
    QObject::connect(scanner_, &qsc::app::DeviceScanner::SigProposedSerialChanged, this,
                     [this]() { emit SigProposedSerialChanged(); });
    QObject::connect(scanner_, &qsc::app::DeviceScanner::SigStatus, this,
                     [this](const QString &text) { SetStatus(text); });

    settings_ = new qsc::app::PlaybackSettings(this);
    QObject::connect(settings_, &qsc::app::PlaybackSettings::SigVideoChanged, this,
                     [this]() { emit SigVideoSettingsChanged(); });
    QObject::connect(settings_, &qsc::app::PlaybackSettings::SigDecodeChanged, this,
                     [this]() { emit SigDecodeSettingsChanged(); emit SigAsyncReadbackChanged(); });
}

DeviceController::~DeviceController()
{
    TearDown();
}

QString DeviceController::Status() const
{
    return status_;
}

QString DeviceController::ProposedSerial() const
{
    return scanner_->ProposedSerial();
}

// 拖文件上传的目标目录
QString DeviceController::UploadDir() const
{
    return AppSettings().UploadDir();
}

// 改上传目录：空串或不是绝对路径就拒绝
bool DeviceController::SetUploadDir(const QString &dir)
{
    const QString trimmed = dir.trimmed();
    if (trimmed.isEmpty() || !trimmed.startsWith(QLatin1Char('/')))
    {
        SetStatus(QStringLiteral("上传目录要写成绝对路径，例如 /sdcard/Download/QmlScrcpy"));
        return false;
    }

    AppSettings().SetUploadDir(trimmed);
    emit SigUploadDirChanged();
    SetStatus(QStringLiteral("上传目录已改为 %1").arg(trimmed));
    return true;
}

// 四个窗口快捷键的当前值
QString DeviceController::ShortcutBack() const
{
    return keeper_->Shortcut(QStringLiteral("back"));
}

QString DeviceController::ShortcutHome() const
{
    return keeper_->Shortcut(QStringLiteral("home"));
}

QString DeviceController::ShortcutScreenshot() const
{
    return keeper_->Shortcut(QStringLiteral("screenshot"));
}

QString DeviceController::ShortcutFullscreen() const
{
    return keeper_->Shortcut(QStringLiteral("fullscreen"));
}

// 改某个窗口快捷键：空串表示禁用，非空必须是合法的 QKeySequence
bool DeviceController::SetShortcut(const QString &name, const QString &sequence)
{
    return keeper_->SetShortcut(name, sequence);
}

// 当前老板键文本，界面显示用
QString DeviceController::BossKey() const
{
    return keeper_->BossKey();
}

// main 把 BossKey 对象交进来，之后改键要靠它重新注册
void DeviceController::SetBossKeyHost(QObject *host)
{
    keeper_->SetBossKeyHost(host);
}

// 界面改老板键：解析成功就重新注册并写进 window.ini，失败给出提示
bool DeviceController::SetBossKey(const QString &text)
{
    return keeper_->SetBossKey(text);
}

// QML 把窗口登记进来：投屏窗在 QML 里声明在主窗内部，QWindow::parent 非空，
// 不在 topLevelWindows() 里，只能让它们自己报到
void DeviceController::RegisterWindow(QObject *window)
{
    keeper_->Register(window);
}

// 老板键：第一次按藏起所有登记过的窗口，再按恢复
void DeviceController::ToggleBossKey()
{
    keeper_->ToggleBossKey();
}

// 控制 socket 有数据：按协议切出设备消息，剪贴板推过来就写进电脑剪贴板
void DeviceController::OnControlData()
{
    if (control_reader_ == nullptr)
    {
        return;
    }

    control_buffer_.append(control_reader_->readAll());
    for (;;)
    {
        qsc::protocol::DeviceMessage message;
        const qint32 consumed = qsc::protocol::DeviceMessage::TryParse(control_buffer_, message);
        if (consumed == 0)
        {
            break;                                                                              // 半条消息，等下一次数据
        }
        if (consumed < 0)
        {
            qWarning() << "[session] 设备消息解析失败，丢弃缓存" << control_buffer_.size();
            control_buffer_.clear();
            break;
        }
        control_buffer_.remove(0, consumed);

        if (message.type_ == qsc::protocol::DeviceMessage::Type::Clipboard)
        {
            // 写电脑剪贴板时会触发 dataChanged，别把同一条内容又推回设备
            suppress_clipboard_ = true;
            QGuiApplication::clipboard()->setText(message.text_);
            suppress_clipboard_ = false;
            SetStatus(QStringLiteral("已从手机取回剪贴板（%1 字）").arg(message.text_.size()));
        }
        else if (message.type_ == qsc::protocol::DeviceMessage::Type::AckClipboard)
        {
            qCDebug(qsc::base::lcInput) << "[clipboard] 设备确认序号" << message.sequence_;
        }
    }
}

// 电脑剪贴板变了且开了自动同步：推给设备（suppress 期间跳过，避免自己写自己的回环）
void DeviceController::OnLocalClipboardChanged()
{
    if (suppress_clipboard_ || session_ == nullptr)
    {
        return;
    }

    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
    {
        return;
    }

    ++clipboard_sequence_;
    WriteControl(qsc::protocol::ControlMessage::SetClipboard(clipboard_sequence_, false, text));
}
// 恢复默认配置：清掉 window.ini、删掉 config.ini，当前会话不重启不会变
void DeviceController::ResetAllConfig()
{
    AppSettings settings;
    const bool removed = settings.ResetAllConfig();
    SetStatus(removed ? QStringLiteral("已恢复默认配置，重启程序后生效")
                      : QStringLiteral("配置已重置，重启程序后生效"));
}

// 会话建起来没有
bool DeviceController::IsConnected() const
{
    return connected_;
}

// 当前生效的码率（bps），界面用来显示
int DeviceController::VideoBitRate() const
{
    return settings_->BitRate();
}

// 当前生效的帧率上限，0 表示不限
int DeviceController::VideoMaxFps() const
{
    return settings_->MaxFps();
}

// 当前用的是哪一档画质参数：有线还是无线
QString DeviceController::VideoProfile() const
{
    return video_profile_;
}

// 改码率和帧率：这两个是 server 启动参数，改完必须重连才生效
bool DeviceController::DecodeHardware() const
{
    return settings_->Hardware();
}

// 硬解/软解切换：存配置，连着就重连（解码器在连接时创建）
void DeviceController::SetDecodeHardware(bool hardware)
{
    if (DecodeHardware() == hardware)
    {
        return;
    }

    settings_->SetHardware(hardware);
    qInfo() << "[decode] 切到" << (hardware ? "硬解" : "软解");

    if (session_ == nullptr || reader_ == nullptr)
    {
        SetStatus(hardware ? QStringLiteral("解码方式：硬解，下次连接生效")
                           : QStringLiteral("解码方式：软解，下次连接生效"));
        return;
    }

    const QString serial = device_name_;
    SetStatus(QStringLiteral("按新的解码方式重连中（1~2 秒）…"));
    QCoreApplication::processEvents();
    Disconnect();
    if (!serial.isEmpty())
    {
        Connect(serial);
    }
}

int DeviceController::DecodeThreads() const
{
    return settings_->Threads();
}

// 软解线程数：只影响软解，硬解时保存即可
void DeviceController::SetDecodeThreads(int threads)
{
    const int value = qBound(1, threads, 8);
    if (DecodeThreads() == value)
    {
        return;
    }

    settings_->SetThreads(value);
    qInfo() << "[decode] 软解线程数设为" << value;

    if (session_ == nullptr || reader_ == nullptr || DecodeHardware())
    {
        SetStatus(QStringLiteral("软解线程数：%1，下次软解时生效").arg(value));
        return;
    }

    const QString serial = device_name_;
    SetStatus(QStringLiteral("按新的线程数重连中（1~2 秒）…"));
    QCoreApplication::processEvents();
    Disconnect();
    if (!serial.isEmpty())
    {
        Connect(serial);
    }
}

bool DeviceController::AsyncReadback() const
{
    return settings_->AsyncReadback();
}

// 界面切换读回方式：先存配置；连着设备就重连一次（解码器在连接时创建）
void DeviceController::SetAsyncReadback(bool enabled)
{
    if (AsyncReadback() == enabled)
    {
        return;
    }

    settings_->SetAsyncReadback(enabled);

    qInfo() << "[decode] 读回方式切到" << (enabled ? "异步（双 staging）" : "同步");

    if (session_ == nullptr || reader_ == nullptr)
    {
        SetStatus(enabled ? QStringLiteral("读回方式：异步，下次连接生效")
                          : QStringLiteral("读回方式：同步，下次连接生效"));
        return;
    }

    const QString serial = device_name_;
    SetStatus(QStringLiteral("按新的读回方式重连中（1~2 秒）…"));
    QCoreApplication::processEvents();
    Disconnect();
    if (!serial.isEmpty())
    {
        Connect(serial);
    }
}

void DeviceController::ApplyVideoSettings(int bit_rate, int max_fps)
{
    settings_->SetProfile(video_profile_);
    settings_->ApplyVideo(bit_rate, max_fps);
    emit SigVideoSettingsChanged();

    qInfo() << "[video] 应用画质：码率" << (bit_rate / 1000000.0) << "Mbps，帧率" << max_fps << "档位"
            << video_profile_ << (session_ == nullptr ? "（未连接，仅保存）" : "（断开重连）");

    // 没连设备就只存配置，下次连接自然生效
    if (session_ == nullptr || reader_ == nullptr)
    {
        SetStatus(QStringLiteral("画质已保存：%1 Mbps / %2 fps，下次连接生效")
                      .arg(bit_rate / 1000000.0, 0, 'f', 1)
                      .arg(max_fps > 0 ? QString::number(max_fps) : QStringLiteral("不限")));
        return;
    }

    const QString serial = device_name_;
    SetStatus(QStringLiteral("按新画质重连中（1~2 秒）…"));
    QCoreApplication::processEvents();

    // scrcpy 的码率和帧率只在 server 启动时生效，所以只能断开重连
    Disconnect();
    if (!serial.isEmpty())
    {
        Connect(serial);
    }
}

// 控制台窗口当前显示着没有
bool DeviceController::ShowConsole() const
{
    return show_console_;
}

// 显示或隐藏命令行窗口，并把选择记进配置
void DeviceController::SetShowConsole(bool show)
{
    if (show_console_ == show)
    {
        return;
    }

    show_console_ = show;
    qsc::base::SetConsoleWindowVisible(show_console_);
    AppSettings().SetShowConsole(show_console_);
    emit SigShowConsoleChanged();
    SetStatus(show_console_ ? QStringLiteral("命令行窗口已显示") : QStringLiteral("命令行窗口已隐藏"));
}

// 设备地址列表，和 deviceLabels 下标一一对应
QStringList DeviceController::Devices() const
{
    return scanner_->Devices();
}

// 下拉框显示文字
QStringList DeviceController::DeviceLabels() const
{
    return scanner_->Labels();
}

// 从 "ip route" 的输出里抠出本机 wifi 地址，例如 "src 192.168.31.115"
QString ParseWifiAddress(const QString &route_output)
{
    const QStringList words = route_output.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (int index = 0; index + 1 < words.size(); ++index)
    {
        if (words.at(index) == QStringLiteral("src"))
        {
            return words.at(index + 1);
        }
    }
    return QString();
}

namespace
{

// 本机所有非回环 IPv4，用来判断"哪个候选地址和我同网段"
QStringList HostLanAddresses()
{
    QStringList addresses;
    const QList<QHostAddress> all = QNetworkInterface::allAddresses();
    for (const QHostAddress &address : all)
    {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
        {
            addresses.append(address.toString());
        }
    }
    return addresses;
}

} // namespace

// 切无线：阻塞流程在 WifiBridge 里，这里只做转发
DeviceController::WifiSwitchResult DeviceController::SwitchToWifiBlocking(const QString &serial)
{
    return qsc::app::WifiBridge::Switch(serial);
}

// 切无线：先把当前会话收干净，阻塞的 adb 调用丢给工作线程，界面不会假死
void DeviceController::EnableWifi()
{
    if (wifi_busy_)
    {
        SetStatus(QStringLiteral("正在切无线，稍等"));
        return;
    }

    const QString serial = !device_name_.isEmpty() ? device_name_
                                                   : scanner_->FirstOnline();
    if (serial.isEmpty())
    {
        SetStatus(QStringLiteral("先插线连一台设备，才能切无线"));
        return;
    }

    // tcpip 会重启设备上的 adbd，当前连接必然断：先自己断开，界面才不会报"视频出错"
    if (session_)
    {
        Disconnect();
    }

    wifi_busy_ = true;
    SetStatus(QStringLiteral("正在切无线调试（会断开当前连接）…"));

    qsc::base::TaskRunner::Instance().Post([this, serial]()
    {
        const DeviceController::WifiSwitchResult result = DeviceController::SwitchToWifiBlocking(serial);
        QMetaObject::invokeMethod(this, [this, result]()
        {
            ApplyWifiResult(result);
        }, Qt::QueuedConnection);
    });
}

// 工作线程算完，回主线程刷新列表和状态
void DeviceController::ApplyWifiResult(const WifiSwitchResult &result)
{
    wifi_busy_ = false;
    RefreshDevices();

    if (!result.ok_)
    {
        SetStatus(result.message_);
        return;
    }

    scanner_->SetProposedSerial(result.endpoint_);
    emit SigProposedSerialChanged();
    SetStatus(QStringLiteral("无线已就绪：%1，可以拔线了，点「连接」").arg(result.endpoint_));
}

// 当前连着的设备串号，没连时是空
QString DeviceController::DeviceName() const
{
    return device_name_;
}

// 启动时挑串号：上次连过的 > 环境变量给的；两个都得在在线列表里，省得白等一次失败连接
QString DeviceController::PickStartupSerial(const QString &fallback) const
{
    return scanner_->PickStartupSerial(fallback);
}

// 改状态文字，值没变就不发信号
void DeviceController::SetStatus(const QString &status)
{
    if (status_ == status)
    {
        return;
    }
    status_ = status;
    emit SigStatusChanged();
}

// 跑 adb devices，把第一台在线设备填进建议值
void DeviceController::RefreshDevices()
{
    scanner_->Refresh();
}

// 同步建链并接到画布上，会阻塞一两秒
void DeviceController::Connect(const QString &serial)
{
    if (serial.trimmed().isEmpty())
    {
        SetStatus(QStringLiteral("先填序列号"));
        return;
    }
    if (reader_ != nullptr)
    {
        SetStatus(QStringLiteral("已经连着了，先断开"));
        return;
    }

    qsc::session::ServerParams params;
    params.serial_ = serial.trimmed();
    params.server_local_path_ = QCoreApplication::applicationDirPath() + "/scrcpy-server";
    AppSettings settings;                                                                       // 采集参数走配置，改 config.ini 就能调
    video_profile_ = AppSettings::VideoProfile(serial.trimmed());                               // 有线 / 无线各一套画质参数
    params.max_size_ = settings.VideoMaxSize();                                                 // 0 = 不缩放，用设备原生分辨率
    params.bit_rate_ = settings.VideoBitRate(video_profile_);
    params.max_fps_ = settings.VideoMaxFps(video_profile_);                                     // 0 = 不限，无线嫌卡就填 60
    emit SigVideoSettingsChanged();

    qInfo() << "[video] 本次连接画质：码率" << (params.bit_rate_ / 1000000.0) << "Mbps，帧率" << params.max_fps_
            << "档位" << video_profile_ << "串号" << params.serial_;

    // 无线才采样 WiFi 健康状态；有线不显示这一块
    if (video_profile_ == QStringLiteral("wireless"))
    {
        wifi_status_.Start(params.serial_);
    }
    else
    {
        wifi_status_.Stop();
    }
    params.log_level_ = QStringLiteral("error");

    // 这里同步阻塞 1~2 秒，界面会卡一下（建链是阻塞的）
    SetStatus(QStringLiteral("正在连接 %1 …").arg(params.serial_));
    QCoreApplication::processEvents();                                                          // 让状态文字先画出来

    device_name_ = params.serial_;
    emit SigDeviceNameChanged();

    session_ = std::make_unique<qsc::session::ServerSession>(params);
    const qsc::session::ConnectResult result = session_->Start();
    ApplyConnected(result);
}

// 连接结果落地：接解码器和读线程
void DeviceController::ApplyConnected(const qsc::session::ConnectResult &result)
{
    if (!result.ok_)
    {
        SetStatus(QStringLiteral("连接失败：%1").arg(result.error_));
        TearDown();
        return;
    }
    if (video_ == nullptr || video_->Surface() == nullptr)
    {
        SetStatus(QStringLiteral("连上了，但界面里没有视频画布"));
        TearDown();
        return;
    }

    decoder_ = std::make_unique<qsc::stream::H264Decoder>();
    QString error;
    // 配置里 [decode] hardware 决定走硬解还是软解
    decoder_->SetHardwareAllowed(AppSettings().DecodeHardware());
    decoder_->SetAsyncReadback(AppSettings().DecodeAsyncReadback());
    decoder_->SetSoftThreads(AppSettings().DecodeThreads());                                    // 软解线程数
    if (!decoder_->Open(&error))
    {
        SetStatus(QStringLiteral("解码器打开失败：%1").arg(error));
        TearDown();
        return;
    }

    QTcpSocket *video = session_->TakeVideoSocket();
    if (video == nullptr)
    {
        SetStatus(QStringLiteral("没有拿到视频 socket"));
        TearDown();
        return;
    }

    reader_ = new qsc::stream::VideoReader(video, decoder_.get());
    // socket 在主线程建、却在视频线程读：先摘 parent 再搬过去
    video->setParent(nullptr);
    video->moveToThread(reader_);

    // 直连：Present 跑在视频线程里，不吃 GUI 线程的事件循环
    QObject::connect(reader_, &qsc::stream::VideoReader::SigFrameReady, video_->Surface(), &VideoItem::Present,
                     Qt::DirectConnection);
    QObject::connect(reader_, &qsc::stream::VideoReader::SigFailed, this, [this](const QString &reason)
    {
        SetStatus(QStringLiteral("视频出错：%1").arg(reason));
    });
    reader_->start();

    // 控制 socket 是双向的：设备会主动推剪贴板、回 Ack，这里挂上读
    control_reader_ = session_->ControlSocket();
    if (control_reader_ != nullptr)
    {
        QObject::connect(control_reader_, &QTcpSocket::readyRead, this, &DeviceController::OnControlData);
        control_buffer_.clear();
    }

    // 开了自动同步就盯着电脑剪贴板
    if (AppSettings().ClipboardAutoSync() && clipboard_connection_ == QMetaObject::Connection())
    {
        clipboard_connection_ = QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
                                                 this, &DeviceController::OnLocalClipboardChanged);
    }

    // 记住这次连的设备，下次启动优先连它
    AppSettings().SetLastSerial(device_name_);

    // 连上之后读这台设备自己的键位配置，切换设备时不会串
    if (video_ != nullptr)
    {
        video_->LoadKeyMapFor(device_name_);
    }

    // 连上了才让投屏窗出来（启动时只显示主窗）
    if (!connected_)
    {
        connected_ = true;
        emit SigConnectedChanged();
    }

    SetStatus(QStringLiteral("已连接 %1  %2x%3%4")
                  .arg(result.device_name_)
                  .arg(result.frame_size_.width())
                  .arg(result.frame_size_.height())
                  .arg(result.used_forward_ ? QStringLiteral("  forward") : QString()));
}

// 断开并回到未连接状态
void DeviceController::Disconnect()
{
    wifi_status_.Stop();                                                                        // 断线就别再问手机了
    if (reader_ == nullptr && !session_)
    {
        SetStatus(QStringLiteral("没在连接"));
        return;
    }
    TearDown();
    SetStatus(QStringLiteral("已断开"));
}

// 视频模块把翻好的输入消息交过来：写进控制 socket，没连接就丢掉
bool DeviceController::WriteControl(const QByteArray &message)
{
    if (!session_ || message.isEmpty())
    {
        qCDebug(qsc::base::lcInput) << "没有会话，输入消息丢掉" << message.size();
        return false;
    }
    const bool sent = session_->SendControl(message);
    qCDebug(qsc::base::lcInput) << "写控制 socket" << message.size() << "字节 结果" << sent;
    return sent;
}

// 视频模块报状态：统一走主窗那条状态栏
void DeviceController::ReportStatus(const QString &text)
{
    SetStatus(text);
}

// 当前设备串号，adb 命令要用
QString DeviceController::DeviceSerial() const
{
    return device_name_;
}

// main 把视频控制器交进来：接帧信号、取画布都要用它
void DeviceController::AttachVideoController(VideoController *video)
{
    video_ = video;
}

// 停线程、清画布、关解码器、拆隧道
void DeviceController::TearDown()
{
    keeper_->RestoreIfHidden();

    // 控制 socket 的读连接跟着会话走，这里断干净
    if (control_reader_ != nullptr)
    {
        QObject::disconnect(control_reader_, nullptr, this, nullptr);
        control_reader_ = nullptr;
    }
    control_buffer_.clear();

    // 会话没了就把投屏窗收起来，回到"只有主窗"的状态
    if (connected_)
    {
        connected_ = false;
        emit SigConnectedChanged();
    }

    if (!device_name_.isEmpty())
    {
        device_name_.clear();
        emit SigDeviceNameChanged();
    }

    // 顺序不能反：读线程里存着解码器的裸指针，先停线程再销毁解码器
    if (reader_ != nullptr)
    {
        reader_->Stop();
        delete reader_;
        reader_ = nullptr;
    }
    if (decoder_)
    {
        decoder_->Close();
        decoder_.reset();
    }
    if (session_)
    {
        session_->Stop();
        session_.reset();
    }
}
