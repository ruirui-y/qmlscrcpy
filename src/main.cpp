#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QSurfaceFormat>
#include <QUrl>

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

    // 用 Controls 2 的默认样式。先不指定 Fusion：样式插件是单独的 qml 子目录，
    // 部署时多一个变量，P0 不引入。
    QQmlApplicationEngine engine;

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
    return app.exec();
}
