#include <QByteArray>
#include <QPointF>
#include <QSize>
#include <QtTest>

#include "input/coordinate_mapper.h"
#include "input/touch_controller.h"

namespace
{

// 协议是大端，测试里按固定偏移把字段读回来
quint8 U8(const QByteArray &data, int offset)
{
    return static_cast<quint8>(data.at(offset));
}

// 从大端字节里读 2 字节
quint16 U16(const QByteArray &data, int offset)
{
    return static_cast<quint16>((static_cast<quint8>(data.at(offset)) << 8)
                                | static_cast<quint8>(data.at(offset + 1)));
}

// 从大端字节里读 4 字节
quint32 U32(const QByteArray &data, int offset)
{
    quint32 value = 0;
    for (int i = 0; i < 4; ++i)
    {
        value = (value << 8) | static_cast<quint8>(data.at(offset + i));
    }
    return value;
}

// 从大端字节里读 8 字节
quint64 U64(const QByteArray &data, int offset)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
    {
        value = (value << 8) | static_cast<quint8>(data.at(offset + i));
    }
    return value;
}

// 按有符号读 2 字节定点
qint16 I16(const QByteArray &data, int offset)
{
    return static_cast<qint16>(U16(data, offset));
}

} // namespace

// 坐标映射和触摸消息拼装，都是纯逻辑，不碰设备
class InputTest : public QObject
{
    Q_OBJECT

private slots:
    void FillModeMapsByScale();
    void LetterboxMarksOutside();
    void PressBuildsTouchMessage();
    void ReleaseClearsButtons();
    void ScrollOneNotchIsFullDeflection();
};

// 拉满模式：宽高各自缩放，不留黑边
void InputTest::FillModeMapsByScale()
{
    qsc::input::CoordinateMapper mapper;
    mapper.SetFrameSize(QSize(100, 200));
    mapper.SetItemSize(QSizeF(200, 200));
    mapper.SetKeepAspect(false);

    bool inside = false;
    const QPoint pos = mapper.Map(QPointF(100, 100), &inside);

    QVERIFY(inside);
    // 横向放大 2 倍、纵向 1 倍：x 要折半，y 不动
    QCOMPARE(pos, QPoint(50, 100));
}

// 等比模式：帧 1:2 放进 1:1 的画布，左右各有黑边，点黑边不算命中
void InputTest::LetterboxMarksOutside()
{
    qsc::input::CoordinateMapper mapper;
    mapper.SetFrameSize(QSize(100, 200));
    mapper.SetItemSize(QSizeF(200, 200));
    mapper.SetKeepAspect(true);

    // 缩放到 1（一个帧像素 = 一个画布像素）：画面 100x200 居中，左右各 50px 黑边
    bool inside = false;
    const QPoint center = mapper.Map(QPointF(100, 100), &inside);
    QVERIFY(inside);
    QCOMPARE(center, QPoint(50, 100));

    const QPoint near_right = mapper.Map(QPointF(120, 100), &inside);
    QVERIFY(inside);
    QCOMPARE(near_right, QPoint(70, 100));

    // 左侧 50px 是黑边
    mapper.Map(QPointF(10, 100), &inside);
    QVERIFY(!inside);

    // 右侧超出画面
    mapper.Map(QPointF(150, 100), &inside);
    QVERIFY(!inside);
}

// 按下：类型 2 + action 0 + 固定指针 id + 帧坐标 + 帧尺寸 + 满压力 + 主键
void InputTest::PressBuildsTouchMessage()
{
    qsc::input::CoordinateMapper mapper;
    mapper.SetFrameSize(QSize(100, 200));
    mapper.SetItemSize(QSizeF(200, 200));
    mapper.SetKeepAspect(false);

    qsc::input::TouchController controller(&mapper);
    const QByteArray message = controller.Press(QPointF(100, 100));

    QCOMPARE(message.size(), 32);
    QCOMPARE(U8(message, 0), quint8(2));
    QCOMPARE(U8(message, 1), quint8(0));
    QCOMPARE(U64(message, 2), quint64(0xFFFFFFFFFFFFFFFFULL));
    QCOMPARE(U32(message, 10), quint32(50));
    QCOMPARE(U32(message, 14), quint32(100));
    QCOMPARE(U16(message, 18), quint16(100));
    QCOMPARE(U16(message, 20), quint16(200));
    QCOMPARE(U16(message, 22), quint16(0xFFFF));
    QCOMPARE(U32(message, 24), quint32(1));
    QCOMPARE(U32(message, 28), quint32(1));
}

// 抬起：action 1，压力归零，buttons 清空
void InputTest::ReleaseClearsButtons()
{
    qsc::input::CoordinateMapper mapper;
    mapper.SetFrameSize(QSize(100, 200));
    mapper.SetItemSize(QSizeF(200, 200));
    mapper.SetKeepAspect(false);

    qsc::input::TouchController controller(&mapper);
    controller.Press(QPointF(100, 100));
    const QByteArray message = controller.Release(QPointF(100, 100));

    QCOMPARE(U8(message, 1), quint8(1));
    QCOMPARE(U16(message, 22), quint16(0));
    QCOMPARE(U32(message, 28), quint32(0));
}

// 一格滚轮：垂直方向打满 -1.0（定点 0x8000），水平方向为 0
void InputTest::ScrollOneNotchIsFullDeflection()
{
    qsc::input::CoordinateMapper mapper;
    mapper.SetFrameSize(QSize(100, 200));
    mapper.SetItemSize(QSizeF(200, 200));
    mapper.SetKeepAspect(false);

    qsc::input::TouchController controller(&mapper);
    const QByteArray message = controller.Scroll(QPointF(100, 100), 1);

    QCOMPARE(message.size(), 21);
    QCOMPARE(U8(message, 0), quint8(3));
    QCOMPARE(U32(message, 1), quint32(50));
    QCOMPARE(U32(message, 5), quint32(100));
    QCOMPARE(U16(message, 9), quint16(100));
    QCOMPARE(U16(message, 11), quint16(200));
    QCOMPARE(I16(message, 13), qint16(0));
    QCOMPARE(static_cast<quint16>(I16(message, 15)), quint16(0x8000));
}

QTEST_GUILESS_MAIN(InputTest)

#include "tst_input.moc"
