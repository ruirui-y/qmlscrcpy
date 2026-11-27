#include "app_settings.h"

#include "settings_schema.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include "base/task_runner.h"

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

// 用户配置：程序只在用户改设置时写它，平时只读
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

// 按 key 找一条 schema；拼错键名时返回空，调用方打一条警告
const qsc::app::SettingEntry *FindEntry(const QString &key)
{
    for (const qsc::app::SettingEntry &entry : qsc::app::kSettings)
    {
        if (key == QLatin1String(entry.key))
        {
            return &entry;
        }
    }
    return nullptr;
}

// schema 里的默认值统一是字符串，读的时候按类型解析
QVariant ParseFallback(const qsc::app::SettingEntry &entry)
{
    const QString text = QString::fromLatin1(entry.fallback);
    switch (entry.kind)
    {
    case qsc::app::SettingKind::Int:
        return text.toInt();
    case qsc::app::SettingKind::Double:
        return text.toDouble();
    case qsc::app::SettingKind::Bool:
        return text == QLatin1String("true");
    case qsc::app::SettingKind::Text:
    default:
        return text;
    }
}

// 把值序列化成 ini 里写的文本；bool 统一写 true/false，别写成 1/0
QString FormatValue(const qsc::app::SettingEntry &entry, const QVariant &value)
{
    switch (entry.kind)
    {
    case qsc::app::SettingKind::Bool:
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case qsc::app::SettingKind::Int:
        return QString::number(value.toInt());
    case qsc::app::SettingKind::Double:
        return QString::number(value.toDouble(), 'g', 6);
    case qsc::app::SettingKind::Text:
    default:
        return value.toString();
    }
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

} // namespace

AppSettings::AppSettings(QObject *parent) : QObject(parent)
{
    // 两个 ini 都要在读写之前就位：config.ini 是用户配置，window.ini 是程序状态
    QDir().mkpath(ConfigDirPath());
    EnsureDefaultWindow();
    MigrateLegacyKeys();
    RenderConfigFile();
}

// 首次运行写一份带注释的 config.ini。内容由 RenderConfigFile 生成，
// 这里只在文件不存在时兜底，避免构造函数里两条路径写同一个文件
void AppSettings::EnsureDefaultConfig()
{
    RenderConfigFile();
}

// window.ini 不存在时写一份模板；存在的就别动
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

// 按 schema 重写整份 config.ini：分节、注释、默认值全部来自 settings_schema.h，
// 值取当前文件里的（没有就用默认值）。表里没登记的键原样保留在原来的段里
void AppSettings::RenderConfigFile()
{
    QSettings &config = ConfigStore();
    config.sync();                                                                              // 先把内存里的改动落盘，免得下面重写时丢掉

    QStringList lines;
    QByteArray section;
    bool first_section = true;
    for (const qsc::app::SettingEntry &entry : qsc::app::kSettings)
    {
        if (entry.in_window)
        {
            continue;                                                                           // window.ini 的条目不进 config.ini
        }
        if (section != QByteArray(entry.section))
        {
            section = entry.section;
            if (!first_section)
            {
                lines << QString();
            }
            first_section = false;
            lines << QStringLiteral("[%1]").arg(QString::fromLatin1(entry.section));
        }
        if (entry.comment[0] != '\0')
        {
            lines << QStringLiteral("; %1").arg(QString::fromUtf8(entry.comment));
        }

        const QString key = QString::fromLatin1(entry.key);
        const QString leaf = key.section(QLatin1Char('/'), -1);
        const QVariant raw = config.value(key);
        const QVariant value = raw.isValid() ? raw : ParseFallback(entry);
        lines << QStringLiteral("%1=%2").arg(leaf, FormatValue(entry, value));
    }

    // 表里没有的键：按原段名原样附在最后，不丢用户手加的条目
    QMap<QString, QStringList> unknown;
    for (const QString &key : config.allKeys())
    {
        if (FindEntry(key) != nullptr)
        {
            continue;
        }
        const int slash = key.indexOf(QLatin1Char('/'));
        const QString group = slash > 0 ? key.left(slash) : QStringLiteral("misc");
        const QString leaf = slash > 0 ? key.mid(slash + 1) : key;
        unknown[group] << QStringLiteral("%1=%2").arg(leaf, config.value(key).toString());
    }
    for (auto it = unknown.cbegin(); it != unknown.cend(); ++it)
    {
        lines << QString();
        lines << QStringLiteral("; 下面这个段不在配置表里，原样保留")
              << QStringLiteral("[%1]").arg(it.key());
        lines << it.value();
    }

    QFile file(ConfigPath());
    // 不能用 QIODevice::Text：它会把我们写的 \r\n 再翻一次，变成 \r\r\n（文件里每行之间多一个空行）
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        qWarning() << "[app] 写不了配置文件" << ConfigPath();
        return;
    }
    file.write(lines.join(QStringLiteral("\r\n")).toUtf8());
    file.write("\r\n");
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

QString AppSettings::GetText(const char *key) const
{
    const qsc::app::SettingEntry *entry = FindEntry(QString::fromLatin1(key));
    if (entry == nullptr)
    {
        qWarning() << "[settings] 配置表里没有这个键" << key;
        return QString();
    }
    QSettings &store = entry->in_window ? WindowStore() : ConfigStore();
    const QVariant raw = store.value(QString::fromLatin1(entry->key));
    return raw.isValid() ? raw.toString() : ParseFallback(*entry).toString();
}

int AppSettings::GetInt(const char *key) const
{
    const qsc::app::SettingEntry *entry = FindEntry(QString::fromLatin1(key));
    if (entry == nullptr)
    {
        qWarning() << "[settings] 配置表里没有这个键" << key;
        return 0;
    }
    QSettings &store = entry->in_window ? WindowStore() : ConfigStore();
    bool ok = true;
    const QVariant raw = store.value(QString::fromLatin1(entry->key));
    int value = raw.isValid() ? raw.toInt(&ok) : ParseFallback(*entry).toInt();
    if (!ok)
    {
        // 用户手改成了 abc 这种：报一条明确警告，回落到默认值，别静默给 0
        qWarning() << "[settings]" << entry->key << "不是整数（当前" << raw.toString() << "），按默认值"
                   << entry->fallback << "处理";
        value = ParseFallback(*entry).toInt();
    }
    if (entry->min_value != qsc::app::kNoLimit || entry->max_value != qsc::app::kNoLimit)
    {
        const int clamped = qBound(entry->min_value, value, entry->max_value);
        if (clamped != value)
        {
            qWarning() << "[settings]" << entry->key << "超出范围" << entry->min_value << "~" << entry->max_value
                       << "，按" << clamped << "处理";
            value = clamped;
        }
    }
    return value;
}

double AppSettings::GetDouble(const char *key) const
{
    const qsc::app::SettingEntry *entry = FindEntry(QString::fromLatin1(key));
    if (entry == nullptr)
    {
        qWarning() << "[settings] 配置表里没有这个键" << key;
        return 0.0;
    }
    QSettings &store = entry->in_window ? WindowStore() : ConfigStore();
    bool ok = true;
    const QVariant raw = store.value(QString::fromLatin1(entry->key));
    double value = raw.isValid() ? raw.toDouble(&ok) : ParseFallback(*entry).toDouble();
    if (!ok)
    {
        qWarning() << "[settings]" << entry->key << "不是小数（当前" << raw.toString() << "），按默认值"
                   << entry->fallback << "处理";
        value = ParseFallback(*entry).toDouble();
    }
    return value;
}

bool AppSettings::GetBool(const char *key) const
{
    const qsc::app::SettingEntry *entry = FindEntry(QString::fromLatin1(key));
    if (entry == nullptr)
    {
        qWarning() << "[settings] 配置表里没有这个键" << key;
        return false;
    }
    QSettings &store = entry->in_window ? WindowStore() : ConfigStore();
    const QVariant raw = store.value(QString::fromLatin1(entry->key));
    if (!raw.isValid())
    {
        return ParseFallback(*entry).toBool();
    }
    // QVariant::toBool 会把任何非零/非空当 true，这里只认明确的几种写法
    const QString text = raw.toString().trimmed().toLower();
    if (text == QLatin1String("true") || text == QLatin1String("1"))
    {
        return true;
    }
    if (text == QLatin1String("false") || text == QLatin1String("0"))
    {
        return false;
    }
    qWarning() << "[settings]" << entry->key << "不是 true/false（当前" << raw.toString() << "），按默认值"
               << entry->fallback << "处理";
    return ParseFallback(*entry).toBool();
}

void AppSettings::SetValue(const char *key, const QVariant &value)
{
    const qsc::app::SettingEntry *entry = FindEntry(QString::fromLatin1(key));
    if (entry == nullptr)
    {
        qWarning() << "[settings] 配置表里没有这个键" << key;
        return;
    }

    QVariant stored = value;
    const bool has_range = entry->min_value != qsc::app::kNoLimit || entry->max_value != qsc::app::kNoLimit;
    if (entry->kind == qsc::app::SettingKind::Int && has_range)
    {
        stored = qBound(entry->min_value, value.toInt(), entry->max_value);
    }

    QSettings &store = entry->in_window ? WindowStore() : ConfigStore();
    store.setValue(QString::fromLatin1(entry->key), stored);
    store.sync();
}

// 日志级别：配置里写字符串，解析交给调用方
QString AppSettings::LogLevelText() const
{
    return GetText("log/level");
}

bool AppSettings::LogToFile() const
{
    return GetBool("log/to_file");
}

int AppSettings::LogKeepDays() const
{
    return GetInt("log/keep_days");
}

QString AppSettings::WheelMode() const
{
    return GetText("input/wheel_mode");
}

void AppSettings::SetWheelMode(const QString &mode)
{
    SetValue("input/wheel_mode", mode);
}

// 拖动模式一格滚轮拖多少像素：太小会被当点击，太大一下翻过头
int AppSettings::DragPixels() const
{
    return GetInt("input/drag_pixels");
}

void AppSettings::SetDragPixels(int pixels)
{
    SetValue("input/drag_pixels", pixels);
}

// 左键拖拽倍率：1.0 = 保持 1:1 手感
double AppSettings::DragScale() const
{
    return GetDouble("input/drag_scale");
}

void AppSettings::SetDragScale(double scale)
{
    SetValue("input/drag_scale", scale);
}

// 捏合修饰键：Ctrl 默认；填 none 关掉这个功能
QString AppSettings::PinchModifier() const
{
    return GetText("input/pinch_modifier");
}

void AppSettings::SetPinchModifier(const QString &modifier)
{
    SetValue("input/pinch_modifier", modifier);
}

// 电脑剪贴板一变就推到设备（默认关：某些场景会把敏感内容同步过去）
bool AppSettings::ClipboardAutoSync() const
{
    return GetBool("input/clipboard_autosync");
}

// 版本检查地址：留空表示走内置源列表（Gitee → jsDelivr → GitHub）
QString AppSettings::UpdateUrl() const
{
    return GetText("update/url");
}

// 拖文件上传的目标目录：默认 Download/QmlScrcpy
QString AppSettings::UploadDir() const
{
    const QString dir = GetText("transfer/upload_dir");
    return dir.isEmpty() ? QStringLiteral("/sdcard/Download/QmlScrcpy") : dir;
}

void AppSettings::SetUploadDir(const QString &dir)
{
    SetValue("transfer/upload_dir", dir);
}

// 窗口快捷键：name 取 back / home / screenshot / fullscreen
QString AppSettings::Shortcut(const QString &name) const
{
    const QByteArray key = QStringLiteral("keys/%1").arg(name).toLatin1();
    return GetText(key.constData());
}

void AppSettings::SetShortcut(const QString &name, const QString &sequence)
{
    const QByteArray key = QStringLiteral("keys/%1").arg(name).toLatin1();
    SetValue(key.constData(), sequence);
}

// 老板键的组合：只在 config.ini
QString AppSettings::BossKeyText() const
{
    return GetText("ui/boss_key");
}

void AppSettings::SetBossKeyText(const QString &text)
{
    SetValue("ui/boss_key", text);
}

// 控制台窗口要不要显示：默认不显示，只有用户在界面上打开过才显示
bool AppSettings::ShowConsole() const
{
    return GetBool("ui/show_console");
}

void AppSettings::SetShowConsole(bool show)
{
    SetValue("ui/show_console", show);
}

// 上次连上的设备串号（有线是序列号，无线是 IP:端口）
QString AppSettings::LastSerial() const
{
    return GetText("device/last_serial");
}

void AppSettings::SetLastSerial(const QString &serial)
{
    SetValue("device/last_serial", serial);
}

// 串号里带冒号（IP:端口）就算无线，两套画质参数分开存
QString AppSettings::VideoProfile(const QString &serial)
{
    return serial.contains(QLatin1Char(':')) ? QStringLiteral("wireless") : QStringLiteral("wired");
}

int AppSettings::VideoBitRate(const QString &profile) const
{
    const QByteArray key = QStringLiteral("video_%1/bit_rate")
                               .arg(profile.isEmpty() ? QStringLiteral("wired") : profile)
                               .toLatin1();
    return GetInt(key.constData());
}

void AppSettings::SetVideoBitRate(int bit_rate, const QString &profile)
{
    const QByteArray key = QStringLiteral("video_%1/bit_rate")
                               .arg(profile.isEmpty() ? QStringLiteral("wired") : profile)
                               .toLatin1();
    SetValue(key.constData(), bit_rate);
}

int AppSettings::VideoMaxFps(const QString &profile) const
{
    const QByteArray key = QStringLiteral("video_%1/max_fps")
                               .arg(profile.isEmpty() ? QStringLiteral("wired") : profile)
                               .toLatin1();
    return GetInt(key.constData());
}

void AppSettings::SetVideoMaxFps(int max_fps, const QString &profile)
{
    const QByteArray key = QStringLiteral("video_%1/max_fps")
                               .arg(profile.isEmpty() ? QStringLiteral("wired") : profile)
                               .toLatin1();
    SetValue(key.constData(), max_fps);
}

// 长边上限，0 = 设备原生分辨率
int AppSettings::VideoMaxSize() const
{
    return GetInt("video_wired/max_size");
}

// 是否走 GPU 硬解
bool AppSettings::DecodeHardware() const
{
    return GetBool("decode/hardware");
}

void AppSettings::SetDecodeHardware(bool hardware)
{
    SetValue("decode/hardware", hardware);
}

// 软解线程数：1~8，范围钳制在 schema 里
int AppSettings::DecodeThreads() const
{
    return GetInt("decode/threads");
}

void AppSettings::SetDecodeThreads(int threads)
{
    SetValue("decode/threads", threads);
}

// 连点间隔：0 表示关闭，按住空格时按这个间隔在鼠标位置连点
int AppSettings::ClickerInterval() const
{
    return GetInt("input/click_interval");
}

void AppSettings::SetClickerInterval(int ms)
{
    SetValue("input/click_interval", ms);
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

// 删掉某个姿势的窗口尺寸记录：下次定尺寸时就回到默认值
void AppSettings::ResetVideoGeometry(const QString &key)
{
    QSettings &store = WindowStore();
    store.remove(key);
    store.sync();
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

// 恢复默认配置：window.ini 清空、config.ini 重写成模板
bool AppSettings::ResetAllConfig()
{
    // 先把 QSettings 里的内容清掉并落盘，否则对象析构时会把旧值写回去
    QSettings &window_store = WindowStore();
    window_store.clear();
    window_store.sync();

    QSettings &config = ConfigStore();
    config.clear();
    config.sync();

    RenderConfigFile();
    return true;
}
