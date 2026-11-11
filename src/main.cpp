#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>

#include "device_controller.h"
#include "stream/frame.h"
#include "ui/video_item.h"

int main(int argc, char *argv[])
{
    // 高 DPI 在 Qt5 要手动开（Qt6 才默认），这两行必须在 QGuiApplication 构造之前
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // 视频帧最终走 OpenGL 纹理上传，先把默认格式定死，免得每台机器默认值不一样
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(2, 0);                        // GL 2.0：老核显也能跑的最低线
    format.setProfile(QSurfaceFormat::NoProfile);   // 不要求核心模式，兼容面最大
    format.setSwapInterval(0);                      // 关垂直同步：投屏延迟第一刀
    QSurfaceFormat::setDefaultFormat(format);

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("QmlScrcpy"));
    app.setApplicationVersion(QStringLiteral(QS_APP_VERSION));

    // Frame 要跨线程用信号槽传，先注册类型；VideoItem 暴露给 QML
    qRegisterMetaType<qsc::stream::Frame>("qsc::stream::Frame");
    qmlRegisterType<VideoItem>("QmlScrcpy", 1, 0, "VideoSurface");

    // 连接/断开这套逻辑放控制器里，界面只负责点按钮和绑状态文字
    DeviceController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("deviceController"), &controller);

    // 加载失败必须立刻报错退出，否则会静默变成一个看不见的空窗口
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &app,
        [](QObject *object, const QUrl &objectUrl) {
            if (object) {
                return;
            }
            qCritical() << "QML 加载失败:" << objectUrl.toString();
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);

    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    auto *surface = window != nullptr
                        ? window->findChild<VideoItem *>(QStringLiteral("videoSurface"))
                        : nullptr;
    if (surface == nullptr) {
        qCritical() << "QML 里找不到 objectName 为 videoSurface 的 VideoSurface";
        return -1;
    }
    controller.SetSurface(surface);

    // 进界面先刷一次设备列表，输入框里就有默认值了
    controller.RefreshDevices();

    // 调试方便：设了 QS_DEVICE_SERIAL 就自动连，省得每次点按钮
    const QString auto_serial = QString::fromLocal8Bit(qgetenv("QS_DEVICE_SERIAL"));
    if (!auto_serial.isEmpty()) {
        controller.Connect(auto_serial);
    }

    // 调试用：QS_GRAB=路径 时等画面出来抓一张截图再退出，方便无人值守核对
    const QString grab_path = QString::fromLocal8Bit(qgetenv("QS_GRAB"));
    if (!grab_path.isEmpty()) {
        QTimer::singleShot(3000, &app, [window, grab_path]() {
            const QImage image = window->grabWindow();
            const bool saved = image.save(grab_path);
            qInfo() << "[app] screenshot" << grab_path << image.size() << saved;
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
