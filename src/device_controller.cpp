#include "device_controller.h"

#include "app_settings.h"
#include "base/log.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTcpSocket>

#include "adb/adb_runner.h"
#include "ui/video_item.h"

DeviceController::DeviceController(QObject *parent) : QObject(parent)
{
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
    }
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

    proposed_serial_ = serials.isEmpty() ? QString() : serials.first();
    emit ProposedSerialChanged();

    SetStatus(serials.isEmpty() ? QStringLiteral("没找到在线设备")
                                : QStringLiteral("找到 %1 台设备").arg(serials.size()));
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
    params.max_size_ = settings.VideoMaxSize();                                                 // 0 = 不缩放，用设备原生分辨率
    params.bit_rate_ = settings.VideoBitRate();
    params.log_level_ = QStringLiteral("error");

    // 这里同步阻塞 1~2 秒，界面会卡一下（建链是阻塞的）
    SetStatus(QStringLiteral("正在连接 %1 …").arg(params.serial_));
    QCoreApplication::processEvents();                                                          // 让状态文字先画出来

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
