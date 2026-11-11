#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QTcpServer>

#include "session/server_args.h"

class QProcess;
class QTcpSocket;

namespace qsc::session {

// 一次连接尝试的结果。失败时 error_ 直接拿去显示，不抛异常。
struct ConnectResult
{
    bool ok_ = false;
    QString error_;            // 失败原因
    bool used_forward_ = false;  // true 说明 reverse 失败后降级到了 forward
    QString device_name_;      // 设备型号，服务端上报
    QSize frame_size_;         // 视频帧尺寸，旋转前
    quint32 codec_id_ = 0;     // 服务端用的编码器 id（当前固定 H.264）
};

// 把 scrcpy-server 推上设备、拉起来、把两条 socket 建好。
//
// 这是一条阻塞的线性流程，必须在工作线程里调用（内部全是 waitFor*）：
//
//   push ─▶ reverse ──成功──▶ listen ──────────────┐
//              │                                   ├─▶ readInfo ─▶ 交回两条 socket
//              └──失败──▶ forward ─▶ execute ─▶ connect（带重试）
//
// 为什么不写成信号槽接力的状态机：每次失败都要回退到上一步重来，写成信号槽之后
// "现在到底走到哪一步"只能靠日志猜；阻塞写法里这七步就是从上往下七段代码。
class ServerSession
{
public:
    explicit ServerSession(ServerParams params);
    ~ServerSession();

    // 阻塞跑完整套流程。成功后两条 socket 建立完毕，可以直接交给读线程/控制线程。
    ConnectResult Start();

    // 幂等。不管 Start 成没成功，都能调。
    void Stop();

    // 成功后由调用方接管所有权；没接手之前由本对象负责关闭
    QTcpSocket *TakeVideoSocket();
    QTcpSocket *TakeControlSocket();

private:
    bool PushServer(QString *error);
    bool SetUpTunnel(QString *error);
    bool StartServerProcess(QString *error);
    bool ConnectSockets(ConnectResult *result);
    bool AcceptForReverse(ConnectResult *result);
    bool ConnectForForward(ConnectResult *result);
    bool ReadInfo(QTcpSocket *socket, ConnectResult *result);
    void RemoveTunnel();
    QString ResolveLocalServerPath() const;
    QString SocketName() const;

    // 把服务端进程这段时间打印的东西抽出来。
    // 这条流程里没有事件循环，readyRead 不会自己触发，只能靠主动读；
    // 失败时把它的输出拼进错误信息，比只报"连接超时"有用得多。
    void DrainServerOutput();
    QString ServerOutputTail() const;

private:
    ServerParams params_;
    bool forward_mode_ = false;   // true 表示降级到了 forward
    bool tunnel_enabled_ = false; // 有没有挂过 adb reverse / forward
    QTcpServer server_socket_;    // 只在 reverse 模式下用来 accept
    QProcess *server_process_ = nullptr;
    QByteArray server_output_;         // 服务端进程到目前为止的输出
    QTcpSocket *video_socket_ = nullptr;
    QTcpSocket *control_socket_ = nullptr;
};

} // namespace qsc::session
