#include <QStringList>
#include <QtTest>

#include "adb/net_address.h"

// 无线地址解析与排序：纯字符串处理，不需要设备
class NetAddressTest : public QObject
{
    Q_OBJECT

private slots:
    void SameLan24ComparesPrefix();
    void RouteAddressesKeepsEveryInterface();
    void AddrInetParsesWlan0Output();
    void OrderByLocalLanPrefersSameSubnet();
};

// 前三位一样算同一个 /24；非法输入一律当不同
void NetAddressTest::SameLan24ComparesPrefix()
{
    QVERIFY(qsc::adb::SameLan24(QStringLiteral("192.168.31.114"), QStringLiteral("192.168.31.5")));
    QVERIFY(!qsc::adb::SameLan24(QStringLiteral("10.33.61.144"), QStringLiteral("192.168.31.5")));
    QVERIFY(!qsc::adb::SameLan24(QStringLiteral("192.168.31"), QStringLiteral("192.168.31.5")));
    QVERIFY(!qsc::adb::SameLan24(QString(), QString()));
}

// ip route 里可能同时有移动网络和 wifi，两个都要抓出来
void NetAddressTest::RouteAddressesKeepsEveryInterface()
{
    const QString output = QStringLiteral(
        "10.33.61.128/27 dev rmnet_data3 proto kernel scope link src 10.33.61.144 \n"
        "192.168.31.0/24 dev wlan0 proto kernel scope link src 192.168.31.114 \n");

    const QStringList addresses = qsc::adb::RouteAddresses(output);
    QCOMPARE(addresses.size(), 2);
    QCOMPARE(addresses.at(0), QStringLiteral("10.33.61.144"));
    QCOMPARE(addresses.at(1), QStringLiteral("192.168.31.114"));
}

// 路由表里没有 wifi 时，用 addr show 的输出兜底
void NetAddressTest::AddrInetParsesWlan0Output()
{
    const QString output = QStringLiteral(
        "27: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc htb state UP\n"
        "    inet 192.168.31.114/24 brd 192.168.31.255 scope global wlan0\n"
        "       valid_lft forever preferred_lft forever\n");

    QCOMPARE(qsc::adb::AddrInet(output), QStringLiteral("192.168.31.114"));
    QCOMPARE(qsc::adb::AddrInet(QString()), QString());
}

// 和本机同网段的排前面，其余保持原顺序
void NetAddressTest::OrderByLocalLanPrefersSameSubnet()
{
    const QStringList candidates = QStringList() << QStringLiteral("10.33.61.144")
                                                 << QStringLiteral("192.168.31.114")
                                                 << QStringLiteral("172.16.0.9");
    const QStringList local_addresses = QStringList() << QStringLiteral("192.168.31.20");

    const QStringList ordered = qsc::adb::OrderByLocalLan(candidates, local_addresses);
    QCOMPARE(ordered.size(), 3);
    QCOMPARE(ordered.at(0), QStringLiteral("192.168.31.114"));
    QCOMPARE(ordered.at(1), QStringLiteral("10.33.61.144"));
    QCOMPARE(ordered.at(2), QStringLiteral("172.16.0.9"));
}

QTEST_GUILESS_MAIN(NetAddressTest)

#include "tst_net_address.moc"
