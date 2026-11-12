#include "app_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSettings>

namespace
{

// 配置文件统一放 exe 下的 config 目录：绿色软件，拷走整目录就能用
QString ConfigDirPath()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/config");
}

// 拼出 config 目录下的某个文件路径
QString FilePathInConfigDir(const char *file_name)
{
    return ConfigDirPath() + QLatin1Char('/') + QString::fromLatin1(file_name);
}

// 把旧版本散落在 exe 根目录的 ini 搬到 config 下，搬过的不再重复搬
void MigrateLegacyIni(const char *file_name)
{
    const QString legacy_path = QCoreApplication::applicationDirPath() + QLatin1Char('/')
                                + QString::fromLatin1(file_name);
    const QString target_path = FilePathInConfigDir(file_name);
    if (QFile::exists(legacy_path) && !QFile::exists(target_path))
    {
        QDir().mkpath(ConfigDirPath());
        QFile::rename(legacy_path, target_path);
    }
}

// 用户配置：只读，绝不让 QSettings 改写，否则注释会被转义成 %23 之类
QSettings &ConfigStore()
{
    static QSettings settings(FilePathInConfigDir("config.ini"), QSettings::IniFormat);
    return settings;
}

// 程序自己的状态：读写都在这，随便重写
QSettings &WindowStore()
{
    static QSettings settings(FilePathInConfigDir("window.ini"), QSettings::IniFormat);
    return settings;
}

// 默认配置模板，带注释；只在文件不存在时写一次
const char *const kDefaultConfig = R"(# QmlScrcpy 配置，改完重启生效
# 日志级别：debug / info / warning / critical
[log]
level=info
to_file=true
keep_days=15

# 滚轮模式：scroll = 发滚动事件（微信这类原生列表），drag = 模拟手指滑动（阅读器、地图）
[input]
wheel_mode=scroll
# 滑动模式下一格滚轮手指移动多少像素（太小会被当点击，太大一下翻过头）
drag_pixels=60
# 电脑剪贴板一变就自动推到设备（会同步敏感内容，默认关）
clipboard_autosync=false

# 老板键：一键隐藏/恢复所有窗口，格式 Ctrl+Alt+B
[ui]
boss_key=Ctrl+Alt+B

# 视频采集：长边上限，0 = 设备原生分辨率（更清晰，码率也要跟着涨）
[video]
max_size=0
bit_rate=8000000
# 帧率上限：0 = 不限（120Hz 屏能跑 120fps）；实测这项对观感几乎没影响，一般不用改
max_fps=0

# 无线连接单独一套（串号里带冒号就用这组）：6M 在局域网下够用且不排队
[video_wireless]
bit_rate=6000000
max_fps=0)";

} // namespace

AppSettings::AppSettings(QObject *parent) : QObject(parent)
{
    // 目录和迁移要在读写配置之前完成，否则 QSettings 会在根目录又建一份
    QDir().mkpath(ConfigDirPath());
    MigrateLegacyIni("config.ini");
    MigrateLegacyIni("window.ini");

    EnsureDefaultConfig();

    // 老版本把画质覆盖值存在 [video] 里（不分有线无线），现在分档了，清掉免得和 [video_wired] 混着看
    QSettings &window_store = WindowStore();
    if (window_store.contains(QStringLiteral("video/bit_rate")))
    {
        window_store.remove(QStringLiteral("video"));
        window_store.sync();
    }
}

// config.ini 不存在时写一份带注释的模板
void AppSettings::EnsureDefaultConfig()
{
    // 已经存在就不动，用户改过的值不能被覆盖
    if (QFile::exists(ConfigPath()))
    {
        return;
    }

    QDir().mkpath(ConfigDirPath());
    QFile file(ConfigPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return;
    }
    file.write(kDefaultConfig);
    file.close();
}

QString AppSettings::ConfigPath() const
{
    return FilePathInConfigDir("config.ini");
}

QString AppSettings::WindowPath() const
{
    return FilePathInConfigDir("window.ini");
}

// 日志级别：配置里写字符串，解析交给调用方
QString AppSettings::LogLevelText() const
{
    return ConfigStore().value(QStringLiteral("log/level"), QStringLiteral("info")).toString();
}

bool AppSettings::LogToFile() const
{
    return ConfigStore().value(QStringLiteral("log/to_file"), true).toBool();
}

int AppSettings::LogKeepDays() const
{
    return ConfigStore().value(QStringLiteral("log/keep_days"), 15).toInt();
}

// 滚轮模式：界面里选过的写 window.ini，没选过就用 config.ini 的默认值
QString AppSettings::WheelMode() const
{
    const QString chosen = WindowStore().value(QStringLiteral("input/wheel_mode")).toString();
    if (!chosen.isEmpty())
    {
        return chosen;
    }
    return ConfigStore().value(QStringLiteral("input/wheel_mode"), QStringLiteral("scroll")).toString();
}

// 记下用户选的滚轮模式，写 window.ini
void AppSettings::SetWheelMode(const QString &mode)
{
    QSettings &store = WindowStore();
    store.setValue(QStringLiteral("input/wheel_mode"), mode);
    store.sync();
}

// 拖动模式一格滚轮拖多少像素：太小会被当点击，太大一下翻过头
int AppSettings::DragPixels() const
{
    // 界面里调过的写 window.ini，没调过就用 config.ini 的默认值
    int pixels = WindowStore().value(QStringLiteral("input/drag_pixels"), 0).toInt();
    if (pixels <= 0)
    {
        pixels = ConfigStore().value(QStringLiteral("input/drag_pixels"), 60).toInt();
    }
    return pixels > 0 ? pixels : 60;
}

// 记下界面里调过的滑动像素
void AppSettings::SetDragPixels(int pixels)
{
    QSettings &store = WindowStore();
    store.setValue(QStringLiteral("input/drag_pixels"), pixels);
    store.sync();
}

// 电脑剪贴板一变就推到设备（默认关：某些场景会把敏感内容同步过去）
bool AppSettings::ClipboardAutoSync() const
{
    return ConfigStore().value(QStringLiteral("input/clipboard_autosync"), false).toBool();
}

// 拖文件上传的目标目录：界面里改过的优先，其次 config.ini，默认 Download/QmlScrcpy
QString AppSettings::UploadDir() const
{
    const QString override_value = WindowStore().value(QStringLiteral("transfer/upload_dir")).toString();
    if (!override_value.isEmpty())
    {
        return override_value;
    }

    const QString configured = ConfigStore().value(QStringLiteral("transfer/upload_dir")).toString();
    if (!configured.isEmpty())
    {
        return configured;
    }
    return QStringLiteral("/sdcard/Download/QmlScrcpy");
}

// 记下界面里改的上传目录
void AppSettings::SetUploadDir(const QString &dir)
{
    QSettings &store = WindowStore();
    store.setValue(QStringLiteral("transfer/upload_dir"), dir);
    store.sync();
}
// 窗口快捷键：界面里改过的（window.ini）优先，其次 config.ini，最后用内置默认
QString AppSettings::Shortcut(const QString &name) const
{
    const QString override_value = WindowStore().value(QStringLiteral("keys/%1").arg(name)).toString();
    if (!override_value.isEmpty())
    {
        return override_value;
    }

    const QString configured = ConfigStore().value(QStringLiteral("keys/%1").arg(name)).toString();
    if (!configured.isEmpty())
    {
        return configured;
    }

    // 内置默认：返回 Esc、回桌面 Home、截图 Ctrl+Shift+S、全屏 F11
    static const QHash<QString, QString> kDefaults{
        {QStringLiteral("back"), QStringLiteral("Esc")},
        {QStringLiteral("home"), QStringLiteral("Home")},
        {QStringLiteral("screenshot"), QStringLiteral("Ctrl+Shift+S")},
        {QStringLiteral("fullscreen"), QStringLiteral("F11")},
    };
    return kDefaults.value(name);
}

// 记下界面里改的快捷键（空字符串表示禁用）
void AppSettings::SetShortcut(const QString &name, const QString &sequence)
{
    QSettings &store = WindowStore();
    if (sequence.isEmpty())
    {
        store.setValue(QStringLiteral("keys/%1").arg(name), QString());
    }
    else
    {
        store.setValue(QStringLiteral("keys/%1").arg(name), sequence);
    }
    store.sync();
}
// 老板键的组合：界面里改过的（window.ini）优先，其次 config.ini
QString AppSettings::BossKeyText() const
{
    const QString override_value = WindowStore().value(QStringLiteral("ui/boss_key")).toString();
    if (!override_value.isEmpty())
    {
        return override_value;
    }
    return ConfigStore().value(QStringLiteral("ui/boss_key"), QStringLiteral("Ctrl+Alt+B")).toString();
}

// 记下界面里改的老板键
void AppSettings::SetBossKeyText(const QString &text)
{
    QSettings &store = WindowStore();
    store.setValue(QStringLiteral("ui/boss_key"), text);
    store.sync();
}
// 控制台窗口要不要显示：默认不显示，只有用户在界面上打开过才显示
bool AppSettings::ShowConsole() const
{
    return WindowStore().value(QStringLiteral("ui/show_console"), false).toBool();
}

// 记下用户的选择，下次启动照这个来
void AppSettings::SetShowConsole(bool show)
{
    QSettings &store = WindowStore();
    store.setValue(QStringLiteral("ui/show_console"), show);
    store.sync();
}
// 上次连上的设备串号（有线是序列号，无线是 IP:端口）
QString AppSettings::LastSerial() const
{
    return WindowStore().value(QStringLiteral("device/last_serial")).toString();
}

// 记下这次连的设备串号，下次启动优先连它
void AppSettings::SetLastSerial(const QString &serial)
{
    QSettings &store = WindowStore();
    store.setValue(QStringLiteral("device/last_serial"), serial);
    store.sync();
}

// 采集长边上限：0 表示不缩放
int AppSettings::VideoMaxSize() const
{
    return ConfigStore().value(QStringLiteral("video/max_size"), 0).toInt();
}

// 串号里带冒号（IP:端口）就算无线连接，用来选哪一套画质参数
QString AppSettings::VideoProfile(const QString &serial)
{
    return serial.contains(QLatin1Char(':')) ? QStringLiteral("wireless") : QStringLiteral("wired");
}

// 界面里调过的（window.ini 的 [video_<profile>]）优先，其次 config.ini 的同名段，最后兜底
int AppSettings::VideoBitRate(const QString &profile) const
{
    const QString key = QStringLiteral("video_%1/bit_rate").arg(profile.isEmpty() ? QStringLiteral("wired") : profile);
    const int override_value = WindowStore().value(key, -1).toInt();
    if (override_value > 0)
    {
        return override_value;
    }

    // config.ini 里没写 [video_wireless] 时退回到 [video]
    const int configured = ConfigStore().value(key, -1).toInt();
    if (configured > 0)
    {
        return configured;
    }
    return ConfigStore().value(QStringLiteral("video/bit_rate"), 8000000).toInt();
}

// 记下界面里调的码率；-1 表示没调过（回落到 config.ini）
void AppSettings::SetVideoBitRate(int bit_rate, const QString &profile)
{
    QSettings &store = WindowStore();
    store.setValue(QStringLiteral("video_%1/bit_rate").arg(profile.isEmpty() ? QStringLiteral("wired") : profile), bit_rate);
    store.sync();
}

// 采集帧率上限：界面里调过的优先；0 表示不限，120Hz 屏设备能跑到 120fps
int AppSettings::VideoMaxFps(const QString &profile) const
{
    const QString key = QStringLiteral("video_%1/max_fps").arg(profile.isEmpty() ? QStringLiteral("wired") : profile);
    // 用 -1 当"没调过"的哨兵：0 是合法值（不限帧），不能拿 0 当默认
    const int override_value = WindowStore().value(key, -1).toInt();
    if (override_value >= 0)
    {
        return override_value;
    }
    return ConfigStore().value(key, ConfigStore().value(QStringLiteral("video/max_fps"), 0)).toInt();
}

// 记下界面里调的帧率上限
void AppSettings::SetVideoMaxFps(int max_fps, const QString &profile)
{
    QSettings &store = WindowStore();
    store.setValue(QStringLiteral("video_%1/max_fps").arg(profile.isEmpty() ? QStringLiteral("wired") : profile), max_fps);
    store.sync();
}
// 删掉某个姿势的窗口尺寸记录：下次定尺寸时就回到默认值（横屏 900x400、竖屏按帧比例）
void AppSettings::ResetVideoGeometry(const QString &key)
{
    QSettings &store = WindowStore();
    store.remove(key);
    store.sync();
}
// 恢复默认配置：window.ini 清空、config.ini 删掉（下次启动会重新生成带注释的模板）
bool AppSettings::ResetAllConfig()
{
    // 先把 QSettings 里的内容清掉并落盘，否则对象析构时会把旧值写回去
    QSettings &window_store = WindowStore();
    window_store.clear();
    window_store.sync();

    const bool removed = QFile::remove(ConfigPath());
    EnsureDefaultConfig();
    return removed;
}
// 读窗口尺寸；没记录过返回空矩形
QRect AppSettings::VideoGeometry(const QString &key) const
{
    QSettings &store = WindowStore();
    store.beginGroup(key);
    const int x = store.value(QStringLiteral("x"), -1).toInt();
    const int y = store.value(QStringLiteral("y"), -1).toInt();
    const int width = store.value(QStringLiteral("w"), -1).toInt();
    const int height = store.value(QStringLiteral("h"), -1).toInt();
    store.endGroup();

    if (width <= 0 || height <= 0)
    {
        return QRect();
    }
    return QRect(x, y, width, height);
}

// 记窗口尺寸；位置和尺寸一起写，读的时候方便
void AppSettings::SaveVideoGeometry(const QString &key, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    QSettings &store = WindowStore();
    store.beginGroup(key);
    store.setValue(QStringLiteral("x"), x);
    store.setValue(QStringLiteral("y"), y);
    store.setValue(QStringLiteral("w"), width);
    store.setValue(QStringLiteral("h"), height);
    store.endGroup();
    store.sync();
}

// 读窗口位置；没记录过返回 (-1,-1)
QPoint AppSettings::VideoPosition(const QString &key) const
{
    QSettings &store = WindowStore();
    store.beginGroup(key);
    const int x = store.value(QStringLiteral("x"), -1).toInt();
    const int y = store.value(QStringLiteral("y"), -1).toInt();
    store.endGroup();
    return QPoint(x, y);
}

// 只记位置，不碰尺寸
void AppSettings::SaveVideoPosition(const QString &key, int x, int y)
{
    QSettings &store = WindowStore();
    store.beginGroup(key);
    store.setValue(QStringLiteral("x"), x);
    store.setValue(QStringLiteral("y"), y);
    store.endGroup();
    store.sync();
}
