#pragma once

// 播放参数：画质（码率/帧率）和解码方式（硬解/软解、软解线程数、读回方式）。
// 只负责"读写配置 + 记住当前档位"，不碰连接、不碰 QML —— 重连由 DeviceController 做。

#include <QObject>
#include <QString>

namespace qsc::app
{

class PlaybackSettings : public QObject
{
    Q_OBJECT

public:
    explicit PlaybackSettings(QObject *parent = nullptr);

    void SetProfile(const QString &profile);                                                    // 有线 wired / 无线 wireless
    QString Profile() const { return profile_; }

    int BitRate() const;                                                                        // bps
    int MaxFps() const;                                                                         // 0 = 不限
    void ApplyVideo(int bit_rate, int max_fps);                                                 // 写进配置（画质）

    bool Hardware() const;                                                                      // 硬解？
    void SetHardware(bool hardware);
    int Threads() const;                                                                        // 软解线程数 1~8
    void SetThreads(int threads);
    bool AsyncReadback() const;                                                                 // 异步读回？
    void SetAsyncReadback(bool enabled);

signals:
    void SigVideoChanged();                                                                     // 画质变了
    void SigDecodeChanged();                                                                    // 解码方式/线程/读回方式变了

private:
    QString profile_ = QStringLiteral("wired");                                                 // 当前码率档位
};

} // namespace qsc::app
