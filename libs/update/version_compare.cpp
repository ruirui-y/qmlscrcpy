#include "update/version_compare.h"

#include <QRegularExpression>

namespace qsc::update
{

// 解析 "1.2.3"：只取前三段数字，后面的 -beta / +build 直接丢掉
SemVer Parse(const QString &text)
{
    SemVer version;

    // 先截掉 - 和 + 后面的修饰部分
    QString core = text.trimmed();
    const int cut = core.indexOf(QRegularExpression(QStringLiteral("[-+]")));
    if (cut > 0)
    {
        core = core.left(cut);
    }

    const QStringList parts = core.split(QLatin1Char('.'));
    if (parts.isEmpty() || parts.size() > 3)
    {
        return version;                                                                          // 段数不对，当 0.0.0
    }

    bool ok = true;
    version.major_ = parts.at(0).toInt(&ok);
    if (!ok)
    {
        return SemVer();
    }
    if (parts.size() > 1)
    {
        version.minor_ = parts.at(1).toInt(&ok);
        if (!ok)
        {
            return SemVer();
        }
    }
    if (parts.size() > 2)
    {
        version.patch_ = parts.at(2).toInt(&ok);
        if (!ok)
        {
            return SemVer();
        }
    }

    return version;
}

// 按 major → minor → patch 依次比，第一个不相等的就是结果
int Compare(const QString &a, const QString &b)
{
    const SemVer left = Parse(a);
    const SemVer right = Parse(b);

    if (left.major_ != right.major_)
    {
        return left.major_ > right.major_ ? 1 : -1;
    }
    if (left.minor_ != right.minor_)
    {
        return left.minor_ > right.minor_ ? 1 : -1;
    }
    if (left.patch_ != right.patch_)
    {
        return left.patch_ > right.patch_ ? 1 : -1;
    }
    return 0;
}

} // namespace qsc::update
