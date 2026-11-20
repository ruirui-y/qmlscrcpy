#pragma once

// 切无线：跑 adb tcpip、轮询设备 IP、再 adb connect 到那个 IP。
// 全程阻塞（含 msleep），只能在工作线程调用。

#include <QString>
#include <QStringList>

namespace qsc::app
{

// 三步流程的结果，工作线程算完交回主线程显示
struct WifiSwitchResult
{
    bool ok_ = false;                                                                           // 整条流程成没成
    QString endpoint_;                                                                          // 成功时的 IP:端口
    QString message_;                                                                           // 失败时给界面看的原因
};

class WifiBridge
{
public:
    // 同步跑完整条切无线流程；成功时结果里带 IP:5555
    static WifiSwitchResult Switch(const QString &serial);

private:
    static QStringList HostLanAddresses();                                                      // 本机所有网卡地址，用来给候选排序
};

} // namespace qsc::app
