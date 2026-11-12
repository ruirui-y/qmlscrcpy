#include "adb/net_address.h"

#include <QRegularExpression>

namespace qsc::adb
{

namespace
{

// 取出前三段，例如 "192.168.31.114" -> "192.168.31"
QString LanPrefix(const QString &address)
{
    const QStringList parts = address.trimmed().split(QLatin1Char('.'));
    if (parts.size() != 4)
    {
        return QString();
    }
    return parts.at(0) + QLatin1Char('.') + parts.at(1) + QLatin1Char('.') + parts.at(2);
}

} // namespace

// 同 /24 判断：两边都得是合法 IPv4，前三位一样才算
bool SameLan24(const QString &left, const QString &right)
{
    const QString left_prefix = LanPrefix(left);
    const QString right_prefix = LanPrefix(right);
    return !left_prefix.isEmpty() && left_prefix == right_prefix;
}

// ip route 每行形如：192.168.31.0/24 dev wlan0 proto kernel scope link src 192.168.31.114
QStringList RouteAddresses(const QString &route_output)
{
    QStringList addresses;
    const QStringList lines = route_output.split(QLatin1Char('\n'));
    for (const QString &line : lines)
    {
        const QStringList words = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        for (int index = 0; index + 1 < words.size(); ++index)
        {
            if (words.at(index) == QStringLiteral("src") && !addresses.contains(words.at(index + 1)))
            {
                addresses.append(words.at(index + 1));
            }
        }
    }
    return addresses;
}

// ip -f inet addr show 里只有一行 inet <地址>/<掩码>
QString AddrInet(const QString &addr_output)
{
    const QStringList lines = addr_output.split(QLatin1Char('\n'));
    for (const QString &line : lines)
    {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QStringLiteral("inet ")))
        {
            continue;
        }
        return trimmed.section(QLatin1Char(' '), 1, 1).section(QLatin1Char('/'), 0, 0);
    }
    return QString();
}

// 排序：和本机同网段的先试，其余的保持原顺序跟在后面
QStringList OrderByLocalLan(const QStringList &candidates, const QStringList &local_addresses)
{
    QStringList preferred;
    QStringList others;
    for (const QString &candidate : candidates)
    {
        bool same_lan = false;
        for (const QString &local : local_addresses)
        {
            if (SameLan24(candidate, local))
            {
                same_lan = true;
                break;
            }
        }
        if (same_lan)
        {
            preferred.append(candidate);
        }
        else
        {
            others.append(candidate);
        }
    }
    return preferred + others;
}

} // namespace qsc::adb
