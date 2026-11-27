#pragma once

// 设备扫描：跑 adb devices / mdns，维护下拉框用的设备清单。
// 只管"有哪些设备"，不碰连接和会话。

#include <QObject>
#include <QString>
#include <QStringList>

namespace qsc::app
{

class DeviceScanner : public QObject
{
    Q_OBJECT

public:
    explicit DeviceScanner(QObject *parent = nullptr);

    QStringList Devices() const { return devices_; }                                            // 给下拉框用的地址
    QStringList Labels() const { return device_labels_; }                                       // 和 Devices() 下标对应
    QString ProposedSerial() const { return proposed_serial_; }                                 // 扫描后默认建议连哪台
    void SetProposedSerial(const QString &serial);

    QString FirstOnline() const;
    QString PickStartupSerial(const QString &fallback) const;                                   // 上次连过且还在线的优先

    void Refresh();                                                                             // 跑一次 adb devices + mdns

signals:
    void SigDevicesChanged();
    void SigProposedSerialChanged();
    void SigStatus(const QString &text);                                                        // 扫描结果提示

private:
    QStringList online_serials_;                                                                // 最近一次扫描到的在线设备
    QStringList devices_;                                                                       // 下拉框地址
    QStringList device_labels_;                                                                 // 下拉框文字
    QString proposed_serial_;                                                                   // 扫描后建议连接的那台
};

} // namespace qsc::app
