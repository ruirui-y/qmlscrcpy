#include <QtTest>

#include <QByteArray>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpSocket>

#include "protocol/video_packet.h"
#include "session/server_session.h"

using namespace qsc::protocol;
using namespace qsc::session;

// 真机联调用例：把 server 推上去、拉起来、读设备信息、再读第一个视频包。
//
// 默认跳过——单测不该依赖"这台机器上正好插着设备"。
// 要跑就设两个环境变量：
//   QS_ADB_PATH       adb.exe 路径
//   QS_DEVICE_SERIAL  设备序列号（adb devices 里那一列）
// server 文件默认用仓库里的 third_party/scrcpy-server，也可用 QS_SCRCPY_SERVER 覆盖。
class ServerSessionTest : public QObject
{
    Q_OBJECT

private slots:
    void ConnectAndReadFirstPacket();
};

// 真机建链并读到第一个视频包
void ServerSessionTest::ConnectAndReadFirstPacket()
{
    if (!qEnvironmentVariableIsSet("QS_ADB_PATH")
        || !qEnvironmentVariableIsSet("QS_DEVICE_SERIAL"))
        {
        QSKIP("未设置 QS_ADB_PATH / QS_DEVICE_SERIAL，跳过真机用例");
    }

    const QString serial = QString::fromLocal8Bit(qgetenv("QS_DEVICE_SERIAL"));
    QString server_path = QString::fromLocal8Bit(qgetenv("QS_SCRCPY_SERVER"));
    if (server_path.isEmpty())
    {
        server_path = QStringLiteral(QS_DEFAULT_SERVER_PATH);
    }

    ServerParams params;
    params.serial_ = serial;
    params.server_local_path_ = server_path;
    params.bit_rate_ = 2000000;
    params.max_size_ = 720;
    params.log_level_ = QStringLiteral("debug");
    // 设 QS_FORWARD=1 强制走 forward 模式，用来隔离 reverse 的问题
    if (qEnvironmentVariableIsSet("QS_FORWARD"))
    {
        params.use_reverse_ = false;
    }

    ServerSession session(params);
    const ConnectResult result = session.Start();

    const QByteArray error_text = result.error_.toUtf8();
    QVERIFY2(result.ok_, error_text.constData());
    QVERIFY(!result.device_name_.isEmpty());
    QVERIFY(result.frame_size_.width() > 0);
    QVERIFY(result.frame_size_.height() > 0);

    qInfo() << "设备:" << result.device_name_ << "分辨率:" << result.frame_size_
            << "模式:" << (result.used_forward_ ? "forward" : "reverse");

    // 拿视频 socket 读第一个包，验证包头解析在真机流上也成立
    QTcpSocket *video = session.TakeVideoSocket();
    QVERIFY(video != nullptr);

    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();
    while (buffer.size() < VideoPacketHeader::kSize && timer.elapsed() < 5000)
    {
        if (video->waitForReadyRead(500))
        {
            buffer.append(video->readAll());
        }
    }
    QVERIFY2(buffer.size() >= VideoPacketHeader::kSize, "5 秒内没收到完整的包头");

    VideoPacketHeader header;
    QVERIFY(VideoPacketHeader::Parse(buffer, header));
    QVERIFY(header.size_ > 0);
    qInfo() << "首包:" << header.size_ << "字节; pts:" << header.PtsUs()
            << "; 配置包:" << header.IsConfig() << "; 关键帧:" << header.IsKeyFrame();

    video->close();
    delete video;
    session.Stop();
}

QTEST_GUILESS_MAIN(ServerSessionTest)

#include "tst_server_session.moc"
