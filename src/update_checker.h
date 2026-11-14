#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTimer;

namespace qsc
{

// 启动时查一次最新版本：取远端那个静态 latest.json，比对后给出三种状态。
// 也负责"一键更新"：下载安装包 → 校验 → 静默启动安装程序 → 自己退出。
// 检查失败一律静默，不影响正常使用。
class UpdateChecker : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ CurrentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ LatestVersion NOTIFY SigStateChanged)
    Q_PROPERTY(QString downloadUrl READ DownloadUrl NOTIFY SigStateChanged)
    Q_PROPERTY(QString notes READ Notes NOTIFY SigStateChanged)
    Q_PROPERTY(bool updateAvailable READ UpdateAvailable NOTIFY SigStateChanged)                // 有新版本，软提示
    Q_PROPERTY(bool updateRequired READ UpdateRequired NOTIFY SigStateChanged)                  // 低于最低支持版本，硬拦

    Q_PROPERTY(bool downloading READ Downloading NOTIFY SigDownloadChanged)                     // 正在下载安装包
    Q_PROPERTY(qreal downloadProgress READ DownloadProgress NOTIFY SigDownloadChanged)          // 0~1
    Q_PROPERTY(QString downloadStatus READ DownloadStatus NOTIFY SigDownloadChanged)            // 给人看的一句话
    Q_PROPERTY(bool canAutoUpdate READ CanAutoUpdate NOTIFY SigStateChanged)                    // latest.json 里有 sha256 才能自动更

public:
    explicit UpdateChecker(const QString &current_version, QObject *parent = nullptr);

    QString CurrentVersion() const;
    QString LatestVersion() const;
    QString DownloadUrl() const;
    QString Notes() const;
    bool UpdateAvailable() const;
    bool UpdateRequired() const;

    bool Downloading() const;
    qreal DownloadProgress() const;
    QString DownloadStatus() const;
    bool CanAutoUpdate() const;

    Q_INVOKABLE void Check();                                                                   // 启动时调一次
    Q_INVOKABLE void OpenDownloadPage();                                                        // 用默认浏览器打开下载页
    Q_INVOKABLE void StartDownload();                                                           // 后台下载安装包（可取消）
    Q_INVOKABLE void CancelDownload();                                                          // 取消下载并删掉半截文件
    Q_INVOKABLE void InstallAndRestart();                                                       // 静默启动安装包，然后自己退出

signals:
    void SigStateChanged();
    void SigDownloadChanged();

private:
    void AttemptNextSource();                                                                   // 按源列表依次试
    void AttemptWithCurl(const QString &url);                                                   // 没 OpenSSL 时的兜底
    bool ApplyPayload(const QByteArray &body);                                                  // 成功返回 true
    void OnDownloadFinished(int exit_code);

    QString current_version_;                                                                   // 编译期写入的版本，例如 0.1.5
    QString latest_version_;
    QString download_url_;                                                                      // 下载页（浏览器用）
    QString installer_url_;                                                                     // exe 直链（curl 用），缺了就不能自动更新
    QString notes_;
    QString download_sha256_;                                                                   // latest.json 里给的安装包指纹，空表示不支持自动更新
    qint64 download_size_ = 0;                                                                  // 安装包字节数，用于进度和校验
    qint64 reported_bytes_ = 0;                                                                 // 上次通知界面时的已下载字节数，没变就不发信号
    bool update_available_ = false;
    bool update_required_ = false;
    bool checking_ = false;                                                                     // 正在检查，防止重入
    int source_index_ = 0;                                                                      // 当前试到第几个源
    QStringList sources_;                                                                       // 依次尝试的 latest.json 地址

    bool downloading_ = false;                                                                  // 正在下载
    bool installable_ = false;                                                                  // 下载并校验通过，可以点安装
    QString download_status_;                                                                   // 状态栏文字
    QString local_setup_path_;                                                                  // 下载到哪个临时文件
    QProcess *download_process_ = nullptr;
    QTimer *progress_timer_ = nullptr;
    QNetworkAccessManager *network_ = nullptr;
};

} // namespace qsc
