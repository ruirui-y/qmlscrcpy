#include <QString>
#include <QtTest>

#include "base/hotkey.h"

// 老板键的配置解析：纯字符串处理，不需要真的注册热键
class HotkeyTest : public QObject
{
    Q_OBJECT

private slots:
    void ParsesCtrlAltLetter();
    void ParsesFunctionKeyWithShift();
    void RejectsModifierOnly();
    void RejectsUnknownKey();
    void FormatsBackToText();
};

// "Ctrl+Alt+B" → MOD_CONTROL | MOD_ALT + 'B'
void HotkeyTest::ParsesCtrlAltLetter()
{
    qsc::base::HotkeySpec spec;
    QVERIFY(qsc::base::ParseHotkey(QStringLiteral("Ctrl+Alt+B"), &spec));
    QCOMPARE(spec.modifiers_, qsc::base::kModControl | qsc::base::kModAlt);
    QCOMPARE(spec.virtual_key_, static_cast<unsigned>('B'));
}

// 大小写不敏感，功能键映射到 VK_F*
void HotkeyTest::ParsesFunctionKeyWithShift()
{
    qsc::base::HotkeySpec spec;
    QVERIFY(qsc::base::ParseHotkey(QStringLiteral("ctrl+shift+f9"), &spec));
    QCOMPARE(spec.modifiers_, qsc::base::kModControl | qsc::base::kModShift);
    QCOMPARE(spec.virtual_key_, static_cast<unsigned>(0x78));                                   // VK_F9 = 0x70 + 8
}

// 只有修饰键没有主键、或者只有主键没有修饰键，都不接受
void HotkeyTest::RejectsModifierOnly()
{
    qsc::base::HotkeySpec spec;
    QVERIFY(!qsc::base::ParseHotkey(QStringLiteral("Ctrl+Alt"), &spec));
    QVERIFY(!qsc::base::ParseHotkey(QStringLiteral("B"), &spec));
    QVERIFY(!qsc::base::ParseHotkey(QString(), &spec));
}

// 认不出的键名要明确失败，不能悄悄注册成别的东西
void HotkeyTest::RejectsUnknownKey()
{
    qsc::base::HotkeySpec spec;
    QVERIFY(!qsc::base::ParseHotkey(QStringLiteral("Ctrl+Alt+不认识"), &spec));
    QVERIFY(!qsc::base::ParseHotkey(QStringLiteral("Foo+B"), &spec));
}

// 解析完能拼回可读文本，界面显示和日志用
void HotkeyTest::FormatsBackToText()
{
    qsc::base::HotkeySpec spec;
    QVERIFY(qsc::base::ParseHotkey(QStringLiteral("Ctrl+Alt+B"), &spec));
    QCOMPARE(qsc::base::HotkeyText(spec), QStringLiteral("Ctrl+Alt+B"));

    qsc::base::HotkeySpec f9;
    QVERIFY(qsc::base::ParseHotkey(QStringLiteral("Ctrl+Shift+F9"), &f9));
    QCOMPARE(qsc::base::HotkeyText(f9), QStringLiteral("Ctrl+Shift+F9"));
}

QTEST_GUILESS_MAIN(HotkeyTest)

#include "tst_hotkey.moc"
