#pragma once

#include <QLoggingCategory>
#include <QString>

namespace qsc::base
{

// 日志级别，数字越大越严重
enum class LogLevel
{
    Debug = 0,                                                                                  // 调试细节，默认不落盘
    Info = 1,                                                                                   // 正常流程
    Warning = 2,                                                                                // 可以继续，但不对劲
    Critical = 3,                                                                               // 出错了
};

// 日志落地配置：从 config.ini 的 [log] 段读出来，由 main 装进去
struct LogConfig
{
    LogLevel level = LogLevel::Info;                                                            // 低于这个级别的直接丢
    QString dir_path;                                                                           // 空表示只输出到控制台；非空则按天写 LOG_yyyy_MM_dd.log
    int keep_days = 15;                                                                         // 只保留最近几天的日志，更老的启动时删掉
};

void Init(const LogConfig &config);                                                             // 装 message handler，之后所有 qDebug 都走它
LogLevel ParseLevel(const QString &text);                                                       // "debug"/"info"/"warning"/"critical"

// 各模块的分类；用 qCDebug(qsc::base::lcInput) 这样打
Q_DECLARE_LOGGING_CATEGORY(lcInput)
Q_DECLARE_LOGGING_CATEGORY(lcSession)
Q_DECLARE_LOGGING_CATEGORY(lcStream)
Q_DECLARE_LOGGING_CATEGORY(lcRender)

} // namespace qsc::base
