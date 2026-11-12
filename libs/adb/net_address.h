#pragma once

#include <QString>
#include <QStringList>

namespace qsc::adb
{

// 两个 IPv4 前三位一样就算同一个 /24 局域网。家用路由基本都是 /24，够用且不用知道掩码。
bool SameLan24(const QString &left, const QString &right);

// 从 `ip route` 里抠出所有 src 地址，按出现顺序返回（可能同时有移动网络和 wifi）
QStringList RouteAddresses(const QString &route_output);

// 从 `ip -f inet addr show wlan0` 里抠出 inet 后面的地址
QString AddrInet(const QString &addr_output);

// 把候选地址排个序：和本机同网段的排前面，其余按原顺序跟在后面
QStringList OrderByLocalLan(const QStringList &candidates, const QStringList &local_addresses);

} // namespace qsc::adb
