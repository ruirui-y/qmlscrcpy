#include <QtTest>

#include <QByteArray>
#include <QStringList>

#include <initializer_list>

#include "adb/adb_args.h"
#include "adb/adb_runner.h"

using namespace qsc::adb;

namespace
{

// QCOMPARE 是宏，直接写 QStringList{"a", "b"} 的话花括号里的逗号会被当成宏参数分隔符。
// 包一层函数，逗号就落在括号里了。
QStringList Argv(std::initializer_list<QString> parts)
{
    QStringList list;
    for (const QString &part : parts)
    {
        list << part;
    }
    return list;
}

} // namespace

// adb 参数拼装的契约测试。
// 断言的是"最终交给 QProcess 的 argv 长什么样"——出问题的地方永远是参数顺序、
// -s 的位置、带空格的路径被拆成两个参数这类细节。
class AdbArgsTest : public QObject
{
    Q_OBJECT

private slots:
    void DevicesIsBareCommand();
    void SerialIsInsertedAtFront();
    void EmptySerialIsOmitted();
    void PushKeepsPathWithSpacesAsOneArgument();
    void ReverseUsesTcpLocalAndLocalAbstractRemote();
    void ForwardVariants();
    void ShellKeepsEveryWordSeparate();
    void TcpipAndConnectDoNotTakeSerial();
    void SocketNameIsFixedWidthHex();
    void ParseDevicesHandlesCrLfAndHeader();
    void ParseDevicesIgnoresAdbNoise();
    void RunRealAdbVersion();
};

void AdbArgsTest::DevicesIsBareCommand()
{
    QCOMPARE(AdbArgs::Devices(), Argv({QStringLiteral("devices")}));
}

void AdbArgsTest::SerialIsInsertedAtFront()
{
    const QStringList args = AdbArgs::Push(QStringLiteral("ABC"), QStringLiteral("C:/x.apk"),
                                           QStringLiteral("/sdcard/x.apk"));

    QCOMPARE(args.size(), 5);
    QCOMPARE(args.at(0), QStringLiteral("-s"));
    QCOMPARE(args.at(1), QStringLiteral("ABC"));
    QCOMPARE(args.at(2), QStringLiteral("push"));
}

void AdbArgsTest::EmptySerialIsOmitted()
{
    // 只有一台设备时 serial 可以为空，这时不能留下一个孤零零的 -s，
    // 否则 adb 会把后面的子命令当成序列号
    const QStringList args = AdbArgs::Shell(QString(), QStringList{QStringLiteral("getprop")});
    QCOMPARE(args, Argv({QStringLiteral("shell"), QStringLiteral("getprop")}));
}

void AdbArgsTest::PushKeepsPathWithSpacesAsOneArgument()
{
    const QStringList args = AdbArgs::Push(QStringLiteral("ABC"),
                                           QStringLiteral("C:/my dir/x.apk"),
                                           QStringLiteral("/sdcard/我的 文件.apk"));

    QCOMPARE(args.size(), 5);
    QCOMPARE(args.at(3), QStringLiteral("C:/my dir/x.apk"));
    QCOMPARE(args.at(4), QStringLiteral("/sdcard/我的 文件.apk"));
}

void AdbArgsTest::ReverseUsesTcpLocalAndLocalAbstractRemote()
{
    const QStringList args = AdbArgs::Reverse(QStringLiteral("ABC"),
                                              QStringLiteral("scrcpy_000003e7"), 27183);

    // reverse 是"设备端监听地址在前、主机目标在后"：设备和 forward 正好相反，写反了设备端连不上
    QCOMPARE(args, Argv({QStringLiteral("-s"), QStringLiteral("ABC"), QStringLiteral("reverse"),
                         QStringLiteral("localabstract:scrcpy_000003e7"),
                         QStringLiteral("tcp:27183")}));

    // 删的时候按设备端名字删，不是按端口
    QCOMPARE(AdbArgs::ReverseRemove(QStringLiteral("ABC"), QStringLiteral("scrcpy_000003e7")),
             Argv({QStringLiteral("-s"), QStringLiteral("ABC"), QStringLiteral("reverse"),
                   QStringLiteral("--remove"), QStringLiteral("localabstract:scrcpy_000003e7")}));
}

void AdbArgsTest::ForwardVariants()
{
    QCOMPARE(AdbArgs::Forward(QStringLiteral("ABC"), 27183, QStringLiteral("scrcpy_00000001")),
             Argv({QStringLiteral("-s"), QStringLiteral("ABC"), QStringLiteral("forward"),
                   QStringLiteral("tcp:27183"),
                   QStringLiteral("localabstract:scrcpy_00000001")}));

    QCOMPARE(AdbArgs::ForwardRemove(QStringLiteral("ABC"), 27183),
             Argv({QStringLiteral("-s"), QStringLiteral("ABC"), QStringLiteral("forward"),
                   QStringLiteral("--remove"), QStringLiteral("tcp:27183")}));
}

void AdbArgsTest::ShellKeepsEveryWordSeparate()
{
    const QStringList command{QStringLiteral("CLASSPATH=/data/local/tmp/scrcpy-server.jar"),
                              QStringLiteral("app_process"), QStringLiteral("/"),
                              QStringLiteral("com.genymobile.scrcpy.Server"),
                              QStringLiteral("3.3.3")};

    const QStringList args = AdbArgs::Shell(QStringLiteral("ABC"), command);

    // -s + serial + "shell" + 命令本身 5 段 = 8
    QCOMPARE(args.size(), 3 + command.size());
    QCOMPARE(args.at(0), QStringLiteral("-s"));
    QCOMPARE(args.at(1), QStringLiteral("ABC"));
    QCOMPARE(args.at(2), QStringLiteral("shell"));
    QCOMPARE(args.at(3), QStringLiteral("CLASSPATH=/data/local/tmp/scrcpy-server.jar"));
    QCOMPARE(args.at(7), QStringLiteral("3.3.3"));
}

void AdbArgsTest::TcpipAndConnectDoNotTakeSerial()
{
    QCOMPARE(AdbArgs::Tcpip(QStringLiteral("ABC"), 5555),
             Argv({QStringLiteral("-s"), QStringLiteral("ABC"), QStringLiteral("tcpip"),
                   QStringLiteral("5555")}));

    // connect / disconnect 是 adb 服务端的事，跟某一台设备无关，不能带 -s
    QCOMPARE(AdbArgs::Connect(QStringLiteral("192.168.0.174:5555")),
             Argv({QStringLiteral("connect"), QStringLiteral("192.168.0.174:5555")}));
    QCOMPARE(AdbArgs::Disconnect(QStringLiteral("192.168.0.174:5555")),
             Argv({QStringLiteral("disconnect"), QStringLiteral("192.168.0.174:5555")}));
}

void AdbArgsTest::SocketNameIsFixedWidthHex()
{
    QCOMPARE(AdbArgs::SocketName(0u), QStringLiteral("scrcpy_00000000"));
    QCOMPARE(AdbArgs::SocketName(999u), QStringLiteral("scrcpy_000003e7"));
    QCOMPARE(AdbArgs::SocketName(0xFFFFFFFFu), QStringLiteral("scrcpy_ffffffff"));

    // 定宽很重要：设备端按这个名字建 socket，宽度不一致就连不上
    QCOMPARE(AdbArgs::SocketName(1u).size(), 15);
}

void AdbArgsTest::ParseDevicesHandlesCrLfAndHeader()
{
    // Windows 上 adb 输出的是 \r\n，state 后面会带一个 \r，不 trim 就比不对
    const QByteArray sample = "List of devices attached\r\n"
                              "P7C0218510000537\tdevice\r\n"
                              "192.168.0.174:5555\tdevice\r\n"
                              "emulator-5554\toffline\r\n"
                              "R58M12345\tunauthorized\r\n"
                              "\r\n";

    const QList<AdbDevice> devices = ParseDevices(sample);

    QCOMPARE(devices.size(), 4);
    QCOMPARE(devices.at(0).serial_, QStringLiteral("P7C0218510000537"));
    QCOMPARE(devices.at(0).state_, QStringLiteral("device"));
    QCOMPARE(devices.at(1).serial_, QStringLiteral("192.168.0.174:5555"));
    QCOMPARE(devices.at(2).state_, QStringLiteral("offline"));
    QCOMPARE(devices.at(3).state_, QStringLiteral("unauthorized"));
}

void AdbArgsTest::ParseDevicesIgnoresAdbNoise()
{
    const QByteArray sample = "* daemon not running; starting now at tcp:5037\r\n"
                              "* daemon started successfully\r\n"
                              "List of devices attached\r\n"
                              "XYZ\tdevice\r\n";

    const QList<AdbDevice> devices = ParseDevices(sample);

    QCOMPARE(devices.size(), 1);
    QCOMPARE(devices.at(0).serial_, QStringLiteral("XYZ"));
    QCOMPARE(devices.at(0).state_, QStringLiteral("device"));
}

void AdbArgsTest::RunRealAdbVersion()
{
    // 真跑一次 adb，验证 QProcess 这条路通不通。
    // 没配 QS_ADB_PATH 就跳过：单测不该依赖机器上装没装 platform-tools。
    if (!qEnvironmentVariableIsSet("QS_ADB_PATH"))
    {
        QSKIP("未设置 QS_ADB_PATH，跳过真实 adb 调用");
    }

    SetAdbPath(QString::fromLocal8Bit(qgetenv("QS_ADB_PATH")));
    const AdbResult result = Run(QStringList{QStringLiteral("version")});

    QVERIFY2(result.started_, result.std_err_.constData());
    QVERIFY2(result.Ok(), result.std_err_.constData());
    QVERIFY(result.std_out_.contains("Android Debug Bridge"));
}

QTEST_GUILESS_MAIN(AdbArgsTest)

#include "tst_adb.moc"
