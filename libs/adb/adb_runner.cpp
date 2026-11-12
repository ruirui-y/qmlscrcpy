#include "adb/adb_runner.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace qsc::adb
{
namespace
{

// 显式设置过的 adb 路径。只在启动时写一次，不做线程保护。
QString &ExplicitAdbPath()
{
    static QString path;
    return path;
}

} // namespace

QString AdbResult::ErrorText() const
{
    const QString from_err = QString::fromUtf8(std_err_).trimmed();
    if (!from_err.isEmpty())
    {
        return from_err;
    }
    return QString::fromUtf8(std_out_).trimmed();
}

// 解析 adb devices 输出，跳过表头和 adb 自己打的噪声行
QList<AdbDevice> ParseDevices(const QByteArray &std_out)
{
    QList<AdbDevice> devices;

    const QString text = QString::fromUtf8(std_out);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const QRegularExpression whitespace(QStringLiteral("\\s+"));

    for (const QString &raw_line : lines)
    {
        const QString line = raw_line.trimmed();                                                // 去掉 Windows 的 \r
        if (line.isEmpty())
        {
            continue;
        }
        if (line.startsWith(QLatin1String("List of devices")))
        {
            continue;
        }
        // adb 自己的噪声，比如 "* daemon not running; starting now at tcp:5037"
        if (line.startsWith(QLatin1Char('*')))
        {
            continue;
        }

        const QStringList fields = line.split(whitespace, Qt::SkipEmptyParts);
        if (fields.size() < 2)
        {
            continue;
        }

        AdbDevice device;
        device.serial_ = fields.at(0);
        device.state_ = fields.at(1);
        devices.append(device);
    }

    return devices;
}

// 指定 adb 路径，测试和自定义安装用
void SetAdbPath(const QString &path)
{
    ExplicitAdbPath() = path;
}

// 取 adb 路径：显式设置 > 环境变量 > 同目录 > Tools 子目录 > PATH
QString AdbPath()
{
    if (!ExplicitAdbPath().isEmpty())
    {
        return ExplicitAdbPath();
    }

    const QByteArray from_env = qgetenv("QS_ADB_PATH");
    if (!from_env.isEmpty())
    {
        return QString::fromLocal8Bit(from_env);
    }

    const QString beside_exe = QCoreApplication::applicationDirPath() + QStringLiteral("/adb.exe");
    if (QFileInfo::exists(beside_exe))
    {
        return beside_exe;
    }

    // 安装包把 adb 放在 Tools\ 下（见 setup/collect.ps1），这里也认一下
    const QString in_tools = QCoreApplication::applicationDirPath() + QStringLiteral("/Tools/adb.exe");
    if (QFileInfo::exists(in_tools))
    {
        return in_tools;
    }

    return QStringLiteral("adb");
}

// 跑一条 adb 命令并等它结束，超时就杀进程
AdbResult Run(const QStringList &args, int timeout_ms)
{
    AdbResult result;

    QProcess process;
    process.setProgram(AdbPath());
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();

    if (!process.waitForStarted(5000))
    {
        result.std_err_ = process.errorString().toUtf8();
        return result;
    }
    result.started_ = true;

    if (!process.waitForFinished(timeout_ms))
    {
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

// 起一个长跑进程（server 进程用它拉起来）
QProcess *StartLongRunning(const QStringList &args, QString *error)
{
    auto *process = new QProcess();
    process->setProgram(AdbPath());
    process->setArguments(args);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start();

    if (!process->waitForStarted(5000))
    {
        if (error != nullptr)
        {
            *error = process->errorString();
        }
        delete process;
        return nullptr;
    }

    return process;
}

// 追加文件传输：把本地文件推到设备指定目录，边跑边上报进度
// 插在 StartLongRunning 之后、namespace 收尾之前

// 推一个文件到设备：先确保目录存在，再用 ASCII 临时名推，最后在设备上改回原名
//
// 为什么要绕：adb.exe 在 Windows 上处理不了非 ASCII 的本地文件名——
// 中文名会被截成空串，远端路径变成 "/sdcard/Download/." 直接失败。
// 解法是本地用 ASCII 临时名推上去，改名时把 UTF-8 文件名 base64 编码后交给设备解码。
bool PushFile(const QString &serial, const QString &local_path, const QString &remote_dir,
              const ProgressCallback &on_progress, QString *error)
{
    const QFileInfo info(local_path);
    if (!info.isFile())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("不是文件：%1").arg(local_path);
        }
        return false;
    }

    // 目录不存在时 adb push 会失败，先 mkdir
    const AdbResult mkdir_result = Run(
        QStringList() << QStringLiteral("-s") << serial << QStringLiteral("shell") << QStringLiteral("mkdir")
                      << QStringLiteral("-p") << remote_dir, 5000);
    if (!mkdir_result.Ok())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("建目录失败：%1").arg(mkdir_result.ErrorText());
        }
        return false;
    }

    // 临时名只用 ASCII，扩展名保留（有些设备按扩展名决定怎么索引）
    static QAtomicInteger<int> upload_sequence{0};
    const QString suffix = info.suffix().isEmpty() ? QString()
                                                   : QStringLiteral(".%1").arg(info.suffix().toLower());
    const QString temp_name = QStringLiteral("qmlscrcpy_%1%2").arg(upload_sequence.fetchAndAddRelaxed(1)).arg(suffix);
    const QString remote_temp = remote_dir + QLatin1Char('/') + temp_name;

    QProcess process;
    process.setProgram(AdbPath());
    process.setArguments(QStringList() << QStringLiteral("-s") << serial << QStringLiteral("push") << local_path
                                       << remote_temp);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();

    if (!process.waitForStarted(5000))
    {
        if (error != nullptr)
        {
            *error = process.errorString();
        }
        return false;
    }

    // adb push 把进度写在 stdout 里，形如 "[ 45%] ..."，边跑边解析
    QRegularExpression percent_rule(QStringLiteral("\\[\\s*(\\d+)%\\]"));
    QByteArray pending;                                                                         // 还没凑成整行的输出
    const auto consume = [&](const QByteArray &chunk)
    {
        pending.append(chunk);
        for (;;)
        {
            const int newline = pending.indexOf('\n');
            if (newline < 0)
            {
                break;
            }
            const QByteArray line = pending.left(newline);
            pending.remove(0, newline + 1);

            const QString text = QString::fromLocal8Bit(line);
            const QRegularExpressionMatch match = percent_rule.match(text);
            if (match.hasMatch() && on_progress != nullptr)
            {
                on_progress(match.captured(1).toInt());
            }
        }
    };

    while (process.state() != QProcess::NotRunning)
    {
        if (process.waitForReadyRead(200))
        {
            consume(process.readAllStandardOutput());
        }
    }
    consume(process.readAllStandardOutput());
    const QByteArray stderr_text = process.readAllStandardError();

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        if (error != nullptr)
        {
            const QString message = QString::fromLocal8Bit(stderr_text).trimmed();
            *error = message.isEmpty() ? QStringLiteral("adb push 退出码 %1").arg(process.exitCode()) : message;
        }
        return false;
    }

    if (on_progress != nullptr)
    {
        on_progress(100);
    }

    // 改回原名：文件名里的 UTF-8 字节用 base64 传，避免再被 Windows 代码页破坏
    const QString base_name = info.fileName();
    QString rename_command;
    bool plain_ascii = true;
    for (const QChar ch : base_name)
    {
        if (ch.unicode() > 0x7F)
        {
            plain_ascii = false;
            break;
        }
    }

    const QString target = remote_dir + QLatin1Char('/') + base_name;
    if (plain_ascii)
    {
        rename_command = QStringLiteral("mv \"%1\" \"%2\"").arg(remote_temp, target);
    }
    else
    {
        // 解码出来只有文件名，必须把目录拼回去，否则 mv 会按相对路径落到只读的 /
        const QString encoded = QString::fromLatin1(base_name.toUtf8().toBase64());
        rename_command = QStringLiteral("mv \"%1\" \"%2/$(echo %3 | base64 -d)\"")
                             .arg(remote_temp, remote_dir, encoded);
    }

    const AdbResult rename_result = Run(QStringList() << QStringLiteral("-s") << serial << QStringLiteral("shell")
                                                      << rename_command, 8000);
    if (!rename_result.Ok())
    {
        if (error != nullptr)
        {
            *error = QStringLiteral("文件推上去了但改名失败（设备上是 %1）：%2")
                         .arg(temp_name, rename_result.ErrorText());
        }
        return false;
    }

    return true;
}

} // namespace qsc::adb
