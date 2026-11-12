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

    // [input] 段：滚轮模式，window.ini 里选过的优先，其次 config.ini 的默认值
    QString WheelMode() const;
    void SetWheelMode(const QString &mode);
    // [input] 段：剪贴板
    bool ClipboardAutoSync() const;                                                             // 电脑剪贴板一变就推到设备

    // [update] 段：版本检查用的 latest.json 地址（留空用内置源列表）
    QString UpdateUrl() const;

    // [transfer] 段：拖文件上传到设备哪个目录
    QString UploadDir() const;
    void SetUploadDir(const QString &dir);

    // [keys] 段：窗口快捷键，name 取 back / home / screenshot / fullscreen
    QString Shortcut(const QString &name) const;
    void SetShortcut(const QString &name, const QString &sequence);

    // [ui] 段：老板键
    QString BossKeyText() const;                                                                // 例如 Ctrl+Alt+B，界面里改过的优先
    void SetBossKeyText(const QString &text);                                                   // 界面里改的写 window.ini

    // [ui] 段：控制台窗口要不要显示
    bool ShowConsole() const;
    void SetShowConsole(bool show);

    int DragPixels() const;                                                                     // 滑动模式下一格滚轮手指移动多少像素，window.ini 里调过的优先
    void SetDragPixels(int pixels);

    // [device] 段：上次连上的设备，启动时优先自动连它
    QString LastSerial() const;
    void SetLastSerial(const QString &serial);

    // [video] 段
    int VideoMaxSize() const;                                                                   // 长边上限，0 = 设备原生分辨率
    // 有线和无线各一套画质参数：serial 带冒号（IP:端口）算无线
    static QString VideoProfile(const QString &serial);
    int VideoBitRate(const QString &profile = QString()) const;
    int VideoMaxFps(const QString &profile = QString()) const;                                  // 0 = 不限帧（跟着屏幕刷新走）
    void SetVideoBitRate(int bit_rate, const QString &profile = QString());                     // 界面里调过的码率写 window.ini
    void SetVideoMaxFps(int max_fps, const QString &profile = QString());                       // 界面里调过的帧率写 window.ini

    // [video_window] 段：窗口尺寸，QML 直接调
    Q_INVOKABLE QRect VideoGeometry(const QString &key) const;
    Q_INVOKABLE void SaveVideoGeometry(const QString &key, int x, int y, int width, int height);
    Q_INVOKABLE void ResetVideoGeometry(const QString &key);                                    // 删掉某个姿势的尺寸记录，恢复默认值
    Q_INVOKABLE bool ResetAllConfig();                                                          // 清空 window.ini 并删掉 config.ini，重启后回到出厂状态

    // [video_pos] 段：窗口位置
    Q_INVOKABLE QPoint VideoPosition(const QString &key) const;
    Q_INVOKABLE void SaveVideoPosition(const QString &key, int x, int y);
    Q_INVOKABLE void SaveVideoStateAsync(const QString &pos_key, int x, int y,
                                         const QString &geom_key, int width, int height);       // 后台线程落盘，不卡 UI

private:
    void EnsureDefaultConfig();                                                                 // 第一次运行写一份带注释的模板，免得用户对着空文件猜
};
