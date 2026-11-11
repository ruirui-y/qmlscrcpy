#include "device_controller.h"

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

void DeviceController::SetSurface(VideoItem *surface)
{
    surface_ = surface;
}

void DeviceController::SetStatus(const QString &status)
{
    if (status_ == status) {
        return;
    }
    status_ = status;
    emit StatusChanged();
}

void DeviceController::RefreshDevices()
{
    // 只是列一下设备，几十毫秒，直接在主线程做
    const qsc::adb::AdbResult result = qsc::adb::Run(QStringList{QStringLiteral("devices")}, 5000);
    if (!result.Ok()) {
        SetStatus(QStringLiteral("adb devices 失败：%1").arg(result.ErrorText()));
        return;
    }

    QStringList serials;
    const QList<qsc::adb::AdbDevice> devices = qsc::adb::ParseDevices(result.std_out_);
    for (const qsc::adb::AdbDevice &device : devices) {
        if (device.state_ == QStringLiteral("device")) {
            serials << device.serial_;
        }
    }

    proposed_serial_ = serials.isEmpty() ? QString() : serials.first();
    emit ProposedSerialChanged();

    SetStatus(serials.isEmpty() ? QStringLiteral("没找到在线设备")
                                : QStringLiteral("找到 %1 台设备").arg(serials.size()));
}

void DeviceController::Connect(const QString &serial)
{
    if (serial.trimmed().isEmpty()) {
        SetStatus(QStringLiteral("先填序列号"));
        return;
    }
    if (reader_ != nullptr) {
        SetStatus(QStringLiteral("已经连着了，先断开"));
        return;
    }

    qsc::session::ServerParams params;
    params.serial_ = serial.trimmed();
    params.server_local_path_ = QCoreApplication::applicationDirPath() + "/scrcpy-server";
    params.max_size_ = 720;
    params.bit_rate_ = 2000000;
    params.log_level_ = QStringLiteral("error");

    // 这里同步阻塞 1~2 秒，界面会卡一下。
    // 不丢到工作线程的原因：ServerSession 里的 QTcpServer / QProcess 属于创建它的线程，
    // 在工作线程建、回主线程 Stop() 就会给别的线程的对象发事件，Qt 直接断言崩掉。
    // P4 会把它改成"一个专门的连接线程，session 从头到尾属于那个线程"。
    SetStatus(QStringLiteral("正在连接 %1 …").arg(params.serial_));
    QCoreApplication::processEvents();    // 让状态文字先画出来

    session_ = std::make_unique<qsc::session::ServerSession>(params);
    const qsc::session::ConnectResult result = session_->Start();
    ApplyConnected(result);
}

void DeviceController::ApplyConnected(const qsc::session::ConnectResult &result)
{
    if (!result.ok_) {
        SetStatus(QStringLiteral("连接失败：%1").arg(result.error_));
        TearDown();
        return;
    }
    if (surface_ == nullptr) {
        SetStatus(QStringLiteral("连上了，但界面里没有视频画布"));
        TearDown();
        return;
    }

    decoder_ = std::make_unique<qsc::stream::H264Decoder>();
    QString error;
    if (!decoder_->Open(&error)) {
        SetStatus(QStringLiteral("解码器打开失败：%1").arg(error));
        TearDown();
        return;
    }

    QTcpSocket *video = session_->TakeVideoSocket();
    if (video == nullptr) {
        SetStatus(QStringLiteral("没有拿到视频 socket"));
        TearDown();
        return;
    }

    reader_ = new qsc::stream::VideoReader(video, decoder_.get());
    // socket 是在主线程建的，读却发生在视频线程：先摘 parent 再搬过去，
    // 否则 Qt 会一直报 "Socket notifiers cannot be enabled or disabled from another thread"
    video->setParent(nullptr);
    video->moveToThread(reader_);

    QObject::connect(reader_, &qsc::stream::VideoReader::FrameReady, surface_, &VideoItem::Present);
    QObject::connect(reader_, &qsc::stream::VideoReader::Failed, this, [this](const QString &reason) {
        SetStatus(QStringLiteral("视频出错：%1").arg(reason));
    });
    reader_->start();

    SetStatus(QStringLiteral("已连接 %1  %2x%3%4")
                  .arg(result.device_name_)
                  .arg(result.frame_size_.width())
                  .arg(result.frame_size_.height())
                  .arg(result.used_forward_ ? QStringLiteral("  forward") : QString()));
}

void DeviceController::Disconnect()
{
    if (reader_ == nullptr && !session_) {
        SetStatus(QStringLiteral("没在连接"));
        return;
    }
    TearDown();
    SetStatus(QStringLiteral("已断开"));
}

void DeviceController::TearDown()
{
    // 顺序不能反：读线程里存着解码器的裸指针，先停线程再销毁解码器
    if (reader_ != nullptr) {
        reader_->Stop();
        delete reader_;
        reader_ = nullptr;
    }
    if (decoder_) {
        decoder_->Close();
        decoder_.reset();
    }
    if (session_) {
        session_->Stop();
        session_.reset();
    }
}
