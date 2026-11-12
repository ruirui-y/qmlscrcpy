#include <QDebug>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include "app_settings.h"
#include "base/console_window.h"
#include "base/hotkey.h"
#include "boss_key.h"
#include "base/log.h"
#include "device_controller.h"
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

} // namespace

// 程序入口：配好渲染格式、注册 QML 类型、拉起控制窗，然后进事件循环
int main(int argc, char *argv[])
{
    // 高 DPI 在 Qt5 要手动开（Qt6 才默认），这两行必须在 QGuiApplication 构造之前
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // 视频帧最终走 OpenGL 纹理上传，先把默认格式定死，免得每台机器默认值不一样
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(2, 0);                                                                    // GL 2.0：老核显也能跑的最低线
    format.setProfile(QSurfaceFormat::NoProfile);                                               // 不要求核心模式，兼容面最大
    format.setSwapInterval(0);                                                                  // 关垂直同步：投屏延迟第一刀
    QSurfaceFormat::setDefaultFormat(format);

    QGuiApplication app(argc, argv);

    // 任务栏和 Alt+Tab 用这个图标：exe 自带的那份只在文件管理器里生效
    app.setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    app.setApplicationName(QStringLiteral("QmlScrcpy"));
    app.setApplicationVersion(QStringLiteral(QS_APP_VERSION));

    // Frame 要跨线程用信号槽传，先注册类型；VideoItem 暴露给 QML
    qRegisterMetaType<qsc::stream::Frame>("qsc::stream::Frame");

    // 每秒打一行链路统计（收/推/丢/渲/积压），QS_PERF=0 可以关掉
    qsc::stream::PerfStats::Get().enabled = qgetenv("QS_PERF") != "0";
    qmlRegisterType<VideoItem>("QmlScrcpy", 1, 0, "VideoSurface");

    // 连接/断开这套逻辑放控制器里，界面只负责点按钮和绑状态文字
    DeviceController controller;
    AppSettings app_settings;

    // 日志要先装好：QML 加载失败这类信息只能靠 message handler 拿到
    qsc::base::LogConfig log_config;
    log_config.level = qsc::base::ParseLevel(app_settings.LogLevelText());
    if (app_settings.LogToFile())
    {
        log_config.dir_path = QCoreApplication::applicationDirPath() + QStringLiteral("/Log");
        log_config.keep_days = app_settings.LogKeepDays();
    }
    // 命令行临时调级别，不用改配置文件
    const QByteArray env_level = qgetenv("QS_LOG_LEVEL");
    if (!env_level.isEmpty())
    {
        log_config.level = qsc::base::ParseLevel(QString::fromLocal8Bit(env_level));
    }
    qsc::base::Init(log_config);
    qInfo() << "[app] 配置" << app_settings.ConfigPath() << "窗口状态" << app_settings.WindowPath() << "日志目录" << log_config.dir_path;

    // 控制台窗口按上次的选择决定显示还是隐藏（GUI 子系统下这两个调用是空操作）
    qsc::base::SetConsoleWindowVisible(app_settings.ShowConsole());

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("deviceController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("appSettings"), &app_settings);

    // 加载失败必须立刻报错退出，否则会静默变成一个看不见的空窗口
    // 老板键：全局热键，按一次藏起所有窗口，再按恢复
    qsc::BossKey boss_key;
    qsc::base::HotkeySpec hotkey;
    if (qsc::base::ParseHotkey(app_settings.BossKeyText(), &hotkey) && boss_key.Bind(hotkey))
    {
        QObject::connect(&boss_key, &qsc::BossKey::Triggered, &controller, &DeviceController::ToggleBossKey);
        app.installNativeEventFilter(&boss_key);
        qInfo() << "[app] 老板键已注册" << qsc::base::HotkeyText(hotkey);
    }
    else
    {
        qWarning() << "[app] 老板键注册失败，检查 config.ini 的 ui/boss_key：" << app_settings.BossKeyText();
    }

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

    // 控制窗自己会拉起投屏窗（VideoWindow.qml）
    engine.load(QUrl(QStringLiteral("qrc:/qml/MainWindow.qml")));

    // 进界面先刷一次设备列表，输入框里就有默认值了
    controller.RefreshDevices();

    // 启动后自动连一次：优先上次设备，其次 QS_DEVICE_SERIAL，都得在线；只这一次
    const QString fallback = QString::fromLocal8Bit(qgetenv("QS_DEVICE_SERIAL"));
    const QString auto_serial = controller.PickStartupSerial(fallback);
    if (!auto_serial.isEmpty())
    {
        qInfo() << "[app] 自动连接上次的设备" << auto_serial;
        controller.Connect(auto_serial);
    }

    // 调试用：QS_GRAB=路径 时等画面出来抓一张截图再退出，方便无人值守核对
    const QString grab_path = QString::fromLocal8Bit(qgetenv("QS_GRAB"));
    if (!grab_path.isEmpty())
    {
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

    return app.exec();
}
