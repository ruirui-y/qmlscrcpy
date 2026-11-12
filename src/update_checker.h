#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

namespace qsc
{

// 启动时查一次最新版本：取远端那个静态 latest.json，比对后给出三种状态。
// 不阻塞启动、失败静默——断网或 CDN 抽风都不能影响正常使用。
class UpdateChecker : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ CurrentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ LatestVersion NOTIFY StateChanged)
    Q_PROPERTY(QString downloadUrl READ DownloadUrl NOTIFY StateChanged)
    Q_PROPERTY(QString notes READ Notes NOTIFY StateChanged)
    Q_PROPERTY(bool updateAvailable READ UpdateAvailable NOTIFY StateChanged)                  // 有新版本，软提示
    Q_PROPERTY(bool updateRequired READ UpdateRequired NOTIFY StateChanged)                    // 低于最低支持版本，硬拦

public:
    explicit UpdateChecker(const QString &current_version, QObject *parent = nullptr);

    QString CurrentVersion() const;
    QString LatestVersion() const;
    QString DownloadUrl() const;
    QString Notes() const;
    bool UpdateAvailable() const;
    bool UpdateRequired() const;

    Q_INVOKABLE void Check();                                                                   // 启动时调一次
    Q_INVOKABLE void OpenDownloadPage();                                                        // 用默认浏览器打开下载页

signals:
    void StateChanged();

private:
    void AttemptNextSource();                                                                    // 按源列表依次试
    void AttemptWithCurl(const QString &url);                                                    // 没 OpenSSL 时的兜底
    bool ApplyPayload(const QByteArray &body);                                                   // 成功返回 true

    QString current_version_;                                                                   // 编译期写入的版本，例如 0.1.4
    QString latest_version_;
    QString download_url_;
    QString notes_;
    bool update_available_ = false;
    bool update_required_ = false;
    bool checking_ = false;                                                                     // 正在检查，防止重入
    int source_index_ = 0;                                                                       // 当前试到第几个源
    QStringList sources_;                                                                        // 依次尝试的 latest.json 地址                                                                     // 防止重复发起
    QNetworkAccessManager *network_ = nullptr;
};

} // namespace qsc
