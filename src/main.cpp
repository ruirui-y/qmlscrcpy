#include <QDebug>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include "adb/adb_runner.h"
#include "app_settings.h"
#include "base/console_window.h"
#include "base/hotkey.h"
#include "base/task_runner.h"
#include "boss_key.h"
#include "base/log.h"
#include "device_controller.h"
#include "video_controller.h"
#include "update_checker.h"
#include "stream/frame.h"
#include "stream/perf_stats.h"
#include "ui/video_item.h"

namespace
{

// 抓投屏那扇窗（真正持有 VideoSurface 的那扇）截图，调试用
QQuickWindow *FindVideoWindow()
{
    const QList<QWindow *> windows = QGuiApplication::topLevelWindows();
    for (QWindow *window : windows)
    {
        auto *quick_window = qobject_cast<QQuickWindow *>(window);
        if (quick_window == nullptr)
        {
            continue;
        }
        // 控制窗的对象树里也挂着视频窗（QML 里嵌套声明），所以要确认这一项真的属于它
        auto *video_item = quick_window->findChild<VideoItem *>();
        if (video_item != nullptr && video_item->window() == quick_window)
        {
            return quick_window;
        }
    }
    return nullptr;
}

// 高 DPI 和默认 GL 格式，都必须在 QGuiApplication 构造之前定好
void PrepareSurfaceFormat()
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // 视频帧最终走 OpenGL 纹理上传，先把默认格式定死，免得每台机器默认值不一样
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(2, 0);                                                                    // GL 2.0：老核显也能跑的最低线
    format.setProfile(QSurfaceFormat::NoProfile);                                               // 不要求核心模式，兼容面最大
    format.setSwapInterval(0);                                                                  // 关垂直同步：投屏延迟第一刀
    QSurfaceFormat::setDefaultFormat(format);
}

// 任务栏和 Alt+Tab 用的图标与程序元信息
void ConfigureApplication(QGuiApplication &app)
{
    app.setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    app.setApplicationName(QStringLiteral("QmlScrcpy"));
    app.setApplicationVersion(QStringLiteral(QS_APP_VERSION));
}

// 注册跨线程类型与 QML 类型，并把性能统计的开关定下来
void RegisterTypes()
{
    // Frame 要跨线程用信号槽传，先注册类型；VideoItem 暴露给 QML
    qRegisterMetaType<qsc::stream::Frame>("qsc::stream::Frame");
    qmlRegisterType<VideoItem>("QmlScrcpy", 1, 0, "VideoSurface");

    // 每秒打一行链路统计（收/推/丢/渲/积压），QS_PERF=0 可以关掉
    qsc::stream::PerfStats::Get().enabled = qgetenv("QS_PERF") != "0";
}

// 装日志：级别来自 config.ini，QS_LOG_LEVEL 可以临时覆盖；装完才打启动信息
void InitLogging(const AppSettings &settings)
{
    qsc::base::LogConfig log_config;
    log_config.level = qsc::base::ParseLevel(settings.LogLevelText());
    if (settings.LogToFile())
    {
        log_config.dir_path = QCoreApplication::applicationDirPath() + QStringLiteral("/Log");
        log_config.keep_days = settings.LogKeepDays();
    }

    // 命令行临时调级别，不用改配置文件
    const QByteArray env_level = qgetenv("QS_LOG_LEVEL");
    if (!env_level.isEmpty())
    {
        log_config.level = qsc::base::ParseLevel(QString::fromLocal8Bit(env_level));
    }
    qsc::base::Init(log_config);

    // 这两行是排查环境问题的快照：用的哪份 adb、读的哪两个 ini、日志落在哪
    qInfo() << "[adb] 使用" << qsc::adb::AdbPath();
    qInfo() << "[app] 配置" << settings.ConfigPath() << "窗口状态" << settings.WindowPath() << "日志目录"
            << log_config.dir_path;
}

// 把 C++ 对象交给 QML：QML 里直接用 deviceController / appSettings / updateChecker
void ExposeToQml(QQmlApplicationEngine &engine, DeviceController &controller, VideoController &video,
                 AppSettings &settings, qsc::UpdateChecker &update_checker)
{
    engine.rootContext()->setContextProperty(QStringLiteral("deviceController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("videoController"), &video);
    engine.rootContext()->setContextProperty(QStringLiteral("appSettings"), &settings);
    engine.rootContext()->setContextProperty(QStringLiteral("updateChecker"), &update_checker);
}

// QML 加载失败就退出：GUI 子系统没有控制台，不退出会变成一个看不见的空进程
void GuardQmlLoading(QQmlApplicationEngine &engine, QGuiApplication &app)
{
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &app,
        [](QObject *object, const QUrl &objectUrl)
        {
            if (object)
            {
                return;
            }
            qCritical() << "QML 加载失败:" << objectUrl.toString();
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
}

// 注册并绑定老板键；注册失败只告警，不影响其余功能
bool SetupBossKey(qsc::BossKey &boss_key, DeviceController &controller, const AppSettings &settings,
                  QGuiApplication &app)
{
    controller.SetBossKeyHost(&boss_key);

    qsc::base::HotkeySpec hotkey;
    if (!qsc::base::ParseHotkey(settings.BossKeyText(), &hotkey) || !boss_key.Bind(hotkey))
    {
        qWarning() << "[app] 老板键注册失败，检查 config.ini 的 ui/boss_key：" << settings.BossKeyText();
        return false;
    }

    QObject::connect(&boss_key, &qsc::BossKey::SigTriggered, &controller, &DeviceController::ToggleBossKey);
    app.installNativeEventFilter(&boss_key);
    qInfo() << "[app] 老板键已注册" << qsc::base::HotkeyText(hotkey);
    return true;
}

// 启动时自动连一次：优先上次连过的设备，其次 QS_DEVICE_SERIAL；两个都得在线
void AutoConnectOnStartup(DeviceController &controller)
{
    const QString fallback = QString::fromLocal8Bit(qgetenv("QS_DEVICE_SERIAL"));
    const QString auto_serial = controller.PickStartupSerial(fallback);
    if (auto_serial.isEmpty())
    {
        return;
    }

    qInfo() << "[app] 自动连接上次的设备" << auto_serial;
    controller.Connect(auto_serial);
}

// 调试用：QS_GRAB=路径 时等画面出来抓一张截图再退出，方便无人值守核对
void SetupScreenshotOnStartup(QGuiApplication &app)
{
    const QString grab_path = QString::fromLocal8Bit(qgetenv("QS_GRAB"));
    if (grab_path.isEmpty())
    {
        return;
    }

    QTimer::singleShot(6000, &app, [grab_path]()
    {
        QQuickWindow *video_window = FindVideoWindow();
        if (video_window == nullptr)
        {
            qInfo() << "[app] 没找到投屏窗口";
            QCoreApplication::quit();
            return;
        }

        const QImage image = video_window->grabWindow();
        const bool saved = image.save(grab_path);
        qInfo() << "[app] screenshot" << grab_path << image.size() << saved;

        // 顺手把控制窗也存一张，方便核对状态文字和帧率
        for (QWindow *window : QGuiApplication::topLevelWindows())
        {
            auto *quick_window = qobject_cast<QQuickWindow *>(window);
            if (quick_window == nullptr || quick_window == video_window)
            {
                continue;
            }
            const QString control_path = grab_path + QStringLiteral(".control.png");
            const bool control_saved = quick_window->grabWindow().save(control_path);
            qInfo() << "[app] screenshot" << control_path << control_saved;
            break;
        }

        QCoreApplication::quit();
    });
}

// 退出清理：顺序错了 adb daemon 会被客户端重新拉起来，安装包就覆盖不了 adb.exe
void CleanupOnExit(DeviceController &controller)
{
    // 先断开：TearDown 里还要跑 adb 收尾命令（forward --remove 之类）
    controller.Disconnect();

    // 后台线程里可能还有 adb push 之类在跑，先收掉
    qsc::base::ShutdownTaskRunner();

    // 最后让 server 收工：它常驻在安装目录，会锁住 adb.exe
    const qsc::adb::AdbResult kill_result = qsc::adb::Run(QStringList() << QStringLiteral("kill-server"), 3000);
    qInfo() << "[adb] 退出时 kill-server 完成，退出码" << kill_result.exit_code_;

    // 客户端在收尾阶段可能又把 server 拉起来，隔一会儿补一刀
    QThread::msleep(300);
    const qsc::adb::AdbResult final_check = qsc::adb::Run(QStringList() << QStringLiteral("kill-server"), 3000);
    qInfo() << "[adb] 二次确认完成，退出码" << final_check.exit_code_;
}

} // namespace

// 程序入口：配环境 → 建对象 → 交给 QML → 进事件循环 → 退出清理
int main(int argc, char *argv[])
{
    PrepareSurfaceFormat();

    QGuiApplication app(argc, argv);
    ConfigureApplication(app);
    RegisterTypes();

    // 连接/断开这套逻辑放控制器里，界面只负责点按钮和绑状态文字
    DeviceController controller;
    AppSettings app_settings;

    // 日志要先装好：QML 加载失败这类信息只能靠 message handler 拿到
    InitLogging(app_settings);

    // 控制台窗口按上次的选择决定显示还是隐藏（GUI 子系统下这两个调用是空操作）
    qsc::base::SetConsoleWindowVisible(app_settings.ShowConsole());

    // 版本检查：必须声明在 engine 之前——析构顺序是反的，这样它比 QML 引擎活得久，
    // 否则窗口析构时 QML 绑定会读到 null 并刷一堆 TypeError
    qsc::UpdateChecker update_checker(QStringLiteral(QS_APP_VERSION));

    // 视频窗那一侧的行为单独一个对象，主窗只管连接/设备/设置
    VideoController video_controller(&controller);
    controller.AttachVideoController(&video_controller);

    QQmlApplicationEngine engine;
    ExposeToQml(engine, controller, video_controller, app_settings, update_checker);
    GuardQmlLoading(engine, app);

    qsc::BossKey boss_key;
    SetupBossKey(boss_key, controller, app_settings, app);

    // 控制窗自己会拉起投屏窗（VideoWindow.qml）
    engine.load(QUrl(QStringLiteral("qrc:/qml/MainWindow.qml")));

    // 进界面先刷一次设备列表，输入框里就有默认值了
    controller.RefreshDevices();
    AutoConnectOnStartup(controller);
    SetupScreenshotOnStartup(app);

    update_checker.Check();

    const int exit_code = app.exec();

    CleanupOnExit(controller);
    return exit_code;
}
