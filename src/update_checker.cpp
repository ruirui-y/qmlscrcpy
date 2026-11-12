#include "update_checker.h"

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSslSocket>
#include <QUrl>

#include "app_settings.h"
#include "base/log.h"
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

    // 带一个按天变化的参数，避免 CDN 缓存过久
    const QString cache_buster = QStringLiteral("?v=%1").arg(QDate::currentDate().toJulianDay());

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
    request.setTransferTimeout(5000);                                                            // 5 秒没回来就当这个源失败

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

    // 本地版本 < minimum：必须更新；< latest：提示更新
    const QString minimum_version = object.value(QStringLiteral("minimum")).toString();
    update_required_ = !minimum_version.isEmpty()
        && update::Compare(current_version_, minimum_version) < 0;
    update_available_ = update::Compare(current_version_, latest_version_) < 0;

    checking_ = false;
    qCInfo(base::lcApp) << "[update] 本地" << current_version_ << "远端" << latest_version_
                        << "提示更新" << update_available_ << "强制" << update_required_;

    emit StateChanged();
    return true;
}

// 打开浏览器去下载（链接来自 latest.json，服务端想换就换）
void UpdateChecker::OpenDownloadPage()
{
    if (download_url_.isEmpty())
    {
        return;
    }
    QDesktopServices::openUrl(QUrl(download_url_));
}

} // namespace qsc
