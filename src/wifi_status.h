#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

class QProcess;

namespace qsc::app
{

// WiFi 健康状态：每 3 秒问一次手机 `cmd wifi status`，把 RSSI、频段、速率、
// 重传率整理成一行文本和一个颜色等级，界面只看等级就知道卡是不是信号问题。
class WifiStatus : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ Available NOTIFY SigChanged)                                  // 当前是不是无线连接且拿到了数据
    Q_PROPERTY(QString summary READ Summary NOTIFY SigChanged)                                  // 一行：WiFi 5G -46 dBm · 1921 Mbps · 重传 1.2%
    Q_PROPERTY(QString detail READ Detail NOTIFY SigChanged)                                    // 悬停详情：SSID / BSSID / 标准 / Tx Rx / 丢包
    Q_PROPERTY(int level READ Level NOTIFY SigChanged)                                          // 0 好 / 1 一般 / 2 差 / 3 很差

public:
    explicit WifiStatus(QObject *parent = nullptr);

    // 连上无线之后调一次；串号变了重新调，Stop 会停掉定时器
    void Start(const QString &serial);
    void Stop();

    bool Available() const { return available_; }
    QString Summary() const { return summary_; }
    QString Detail() const { return detail_; }
    int Level() const { return level_; }

signals:
    void SigChanged();

private slots:
    void SlotSample();
    void SlotFinished();

private:
    // 解析 `cmd wifi status` 的一整段输出，失败就把 available 置 false
    void Apply(const QString &text);
    int Classify(int rssi, double retry_percent, bool lost) const;

    QTimer timer_;                                                                              // 3 秒一次
    QProcess *process_ = nullptr;                                                               // 同一时刻只跑一个
    QString serial_;                                                                            // 当前采样的设备
    bool available_ = false;
    QString summary_;
    QString detail_;
    int level_ = 0;
    qint64 last_tx_ok_ = 0;                                                                     // 上一次的成功发送包数，用来算这一轮的增量
    qint64 last_tx_retry_ = 0;
    qint64 last_lost_ = 0;
};

} // namespace qsc::app
