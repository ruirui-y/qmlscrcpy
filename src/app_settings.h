#pragma once

#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>

// 两个 INI，职责分开：
//   config.ini  用户手改（日志级别、采集参数），程序只读，注释不会被 QSettings 破坏
//   window.ini  程序自己写（窗口位置尺寸），没有注释，随便重写
// 都用 QSettings 的 IniFormat + 显式路径：不传组织名/应用名时它默认走注册表，这里刻意避开。
class AppSettings : public QObject
{
    Q_OBJECT

public:
    explicit AppSettings(QObject *parent = nullptr);

    QString ConfigPath() const;                                                                 // config.ini 路径，打日志用
    QString WindowPath() const;                                                                 // window.ini 路径，打日志用

    // [log] 段
    QString LogLevelText() const;                                                               // debug / info / warning / critical
    bool LogToFile() const;
    int LogKeepDays() const;

    // [video] 段
    int VideoMaxSize() const;                                                                   // 长边上限，0 = 设备原生分辨率
    int VideoBitRate() const;

    // [video_window] 段：窗口尺寸，QML 直接调
    Q_INVOKABLE QRect VideoGeometry(const QString &key) const;
    Q_INVOKABLE void SaveVideoGeometry(const QString &key, int x, int y, int width, int height);

    // [video_pos] 段：窗口位置
    Q_INVOKABLE QPoint VideoPosition(const QString &key) const;
    Q_INVOKABLE void SaveVideoPosition(const QString &key, int x, int y);

private:
    void EnsureDefaultConfig();                                                                 // 第一次运行写一份带注释的模板，免得用户对着空文件猜
};
