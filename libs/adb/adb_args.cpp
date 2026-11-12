#include "adb/adb_args.h"

namespace qsc::adb
{

QStringList AdbArgs::WithSerial(const QString &serial, const QStringList &rest)
{
    if (serial.isEmpty())
    {
        return rest;
    }

    QStringList args;
    args << QStringLiteral("-s") << serial;
    args << rest;
    return args;
}

QStringList AdbArgs::Devices()
{
    return QStringList{QStringLiteral("devices")};
}

QStringList AdbArgs::Push(const QString &serial, const QString &local, const QString &remote)
{
    // 本地路径带空格也必须是一个参数，所以这里用 QStringList 逐个塞，不拼字符串
    return WithSerial(serial, QStringList{QStringLiteral("push"), local, remote});
}

QStringList AdbArgs::Reverse(const QString &serial, const QString &socket_name, quint16 local_port)
{
    // 设备端监听地址在前，主机目标在后；写反了设备端 server 连不上自己的 abstract socket
    const QString remote = QStringLiteral("localabstract:%1").arg(socket_name);
    const QString host = QStringLiteral("tcp:%1").arg(local_port);
    return WithSerial(serial, QStringList{QStringLiteral("reverse"), remote, host});
}

QStringList AdbArgs::ReverseRemove(const QString &serial, const QString &socket_name)
{
    const QString remote = QStringLiteral("localabstract:%1").arg(socket_name);
    return WithSerial(serial, QStringList{QStringLiteral("reverse"), QStringLiteral("--remove"), remote});
}

QStringList AdbArgs::Forward(const QString &serial, quint16 local_port, const QString &socket_name)
{
    const QString local = QStringLiteral("tcp:%1").arg(local_port);
    const QString remote = QStringLiteral("localabstract:%1").arg(socket_name);
    return WithSerial(serial, QStringList{QStringLiteral("forward"), local, remote});
}

QStringList AdbArgs::ForwardRemove(const QString &serial, quint16 local_port)
{
    const QString local = QStringLiteral("tcp:%1").arg(local_port);
    return WithSerial(serial, QStringList{QStringLiteral("forward"), QStringLiteral("--remove"), local});
}

QStringList AdbArgs::Shell(const QString &serial, const QStringList &command)
{
    QStringList rest{QStringLiteral("shell")};
    rest << command;
    return WithSerial(serial, rest);
}

QStringList AdbArgs::Tcpip(const QString &serial, quint16 port)
{
    return WithSerial(serial, QStringList{QStringLiteral("tcpip"), QString::number(port)});
}

QStringList AdbArgs::Connect(const QString &endpoint)
{
    return QStringList{QStringLiteral("connect"), endpoint};
}

QStringList AdbArgs::Disconnect(const QString &endpoint)
{
    return QStringList{QStringLiteral("disconnect"), endpoint};
}

QString AdbArgs::SocketName(quint32 scid)
{
    // 8 位定宽、小写十六进制、前面补 0：scid=999 → scrcpy_000003e7
    return QStringLiteral("scrcpy_%1").arg(scid, 8, 16, QLatin1Char('0'));
}

} // namespace qsc::adb
