#pragma once

#include <QString>

namespace qsc::update
{

// 版本号比较：只认 "1.2.3" 这种三段式，多出来的后缀（-beta、+build）忽略。
// 解析不出来的一律当 0.0.0，这样"服务端给错格式"只会更保守，不会误报成新版。
struct SemVer
{
    int major_ = 0;                                                                             // 主版本
    int minor_ = 0;                                                                             // 次版本
    int patch_ = 0;                                                                             // 修订号
};

// 把 "0.1.4" 解析成三段；文本非法时全部为 0
SemVer Parse(const QString &text);

// a 比 b 新返回 1，一样返回 0，旧返回 -1
int Compare(const QString &a, const QString &b);

} // namespace qsc::update
