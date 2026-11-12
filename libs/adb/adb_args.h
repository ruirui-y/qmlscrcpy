#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace qsc::adb
{

// adb 命令的参数拼装，全是纯函数：给输入产出 argv，不碰进程也不碰文件。
// 单独拆出来的理由：参数顺序、-s 放哪儿、带空格的路径会不会被拆成两个参数，
// 这些靠跑真机试错太慢，做成纯函数就能在单测里钉死。
class AdbArgs
{
public:
    // adb devices，一次列全部，不需要 -s
    static QStringList Devices();

    // adb -s <serial> push <本地> <设备路径>
    static QStringList Push(const QString &serial, const QString &local, const QString &remote);

    // adb -s <serial> reverse localabstract:<socket 名> tcp:<主机端口>
    // 顺序是"设备端在前、主机端在后"：设备监听那个 abstract 名字，收到连接转给主机端口。
    // 写反了设备端的 server 会直接 connect 失败。
    static QStringList Reverse(const QString &serial, const QString &socket_name, quint16 local_port);

    // adb -s <serial> reverse --remove localabstract:<socket 名>，按设备端名字删，不是按端口
    static QStringList ReverseRemove(const QString &serial, const QString &socket_name);

    // adb -s <serial> forward tcp:<主机端口> localabstract:<socket 名>
    // forward 和 reverse 正好相反：主机监听端口，转发到设备上的 abstract socket
    static QStringList Forward(const QString &serial, quint16 local_port, const QString &socket_name);

    // adb -s <serial> forward --remove tcp:<主机端口>
    static QStringList ForwardRemove(const QString &serial, quint16 local_port);

    // adb -s <serial> shell <命令...>
    // 命令的每一段都是一个独立参数，不要把整条 shell 串拼成一个再交给 adb
    static QStringList Shell(const QString &serial, const QStringList &command);

    // adb -s <serial> tcpip <端口>，把设备上的 adbd 切到网络模式
    static QStringList Tcpip(const QString &serial, quint16 port);

    // adb connect / disconnect <host:port>，是 adb 服务端的事，跟某一台设备无关，不能带 -s
    static QStringList Connect(const QString &endpoint);
    static QStringList Disconnect(const QString &endpoint);

    // socket 名：scrcpy_ 加 8 位小写十六进制 scid，设备端按这个名字建 localabstract
    static QString SocketName(quint32 scid);

private:
    // 在参数最前面插 -s <serial>；serial 为空就不插，只有一台设备时 adb 也认
    static QStringList WithSerial(const QString &serial, const QStringList &rest);
};

} // namespace qsc::adb
