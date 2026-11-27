#include "app_settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include "base/task_runner.h"

#include <QSettings>
#include <QRegularExpression>
#include <QThread>

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

// window.ini 的模板：只放"这台机器上的状态"，所有用户设置都在 config.ini
// QSettings 只认分号注释，而且每次 sync 都会重写整个文件，所以这里不放注释
const char *const kDefaultWindow = R"([device]
last_serial=

[control_window]
x=-1
y=-1
w=-1
h=-1

[video_window]
x=-1
y=-1
w=-1
h=-1

[video_pos]
x=-1
y=-1

[video_window_landscape]
x=-1
y=-1
w=-1
h=-1

[video_pos_landscape]
x=-1
y=-1
)";

// 默认配置模板，只在文件不存在时写一次。所有用户设置都在这里
// 注释只能用分号：QSettings 的 INI 格式认 ';' 不认 '#'，写 '#' 会被当成键名，
// 下次 sync 时整行变成 %23%20%U6EDA... 这种垃圾键（见 docs/开发日志 P46）
const char *const kDefaultConfig = R"(; QmlScrcpy 配置，改完重启生效
; 日志级别：debug / info / warning / critical
[log]
level=info
to_file=true
keep_days=15

; 滚轮模式：scroll = 发滚动事件（微信这类原生列表），drag = 模拟手指滑动（阅读器、地图）
[input]
wheel_mode=scroll
; 滑动模式下一格滚轮手指移动多少像素（太小会被当点击，太大一下翻过头）
drag_pixels=60
drag_scale=1.0
click_interval=0
pinch_modifier=ctrl
; 电脑剪贴板一变就自动推到设备（会同步敏感内容，默认关）
clipboard_autosync=false

; 老板键：一键隐藏/恢复所有窗口，格式 Ctrl+Alt+B
; 控制台开关：1 = 显示命令行窗口
[ui]
boss_key=Ctrl+Alt+B
show_console=false

; 窗口快捷键（空值表示禁用这个键）
[keys]
back=Esc
home=Home
screenshot=Ctrl+Shift+S
fullscreen=F11

; 拖文件传到设备上的哪个目录
[transfer]
upload_dir=/sdcard/Download/QmlScrcpy

; 视频采集（有线）：长边上限，0 = 设备原生分辨率（更清晰，码率也要跟着涨）
[video_wired]
max_size=0
bit_rate=8000000
; 帧率上限：0 = 不限。它只是上限，能不能跑到看应用自己：
;   应用锁 60Hz 时设 120 也只有 58 帧；抖音切到 120Hz 后能到 97 帧
max_fps=0

; 无线连接单独一套（串号里带冒号就用这组）：6M 在局域网下够用且不排队
; 码率同时也约束帧率：实测 2M 只出 72 帧、6M 94 帧、12M 114 帧
[video_wireless]
bit_rate=6000000
max_fps=0

; 硬解：1 = d3d12va 硬解，0 = 软解
; 软解线程数：1~8，只在软解时生效
[decode]
hardware=1
threads=4)";

} // namespace

AppSettings::AppSettings(QObject *parent) : QObject(parent)
{
    // 两个 ini 都要在读写之前就位：config.ini 是用户配置，window.ini 是程序状态
    QDir().mkpath(ConfigDirPath());
    EnsureDefaultConfig();
    EnsureDefaultWindow();
    MigrateLegacyKeys();
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

// window.ini 不存在时写一份带注释的模板，和 config.ini 一样
void AppSettings::EnsureDefaultWindow()
{
    if (QFile::exists(WindowPath()))
    {
        return;
    }

    QDir().mkpath(ConfigDirPath());
    QFile file(WindowPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return;
    }
    file.write(kDefaultWindow);
    file.close();
}

// 一次性迁移：老版本把"界面改过的设置"写在 window.ini 里，现在统一放 config.ini。
// 规则：config.ini 里没有这个键才搬（别覆盖用户手改的值），搬完从 window.ini 删掉
void AppSettings::MigrateLegacyKeys()
{
    // 这些前缀属于"设置"，应该待在 config.ini；其余（device、几何）本来就是状态，留在 window.ini
    // decode/ 只删不搬：老 window.ini 里的 hardware 是死键，config.ini 里的那份才是真的
    static const char *const kSettingPrefixes[] = {
        "input/", "keys/", "transfer/", "ui/boss_key", "ui/show_console", "video_wired/", "video_wireless/",
    };
    static const char *const kDroppedPrefixes[] = {"decode/"};

    QSettings &window = WindowStore();
    QSettings &config = ConfigStore();
    bool moved = false;

    // 先把 window.ini 里的设置搬走
    const QStringList window_keys = window.allKeys();
    for (const QString &key : window_keys)
    {
        bool is_setting = false;
        for (const char *prefix : kSettingPrefixes)
        {
            if (key.startsWith(QLatin1String(prefix)))
            {
                is_setting = true;
                break;
            }
        }
        if (!is_setting)
        {
            continue;
        }

        const QVariant value = window.value(key);
        const QString existing = config.value(key).toString();
        if (existing.isEmpty())
        {
            config.setValue(key, value);
        }
        window.remove(key);
        moved = true;
    }

    // 死键直接删掉（config.ini 里那份才是生效的）
    for (const QString &key : window.allKeys())
    {
        for (const char *prefix : kDroppedPrefixes)
        {
            if (key.startsWith(QLatin1String(prefix)))
            {
                window.remove(key);
                moved = true;
                break;
            }
        }
    }

    // 老的 [video] 段改名成 [video_wired]，和 wireless 那套对齐
    const QStringList config_keys = config.allKeys();
    for (const QString &key : config_keys)
    {
        if (!key.startsWith(QLatin1String("video/")))
        {
            continue;
        }
        const QString renamed = QStringLiteral("video_wired/") + key.mid(6);
        if (!config.contains(renamed))
        {
            config.setValue(renamed, config.value(key));
        }
        config.remove(key);
        moved = true;
    }

    // 清垃圾键：0.4.x 之前的模板用 '#' 写注释，QSettings 把它当键名，sync 时转义成
    // %23%20%U6EDA... 这种行（用户打开配置看到一堆乱码）。
    // 真正的键都是 ASCII 的 snake_case，叶子名里出现别的字符就一定是那种垃圾
    for (const QString &key : config.allKeys())
    {
        const QString leaf = key.section(QLatin1Char('/'), -1);
        static const QRegularExpression kPlainKey(QStringLiteral("^[A-Za-z0-9_]+$"));
        if (!kPlainKey.match(leaf).hasMatch())
        {
            config.remove(key);
            moved = true;
        }
    }

    // async_readback 是读回时代的死键，代码已经不读它了，留着只会让人以为还有这个选项
    if (config.contains(QStringLiteral("decode/async_readback")))
    {
        config.remove(QStringLiteral("decode/async_readback"));
        moved = true;
    }

    if (moved)
    {
        config.sync();
        window.sync();
        qInfo() << "[app] 配置已迁移/清理：设置集中到 config.ini，window.ini 只留窗口状态";
    }
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

// 滚轮模式：只在 config.ini，界面改过就直接覆盖默认值
QString AppSettings::WheelMode() const
{
    return ConfigStore().value(QStringLiteral("input/wheel_mode"), QStringLiteral("scroll")).toString();
}

void AppSettings::SetWheelMode(const QString &mode)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("input/wheel_mode"), mode);
    store.sync();
}

// 拖动模式一格滚轮拖多少像素：太小会被当点击，太大一下翻过头
int AppSettings::DragPixels() const
{
    const int pixels = ConfigStore().value(QStringLiteral("input/drag_pixels"), 60).toInt();
    return pixels > 0 ? pixels : 60;
}

// 记下界面里调过的滑动像素
void AppSettings::SetDragPixels(int pixels)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("input/drag_pixels"), pixels);
    store.sync();
}

// 左键拖拽倍率：1.0 = 保持 1:1 手感
double AppSettings::DragScale() const
{
    return ConfigStore().value(QStringLiteral("input/drag_scale"), 1.0).toDouble();
}

// 记下拖拽倍率，改完立即生效
void AppSettings::SetDragScale(double scale)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("input/drag_scale"), scale);
    store.sync();
}

// 捏合修饰键：Ctrl 默认；填 none 关掉这个功能
QString AppSettings::PinchModifier() const
{
    return ConfigStore().value(QStringLiteral("input/pinch_modifier"), QStringLiteral("ctrl")).toString();
}

// 记下捏合修饰键，立即生效
void AppSettings::SetPinchModifier(const QString &modifier)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("input/pinch_modifier"), modifier);
    store.sync();
}

// 电脑剪贴板一变就推到设备（默认关：某些场景会把敏感内容同步过去）
bool AppSettings::ClipboardAutoSync() const
{
    return ConfigStore().value(QStringLiteral("input/clipboard_autosync"), false).toBool();
}

// 版本检查地址：留空表示走内置源列表（Gitee → jsDelivr → GitHub）
QString AppSettings::UpdateUrl() const
{
    return ConfigStore().value(QStringLiteral("update/url")).toString();
}
// 拖文件上传的目标目录：默认 Download/QmlScrcpy
QString AppSettings::UploadDir() const
{
    const QString value = ConfigStore().value(QStringLiteral("transfer/upload_dir")).toString();
    return value.isEmpty() ? QStringLiteral("/sdcard/Download/QmlScrcpy") : value;
}

// 记下界面里改的上传目录
void AppSettings::SetUploadDir(const QString &dir)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("transfer/upload_dir"), dir);
    store.sync();
}

// 窗口快捷键：空字符串表示禁用这个键
QString AppSettings::Shortcut(const QString &name) const
{
    return ConfigStore().value(QStringLiteral("keys/%1").arg(name)).toString();
}

// 记下界面里改的快捷键（空字符串表示禁用）
void AppSettings::SetShortcut(const QString &name, const QString &sequence)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("keys/%1").arg(name), sequence);
    store.sync();
}

// 老板键的组合：只在 config.ini
QString AppSettings::BossKeyText() const
{
    return ConfigStore().value(QStringLiteral("ui/boss_key")).toString();
}

// 记下界面里改的老板键
void AppSettings::SetBossKeyText(const QString &text)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("ui/boss_key"), text);
    store.sync();
}

// 控制台窗口要不要显示：默认不显示，只有用户在界面上打开过才显示
bool AppSettings::ShowConsole() const
{
    return ConfigStore().value(QStringLiteral("ui/show_console")).toBool();
}

// 记下用户的选择，下次启动照这个来
void AppSettings::SetShowConsole(bool show)
{
    QSettings &store = ConfigStore();                                                           // 读在 ConfigStore，写也必须写这里，否则重启就丢
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
    return ConfigStore().value(QStringLiteral("video_wired/max_size"), 0).toInt();
}

// 串号里带冒号（IP:端口）就算无线连接，用来选哪一套画质参数
QString AppSettings::VideoProfile(const QString &serial)
{
    return serial.contains(QLatin1Char(':')) ? QStringLiteral("wireless") : QStringLiteral("wired");
}

// 码率只在 config.ini，界面改过就直接覆盖默认值
int AppSettings::VideoBitRate(const QString &profile) const
{
    const QString key = QStringLiteral("video_%1/bit_rate").arg(profile.isEmpty() ? QStringLiteral("wired") : profile);
    const int configured = ConfigStore().value(key, -1).toInt();
    return configured > 0 ? configured : ConfigStore().value(QStringLiteral("video_wired/bit_rate"), 8000000).toInt();
}

// 记下界面里调的码率
void AppSettings::SetVideoBitRate(int bit_rate, const QString &profile)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("video_%1/bit_rate").arg(profile.isEmpty() ? QStringLiteral("wired") : profile), bit_rate);
    store.sync();
}

// 采集帧率上限：0 表示不限，120Hz 屏设备能跑到 120fps
int AppSettings::VideoMaxFps(const QString &profile) const
{
    const QString key = QStringLiteral("video_%1/max_fps").arg(profile.isEmpty() ? QStringLiteral("wired") : profile);
    // 用 -1 当"没调过"的哨兵：0 是合法值（不限帧），不能拿 0 当默认
    const int configured = ConfigStore().value(key, -1).toInt();
    if (configured >= 0)
    {
        return configured;
    }
    return ConfigStore().value(QStringLiteral("video_wired/max_fps"), 0).toInt();
}

// 记下界面里调的帧率上限
void AppSettings::SetVideoMaxFps(int max_fps, const QString &profile)
{
    QSettings &store = ConfigStore();
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
    // 不调 sync()：本函数在"拖动/缩放停手"后触发，同步落盘会在 GUI 线程卡一下。
    // QSettings 自己会批次写盘，程序正常退出时也会 flush。
}

// 位置和尺寸一起丢到后台线程写盘。
// 停手后主线程直接写 ini 会卡一下（QSettings 的落盘是同步的），所以这里换成独立线程：
// 线程里自己开一个 QSettings，写完 sync 再退出，主线程只负责传值。
void AppSettings::SaveVideoStateAsync(const QString &pos_key, int x, int y,
                                      const QString &geom_key, int width, int height)
{
    const QString path = WindowPath();
    const bool save_geometry = width > 0 && height > 0;

    qsc::base::TaskRunner::Instance().Post([path, pos_key, x, y, geom_key, width, height, save_geometry]()
    {
        // Qt6 的 QSettings 读写 ini 一律按 UTF-8，指定编码的 setIniCodec 已经删掉了
        QSettings store(path, QSettings::IniFormat);

        store.beginGroup(pos_key);
        store.setValue(QStringLiteral("x"), x);
        store.setValue(QStringLiteral("y"), y);
        store.endGroup();

        // 尺寸只在定下来之后才记，否则会写进一堆临时值
        if (save_geometry)
        {
            store.beginGroup(geom_key);
            store.setValue(QStringLiteral("x"), x);
            store.setValue(QStringLiteral("y"), y);
            store.setValue(QStringLiteral("w"), width);
            store.setValue(QStringLiteral("h"), height);
            store.endGroup();
        }

        store.sync();
    });
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
    // 同上：位置保存也在拖动结束时触发，别同步落盘
}

// 是否走 GPU 硬解：1 硬解（默认），0 软解
bool AppSettings::DecodeHardware() const
{
    return ConfigStore().value(QStringLiteral("decode/hardware"), 1).toInt() != 0;
}

void AppSettings::SetDecodeHardware(bool hardware)
{
    ConfigStore().setValue(QStringLiteral("decode/hardware"), hardware ? 1 : 0);
}

// 软解线程数：1~8，默认 4（少了送包会阻塞，多了攒帧变多）
int AppSettings::DecodeThreads() const
{
    const int value = ConfigStore().value(QStringLiteral("decode/threads"), 4).toInt();
    return qBound(1, value, 8);
}

void AppSettings::SetDecodeThreads(int threads)
{
    ConfigStore().setValue(QStringLiteral("decode/threads"), qBound(1, threads, 8));
}



// 连点间隔：0 表示关闭，按住空格时按这个间隔在鼠标位置连点
int AppSettings::ClickerInterval() const
{
    return ConfigStore().value(QStringLiteral("input/click_interval"), 0).toInt();
}

void AppSettings::SetClickerInterval(int ms)
{
    QSettings &store = ConfigStore();
    store.setValue(QStringLiteral("input/click_interval"), ms);
    store.sync();
}
