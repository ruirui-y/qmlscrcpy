#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

class QProcess;

namespace qsc::adb
{

// 一次 adb 调用的结果。
// 故意区分"进程没起来"和"起来了但退出码非 0"：前者是环境问题（找不到 adb.exe），
// 后者是命令本身失败（设备没连、端口被占），排查方向完全不同。
struct AdbResult
{
    bool started_ = false;                                                                      // 进程有没有起来
    bool timed_out_ = false;                                                                    // 是不是超时被杀掉的
    int exit_code_ = -1;                                                                        // 进程退出码，没起来时是 -1
    QByteArray std_out_;                                                                        // 标准输出原文
    QByteArray std_err_;                                                                        // 标准错误原文，出错时给界面显示用

    bool Ok() const { return started_ && !timed_out_ && exit_code_ == 0; }

    // 出错时给界面看的文字：优先 stderr，但没有时退回 stdout。
    // 某些 adb 版本把 "device not found" 之类打到 stdout，只看 stderr 会得到一句空错误。
    QString ErrorText() const;
};

// adb devices 输出里的一行
struct AdbDevice
{
    QString serial_;                                                                            // 序列号，或者 wifi 模式下的 "192.168.0.174:5555"
    QString state_;                                                                             // device / offline / unauthorized
};

// 解析 adb devices 的输出。
// adb 在 Windows 上输出的是 \r\n，所以每行都要 trim，不然 state 会带上 \r 比不过。
QList<AdbDevice> ParseDevices(const QByteArray &std_out);

// adb.exe 的路径查找顺序：显式设置 → 环境变量 QS_ADB_PATH → 程序目录 → PATH 里的 adb
void SetAdbPath(const QString &path);
QString AdbPath();

// 同步跑一条 adb 命令，适合跑完就结束的（push / reverse / devices）。超时会杀进程。
AdbResult Run(const QStringList &args, int timeout_ms = 15000);

// 起一个不会自己退出的进程：adb shell app_process ... 会一直挂着，它退出就等于服务端挂了。
// 返回的 QProcess 由调用方负责 delete；起不来返回 nullptr 并写 error。
QProcess *StartLongRunning(const QStringList &args, QString *error);

} // namespace qsc::adb
