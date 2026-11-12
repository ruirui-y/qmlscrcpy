#include <QByteArray>
#include <QtTest>

#include "input/key_controller.h"
#include "input/keycode_table.h"

namespace
{

// 读 1 字节
quint8 U8(const QByteArray &data, int offset)
{
    return static_cast<quint8>(data.at(offset));
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

} // namespace

// 按键映射和按键消息拼装，纯逻辑
class KeyTest : public QObject
{
    Q_OBJECT

private slots:
    void LettersAndDigits();
    void EditingKeys();
    void ModifierKeys();
    void UnknownKeyIsZero();
    void MetaStateBits();
    void AutoRepeatCounts();
    void PressMessageLayout();
};

// 字母和数字在安卓键码里是连续区间
void KeyTest::LettersAndDigits()
{
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_A), quint32(29));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Z), quint32(54));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_0), quint32(7));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_9), quint32(16));
}

// 退格、删除、回车、Tab 这些编辑键
void KeyTest::EditingKeys()
{
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Backspace), quint32(67));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Delete), quint32(112));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Return), quint32(66));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Enter), quint32(66));                           // 小键盘回车同一个键
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Tab), quint32(61));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Escape), quint32(111));
}

// 修饰键统一映射到左键码，设备不区分左右时也能用
void KeyTest::ModifierKeys()
{
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Shift), quint32(59));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Control), quint32(113));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_Alt), quint32(58));
}

// 没映射的键返回 0，调用方据此跳过，不会发出垃圾键码
void KeyTest::UnknownKeyIsZero()
{
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_F35), quint32(0));
    QCOMPARE(qsc::input::AndroidKeycode(Qt::Key_unknown), quint32(0));
}

// 修饰键是位掩码：通用位和左专用位都要置
void KeyTest::MetaStateBits()
{
    QCOMPARE(qsc::input::AndroidMetaState(Qt::NoModifier), quint32(0));
    QCOMPARE(qsc::input::AndroidMetaState(Qt::ShiftModifier), quint32(0x41));
    QCOMPARE(qsc::input::AndroidMetaState(Qt::ControlModifier), quint32(0x3000));
    QCOMPARE(qsc::input::AndroidMetaState(Qt::AltModifier), quint32(0x12));
    QCOMPARE(qsc::input::AndroidMetaState(Qt::ShiftModifier | Qt::ControlModifier), quint32(0x3041));
}

// 自动重复由我们自己数：按下 0，重复 1、2，抬起后重新从 0 开始
void KeyTest::AutoRepeatCounts()
{
    qsc::input::KeyController controller;

    // 先存成局部量：QCOMPARE 会把参数从头展开一次，直接塞临时对象会在第二次求值时越界
    const QByteArray first = controller.Press(Qt::Key_A, Qt::NoModifier, false);
    QCOMPARE(first.size(), 14);
    const QByteArray second = controller.Press(Qt::Key_A, Qt::NoModifier, true);
    QCOMPARE(second.size(), 14);
    const QByteArray third = controller.Press(Qt::Key_A, Qt::NoModifier, true);
    QCOMPARE(third.size(), 14);
    QCOMPARE(U32(first, 6), quint32(0));
    QCOMPARE(U32(second, 6), quint32(1));
    QCOMPARE(U32(third, 6), quint32(2));

    const QByteArray released = controller.Release(Qt::Key_A, Qt::NoModifier);
    QCOMPARE(released.size(), 14);
    QCOMPARE(U32(released, 6), quint32(0));

    const QByteArray again = controller.Press(Qt::Key_A, Qt::NoModifier, false);
    QCOMPARE(again.size(), 14);
    QCOMPARE(U32(again, 6), quint32(0));
}

// 一条按键消息 = 类型 + action + keycode + repeat + metaState，共 14 字节
void KeyTest::PressMessageLayout()
{
    qsc::input::KeyController controller;
    const QByteArray message = controller.Press(Qt::Key_S, Qt::ShiftModifier, false);

    QCOMPARE(message.size(), 14);
    QCOMPARE(U8(message, 0), quint8(0));
    QCOMPARE(U8(message, 1), quint8(0));
    QCOMPARE(U32(message, 2), quint32(47));
    QCOMPARE(U32(message, 6), quint32(0));
    QCOMPARE(U32(message, 10), quint32(0x41));
}

QTEST_GUILESS_MAIN(KeyTest)

#include "tst_key.moc"
