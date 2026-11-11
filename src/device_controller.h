#pragma once

#include <QObject>
#include <QSize>
#include <QString>

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

public:
    explicit DeviceController(QObject *parent = nullptr);
    ~DeviceController();

    QString Status() const;
    QString ProposedSerial() const;                                                             // 刷新后建议填进输入框的序列号

    Q_INVOKABLE void SetSurface(QObject *surface);                                              // QML 里把 VideoSurface 传进来

    Q_INVOKABLE void RefreshDevices();                                                          // adb devices，刷新后填 proposedSerial
    Q_INVOKABLE void Connect(const QString &serial);                                            // 同步阻塞，连完才返回
    Q_INVOKABLE void Disconnect();

signals:
    void StatusChanged();
    void ProposedSerialChanged();

private:
    void ApplyConnected(const qsc::session::ConnectResult &result);                             // 连接结果落地：接解码器和读线程
    void TearDown();                                                                            // 停线程、关解码器、拆隧道
    void SetStatus(const QString &status);

    VideoItem *surface_ = nullptr;                                                              // QML 那条视频画布，由 main 注入
    QString status_;                                                                            // 给界面绑定的状态文字
    QString proposed_serial_;                                                                   // 刷新出来的第一台设备序列号

    std::unique_ptr<qsc::session::ServerSession> session_;                                      // 连接成功后一直持有
    std::unique_ptr<qsc::stream::H264Decoder> decoder_;                                         // 解码器，和读线程一一对应
    qsc::stream::VideoReader *reader_ = nullptr;                                                // 读线程，连着的时候存在
};
