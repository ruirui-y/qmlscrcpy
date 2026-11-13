#include "update_checker.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QClipboard>
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSslSocket>
#include <QUrl>

#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

#include "app_settings.h"
#include "base/log.h"
#include "base/task_runner.h"
#include "update/version_compare.h"

namespace qsc
{

namespace
{

// 国内先试 Gitee（GitHub 直连经常不通），再试 CDN，最后才是 GitHub 原文。
// 三个都失败就当"本次没查到"，不影响使用。
QStringList LatestJsonSources()
{
    QStringList sources;

    // config.ini 里配了 update/url 就只用它，方便自己换源
    const QString configured = AppSettings().UpdateUrl();
    if (!configured.isEmpty())
    {
        sources.append(configured);
        return sources;
    }

    // 带一个按小时变化的参数：Gitee raw 有自己的缓存，按天会让新版本等一整天
    const QString cache_buster =
        QStringLiteral("?v=%1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHH")));

    sources.append(QStringLiteral("https://gitee.com/bai-maxifeng/qmlscrcpy/raw/master/latest.json") + cache_buster);
    sources.append(QStringLiteral("https://cdn.jsdelivr.net/gh/ruirui-y/qmlscrcpy@master/latest.json") + cache_buster);
    sources.append(QStringLiteral("https://raw.githubusercontent.com/ruirui-y/qmlscrcpy/master/latest.json")
                   + cache_buster);
    return sources;
}

} // namespace

UpdateChecker::UpdateChecker(const QString &current_version, QObject *parent)
    : QObject(parent)
    , current_version_(current_version)
    , sources_(LatestJsonSources())
{
}

QString UpdateChecker::CurrentVersion() const
{
    return current_version_;
}

QString UpdateChecker::LatestVersion() const
{
    return latest_version_;
}

QString UpdateChecker::DownloadUrl() const
{
    return download_url_;
}

QString UpdateChecker::Notes() const
{
    return notes_;
}

bool UpdateChecker::UpdateAvailable() const
{
    return update_available_;
}

bool UpdateChecker::UpdateRequired() const
{
    return update_required_;
}

// 发起一次检查：按源列表依次试，任意一个成功就停
void UpdateChecker::Check()
{
    if (checking_ || sources_.isEmpty())
    {
        return;
    }
    checking_ = true;
    source_index_ = 0;
    AttemptNextSource();
}

void UpdateChecker::AttemptNextSource()
{
    if (source_index_ >= sources_.size())
    {
        checking_ = false;
        qCInfo(base::lcApp) << "[update] 所有源都不可用（不影响使用）";
        return;
    }

    const QString url = sources_.at(source_index_);
    source_index_ += 1;

    // Qt 5.15 在 Windows 上走 OpenSSL，没装 OpenSSL 的机器一律 TLS 失败，
    // 这时退到系统自带的 curl.exe（Win10 1803+，用 SChannel），效果一样。
    if (!QSslSocket::supportsSsl())
    {
        AttemptWithCurl(url);
        return;
    }

    if (network_ == nullptr)
    {
        network_ = new QNetworkAccessManager(this);
    }

    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(5000);                                                           // 5 秒没回来就当这个源失败

    QNetworkReply *reply = network_->get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            qCInfo(base::lcApp) << "[update] 这个源失败：" << reply->url().toString() << reply->errorString();
            AttemptNextSource();
            return;
        }

        if (!ApplyPayload(reply->readAll()))
        {
            AttemptNextSource();
        }
    });
}

// 用系统 curl 拉 JSON：解析逻辑和网络路径完全一样，只是换了传输手段
void UpdateChecker::AttemptWithCurl(const QString &url)
{
    auto *process = new QProcess(this);
    process->setProgram(QStringLiteral("curl.exe"));
    process->setArguments(QStringList() << QStringLiteral("-s") << QStringLiteral("--max-time")
                                       << QStringLiteral("6") << QStringLiteral("-L") << url);

    QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                     [this, process](int exit_code, QProcess::ExitStatus status)
    {
        const QByteArray body = process->readAllStandardOutput();
        process->deleteLater();

        if (status != QProcess::NormalExit || exit_code != 0)
        {
            qCInfo(base::lcApp) << "[update] curl 这个源失败，退出码" << exit_code;
            AttemptNextSource();
            return;
        }

        if (!ApplyPayload(body))
        {
            AttemptNextSource();
        }
    });
    process->start();
}

// 解析远端 JSON：字段缺失或格式不对就返回 false，让调用方换下一个源
bool UpdateChecker::ApplyPayload(const QByteArray &body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
    {
        qCInfo(base::lcApp) << "[update] 拿到的不是 JSON 对象，换下一个源";
        return false;
    }

    const QJsonObject object = document.object();
    const QString latest_version = object.value(QStringLiteral("latest")).toString();
    if (latest_version.isEmpty())
    {
        qCInfo(base::lcApp) << "[update] JSON 里没有 latest 字段，换下一个源";
        return false;
    }

    latest_version_ = latest_version;
    download_url_ = object.value(QStringLiteral("url")).toString();
    notes_ = object.value(QStringLiteral("notes")).toString();
    installer_url_ = object.value(QStringLiteral("exe")).toString();
    download_sha256_ = object.value(QStringLiteral("sha256")).toString().toLower();
    download_size_ = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());

    // 本地版本 < minimum：必须更新；< latest：提示更新
    const QString minimum_version = object.value(QStringLiteral("minimum")).toString();
    update_required_ = !minimum_version.isEmpty()
        && update::Compare(current_version_, minimum_version) < 0;
    update_available_ = update::Compare(current_version_, latest_version_) < 0;

    checking_ = false;
    qCInfo(base::lcApp) << "[update] 本地" << current_version_ << "远端" << latest_version_
                        << "提示更新" << update_available_ << "强制" << update_required_;

    emit SigStateChanged();
    return true;
}

// 打开浏览器去下载（链接来自 latest.json，服务端想换就换）
void UpdateChecker::OpenDownloadPage()
{
    if (download_url_.isEmpty())
    {
        qCInfo(base::lcApp) << "[update] 没有下载页地址，忽略";
        download_status_ = QStringLiteral("没有下载页地址");
        emit SigDownloadChanged();
        return;
    }

    qCInfo(base::lcApp) << "[update] 打开下载页" << download_url_;

    // 先复制到剪贴板：不管浏览器能不能起来，用户至少能直接粘贴
    QGuiApplication::clipboard()->setText(download_url_);

    // 用 Win32 的 ShellExecuteW 而不是 QDesktopServices：后者在提权进程、
    // 默认浏览器关联异常时会静默返回 false，调用方完全看不出发生了什么
    const std::wstring wide_url = download_url_.toStdWString();
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const qintptr code = reinterpret_cast<qintptr>(opened);
    qCInfo(base::lcApp) << "[update] ShellExecuteW 返回" << code;

    // ShellExecuteW 约定：返回值 > 32 才算成功
    if (code > 32)
    {
        download_status_ = QStringLiteral("已用系统默认浏览器打开（链接也复制到剪贴板了）");
        emit SigDownloadChanged();
        return;
    }

    qCInfo(base::lcApp) << "[update] ShellExecuteW 失败，改用 cmd start";
    const bool started = QProcess::startDetached(QStringLiteral("cmd.exe"),
                                                QStringList() << QStringLiteral("/c") << QStringLiteral("start")
                                                              << QString() << download_url_);
    if (!started)
    {
        download_status_ = QStringLiteral("打不开浏览器，链接已复制：%1").arg(download_url_);
        qCInfo(base::lcApp) << "[update] 系统命令也失败";
        emit SigDownloadChanged();
    }
}


bool UpdateChecker::Downloading() const
{
    return downloading_;
}

qreal UpdateChecker::DownloadProgress() const
{
    if (download_size_ <= 0)
    {
        return 0.0;
    }
    const qint64 received = local_setup_path_.isEmpty() ? 0 : QFileInfo(local_setup_path_).size();
    return qBound(0.0, static_cast<qreal>(received) / static_cast<qreal>(download_size_), 1.0);
}

QString UpdateChecker::DownloadStatus() const
{
    return download_status_;
}

// 只有远端给了 sha256 才允许"下载后自动安装"，否则只能开浏览器
bool UpdateChecker::CanAutoUpdate() const
{
    return !installer_url_.isEmpty() && !download_sha256_.isEmpty() && download_sha256_.size() == 64
        && update_available_;
}

// 后台拉安装包：用系统 curl，进度靠轮询文件大小算
void UpdateChecker::StartDownload()
{
    if (downloading_ || !CanAutoUpdate())
    {
        return;
    }

    // 安装包放临时目录，避免污染程序目录（程序目录可能没有写权限）
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/QmlScrcpy");
    QDir().mkpath(dir);
    local_setup_path_ = dir + QStringLiteral("/QmlScrcpy_Setup_%1.exe").arg(latest_version_);
    QFile::remove(local_setup_path_);                                                           // 上次的半截文件先删掉

    downloading_ = true;
    installable_ = false;
    download_status_ = QStringLiteral("正在下载 %1…").arg(latest_version_);
    emit SigDownloadChanged();

    download_process_ = new QProcess(this);
    download_process_->setProgram(QStringLiteral("curl.exe"));
    download_process_->setArguments(QStringList() << QStringLiteral("-L")                       // 跟随跳转
                                                  << QStringLiteral("--fail")                   // 4xx/5xx 当失败
                                                  << QStringLiteral("--silent")
                                                  << QStringLiteral("--show-error")
                                                  << QStringLiteral("-o") << local_setup_path_
                                                  << installer_url_);                           // exe 直链，不是下载页面
    QObject::connect(download_process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     this, [this](int exit_code, QProcess::ExitStatus status)
    {
        if (status != QProcess::NormalExit)
        {
            exit_code = -1;
        }
        OnDownloadFinished(exit_code);
    });
    download_process_->start();

    // curl 的 --progress-bar 写 stderr 且带 \r，解析麻烦；直接轮询文件大小更稳。
    // 100ms 采一次，进度看起来够顺；只有字节数真的变了才通知界面，避免空转刷新
    if (progress_timer_ == nullptr)
    {
        progress_timer_ = new QTimer(this);
        progress_timer_->setInterval(100);
        QObject::connect(progress_timer_, &QTimer::timeout, this, [this]()
        {
            const qint64 now = QFileInfo(local_setup_path_).size();
            if (now == reported_bytes_)
            {
                return;
            }
            reported_bytes_ = now;
            emit SigDownloadChanged();
        });
    }
    reported_bytes_ = 0;
    progress_timer_->start();
}

// 取消下载：杀掉 curl 并把半截文件删掉
void UpdateChecker::CancelDownload()
{
    if (!downloading_)
    {
        return;
    }

    if (download_process_ != nullptr)
    {
        download_process_->kill();
        download_process_->waitForFinished(1000);
    }
    if (progress_timer_ != nullptr)
    {
        progress_timer_->stop();
    }

    QFile::remove(local_setup_path_);
    downloading_ = false;
    installable_ = false;
    download_status_ = QStringLiteral("已取消下载");
    emit SigDownloadChanged();
}

// curl 结束后核对大小并算 sha256（45MB 算一遍约 200ms，丢后台线程免得卡界面）
void UpdateChecker::OnDownloadFinished(int exit_code)
{
    if (progress_timer_ != nullptr)
    {
        progress_timer_->stop();
    }
    downloading_ = false;

    if (exit_code != 0)
    {
        QFile::remove(local_setup_path_);
        download_status_ = QStringLiteral("下载失败（curl 退出码 %1）").arg(exit_code);
        emit SigDownloadChanged();
        return;
    }

    const qint64 actual_size = QFileInfo(local_setup_path_).size();
    if (download_size_ > 0 && actual_size != download_size_)
    {
        QFile::remove(local_setup_path_);
        download_status_ = QStringLiteral("下载不完整（%1 / %2 字节）").arg(actual_size).arg(download_size_);
        emit SigDownloadChanged();
        return;
    }

    download_status_ = QStringLiteral("校验安装包…");
    emit SigDownloadChanged();

    const QString path = local_setup_path_;
    const QString expect = download_sha256_;

    // 校验放后台线程：大文件算哈希不能让界面卡住
    qsc::base::TaskRunner::Instance().Post([this, path, expect]()
    {
        QFile file(path);
        QString actual;
        if (file.open(QIODevice::ReadOnly))
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&file))
            {
                actual = QString::fromLatin1(hash.result().toHex());
            }
        }

        QMetaObject::invokeMethod(this, [this, path, expect, actual]()
        {
            if (actual.isEmpty())
            {
                QFile::remove(path);
                download_status_ = QStringLiteral("读不到下载的安装包");
                emit SigDownloadChanged();
                return;
            }
            if (actual.compare(expect, Qt::CaseInsensitive) != 0)
            {
                QFile::remove(path);
                download_status_ = QStringLiteral("校验失败，已删除安装包");
                emit SigDownloadChanged();
                return;
            }

            installable_ = true;
            download_status_ = QStringLiteral("下载完成，可以安装");
            emit SigDownloadChanged();
        }, Qt::QueuedConnection);
    });
}

// 启动安装包（静默）然后退出自己，让安装程序能覆盖正在运行的文件
void UpdateChecker::InstallAndRestart()
{
    if (!installable_ || local_setup_path_.isEmpty())
    {
        return;
    }

    // /SILENT 有进度界面但不问问题；/CLOSEAPPLICATIONS 让安装包主动关掉我们
    const QStringList args{QStringLiteral("/SILENT"), QStringLiteral("/CLOSEAPPLICATIONS"),
                           QStringLiteral("/NORESTART")};
    if (!QProcess::startDetached(local_setup_path_, args))
    {
        download_status_ = QStringLiteral("启动安装程序失败，去下载页手动安装");
        emit SigDownloadChanged();
        return;
    }

    QCoreApplication::quit();
}

} // namespace qsc
