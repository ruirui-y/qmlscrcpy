#include <QtTest>

#include "update/version_compare.h"

// 版本比较：只认 x.y.z，非法输入一律当 0.0.0（保守，不会误报新版）
class VersionTest : public QObject
{
    Q_OBJECT

private slots:
    void ParseThreeParts();
    void ParseIgnoresSuffix();
    void ParseRejectsGarbage();
    void CompareBasic();
    void CompareNumericNotLexical();
    void CompareInvalidIsOldest();
};

void VersionTest::ParseThreeParts()
{
    const qsc::update::SemVer version = qsc::update::Parse(QStringLiteral("0.1.4"));

    QCOMPARE(version.major_, 0);
    QCOMPARE(version.minor_, 1);
    QCOMPARE(version.patch_, 4);
}

void VersionTest::ParseIgnoresSuffix()
{
    const qsc::update::SemVer version = qsc::update::Parse(QStringLiteral("1.2.3-beta.1+build7"));

    QCOMPARE(version.major_, 1);
    QCOMPARE(version.minor_, 2);
    QCOMPARE(version.patch_, 3);
}

void VersionTest::ParseRejectsGarbage()
{
    // 空、字母、段数超了：都当 0.0.0
    QCOMPARE(qsc::update::Parse(QString()).major_, 0);
    QCOMPARE(qsc::update::Parse(QStringLiteral("abc")).major_, 0);
    QCOMPARE(qsc::update::Parse(QStringLiteral("1.2.3.4")).major_, 0);
}

void VersionTest::CompareBasic()
{
    QCOMPARE(qsc::update::Compare(QStringLiteral("0.1.4"), QStringLiteral("0.1.4")), 0);
    QCOMPARE(qsc::update::Compare(QStringLiteral("0.1.4"), QStringLiteral("0.1.5")), -1);
    QCOMPARE(qsc::update::Compare(QStringLiteral("0.2.0"), QStringLiteral("0.1.9")), 1);
}

void VersionTest::CompareNumericNotLexical()
{
    // 字符串比会得出 "0.10" < "0.9"，必须按数字比
    QCOMPARE(qsc::update::Compare(QStringLiteral("0.10.0"), QStringLiteral("0.9.0")), 1);

    // 少写一段按 0 处理：1.2 == 1.2.0
    QCOMPARE(qsc::update::Compare(QStringLiteral("1.2"), QStringLiteral("1.2.0")), 0);
}

void VersionTest::CompareInvalidIsOldest()
{
    // 远端给错格式时，本地版本应该被判为"更新"，避免把用户拦在旧版本上
    QCOMPARE(qsc::update::Compare(QStringLiteral("0.1.4"), QStringLiteral("oops")), 1);
}

QTEST_GUILESS_MAIN(VersionTest)

#include "tst_version.moc"
