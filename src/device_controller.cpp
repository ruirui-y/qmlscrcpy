#include "device_controller.h"

#include "app_settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QClipboard>
#include <QRegularExpression>
#include <QThread>
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

#include <QDebug>
#include <QTcpSocket>

#include "adb/adb_runner.h"
#include "ui/video_item.h"

DeviceController::DeviceController(QObject *parent) : QObject(parent)
{
    // 滚轮发法跟配置走：界面里选过的优先，没选过就用 config.ini 的默认值
    AppSettings settings;
    wheel_mode_ = settings.WheelMode();
    drag_pixels_ = settings.DragPixels();
    show_console_ = settings.ShowConsole();
    status_ = QStringLiteral("未连接");
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
    return proposed_serial_;
}

// QML 把视频画布交进来，连上后的帧就送这里
void DeviceController::SetSurface(QObject *surface)
{
    surface_ = qobject_cast<VideoItem *>(surface);
    if (surface_ == nullptr)
    {
        qWarning() << "[controller] 传进来的不是 VideoSurface";
        return;
    }

    // 画布把鼠标键盘翻成控制消息，这里负责写进控制 socket
    QObject::connect(surface_, &VideoItem::ControlMessageReady, this, &DeviceController::SendControl);
    surface_->SetWheelMode(wheel_mode_);
    surface_->SetDragPixels(drag_pixels_);
}

// 当前生效的码率（bps），界面用来显示
int DeviceController::VideoBitRate() const
{
    return AppSettings().VideoBitRate(video_profile_);
}

// 当前生效的帧率上限，0 表示不限
int DeviceController::VideoMaxFps() const
{
    return AppSettings().VideoMaxFps(video_profile_);
}

// 当前用的是哪一档画质参数：有线还是无线
QString DeviceController::VideoProfile() const
{
    return video_profile_;
}

// 改码率和帧率：这两个是 server 启动参数，改完必须重连才生效
void DeviceController::ApplyVideoSettings(int bit_rate, int max_fps)
{
    AppSettings settings;
    settings.SetVideoBitRate(bit_rate, video_profile_);
    settings.SetVideoMaxFps(max_fps, video_profile_);
    emit VideoSettingsChanged();

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
    emit ShowConsoleChanged();
    SetStatus(show_console_ ? QStringLiteral("命令行窗口已显示") : QStringLiteral("命令行窗口已隐藏"));
}
// 设备地址列表，和 deviceLabels 下标一一对应
QStringList DeviceController::Devices() const
{
    return devices_;
}

// 下拉框显示文字
QStringList DeviceController::DeviceLabels() const
{
    return device_labels_;
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

// 切无线的全部阻塞动作：adb tcpip → 轮询收集候选 IP → 同网段优先逐个 connect
DeviceController::WifiSwitchResult DeviceController::SwitchToWifiBlocking(const QString &serial)
{
    WifiSwitchResult result;

    const qsc::adb::AdbResult tcpip = qsc::adb::Run(qsc::adb::AdbArgs::Tcpip(serial, 5555), 10000);
    if (!tcpip.Ok())
    {
        result.message_ = QStringLiteral("切无线失败：%1").arg(tcpip.ErrorText());
        return result;
    }

    // adbd 重启要一两秒，轮询到能读到地址为止；手机上可能同时有移动网络，全收下来再排序
    QStringList candidates;
    for (int attempt = 0; attempt < 8 && candidates.isEmpty(); ++attempt)
    {
        QThread::msleep(500);
        const qsc::adb::AdbResult route = qsc::adb::Run(
            qsc::adb::AdbArgs::Shell(serial, QStringList() << QStringLiteral("ip") << QStringLiteral("route")), 5000);
        candidates = qsc::adb::RouteAddresses(QString::fromUtf8(route.std_out_));
        if (!candidates.isEmpty())
        {
            break;
        }
        const qsc::adb::AdbResult addr = qsc::adb::Run(
            qsc::adb::AdbArgs::Shell(serial, QStringList() << QStringLiteral("ip") << QStringLiteral("-f")
                                                           << QStringLiteral("inet") << QStringLiteral("addr")
                                                           << QStringLiteral("show") << QStringLiteral("wlan0")),
            5000);
        const QString inet = qsc::adb::AddrInet(QString::fromUtf8(addr.std_out_));
        if (!inet.isEmpty())
        {
            candidates.append(inet);
        }
    }

    if (candidates.isEmpty())
    {
        result.message_ = QStringLiteral("没读到设备地址：检查手机是否连着 wifi");
        return result;
    }

    // 和本机同网段的排前面，逐个试；成功一个就收工
    const QStringList ordered = qsc::adb::OrderByLocalLan(candidates, HostLanAddresses());
    QString tried;
    for (const QString &address : ordered)
    {
        const QString endpoint = QStringLiteral("%1:5555").arg(address);
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const qsc::adb::AdbResult connected = qsc::adb::Run(qsc::adb::AdbArgs::Connect(endpoint), 8000);
            const QString output = QString::fromUtf8(connected.std_out_);
            if (connected.Ok() && !output.contains(QStringLiteral("failed")))
            {
                result.ok_ = true;
                result.endpoint_ = endpoint;
                return result;
            }
            QThread::msleep(600);
        }
        tried += endpoint + QLatin1Char(' ');
    }

    result.message_ = QStringLiteral("adb connect 都失败（试过 %1）：电脑和设备要连同一个 wifi")
                          .arg(tried.trimmed());
    return result;
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
                                                   : (online_serials_.isEmpty() ? QString() : online_serials_.first());
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

    QThread *worker = QThread::create([this, serial]()
    {
        const DeviceController::WifiSwitchResult result = DeviceController::SwitchToWifiBlocking(serial);
        QMetaObject::invokeMethod(this, [this, result]()
        {
            ApplyWifiResult(result);
        }, Qt::QueuedConnection);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
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

    proposed_serial_ = result.endpoint_;
    emit ProposedSerialChanged();
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
    AppSettings settings;
    const QString last = settings.LastSerial();
    if (!last.isEmpty() && online_serials_.contains(last))
    {
        return last;
    }
    if (!fallback.isEmpty() && online_serials_.contains(fallback))
    {
        return fallback;
    }
    return QString();
}
// 改状态文字，值没变就不发信号
void DeviceController::SetStatus(const QString &status)
{
    if (status_ == status)
    {
        return;
    }
    status_ = status;
    emit StatusChanged();
}

// 跑 adb devices，把第一台在线设备填进建议值
void DeviceController::RefreshDevices()
{
    // 只是列一下设备，几十毫秒，直接在主线程做
    const qsc::adb::AdbResult result = qsc::adb::Run(QStringList{QStringLiteral("devices")}, 5000);
    if (!result.Ok())
    {
        SetStatus(QStringLiteral("adb devices 失败：%1").arg(result.ErrorText()));
        return;
    }

    QStringList serials;
    const QList<qsc::adb::AdbDevice> devices = qsc::adb::ParseDevices(result.std_out_);
    for (const qsc::adb::AdbDevice &device : devices)
    {
        if (device.state_ == QStringLiteral("device"))
        {
            serials << device.serial_;
        }
    }

    // 再扫一遍无线调试广播的设备：已经连过的和刚发现的一起给用户选
    const qsc::adb::AdbResult mdns = qsc::adb::Run(QStringList() << QStringLiteral("mdns") << QStringLiteral("services"), 4000);
    const QStringList mdns_lines = QString::fromUtf8(mdns.std_out_).split(QLatin1Char('\n'));
    for (const QString &line : mdns_lines)
    {
        if (!line.contains(QStringLiteral("_adb-tls-connect._tcp")))
        {
            continue;
        }
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 3)
        {
            continue;
        }
        const QString endpoint = parts.last();
        if (!serials.contains(endpoint))
        {
            serials.append(endpoint);
        }
    }

    online_serials_ = serials;
    devices_ = serials;
    device_labels_.clear();
    for (const QString &serial : serials)
    {
        device_labels_.append(serial.contains(QLatin1Char(':')) ? QStringLiteral("无线  %1").arg(serial)
                                                                : QStringLiteral("USB   %1").arg(serial));
    }
    proposed_serial_ = serials.isEmpty() ? QString() : serials.first();
    emit DevicesChanged();
    emit ProposedSerialChanged();

    SetStatus(serials.isEmpty() ? QStringLiteral("没找到在线设备")
                                : QStringLiteral("找到 %1 台设备").arg(serials.size()));
}

// 滚轮模式：立即生效、写进 window.ini，下次启动还是它
void DeviceController::SetWheelMode(const QString &mode)
{
    const QString normalized = mode == QStringLiteral("drag") ? QStringLiteral("drag") : QStringLiteral("scroll");
    if (wheel_mode_ == normalized)
    {
        return;
    }

    wheel_mode_ = normalized;
    if (surface_ != nullptr)
    {
        surface_->SetWheelMode(wheel_mode_);
    surface_->SetDragPixels(drag_pixels_);
    }
    AppSettings settings;
    settings.SetWheelMode(wheel_mode_);
    emit WheelModeChanged();
    SetStatus(QStringLiteral("滚轮模式：%1").arg(wheel_mode_ == QStringLiteral("drag") ? QStringLiteral("滑动") : QStringLiteral("滚动")));
}

QString DeviceController::WheelMode() const
{
    return wheel_mode_;
}

// 滑动像素加减：范围夹在 10 到 300，太小会被当点击，太大一下翻过头
void DeviceController::AdjustDragPixels(int delta)
{
    const int adjusted = qBound(10, drag_pixels_ + delta, 300);
    if (adjusted == drag_pixels_)
    {
        return;
    }

    drag_pixels_ = adjusted;
    if (surface_ != nullptr)
    {
        surface_->SetDragPixels(drag_pixels_);
    }
    AppSettings settings;
    settings.SetDragPixels(drag_pixels_);
    emit DragPixelsChanged();
    SetStatus(QStringLiteral("滑动像素：%1").arg(drag_pixels_));
}

int DeviceController::DragPixels() const
{
    return drag_pixels_;
}
// 发一次按键：按下和抬起各一条，安卓才会真正触发
void DeviceController::SendKeycode(quint32 keycode)
{
    if (!session_)
    {
        return;
    }

    qsc::protocol::KeycodeEvent event;
    event.keycode_ = keycode;
    event.action_ = 0;
    session_->SendControl(qsc::protocol::ControlMessage::InjectKeycode(event));
    event.action_ = 1;
    session_->SendControl(qsc::protocol::ControlMessage::InjectKeycode(event));
}

// 音量加：走安卓的 KEYCODE_VOLUME_UP（24）
void DeviceController::SendVolumeUp()
{
    SendKeycode(24);
}

// 音量减：KEYCODE_VOLUME_DOWN（25）
void DeviceController::SendVolumeDown()
{
    SendKeycode(25);
}

// 让设备转屏幕：一条无负载消息
void DeviceController::RotateDevice()
{
    if (!session_)
    {
        return;
    }
    session_->SendControl(qsc::protocol::ControlMessage::RotateDevice());
}

// 截图：抓投屏窗当前内容，存到 exe 旁边的 screenshots 目录
void DeviceController::SaveScreenshot()
{
    if (surface_ == nullptr || surface_->window() == nullptr)
    {
        SetStatus(QStringLiteral("没有画面，截不了图"));
        return;
    }

    const QImage image = surface_->window()->grabWindow();
    const QString name = QStringLiteral("shot_%1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/screenshots/") + name;
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool saved = image.save(path);

    // 顺手放进系统剪贴板，截图就是为了贴给别人看
    QGuiApplication::clipboard()->setImage(image);

    SetStatus(saved ? QStringLiteral("截图已保存并复制：%1").arg(name) : QStringLiteral("截图只复制到剪贴板，保存失败"));
    qCInfo(qsc::base::lcRender) << "截图" << path << saved;
}
// 返回上一级：走专门的消息，它在设备息屏时还能顺手点亮屏幕
void DeviceController::SendBack()
{
    if (!session_)
    {
        return;
    }
    session_->SendControl(qsc::protocol::ControlMessage::BackOrScreenOn(true));
    session_->SendControl(qsc::protocol::ControlMessage::BackOrScreenOn(false));
}

// 多任务切换：安卓 KEYCODE_APP_SWITCH（187）
// 不用 KEYCODE_MENU(82)——现代安卓应用基本不响应它，弹不出最近任务
void DeviceController::SendRecents()
{
    SendKeycode(187);
}
// QML 把控制窗交进来：主窗按钮要靠它把窗口提到前台
// 息屏或亮屏：只关显示，不动锁屏状态
void DeviceController::SetDisplayPower(bool on)
{
    if (!session_)
    {
        return;
    }
    session_->SendControl(qsc::protocol::ControlMessage::SetDisplayPower(on));
    SetStatus(on ? QStringLiteral("已亮屏") : QStringLiteral("已息屏"));
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
    AppSettings settings;                                                                        // 采集参数走配置，改 config.ini 就能调
    video_profile_ = AppSettings::VideoProfile(serial.trimmed());                                // 有线 / 无线各一套画质参数
    params.max_size_ = settings.VideoMaxSize();                                                 // 0 = 不缩放，用设备原生分辨率
    params.bit_rate_ = settings.VideoBitRate(video_profile_);
    params.max_fps_ = settings.VideoMaxFps(video_profile_);                                     // 0 = 不限，无线嫌卡就填 60
    emit VideoSettingsChanged();
    params.log_level_ = QStringLiteral("error");

    // 这里同步阻塞 1~2 秒，界面会卡一下（建链是阻塞的）
    SetStatus(QStringLiteral("正在连接 %1 …").arg(params.serial_));
    QCoreApplication::processEvents();                                                          // 让状态文字先画出来

    device_name_ = params.serial_;
    emit DeviceNameChanged();

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
    if (surface_ == nullptr)
    {
        SetStatus(QStringLiteral("连上了，但界面里没有视频画布"));
        TearDown();
        return;
    }

    decoder_ = std::make_unique<qsc::stream::H264Decoder>();
    QString error;
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
    QObject::connect(reader_, &qsc::stream::VideoReader::FrameReady, surface_, &VideoItem::Present,
                     Qt::DirectConnection);
    QObject::connect(reader_, &qsc::stream::VideoReader::Failed, this, [this](const QString &reason)
    {
        SetStatus(QStringLiteral("视频出错：%1").arg(reason));
    });
    reader_->start();

    // 记住这次连的设备，下次启动优先连它
    AppSettings().SetLastSerial(device_name_);

    SetStatus(QStringLiteral("已连接 %1  %2x%3%4")
                  .arg(result.device_name_)
                  .arg(result.frame_size_.width())
                  .arg(result.frame_size_.height())
                  .arg(result.used_forward_ ? QStringLiteral("  forward") : QString()));
}

// 断开并回到未连接状态
void DeviceController::Disconnect()
{
    if (reader_ == nullptr && !session_)
    {
        SetStatus(QStringLiteral("没在连接"));
        return;
    }
    TearDown();
    SetStatus(QStringLiteral("已断开"));
}

// 停线程、清画布、关解码器、拆隧道
// 把画布翻好的输入消息写进控制 socket；没连接时静默丢掉
void DeviceController::SendControl(const QByteArray &message)
{
    if (!session_ || message.isEmpty())
    {
        qCDebug(qsc::base::lcInput) << "没有会话，输入消息丢掉" << message.size();
        return;
    }
    const bool sent = session_->SendControl(message);
    qCDebug(qsc::base::lcInput) << "写控制 socket" << message.size() << "字节 结果" << sent;
}
// 停线程、清画布、关解码器、拆隧道
void DeviceController::TearDown()
{
    if (!device_name_.isEmpty())
    {
        device_name_.clear();
        emit DeviceNameChanged();
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
