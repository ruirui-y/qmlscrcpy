#include <QDebug>
#include <QGuiApplication>
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
    app.setApplicationName(QStringLiteral("QmlScrcpy"));
    app.setApplicationVersion(QStringLiteral(QS_APP_VERSION));

    // Frame 要跨线程用信号槽传，先注册类型；VideoItem 暴露给 QML
    qRegisterMetaType<qsc::stream::Frame>("qsc::stream::Frame");

    // QS_PERF=1 时每秒打一行链路耗时，用来定位拖窗口卡在哪一段
    qsc::stream::PerfStats::Get().enabled = qEnvironmentVariableIsSet("QS_PERF");
    qmlRegisterType<VideoItem>("QmlScrcpy", 1, 0, "VideoSurface");

    // 连接/断开这套逻辑放控制器里，界面只负责点按钮和绑状态文字
    DeviceController controller;
    AppSettings app_settings;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("deviceController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("appSettings"), &app_settings);

    // 加载失败必须立刻报错退出，否则会静默变成一个看不见的空窗口
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

    // 调试方便：设了 QS_DEVICE_SERIAL 就自动连，省得每次点按钮
    const QString auto_serial = QString::fromLocal8Bit(qgetenv("QS_DEVICE_SERIAL"));
    if (!auto_serial.isEmpty())
    {
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
