#include "base/log.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

namespace qsc::base
{

Q_LOGGING_CATEGORY(lcInput, "qsc.input")
Q_LOGGING_CATEGORY(lcSession, "qsc.session")
Q_LOGGING_CATEGORY(lcStream, "qsc.stream")
Q_LOGGING_CATEGORY(lcRender, "qsc.render")

namespace
{

// 日志文件名前缀和日期格式，清理旧文件时按这个解析
const char *const kFilePrefix = "LOG_";
const char *const kFileSuffix = ".log";
const char *const kDateFormat = "yyyy_MM_dd";

QMutex g_mutex;                                                                                 // message handler 会被多线程调用，文件操作要串起来
QFile g_file;
QDate g_file_date;                                                                              // 当前打开的文件是哪一天的
LogConfig g_config;
QtMessageHandler g_previous = nullptr;                                                          // 原来的处理器，转发给它控制台照样有输出

// Qt 的 message type 转成日志里的级别名
const char *LevelName(QtMsgType type)
{
    switch (type)
    {
    case QtDebugMsg:
        return "DEBUG";
    case QtInfoMsg:
        return "INFO";
    case QtWarningMsg:
        return "WARN";
    case QtCriticalMsg:
        return "ERROR";
    case QtFatalMsg:
        return "FATAL";
    default:
        return "LOG";
    }
}

// Qt 的 message type 转成可比较的级别数值
int LevelValue(QtMsgType type)
{
    switch (type)
    {
    case QtDebugMsg:
        return static_cast<int>(LogLevel::Debug);
    case QtInfoMsg:
        return static_cast<int>(LogLevel::Info);
    case QtWarningMsg:
        return static_cast<int>(LogLevel::Warning);
    default:
        return static_cast<int>(LogLevel::Critical);
    }
}

// 线程标识：主线程写 gui，其它线程写线程 id 的十六进制
QString ThreadName()
{
    if (QCoreApplication::instance() != nullptr && QThread::currentThread() == QCoreApplication::instance()->thread())
    {
        return QStringLiteral("gui");
    }
    return QStringLiteral("t%1").arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);
}

// 删掉比"今天 - keep_days"更老的日志；文件名解析不出日期的（用户自己放的文件）不动
void RemoveOldFiles(const QString &dir_path, int keep_days)
{
    if (keep_days <= 0)
    {
        return;
    }

    const QDate oldest_kept = QDate::currentDate().addDays(-keep_days);
    QDir dir(dir_path);                                                                         // remove 不是 const 成员，这里不能用 const 对象
    const QStringList names = dir.entryList(QStringList() << QStringLiteral("LOG_*.log"), QDir::Files);
    for (const QString &name : names)
    {
        // 从 LOG_2026_11_12.log 里截出 2026_11_12
        const QString date_text = name.mid(static_cast<int>(qstrlen(kFilePrefix)),
                                           static_cast<int>(qstrlen(kDateFormat)));
        const QDate file_date = QDate::fromString(date_text, QString::fromLatin1(kDateFormat));
        if (!file_date.isValid() || file_date >= oldest_kept)
        {
            continue;
        }
        dir.remove(name);
    }
}

// 保证当前打开的是"今天"那个文件；跨天就换一份，顺便清旧文件
void EnsureFileForToday()
{
    const QDate today = QDate::currentDate();
    if (g_file.isOpen() && g_file_date == today)
    {
        return;
    }

    if (g_file.isOpen())
    {
        g_file.close();
    }

    const QDir dir(g_config.dir_path);
    QDir().mkpath(g_config.dir_path);
    RemoveOldFiles(g_config.dir_path, g_config.keep_days);

    const QString file_name = QStringLiteral("%1%2%3")
                                  .arg(QString::fromLatin1(kFilePrefix))
                                  .arg(today.toString(QString::fromLatin1(kDateFormat)))
                                  .arg(QString::fromLatin1(kFileSuffix));
    g_file.setFileName(dir.filePath(file_name));
    g_file_date = today;
    g_file.open(QIODevice::WriteOnly | QIODevice::Append);
}

// 把消息写进当天的日志文件，再转给原处理器输出到控制台
void MessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    {
        QMutexLocker locker(&g_mutex);

        // 低于配置级别的只给控制台，不落盘
        const bool write_file = LevelValue(type) >= static_cast<int>(g_config.level);
        if (write_file && !g_config.dir_path.isEmpty())
        {
            EnsureFileForToday();
            if (g_file.isOpen())
            {
                const QString category = context.category != nullptr ? QString::fromLatin1(context.category)
                                                                     : QStringLiteral("default");
                const QString line = QStringLiteral("%1 %2 [%3] %4 %5\n")
                                         .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                                         .arg(QString::fromLatin1(LevelName(type)), -5)
                                         .arg(ThreadName())
                                         .arg(category, -12)
                                         .arg(message);
                g_file.write(line.toUtf8());
                g_file.flush();
            }
        }
    }

    // 控制台还是走原来的处理器，行为不变
    if (g_previous != nullptr)
    {
        g_previous(type, context, message);
    }
}

} // namespace

// 装 message handler，打开当天日志并清理过期文件
void Init(const LogConfig &config)
{
    QMutexLocker locker(&g_mutex);
    g_config = config;

    if (!g_config.dir_path.isEmpty())
    {
        QDir().mkpath(g_config.dir_path);
        EnsureFileForToday();
    }

    if (g_previous == nullptr)
    {
        g_previous = qInstallMessageHandler(MessageHandler);
    }
}

// 配置里的字符串转成日志级别
LogLevel ParseLevel(const QString &text)
{
    const QString lowered = text.trimmed().toLower();
    if (lowered == QStringLiteral("debug"))
    {
        return LogLevel::Debug;
    }
    if (lowered == QStringLiteral("warning") || lowered == QStringLiteral("warn"))
    {
        return LogLevel::Warning;
    }
    if (lowered == QStringLiteral("critical") || lowered == QStringLiteral("error"))
    {
        return LogLevel::Critical;
    }
    return LogLevel::Info;
}

} // namespace qsc::base
