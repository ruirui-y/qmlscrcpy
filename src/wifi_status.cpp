#include "wifi_status.h"

#include <QProcess>
#include <QRegularExpression>

#include "adb/adb_runner.h"

namespace qsc::app
{

WifiStatus::WifiStatus(QObject *parent) : QObject(parent)
{
    timer_.setInterval(3000);
    QObject::connect(&timer_, &QTimer::timeout, this, &WifiStatus::SlotSample);
}

// 连上无线后开始采样；有线连接不调它，界面上自然就不显示
void WifiStatus::Start(const QString &serial)
{
    serial_ = serial.trimmed();
    last_tx_ok_ = 0;
    last_tx_retry_ = 0;
    last_lost_ = 0;
    timer_.start();
    SlotSample();
}

void WifiStatus::Stop()
{
    timer_.stop();
    serial_.clear();
    if (process_ != nullptr)
    {
        process_->kill();
        process_->deleteLater();
        process_ = nullptr;
    }
    if (available_)
    {
        available_ = false;
        summary_.clear();
        detail_.clear();
        level_ = 0;
        emit SigChanged();
    }
}

// 每 3 秒跑一次 adb；上一次还没结束就跳过，避免命令堆起来
void WifiStatus::SlotSample()
{
    if (serial_.isEmpty() || process_ != nullptr)
    {
        return;
    }

    process_ = new QProcess(this);
    process_->setProgram(qsc::adb::AdbPath());
    process_->setArguments(QStringList() << QStringLiteral("-s") << serial_ << QStringLiteral("shell")
                                         << QStringLiteral("cmd wifi status"));
    QObject::connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                     &WifiStatus::SlotFinished);
    QObject::connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { SlotFinished(); });
    process_->start();
}

// 命令结束：解析输出，算这一轮的重传率，更新等级并通知界面
void WifiStatus::SlotFinished()
{
    if (process_ == nullptr)
    {
        return;
    }
    const QString out = QString::fromUtf8(process_->readAllStandardOutput());
    process_->deleteLater();
    process_ = nullptr;
    Apply(out);
}

// 从一整段输出里挑出需要的几个数字，拼成一行文本
void WifiStatus::Apply(const QString &text)
{
    const QRegularExpression rssi_re(QStringLiteral("RSSI:\\s*(-?\\d+)"));
    const QRegularExpression speed_re(QStringLiteral("Link speed:\\s*(\\d+)Mbps"));
    const QRegularExpression freq_re(QStringLiteral("Frequency:\\s*(\\d+)MHz"));
    const QRegularExpression std_re(QStringLiteral("Wi-Fi standard:\\s*([^,\\s]+)"));
    const QRegularExpression ssid_re(QStringLiteral("connected to \"([^\"]+)\""));
    const QRegularExpression ok_re(QStringLiteral("successfulTxPackets:\\s*(\\d+)"));
    const QRegularExpression retry_re(QStringLiteral("retriedTxPackets:\\s*(\\d+)"));
    const QRegularExpression lost_re(QStringLiteral("lostTxPackets:\\s*(\\d+)"));
    const QRegularExpression bssid_re(QStringLiteral("BSSID: ([0-9a-f:]{17})"));

    const auto match = [&text](const QRegularExpression &re) { return re.match(text).captured(1); };
    const QString rssi_text = match(rssi_re);
    if (rssi_text.isEmpty())
    {
        // 有线连接或还没连上，安静地不显示
        if (available_)
        {
            available_ = false;
            summary_.clear();
            detail_.clear();
            level_ = 0;
            emit SigChanged();
        }
        return;
    }

    const int rssi = rssi_text.toInt();
    const int speed = match(speed_re).toInt();
    const int freq = match(freq_re).toInt();
    const QString standard = match(std_re);
    const QString ssid = match(ssid_re);
    const QString bssid = match(bssid_re);
    const qint64 tx_ok = match(ok_re).toLongLong();
    const qint64 tx_retry = match(retry_re).toLongLong();
    const qint64 lost = match(lost_re).toLongLong();

    // 重传率按"这一轮的增量"算，累计值会越跑越平滑，看不出当下好坏
    double retry_percent = 0.0;
    qint64 lost_delta = 0;
    if (last_tx_ok_ > 0 && tx_ok >= last_tx_ok_)
    {
        const qint64 ok_delta = tx_ok - last_tx_ok_;
        const qint64 retry_delta = tx_retry >= last_tx_retry_ ? (tx_retry - last_tx_retry_) : 0;
        lost_delta = lost >= last_lost_ ? (lost - last_lost_) : 0;
        const qint64 total = ok_delta + retry_delta;
        retry_percent = total > 0 ? (100.0 * static_cast<double>(retry_delta) / static_cast<double>(total)) : 0.0;
    }
    last_tx_ok_ = tx_ok;
    last_tx_retry_ = tx_retry;
    last_lost_ = lost;

    const QString band = freq < 3000 ? QStringLiteral("2.4G")
                        : (freq < 5925 ? QStringLiteral("5G") : QStringLiteral("6G"));

    summary_ = QStringLiteral("WiFi %1  %2 dBm  %3 Mbps 重传 %4%")
                   .arg(band)
                   .arg(rssi)
                   .arg(speed)
                   .arg(QString::number(retry_percent, 'f', 1));
    detail_ = QStringLiteral("SSID：%1\nBSSID：%2\n标准：%3\n频段：%4（%5 MHz）\n速率：%6 Mbps\n累计重传：%7 / 发送 %8\n累计丢包：%9")
                  .arg(ssid.isEmpty() ? QStringLiteral("-") : ssid)
                  .arg(bssid.isEmpty() ? QStringLiteral("-") : bssid)
                  .arg(standard.isEmpty() ? QStringLiteral("-") : standard)
                  .arg(band)
                  .arg(freq)
                  .arg(speed)
                  .arg(tx_retry)
                  .arg(tx_ok)
                  .arg(lost);
    level_ = Classify(rssi, retry_percent, lost_delta > 0);
    available_ = true;
    emit SigChanged();
}

// 0 好 / 1 一般 / 2 差 / 3 很差：信号和重传率谁差取谁
int WifiStatus::Classify(int rssi, double retry_percent, bool lost) const
{
    if (rssi < -78 || retry_percent > 20.0 || lost)
    {
        return 3;
    }
    if (rssi < -70 || retry_percent > 10.0)
    {
        return 2;
    }
    if (rssi < -60 || retry_percent > 5.0)
    {
        return 1;
    }
    return 0;
}

} // namespace qsc::app
