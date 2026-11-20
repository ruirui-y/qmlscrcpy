#include "device_scanner.h"

#include "adb/adb_args.h"
#include "adb/adb_runner.h"
#include "app_settings.h"

#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>

namespace qsc::app
{

DeviceScanner::DeviceScanner(QObject *parent)
    : QObject(parent)
{
}

void DeviceScanner::SetProposedSerial(const QString &serial)
{
    if (proposed_serial_ == serial)
    {
        return;
    }
    proposed_serial_ = serial;
    emit SigProposedSerialChanged();
}

// 在线设备里的第一台，用来在没有已连接设备时兜底
QString DeviceScanner::FirstOnline() const
{
    return online_serials_.isEmpty() ? QString() : online_serials_.first();
}

// 上次连过的设备还在线就用它，否则用传进来的兜底
QString DeviceScanner::PickStartupSerial(const QString &fallback) const
{
    const QString last = AppSettings().LastSerial();
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

// 跑一次 adb devices，再补上 mdns 广播里的无线设备
void DeviceScanner::Refresh()
{
    const qint64 start_ms = QDateTime::currentMSecsSinceEpoch();
    const qsc::adb::AdbResult result = qsc::adb::Run(QStringList{QStringLiteral("devices")}, 15000);
    qInfo() << "[adb] devices 耗时" << (QDateTime::currentMSecsSinceEpoch() - start_ms) << "ms";
    if (!result.Ok())
    {
        emit SigStatus(QStringLiteral("adb devices 失败：%1").arg(result.ErrorText()));
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

    // 无线调试的设备不在 adb devices 里，得从 mdns 广播里捞
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
    emit SigDevicesChanged();
    emit SigProposedSerialChanged();

    emit SigStatus(serials.isEmpty() ? QStringLiteral("没找到在线设备")
                                     : QStringLiteral("找到 %1 台设备").arg(serials.size()));
}

} // namespace qsc::app
