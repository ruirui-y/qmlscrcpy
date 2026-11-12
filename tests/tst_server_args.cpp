#include <QtTest>

#include <QStringList>

#include "session/server_args.h"

using namespace qsc::session;

namespace
{

// QCOMPARE 是宏，直接写 QStringList{"a", "b"} 的逗号会被当成宏参数分隔符，包一层
QStringList Argv(std::initializer_list<QString> parts)
{
    QStringList list;
    for (const QString &part : parts)
    {
        list << part;
    }
    return list;
}

// 参数里有没有以某个前缀开头的一段
bool HasArg(const QStringList &args, const QString &prefix)
{
    for (const QString &arg : args)
    {
        if (arg.startsWith(prefix))
        {
            return true;
        }
    }
    return false;
}

} // namespace

// server 启动参数的契约测试。
// 断言的重点不是"有这些参数"，而是"默认情况下哪些参数不能出现"——
// 多传一个参数就可能让三星机型 stack corruption。
class ServerArgsTest : public QObject
{
    Q_OBJECT

private slots:
    void DefaultArgs_shape();
    void DefaultArgs_omitsOptionalParams();
    void MaxSizeZeroIsOmitted();
    void MaxFpsOnlyWhenPositive();
    void TunnelForwardOnlyInForwardMode();
    void CaptureOrientationVariants_data();
    void CaptureOrientationVariants();
    void OptionalParamsAppearOnlyWhenSet();
    void ScidIsFixedWidthHexWhenSet();
};

// 默认参数只带必须的那几个
void ServerArgsTest::DefaultArgs_shape()
{
    ServerParams params;
    const QStringList args = ServerArgs::Build(params);

    // 前五段是固定套路，顺序不能动：CLASSPATH、app_process、/、主类、版本号
    QCOMPARE(args.at(0), QStringLiteral("CLASSPATH=/data/local/tmp/scrcpy-server.jar"));
    QCOMPARE(args.at(1), QStringLiteral("app_process"));
    QCOMPARE(args.at(2), QStringLiteral("/"));
    QCOMPARE(args.at(3), QStringLiteral("com.genymobile.scrcpy.Server"));
    QCOMPARE(args.at(4), QStringLiteral("3.3.3"));

    QVERIFY(args.contains(QStringLiteral("video_bit_rate=8000000")));
    QVERIFY(args.contains(QStringLiteral("audio=false")));
}

// 没设置的可选参数不出现
void ServerArgsTest::DefaultArgs_omitsOptionalParams()
{
    ServerParams params;
    const QStringList args = ServerArgs::Build(params);

    // 能用服务端默认值的一律不传，否则参数太长三星会崩
    QCOMPARE(HasArg(args, QStringLiteral("tunnel_forward")), false);
    QCOMPARE(HasArg(args, QStringLiteral("crop=")), false);
    QCOMPARE(HasArg(args, QStringLiteral("control=")), false);
    QCOMPARE(HasArg(args, QStringLiteral("stay_awake")), false);
    QCOMPARE(HasArg(args, QStringLiteral("codec_options")), false);
    QCOMPARE(HasArg(args, QStringLiteral("encoder_name")), false);
    QCOMPARE(HasArg(args, QStringLiteral("max_fps")), false);
    QCOMPARE(HasArg(args, QStringLiteral("scid=")), false);
}

// max_size 为 0 时不传
void ServerArgsTest::MaxSizeZeroIsOmitted()
{
    ServerParams params;
    params.max_size_ = 0;
    QCOMPARE(HasArg(ServerArgs::Build(params), QStringLiteral("max_size")), false);

    params.max_size_ = 1080;
    QVERIFY(ServerArgs::Build(params).contains(QStringLiteral("max_size=1080")));
}

// max_fps 只有正数才传
void ServerArgsTest::MaxFpsOnlyWhenPositive()
{
    ServerParams params;
    params.max_fps_ = 60;
    QVERIFY(ServerArgs::Build(params).contains(QStringLiteral("max_fps=60")));

    params.max_fps_ = 0;
    QCOMPARE(HasArg(ServerArgs::Build(params), QStringLiteral("max_fps")), false);
}

// 只有 forward 模式才传 tunnel_forward
void ServerArgsTest::TunnelForwardOnlyInForwardMode()
{
    ServerParams params;
    params.tunnel_forward_ = true;
    QVERIFY(ServerArgs::Build(params).contains(QStringLiteral("tunnel_forward=true")));

    params.tunnel_forward_ = false;
    QCOMPARE(HasArg(ServerArgs::Build(params), QStringLiteral("tunnel_forward")), false);
}

// 采集方向的测试数据
void ServerArgsTest::CaptureOrientationVariants_data()
{
    QTest::addColumn<int>("lock");
    QTest::addColumn<int>("orientation");
    QTest::addColumn<QString>("expected");

    // 带 @ 表示锁定；@ 后面没数字表示锁原始方向
    QTest::newRow("不锁，原始方向") << 0 << 0 << QStringLiteral("capture_orientation=0");
    QTest::newRow("不锁，指定 90") << 0 << 90 << QStringLiteral("capture_orientation=90");
    QTest::newRow("锁 180") << 1 << 180 << QStringLiteral("capture_orientation=@180");
    QTest::newRow("锁原始方向") << 2 << 0 << QStringLiteral("capture_orientation=@");
}

// 锁定和解锁两种方向参数形式
void ServerArgsTest::CaptureOrientationVariants()
{
    QFETCH(int, lock);
    QFETCH(int, orientation);
    QFETCH(QString, expected);

    ServerParams params;
    params.capture_orientation_lock_ = lock;
    params.capture_orientation_ = orientation;

    QVERIFY(ServerArgs::Build(params).contains(expected));
}

// 可选参数只有设了才出现
void ServerArgsTest::OptionalParamsAppearOnlyWhenSet()
{
    ServerParams params;
    params.crop_ = QStringLiteral("100:200:0:0");
    params.control_ = false;
    params.stay_awake_ = true;
    params.codec_options_ = QStringLiteral("profile=1,level=2");
    params.encoder_name_ = QStringLiteral("OMX.qcom.video.encoder.avc");

    const QStringList args = ServerArgs::Build(params);
    QVERIFY(args.contains(QStringLiteral("crop=100:200:0:0")));
    QVERIFY(args.contains(QStringLiteral("control=false")));
    QVERIFY(args.contains(QStringLiteral("stay_awake=true")));
    QVERIFY(args.contains(QStringLiteral("codec_options=profile=1,level=2")));
    QVERIFY(args.contains(QStringLiteral("encoder_name=OMX.qcom.video.encoder.avc")));

    // 带逗号的 codec_options 必须还是同一个参数，不能被拆成两段
    QCOMPARE(args.count(QStringLiteral("codec_options=profile=1,level=2")), 1);
}

// scid 传了就按 8 位十六进制
void ServerArgsTest::ScidIsFixedWidthHexWhenSet()
{
    ServerParams params;
    params.scid_ = 999u;
    QVERIFY(ServerArgs::Build(params).contains(QStringLiteral("scid=000003e7")));

    params.scid_ = 0u;
    QVERIFY(ServerArgs::Build(params).contains(QStringLiteral("scid=00000000")));

    // 0xFFFFFFFF 表示"让服务端自己挑"，这种情况不该传
    params.scid_ = 0xFFFFFFFFu;
    QCOMPARE(HasArg(ServerArgs::Build(params), QStringLiteral("scid=")), false);
}

QTEST_GUILESS_MAIN(ServerArgsTest)

#include "tst_server_args.moc"
