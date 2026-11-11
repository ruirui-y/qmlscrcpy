#include <QtTest>

#include <QByteArray>
#include <QElapsedTimer>
#include <QTcpSocket>

#include "protocol/video_packet.h"
#include "session/server_session.h"
#include "stream/h264_decoder.h"
#include "stream/yuv_convert.h"

using namespace qsc::protocol;
using namespace qsc::session;
using namespace qsc::stream;

// 真机用例：连上设备，从视频 socket 读真实包喂给 H264Decoder，确认能解出 YUV420P 的帧。
// 这是 P3 的命门——解码器能开不代表能解，得拿真流喂。
//
// 没设 QS_ADB_PATH / QS_DEVICE_SERIAL 时自动跳过。
class StreamTest : public QObject
{
    Q_OBJECT

private slots:
    void DecodeLiveStream();
};

void StreamTest::DecodeLiveStream()
{
    if (!qEnvironmentVariableIsSet("QS_ADB_PATH")
        || !qEnvironmentVariableIsSet("QS_DEVICE_SERIAL")) {
        QSKIP("未设置 QS_ADB_PATH / QS_DEVICE_SERIAL，跳过真机用例");
    }

    QString server_path = QString::fromLocal8Bit(qgetenv("QS_SCRCPY_SERVER"));
    if (server_path.isEmpty()) {
        server_path = QStringLiteral(QS_DEFAULT_SERVER_PATH);
    }

    ServerParams params;
    params.serial_ = QString::fromLocal8Bit(qgetenv("QS_DEVICE_SERIAL"));
    params.server_local_path_ = server_path;
    params.bit_rate_ = 2000000;
    params.max_size_ = 720;
    params.log_level_ = QStringLiteral("error");    // 只看错误，别刷屏

    ServerSession session(params);
    const ConnectResult result = session.Start();
    const QByteArray start_error = result.error_.toUtf8();
    QVERIFY2(result.ok_, start_error.constData());

    QTcpSocket *video = session.TakeVideoSocket();
    QVERIFY(video != nullptr);

    H264Decoder decoder;
    QString error;
    const bool opened = decoder.Open(&error);
    const QByteArray open_error = error.toUtf8();
    QVERIFY2(opened, open_error.constData());

    QByteArray buffer;
    QVector<Frame> frames;
    int packets = 0;
    QElapsedTimer timer;
    timer.start();

    // 读够 60 个包或者 8 秒就停，够看清解不解得开
    while (packets < 60 && timer.elapsed() < 8000) {
        while (buffer.size() < VideoPacketHeader::kSize) {
            if (!video->waitForReadyRead(500)) {
                break;
            }
            buffer.append(video->readAll());
        }
        if (buffer.size() < VideoPacketHeader::kSize) {
            break;
        }

        VideoPacketHeader header;
        QVERIFY(VideoPacketHeader::Parse(buffer, header));

        const int need = VideoPacketHeader::kSize + static_cast<int>(header.size_);
        while (buffer.size() < need) {
            if (!video->waitForReadyRead(500)) {
                break;
            }
            buffer.append(video->readAll());
        }
        if (buffer.size() < need) {
            break;
        }

        const QByteArray payload = buffer.mid(VideoPacketHeader::kSize, static_cast<int>(header.size_));
        buffer.remove(0, need);
        ++packets;

        // 前 3 个包把头部字节打出来：判断是 Annex-B（00 00 00 01）还是长度前缀
        if (packets <= 3) {
            qInfo() << "packet" << packets << "config" << header.IsConfig() << "size"
                    << payload.size() << "head" << payload.left(16).toHex(' ');
            if (header.IsConfig()) {
                qInfo() << "config full hex" << payload.toHex(' ');
            }
        }

        if (!decoder.Push(payload, header.IsConfig(), header.PtsUs(), frames, &error)) {
            QVERIFY2(false, error.toUtf8().constData());
        }
    }

    qInfo() << "读了" << packets << "个包，解出" << frames.size() << "帧";
    QVERIFY2(!frames.isEmpty(), "60 个包 / 8 秒内没解出任何一帧");

    const Frame &first = frames.first();
    QVERIFY(first.IsValid());
    QVERIFY(first.Size().width() > 0);
    QVERIFY(first.Size().height() > 0);
    QVERIFY(first.Y() != nullptr);
    qInfo() << "第一帧" << first.Size() << "行跨度" << first.StrideY() << first.StrideU()
            << first.StrideV();

    // 行的实际跨度不能小于宽度，否则上屏会斜切
    QVERIFY(first.StrideY() >= first.Size().width());
    QVERIFY(first.StrideU() >= first.Size().width() / 2);

    // 走一遍 CPU 转换：这一步做的是 sws_scale，堆写坏的话在这里就会炸
    YuvConvert convert;
    const QImage rgb = convert.Convert(first);
    QVERIFY(!rgb.isNull());
    QCOMPARE(rgb.size(), first.Size());
    qInfo() << "转换后" << rgb.size() << "格式" << rgb.format();

    video->close();
    delete video;
    session.Stop();
}

QTEST_GUILESS_MAIN(StreamTest)

#include "tst_stream.moc"
