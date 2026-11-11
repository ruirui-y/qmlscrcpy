#include "adb/adb_runner.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace qsc::adb {
namespace {

// 显式设置过的 adb 路径。只在启动时写一次，不做线程保护。
QString &ExplicitAdbPath()
{
    static QString path;
    return path;
}

} // namespace

QList<AdbDevice> ParseDevices(const QByteArray &std_out)
{
    QList<AdbDevice> devices;

    const QString text = QString::fromUtf8(std_out);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const QRegularExpression whitespace(QStringLiteral("\\s+"));

    for (const QString &raw_line : lines) {
        const QString line = raw_line.trimmed();  // 去掉 Windows 的 \r
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1String("List of devices"))) {
            continue;
        }
        // adb 自己的噪声，比如 "* daemon not running; starting now at tcp:5037"
        if (line.startsWith(QLatin1Char('*'))) {
            continue;
        }

        const QStringList fields = line.split(whitespace, Qt::SkipEmptyParts);
        if (fields.size() < 2) {
            continue;
        }

        AdbDevice device;
        device.serial_ = fields.at(0);
        device.state_ = fields.at(1);
        devices.append(device);
    }

    return devices;
}

void SetAdbPath(const QString &path)
{
    ExplicitAdbPath() = path;
}

QString AdbPath()
{
    if (!ExplicitAdbPath().isEmpty()) {
        return ExplicitAdbPath();
    }

    const QByteArray from_env = qgetenv("QS_ADB_PATH");
    if (!from_env.isEmpty()) {
        return QString::fromLocal8Bit(from_env);
    }

    const QString beside_exe = QCoreApplication::applicationDirPath() + QStringLiteral("/adb.exe");
    if (QFileInfo::exists(beside_exe)) {
        return beside_exe;
    }

    return QStringLiteral("adb");
}

AdbResult Run(const QStringList &args, int timeout_ms)
{
    AdbResult result;

    QProcess process;
    process.setProgram(AdbPath());
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();

    if (!process.waitForStarted(5000)) {
        result.std_err_ = process.errorString().toUtf8();
        return result;
    }
    result.started_ = true;

    if (!process.waitForFinished(timeout_ms)) {
        // 卡住的 adb 不能留：它占着设备锁，后面的命令全会被拖死
        process.kill();
        process.waitForFinished(2000);
        result.timed_out_ = true;
        result.std_err_ = QByteArrayLiteral("adb 命令超时");
        return result;
    }

    result.exit_code_ = process.exitCode();
    result.std_out_ = process.readAllStandardOutput();
    result.std_err_ = process.readAllStandardError();
    return result;
}

QProcess *StartLongRunning(const QStringList &args, QString *error)
{
    auto *process = new QProcess();
    process->setProgram(AdbPath());
    process->setArguments(args);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start();

    if (!process->waitForStarted(5000)) {
        if (error != nullptr) {
            *error = process->errorString();
        }
        delete process;
        return nullptr;
    }

    return process;
}

} // namespace qsc::adb
