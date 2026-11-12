#include "app_settings.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>

namespace
{

// 两个文件都在 exe 旁边：绿色软件，拷走整目录就能用
QString FilePathInExeDir(const char *file_name)
{
    return QCoreApplication::applicationDirPath() + QLatin1Char('/') + QString::fromLatin1(file_name);
}

// 用户配置：只读，绝不让 QSettings 改写，否则注释会被转义成 %23 之类
QSettings &ConfigStore()
{
    static QSettings settings(FilePathInExeDir("config.ini"), QSettings::IniFormat);
    return settings;
}

// 程序自己的状态：读写都在这，随便重写
QSettings &WindowStore()
{
    static QSettings settings(FilePathInExeDir("window.ini"), QSettings::IniFormat);
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

# 视频采集：长边上限，0 = 设备原生分辨率（更清晰，码率也要跟着涨）
[video]
max_size=0
bit_rate=12000000
)";

} // namespace

AppSettings::AppSettings(QObject *parent) : QObject(parent)
{
    EnsureDefaultConfig();
}

// config.ini 不存在时写一份带注释的模板
void AppSettings::EnsureDefaultConfig()
{
    // 已经存在就不动，用户改过的值不能被覆盖
    if (QFile::exists(ConfigPath()))
    {
        return;
    }

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
    return FilePathInExeDir("config.ini");
}

QString AppSettings::WindowPath() const
{
    return FilePathInExeDir("window.ini");
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

int AppSettings::VideoBitRate() const
{
    return ConfigStore().value(QStringLiteral("video/bit_rate"), 12000000).toInt();
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
