#include "app_settings.h"

#include <QCoreApplication>
#include <QSettings>

namespace
{

// 配置写在 exe 旁边的 userdata.ini：用户能直接看到、删掉也不影响启动
QSettings &Store()
{
    static QSettings settings(QCoreApplication::applicationDirPath() + QStringLiteral("/userdata.ini"),
                              QSettings::IniFormat);
    return settings;
}

} // namespace

AppSettings::AppSettings(QObject *parent) : QObject(parent) {}

QRect AppSettings::VideoGeometry(const QString &key) const
{
    QSettings &store = Store();
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

void AppSettings::SaveVideoGeometry(const QString &key, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }

    QSettings &store = Store();
    store.beginGroup(key);
    store.setValue(QStringLiteral("x"), x);
    store.setValue(QStringLiteral("y"), y);
    store.setValue(QStringLiteral("w"), width);
    store.setValue(QStringLiteral("h"), height);
    store.endGroup();
    store.sync();
}

QPoint AppSettings::VideoPosition(const QString &key) const
{
    QSettings &store = Store();
    store.beginGroup(key);
    const int x = store.value(QStringLiteral("x"), -1).toInt();
    const int y = store.value(QStringLiteral("y"), -1).toInt();
    store.endGroup();
    return QPoint(x, y);
}

void AppSettings::SaveVideoPosition(const QString &key, int x, int y)
{
    QSettings &store = Store();
    store.beginGroup(key);
    store.setValue(QStringLiteral("x"), x);
    store.setValue(QStringLiteral("y"), y);
    store.endGroup();
    store.sync();
}
