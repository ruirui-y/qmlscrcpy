#include "adb/adb_args.h"

namespace qsc::adb
{

// 把 -s 序列号插到参数最前面；serial 为空就原样返回
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

// adb devices：一次列全部设备，不需要 -s
QStringList AdbArgs::Devices()
{
    return QStringList{QStringLiteral("devices")};
}

// 把本地文件推到设备上；路径带空格也算一个参数
QStringList AdbArgs::Push(const QString &serial, const QString &local, const QString &remote)
{
    // 本地路径带空格也必须是一个参数，所以这里用 QStringList 逐个塞，不拼字符串
    return WithSerial(serial, QStringList{QStringLiteral("push"), local, remote});
}

// 设备侧监听 localabstract 名，转发到本机 tcp 端口
QStringList AdbArgs::Reverse(const QString &serial, const QString &socket_name, quint16 local_port)
{
    // 设备端监听地址在前，主机目标在后；写反了设备端 server 连不上自己的 abstract socket
    const QString remote = QStringLiteral("localabstract:%1").arg(socket_name);
    const QString host = QStringLiteral("tcp:%1").arg(local_port);
    return WithSerial(serial, QStringList{QStringLiteral("reverse"), remote, host});
}

// 按名字撤掉 reverse 隧道
QStringList AdbArgs::ReverseRemove(const QString &serial, const QString &socket_name)
{
    const QString remote = QStringLiteral("localabstract:%1").arg(socket_name);
    return WithSerial(serial, QStringList{QStringLiteral("reverse"), QStringLiteral("--remove"), remote});
}

// 本机监听 tcp 端口，转发到设备侧 localabstract 名
QStringList AdbArgs::Forward(const QString &serial, quint16 local_port, const QString &socket_name)
{
    const QString local = QStringLiteral("tcp:%1").arg(local_port);
    const QString remote = QStringLiteral("localabstract:%1").arg(socket_name);
    return WithSerial(serial, QStringList{QStringLiteral("forward"), local, remote});
}

// 按本机端口撤掉 forward 隧道
QStringList AdbArgs::ForwardRemove(const QString &serial, quint16 local_port)
{
    const QString local = QStringLiteral("tcp:%1").arg(local_port);
    return WithSerial(serial, QStringList{QStringLiteral("forward"), QStringLiteral("--remove"), local});
}

// 在设备上跑一条 shell 命令，每个词单独成参数
QStringList AdbArgs::Shell(const QString &serial, const QStringList &command)
{
    QStringList rest{QStringLiteral("shell")};
    rest << command;
    return WithSerial(serial, rest);
}

// 让设备切成 tcpip 调试模式并监听指定端口
QStringList AdbArgs::Tcpip(const QString &serial, quint16 port)
{
    return WithSerial(serial, QStringList{QStringLiteral("tcpip"), QString::number(port)});
}

// 连一台走网络调试的设备
QStringList AdbArgs::Connect(const QString &endpoint)
{
    return QStringList{QStringLiteral("connect"), endpoint};
}

// 断开某台网络设备
QStringList AdbArgs::Disconnect(const QString &endpoint)
{
    return QStringList{QStringLiteral("disconnect"), endpoint};
}

// 按 scid 拼出 scrcpy 的 socket 名，固定 8 位十六进制
QString AdbArgs::SocketName(quint32 scid)
{
    // 8 位定宽、小写十六进制、前面补 0：scid=999 → scrcpy_000003e7
    return QStringLiteral("scrcpy_%1").arg(scid, 8, 16, QLatin1Char('0'));
}

} // namespace qsc::adb
