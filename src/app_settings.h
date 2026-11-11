#pragma once

#include <QObject>
#include <QRect>
#include <QString>

class AppSettings : public QObject
{
    Q_OBJECT

public:
    explicit AppSettings(QObject *parent = nullptr);

    Q_INVOKABLE QRect VideoGeometry(const QString &key) const;                                  // 没记录过返回空矩形
    Q_INVOKABLE void SaveVideoGeometry(const QString &key, int x, int y, int width, int height);
};
