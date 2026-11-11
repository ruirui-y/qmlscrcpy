#include <QtTest>

#include <QByteArray>
#include <QString>

#include "protocol/control_message.h"
#include "protocol/device_message.h"
#include "protocol/video_packet.h"
#include "protocol/wire.h"

using namespace qsc::protocol;

// 协议层的契约测试：断言的是"线上字节长什么样"，不是实现细节。
// 每条消息的布局都对着 scrcpy 3.x 的 ControlMessage 抄，改错一个字节这里就该红。
class ProtocolTest : public QObject
{
    Q_OBJECT

private slots:
    void VideoHeaderStoresFlagsInTopBits();
    void VideoHeaderFlagCombinations_data();
    void VideoHeaderFlagCombinations();
    void VideoHeaderNeedsTwelveBytes();
    void VideoHeaderRoundTrip();
    void KeycodeLayout();
    void InjectTextTruncatesOnCharBoundary();
    void TouchLayout();
    void TouchPressureIsFixedPoint_data();
    void TouchPressureIsFixedPoint();
    void ScrollNormalizesAndClamps();
    void SimpleMessagesCarryNoPayload();
    void ClipboardLayout();
    void DeviceMessageClipboard();
    void DeviceMessagePartialReturnsZero();
    void DeviceMessageBadTypeIsError();
};

void ProtocolTest::VideoHeaderStoresFlagsInTopBits()
{
    VideoPacketHeader header;
    header.pts_flags_ = VideoPacketHeader::kConfigFlag | 0x0012D687ULL;  // 1234567 微秒
    header.size_ = 0x00001000;

    const QByteArray bytes = header.Serialize();
    QCOMPARE(bytes.size(), VideoPacketHeader::kSize);

    // 大端：最高位在第一个字节，配置包的标志一置上就该是 0x80
    QCOMPARE(static_cast<quint8>(bytes.at(0)), quint8(0x80));
    QCOMPARE(static_cast<quint8>(bytes.at(8)), quint8(0x00));   // 长度高字节
    QCOMPARE(static_cast<quint8>(bytes.at(9)), quint8(0x00));
    QCOMPARE(static_cast<quint8>(bytes.at(10)), quint8(0x10));
    QCOMPARE(static_cast<quint8>(bytes.at(11)), quint8(0x00));  // 长度低字节
}

void ProtocolTest::VideoHeaderFlagCombinations_data()
{
    QTest::addColumn<quint64>("pts_flags");
    QTest::addColumn<bool>("is_config");
    QTest::addColumn<bool>("is_key_frame");
    QTest::addColumn<qint64>("pts_us");

    QTest::newRow("普通帧") << quint64(0x11) << false << false << qint64(0x11);
    QTest::newRow("配置帧") << (VideoPacketHeader::kConfigFlag | 0x22ULL) << true << false << qint64(0x22);
    QTest::newRow("关键帧") << (VideoPacketHeader::kKeyFrameFlag | 0x33ULL) << false << true << qint64(0x33);
    QTest::newRow("关键帧也是配置帧") << (VideoPacketHeader::kConfigFlag | VideoPacketHeader::kKeyFrameFlag | 0x44ULL)
                              << true << true << qint64(0x44);
}

void ProtocolTest::VideoHeaderFlagCombinations()
{
    QFETCH(quint64, pts_flags);
    QFETCH(bool, is_config);
    QFETCH(bool, is_key_frame);
    QFETCH(qint64, pts_us);

    VideoPacketHeader header;
    header.pts_flags_ = pts_flags;

    QCOMPARE(header.IsConfig(), is_config);
    QCOMPARE(header.IsKeyFrame(), is_key_frame);
    QCOMPARE(header.PtsUs(), pts_us);
}

void ProtocolTest::VideoHeaderNeedsTwelveBytes()
{
    VideoPacketHeader header;
    QByteArray bytes;
    for (int i = 0; i < VideoPacketHeader::kSize - 1; ++i) {
        bytes.append(static_cast<char>(i));
    }

    // 差一个字节也必须拒绝：半个头拼出来的长度会把后面整条流读错位
    QCOMPARE(VideoPacketHeader::Parse(bytes, header), false);
}

void ProtocolTest::VideoHeaderRoundTrip()
{
    VideoPacketHeader original;
    original.pts_flags_ = VideoPacketHeader::kKeyFrameFlag | 987654321ULL;
    original.size_ = 123456;

    VideoPacketHeader parsed;
    QCOMPARE(VideoPacketHeader::Parse(original.Serialize(), parsed), true);
    QCOMPARE(parsed.pts_flags_, original.pts_flags_);
    QCOMPARE(parsed.size_, original.size_);
    QCOMPARE(parsed.IsKeyFrame(), true);
    QCOMPARE(parsed.PtsUs(), qint64(987654321));
}

void ProtocolTest::KeycodeLayout()
{
    KeycodeEvent event;
    event.action_ = 0;         // 按下
    event.keycode_ = 4;        // AKEYCODE_BACK
    event.repeat_ = 2;         // 长按第三次
    event.meta_state_ = 0x1000;

    const QByteArray bytes = ControlMessage::InjectKeycode(event);

    // 1 字节类型 + 1 动作 + 4 键码 + 4 重复次数 + 4 修饰键 = 14
    QCOMPARE(bytes.size(), 14);

    WireReader reader(bytes);
    quint8 type = 0;
    quint8 action = 0;
    quint32 keycode = 0;
    quint32 repeat = 0;
    quint32 meta_state = 0;
    QVERIFY(reader.ReadU8(type));
    QVERIFY(reader.ReadU8(action));
    QVERIFY(reader.ReadU32(keycode));
    QVERIFY(reader.ReadU32(repeat));
    QVERIFY(reader.ReadU32(meta_state));

    QCOMPARE(type, static_cast<quint8>(ControlType::InjectKeycode));
    QCOMPARE(action, quint8(0));
    QCOMPARE(keycode, quint32(4));
    QCOMPARE(repeat, quint32(2));
    QCOMPARE(meta_state, quint32(0x1000));
}

void ProtocolTest::InjectTextTruncatesOnCharBoundary()
{
    // 短的照原样写：类型 1 + 长度 4 + 正文 3
    const QByteArray short_text = ControlMessage::InjectText(QStringLiteral("abc"));
    QCOMPARE(short_text.size(), 8);
    QCOMPARE(static_cast<quint8>(short_text.at(0)), static_cast<quint8>(ControlType::InjectText));

    WireReader short_reader(short_text);
    quint8 type = 0;
    quint32 length = 0;
    QByteArray body;
    QVERIFY(short_reader.ReadU8(type));
    QVERIFY(short_reader.ReadU32(length));
    QVERIFY(short_reader.ReadBytes(static_cast<int>(length), body));
    QCOMPARE(length, quint32(3));
    QCOMPARE(body, QByteArray("abc"));

    // 400 个 ASCII 字符：截到 300 字节
    const QByteArray long_ascii = ControlMessage::InjectText(QString(400, QLatin1Char('a')));
    QCOMPARE(long_ascii.size(), 1 + 4 + kMaxInjectTextBytes);

    // "a" + 100 个汉字 = 1 + 300 = 301 字节。截到 300 会劈开最后一个汉字，
    // 必须退到 298，否则设备端收到半个 UTF-8 字符就是乱码
    QString mixed = QStringLiteral("a");
    for (int i = 0; i < 100; ++i) {
        mixed.append(QChar(0x4E2D));  // 中
    }
    const QByteArray mixed_bytes = ControlMessage::InjectText(mixed);

    WireReader mixed_reader(mixed_bytes);
    quint32 mixed_length = 0;
    QVERIFY(mixed_reader.ReadU8(type));
    QVERIFY(mixed_reader.ReadU32(mixed_length));
    QByteArray mixed_body;
    QVERIFY(mixed_reader.ReadBytes(static_cast<int>(mixed_length), mixed_body));

    QCOMPARE(mixed_length, quint32(298));
    const QString decoded = QString::fromUtf8(mixed_body);
    QCOMPARE(decoded.size(), 100);  // 1 个 a + 99 个汉字
    QCOMPARE(decoded.at(0), QChar('a'));
    QCOMPARE(decoded.at(1), QChar(0x4E2D));
}

void ProtocolTest::TouchLayout()
{
    TouchEvent event;
    event.action_ = 0;                    // 按下
    event.pointer_id_ = 0xFFFFFFFFFFFFFFFFULL;
    event.position_ = QPoint(100, 200);
    event.screen_size_ = QSize(1080, 1920);
    event.pressure_ = 1.0f;
    event.action_buttons_ = 1;
    event.buttons_ = 1;

    const QByteArray bytes = ControlMessage::InjectTouch(event);

    // 1 类型 + 1 动作 + 8 指针 + 12 位置 + 2 压力 + 4 动作键 + 4 按键 = 32
    QCOMPARE(bytes.size(), 32);

    WireReader reader(bytes);
    quint8 type = 0;
    quint8 action = 0;
    quint64 pointer_id = 0;
    quint32 x = 0;
    quint32 y = 0;
    quint16 width = 0;
    quint16 height = 0;
    quint16 pressure = 0;
    quint32 action_buttons = 0;
    quint32 buttons = 0;
    QVERIFY(reader.ReadU8(type));
    QVERIFY(reader.ReadU8(action));
    QVERIFY(reader.ReadU64(pointer_id));
    QVERIFY(reader.ReadU32(x));
    QVERIFY(reader.ReadU32(y));
    QVERIFY(reader.ReadU16(width));
    QVERIFY(reader.ReadU16(height));
    QVERIFY(reader.ReadU16(pressure));
    QVERIFY(reader.ReadU32(action_buttons));
    QVERIFY(reader.ReadU32(buttons));

    QCOMPARE(type, static_cast<quint8>(ControlType::InjectTouch));
    QCOMPARE(action, quint8(0));
    QCOMPARE(pointer_id, quint64(0xFFFFFFFFFFFFFFFFULL));
    QCOMPARE(x, quint32(100));
    QCOMPARE(y, quint32(200));
    QCOMPARE(width, quint16(1080));
    QCOMPARE(height, quint16(1920));
    QCOMPARE(pressure, quint16(0xFFFF));
    QCOMPARE(action_buttons, quint32(1));
    QCOMPARE(buttons, quint32(1));
}

void ProtocolTest::TouchPressureIsFixedPoint_data()
{
    QTest::addColumn<float>("pressure");
    QTest::addColumn<quint16>("expected");

    QTest::newRow("抬起") << 0.0f << quint16(0x0000);
    QTest::newRow("一半") << 0.5f << quint16(0x8000);
    QTest::newRow("按到底") << 1.0f << quint16(0xFFFF);
    QTest::newRow("负数截到 0") << -1.0f << quint16(0x0000);
    QTest::newRow("超 1 截到上限") << 2.0f << quint16(0xFFFF);
}

void ProtocolTest::TouchPressureIsFixedPoint()
{
    QFETCH(float, pressure);
    QFETCH(quint16, expected);

    TouchEvent event;
    event.pressure_ = pressure;

    const QByteArray bytes = ControlMessage::InjectTouch(event);

    // 压力固定在偏移 22：1 + 1 + 8 + 12
    const quint16 actual = static_cast<quint16>((static_cast<quint8>(bytes.at(22)) << 8)
                                                | static_cast<quint8>(bytes.at(23)));
    QCOMPARE(actual, expected);
}

void ProtocolTest::ScrollNormalizesAndClamps()
{
    ScrollEvent event;
    event.screen_size_ = QSize(1080, 1920);

    // 32 格 → 归一化 2.0 → 截到 1.0 → 0x7FFF
    event.h_scroll_ = 32.0f;
    event.v_scroll_ = -32.0f;
    QByteArray bytes = ControlMessage::InjectScroll(event);
    QCOMPARE(bytes.size(), 21);
    QCOMPARE(static_cast<quint16>((static_cast<quint8>(bytes.at(13)) << 8) | static_cast<quint8>(bytes.at(14))),
             quint16(0x7FFF));
    QCOMPARE(static_cast<quint16>((static_cast<quint8>(bytes.at(15)) << 8) | static_cast<quint8>(bytes.at(16))),
             quint16(0x8000));  // -32768 按无符号看

    // 8 格 → 归一化 0.5 → 0x4000
    event.h_scroll_ = 8.0f;
    event.v_scroll_ = 0.0f;
    bytes = ControlMessage::InjectScroll(event);
    QCOMPARE(static_cast<quint16>((static_cast<quint8>(bytes.at(13)) << 8) | static_cast<quint8>(bytes.at(14))),
             quint16(0x4000));
    QCOMPARE(static_cast<quint16>((static_cast<quint8>(bytes.at(15)) << 8) | static_cast<quint8>(bytes.at(16))),
             quint16(0x0000));
}

void ProtocolTest::SimpleMessagesCarryNoPayload()
{
    QCOMPARE(ControlMessage::CollapsePanels(), QByteArray(1, static_cast<char>(7)));
    QCOMPARE(ControlMessage::RotateDevice(), QByteArray(1, static_cast<char>(11)));
    QCOMPARE(ControlMessage::ExpandNotificationPanel(), QByteArray(1, static_cast<char>(5)));
    QCOMPARE(ControlMessage::ExpandSettingsPanel(), QByteArray(1, static_cast<char>(6)));

    const QByteArray power_on = ControlMessage::SetDisplayPower(true);
    QCOMPARE(power_on.size(), 2);
    QCOMPARE(static_cast<quint8>(power_on.at(0)), quint8(10));
    QCOMPARE(static_cast<quint8>(power_on.at(1)), quint8(1));

    const QByteArray back_down = ControlMessage::BackOrScreenOn(true);
    QCOMPARE(back_down.size(), 2);
    QCOMPARE(static_cast<quint8>(back_down.at(0)), quint8(4));
    QCOMPARE(static_cast<quint8>(back_down.at(1)), quint8(0));  // 0 = ACTION_DOWN

    const QByteArray get_clipboard = ControlMessage::GetClipboard(ClipboardAction::Cut);
    QCOMPARE(get_clipboard.size(), 2);
    QCOMPARE(static_cast<quint8>(get_clipboard.at(0)), quint8(8));
    QCOMPARE(static_cast<quint8>(get_clipboard.at(1)), quint8(2));
}

void ProtocolTest::ClipboardLayout()
{
    const QByteArray bytes = ControlMessage::SetClipboard(0x0102030405060708ULL, true, QStringLiteral("hi"));

    // 1 类型 + 8 序号 + 1 粘贴标志 + 4 长度 + 2 正文
    QCOMPARE(bytes.size(), 16);

    WireReader reader(bytes);
    quint8 type = 0;
    quint64 sequence = 0;
    quint8 paste = 0;
    quint32 length = 0;
    QByteArray body;
    QVERIFY(reader.ReadU8(type));
    QVERIFY(reader.ReadU64(sequence));
    QVERIFY(reader.ReadU8(paste));
    QVERIFY(reader.ReadU32(length));
    QVERIFY(reader.ReadBytes(static_cast<int>(length), body));

    QCOMPARE(type, static_cast<quint8>(ControlType::SetClipboard));
    QCOMPARE(sequence, quint64(0x0102030405060708ULL));
    QCOMPARE(paste, quint8(1));
    QCOMPARE(length, quint32(2));
    QCOMPARE(body, QByteArray("hi"));

    // 空剪贴板也要有完整的长度字段（0），不能省掉，否则设备端会读串
    const QByteArray empty = ControlMessage::SetClipboard(0, false, QString());
    QCOMPARE(empty.size(), 14);
}

void ProtocolTest::DeviceMessageClipboard()
{
    const QByteArray utf8 = QStringLiteral("剪贴板").toUtf8();
    QByteArray buffer;
    AppendU8(buffer, static_cast<quint8>(DeviceMessage::Type::Clipboard));
    AppendU32(buffer, static_cast<quint32>(utf8.size()));
    buffer.append(utf8);
    // 后面再跟一条，模拟同一个 TCP 包里连着两条消息
    AppendU8(buffer, static_cast<quint8>(DeviceMessage::Type::AckClipboard));
    AppendU64(buffer, 0x1122334455667788ULL);

    DeviceMessage message;
    const qint32 consumed = DeviceMessage::TryParse(buffer, message);
    QCOMPARE(consumed, static_cast<qint32>(1 + 4 + utf8.size()));
    QCOMPARE(message.type_, DeviceMessage::Type::Clipboard);
    QCOMPARE(message.text_, QStringLiteral("剪贴板"));

    // 把第一条消费掉，剩下那条要能接着解
    const qint32 consumed_again = DeviceMessage::TryParse(buffer.mid(consumed), message);
    QCOMPARE(consumed_again, qint32(9));
    QCOMPARE(message.type_, DeviceMessage::Type::AckClipboard);
    QCOMPARE(message.sequence_, quint64(0x1122334455667788ULL));
}

void ProtocolTest::DeviceMessagePartialReturnsZero()
{
    DeviceMessage message;

    QCOMPARE(DeviceMessage::TryParse(QByteArray(), message), qint32(0));

    // 只有类型字节
    QCOMPARE(DeviceMessage::TryParse(QByteArray(1, static_cast<char>(1)), message), qint32(0));

    // 声明长度 10，正文只来了 3 字节
    QByteArray partial;
    AppendU8(partial, 1);
    AppendU32(partial, 10);
    partial.append("abc", 3);
    QCOMPARE(DeviceMessage::TryParse(partial, message), qint32(0));
}

void ProtocolTest::DeviceMessageBadTypeIsError()
{
    DeviceMessage message;

    // UhidOutput：明确不支持，必须报错让上层断开，而不是当"数据不够"一直等
    QCOMPARE(DeviceMessage::TryParse(QByteArray(1, static_cast<char>(3)), message), qint32(-1));

    // 完全不认识的类型
    QCOMPARE(DeviceMessage::TryParse(QByteArray(1, static_cast<char>(99)), message), qint32(-1));

    // 长度大到离谱，说明字节流已经错位
    QByteArray oversized;
    AppendU8(oversized, 1);
    AppendU32(oversized, static_cast<quint32>(kMaxMessageBytes) + 1u);
    QCOMPARE(DeviceMessage::TryParse(oversized, message), qint32(-1));
}

QTEST_GUILESS_MAIN(ProtocolTest)

#include "tst_protocol.moc"
