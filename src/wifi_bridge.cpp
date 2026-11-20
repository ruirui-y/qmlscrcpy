#include "wifi_bridge.h"

#include "adb/adb_args.h"
#include "adb/adb_runner.h"
#include "adb/net_address.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QThread>

namespace qsc::app
{

QStringList WifiBridge::HostLanAddresses()
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

// 切无线的全部阻塞动作：adb tcpip → 轮询收集候选 IP → 同网段优先逐个 connect
WifiSwitchResult WifiBridge::Switch(const QString &serial)
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

    result.message_ = QStringLiteral("adb connect 都失败（试过 %1）：电脑和设备要连同一个 wifi").arg(tried.trimmed());
    return result;
}

} // namespace qsc::app
