#include "session/server_session.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QThread>

#include <utility>

#include "adb/adb_args.h"
#include "adb/adb_runner.h"
#include "protocol/wire.h"

namespace qsc::session {
namespace {

constexpr int kReverseAcceptTimeoutMs = 10000;                                                  // reverse 模式下等设备连过来
constexpr int kForwardConnectAttempts = 30;                                                     // forward 模式下重试次数
constexpr int kForwardConnectIntervalMs = 300;                                                  // 每次重试间隔
constexpr int kForwardConnectTimeoutMs = 1000;                                                  // 单次 connect 超时
constexpr int kInfoPollMs = 300;                                                                // 设备信息轮询间隔
constexpr int kInfoTimeoutMs = 3000;                                                            // 设备信息总超时
constexpr int kDeviceNameBytes = 64;                                                            // 设备名固定 64 字节，后面补 0
constexpr int kInfoBytes = kDeviceNameBytes + 12;                                               // 再加 4 字节编码器 id + 4 宽 + 4 高
constexpr int kPushTimeoutMs = 60000;                                                           // 首次 push 可能要传几十 MB 的 jar
constexpr quint32 kScidUnset = 0xFFFFFFFFu;

} // namespace

ServerSession::ServerSession(ServerParams params) : params_(std::move(params))
{
}

ServerSession::~ServerSession()
{
    Stop();
}

ConnectResult ServerSession::Start()
{
    // 整套流程：
    //
    //   push ─▶ reverse ──成功──▶ listen ──▶ 读设备信息 ──▶ 等控制连接 ──┐
    //              │                                                   ├─▶ 交回两条 socket
    //              └──失败──▶ forward ─▶ execute ─▶ 建两条连接 ─▶ 读设备信息
    //
    // 两条路径的顺序不一样，是为了绕开服务端和 adb 转发的两处阻塞行为，
    // 具体原因写在 AcceptForReverse 和 ConnectForForward 里。
    ConnectResult result;

    // 不传 scid 时服务端会自己抽一个，那我们就不知道 socket 名了。
    // 这里先定死一个，保证参数里的 scid 和 reverse/forward 用的 socket 名一致。
    if (params_.scid_ == kScidUnset) {
        params_.scid_ = static_cast<quint32>(QRandomGenerator::global()->bounded(1, 0x7FFFFFFF));
    }

    qInfo() << "[session] 开始连接" << (params_.serial_.isEmpty() ? QStringLiteral("(默认设备)")
                                                                 : params_.serial_);

    QElapsedTimer timer;
    timer.start();

    QString error;
    if (!PushServer(&error)) {
        result.error_ = error;
        Stop();
        return result;
    }
    if (!SetUpTunnel(&error)) {
        result.error_ = error;
        Stop();
        return result;
    }
    if (!StartServerProcess(&error)) {
        result.error_ = error;
        Stop();
        return result;
    }
    if (!ConnectSockets(&result)) {
        Stop();
        return result;
    }

    result.ok_ = true;
    result.used_forward_ = forward_mode_;
    qInfo() << "[session] 连接完成：" << result.device_name_ << result.frame_size_
            << (forward_mode_ ? "forward" : "reverse") << "耗时" << timer.elapsed() << "ms";
    return result;
}

bool ServerSession::PushServer(QString *error)
{
    const QString local_path = ResolveLocalServerPath();
    if (!QFileInfo::exists(local_path)) {
        *error = QStringLiteral("找不到本地的 scrcpy-server：%1").arg(local_path);
        return false;
    }

    const QStringList args =
        adb::AdbArgs::Push(params_.serial_, local_path, params_.server_remote_path_);
    const adb::AdbResult pushed = adb::Run(args, kPushTimeoutMs);
    if (!pushed.Ok()) {
        *error = QStringLiteral("推送 scrcpy-server 失败：%1")
                     .arg(pushed.ErrorText());
        return false;
    }

    qInfo() << "[session] server 已推送到" << params_.server_remote_path_;
    return true;
}

bool ServerSession::SetUpTunnel(QString *error)
{
    if (params_.use_reverse_) {
        const QStringList reverse_args =
            adb::AdbArgs::Reverse(params_.serial_, SocketName(), params_.local_port_);
        const adb::AdbResult reversed = adb::Run(reverse_args);

        // reverse 设好后是本机先 listen，设备连过来
        if (reversed.Ok()) {
            if (!server_socket_.listen(QHostAddress::LocalHost, params_.local_port_)) {
                *error = QStringLiteral("本机监听 %1 端口失败：%2")
                             .arg(params_.local_port_)
                             .arg(server_socket_.errorString());
                return false;
            }
            forward_mode_ = false;
            tunnel_enabled_ = true;
            qInfo() << "[session] adb reverse 就绪，本机已监听" << params_.local_port_;
            return true;
        }

        // 国产机上 reverse 会报 "more than one device"（同一台机器挂了 USB + 无线两条通道），
        // 这是 adb 自己的问题，不能因此整体失败，必须降级到 forward
        qWarning() << "[session] adb reverse 失败，降级 forward："
                   << reversed.ErrorText();
    }

    const QStringList forward_args =
        adb::AdbArgs::Forward(params_.serial_, params_.local_port_, SocketName());
    const adb::AdbResult forwarded = adb::Run(forward_args);
    if (!forwarded.Ok()) {
        *error = QStringLiteral("adb forward 失败：%1")
                     .arg(forwarded.ErrorText());
        return false;
    }

    forward_mode_ = true;
    tunnel_enabled_ = true;
    qInfo() << "[session] adb forward 就绪";
    return true;
}

bool ServerSession::StartServerProcess(QString *error)
{
    ServerParams launch = params_;
    launch.tunnel_forward_ = forward_mode_;

    const QStringList args = adb::AdbArgs::Shell(params_.serial_, ServerArgs::Build(launch));

    QString start_error;
    server_process_ = adb::StartLongRunning(args, &start_error);
    if (server_process_ == nullptr) {
        *error = QStringLiteral("启动 scrcpy-server 失败：%1").arg(start_error);
        return false;
    }

    // 参数拼错时服务端会立刻退出。等一会儿看一眼，能把它的报错原样带出来，
    // 否则只能看到后面"连接超时"这种没有信息量的错误。
    // 800ms 是因为 JVM 启动本身就要几百毫秒，等太短会漏掉。
    if (server_process_->waitForFinished(800)) {
        DrainServerOutput();
        *error = QStringLiteral("scrcpy-server 立刻退出了：%1").arg(ServerOutputTail());
        return false;
    }

    qInfo() << "[session] server 进程已启动（%1）" << (forward_mode_ ? "forward" : "reverse");
    return true;
}

bool ServerSession::ConnectSockets(ConnectResult *result)
{
    if (forward_mode_) {
        return ConnectForForward(result);
    }
    return AcceptForReverse(result);
}

bool ServerSession::AcceptForReverse(ConnectResult *result)
{
    // 设备端先连视频 socket，再连控制 socket，顺序不能反
    bool timed_out = false;
    if (!server_socket_.waitForNewConnection(kReverseAcceptTimeoutMs, &timed_out) || timed_out) {
        DrainServerOutput();
        result->error_ = QStringLiteral("等待设备连入超时（reverse），端口 %1；服务端输出：%2")
                             .arg(params_.local_port_)
                             .arg(ServerOutputTail());
        return false;
    }
    video_socket_ = server_socket_.nextPendingConnection();
    if (video_socket_ == nullptr) {
        result->error_ = QStringLiteral("拿到视频 socket 失败");
        return false;
    }

    // 先把视频通道上的设备信息读掉，再等控制连接。
    // adb 的 reverse 转发在一条通道还没被读走数据时会挡住下一条连接，
    // 所以这里不能"两条都收下再一起读"。
    if (!ReadInfo(video_socket_, result)) {
        DrainServerOutput();
        result->error_ += QStringLiteral("；服务端输出：%1").arg(ServerOutputTail());
        return false;
    }

    // 第二条：控制 socket。用轮询而不是一次长的 waitForNewConnection，
    // 出问题时能看出"到底有没有连进来"和"监听还在不在"。
    QElapsedTimer control_wait;
    control_wait.start();
    while (control_wait.elapsed() < kReverseAcceptTimeoutMs) {
        if (server_socket_.hasPendingConnections() || server_socket_.waitForNewConnection(500)) {
            break;
        }
        qInfo() << "[session] 还在等控制 socket，" << control_wait.elapsed()
                << "ms；监听中:" << server_socket_.isListening();
    }

    if (!server_socket_.hasPendingConnections()) {
        DrainServerOutput();
        const QString state = (server_process_ != nullptr
                               && server_process_->state() == QProcess::NotRunning)
                                  ? QStringLiteral("已退出，exit=%1").arg(server_process_->exitCode())
                                  : QStringLiteral("仍在运行");
        result->error_ = QStringLiteral("等待控制 socket 超时（reverse）；服务端%1；输出：%2")
                             .arg(state, ServerOutputTail());
        return false;
    }

    control_socket_ = server_socket_.nextPendingConnection();
    if (control_socket_ == nullptr) {
        result->error_ = QStringLiteral("拿到控制 socket 失败");
        return false;
    }

    // 两条都到手了，监听端口可以放掉
    server_socket_.close();
    return true;
}

bool ServerSession::ConnectForForward(ConnectResult *result)
{
    for (int attempt = 0; attempt < kForwardConnectAttempts; ++attempt) {
        auto *video = new QTcpSocket();
        auto *control = new QTcpSocket();

        // 两条连接都要先建好，服务端才会往下走：它先发 1 个 dummy 字节，
        // 然后要等到控制 socket 也连上才开始发设备信息。先连一条就急着读会一直等下去。
        video->connectToHost(QHostAddress::LocalHost, params_.local_port_);
        control->connectToHost(QHostAddress::LocalHost, params_.local_port_);

        bool ready = false;
        if (video->waitForConnected(kForwardConnectTimeoutMs)
            && control->waitForConnected(kForwardConnectTimeoutMs)) {
            if (video->waitForReadyRead(kForwardConnectTimeoutMs)) {
                video->read(1);                                                                 // 吃掉握手用的 dummy 字节
                ready = ReadInfo(video, result);
            }
        }

        if (ready) {
            // control 上也有一个 dummy 字节，吃掉免得后面当垃圾数据
            if (control->waitForReadyRead(kForwardConnectTimeoutMs)) {
                control->read(1);
            }
            video_socket_ = video;
            control_socket_ = control;
            result->error_.clear();
            break;
        }

        delete video;
        delete control;
        QThread::msleep(kForwardConnectIntervalMs);
    }

    if (video_socket_ == nullptr) {
        if (result->error_.isEmpty()) {
            result->error_ = QStringLiteral("连接视频 socket 失败（forward），重试 %1 次")
                                 .arg(kForwardConnectAttempts);
        }
        DrainServerOutput();
        result->error_ += QStringLiteral("；服务端输出：%1").arg(ServerOutputTail());
        return false;
    }

    return true;
}

bool ServerSession::ReadInfo(QTcpSocket *socket, ConnectResult *result)
{
    // 设备信息固定 76 字节：
    //   [0..63]   设备名，UTF-8，后面补 0
    //   [64..67]  编码器 id（当前固定 H.264，暂不解析）
    //   [68..71]  视频宽
    //   [72..75]  视频高
    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();

    while (buffer.size() < kInfoBytes) {
        if (timer.elapsed() > kInfoTimeoutMs) {
            result->error_ = QStringLiteral("读取设备信息超时，只收到 %1/%2 字节")
                                 .arg(buffer.size())
                                 .arg(kInfoBytes);
            return false;
        }
        if (!socket->waitForReadyRead(kInfoPollMs)
            && socket->state() != QAbstractSocket::ConnectedState) {
            result->error_ = QStringLiteral("读取设备信息时连接断开");
            return false;
        }

        // 只读还缺的字节数。用 readAll 会把设备信息之后的流数据一起读进来丢掉，
        // 那样第一个配置包（SPS/PPS）就没了，后面所有帧都解不出来。
        const QByteArray chunk = socket->read(kInfoBytes - buffer.size());
        if (!chunk.isEmpty()) {
            buffer.append(chunk);
        }
    }

    int name_length = static_cast<int>(buffer.indexOf('\0'));
    if (name_length <= 0 || name_length > kDeviceNameBytes) {
        name_length = kDeviceNameBytes;
    }
    result->device_name_ = QString::fromUtf8(buffer.left(name_length));

    protocol::WireReader reader(buffer);
    quint32 codec_id = 0;
    quint32 width = 0;
    quint32 height = 0;
    if (!reader.Skip(kDeviceNameBytes) || !reader.ReadU32(codec_id) || !reader.ReadU32(width)
        || !reader.ReadU32(height)) {
        result->error_ = QStringLiteral("设备信息解析失败");
        return false;
    }

    result->codec_id_ = codec_id;
    result->frame_size_ = QSize(static_cast<int>(width), static_cast<int>(height));
    return true;
}

void ServerSession::DrainServerOutput()
{
    if (server_process_ == nullptr) {
        return;
    }
    // readAll* 不会阻塞，有多少拿多少
    server_output_.append(server_process_->readAllStandardError());
    server_output_.append(server_process_->readAllStandardOutput());
}

QString ServerSession::ServerOutputTail() const
{
    const QString text = QString::fromUtf8(server_output_).trimmed();
    if (text.isEmpty()) {
        return QStringLiteral("(无输出)");
    }
    // 报错信息里只留最后一段，前面的多是 JVM 噪声
    static constexpr int kTailChars = 600;
    if (text.size() <= kTailChars) {
        return text;
    }
    return QStringLiteral("…") + text.right(kTailChars);
}

void ServerSession::RemoveTunnel()
{
    if (!tunnel_enabled_) {
        return;
    }

    if (forward_mode_) {
        adb::Run(adb::AdbArgs::ForwardRemove(params_.serial_, params_.local_port_), 5000);
    } else {
        adb::Run(adb::AdbArgs::ReverseRemove(params_.serial_, SocketName()), 5000);
    }
    tunnel_enabled_ = false;
}

QString ServerSession::ResolveLocalServerPath() const
{
    if (!params_.server_local_path_.isEmpty()) {
        return params_.server_local_path_;
    }
    // 默认和 exe 放一起：部署时 third_party/scrcpy-server 会被拷到输出目录
    return QCoreApplication::applicationDirPath() + QStringLiteral("/scrcpy-server");
}

QString ServerSession::SocketName() const
{
    return adb::AdbArgs::SocketName(params_.scid_);
}

void ServerSession::Stop()
{
    if (video_socket_ != nullptr) {
        video_socket_->close();
        delete video_socket_;
        video_socket_ = nullptr;
    }
    if (control_socket_ != nullptr) {
        control_socket_->close();
        delete control_socket_;
        control_socket_ = nullptr;
    }

    server_socket_.close();

    if (server_process_ != nullptr) {
        // adb shell 起来的是一个长驻进程，杀掉 adb 客户端会连带把设备上的服务端收掉
        server_process_->kill();
        server_process_->waitForFinished(3000);
        delete server_process_;
        server_process_ = nullptr;
    }

    RemoveTunnel();
    forward_mode_ = false;
    server_output_.clear();
}

QTcpSocket *ServerSession::TakeVideoSocket()
{
    QTcpSocket *socket = video_socket_;
    video_socket_ = nullptr;
    return socket;
}

QTcpSocket *ServerSession::TakeControlSocket()
{
    QTcpSocket *socket = control_socket_;
    control_socket_ = nullptr;
    return socket;
}

} // namespace qsc::session
