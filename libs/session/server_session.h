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
    // 整套流程是否走通
    bool ok_ = false;

    // 失败原因，失败时才有值
    QString error_;

    // true 说明 reverse 失败后降级到了 forward
    bool used_forward_ = false;

    // 服务端上报的设备型号
    QString device_name_;

    // 视频帧尺寸，设备旋转前
    QSize frame_size_;

    // 服务端用的编码器 id，当前固定 H.264
    quint32 codec_id_ = 0;
};

// 把 scrcpy-server 推上设备、拉起来、把视频和控制两条 socket 建好。
// 里面全是阻塞的 waitFor*，必须在工作线程里调用；成功失败都通过 ConnectResult 返回。
class ServerSession
{
public:
    explicit ServerSession(ServerParams params);
    ~ServerSession();

    // 阻塞跑完整套流程，成功后两条 socket 建立完毕，可以交给读线程和控制线程
    ConnectResult Start();

    // 幂等，不管 Start 成没成功都能调
    void Stop();

    // 成功后由调用方接管所有权；没人接手时由本对象负责关闭
    QTcpSocket *TakeVideoSocket();
    QTcpSocket *TakeControlSocket();

private:
    // 把 server jar 推到设备上
    bool PushServer(QString *error);

    // 挂 adb 隧道：先试 reverse，失败降级 forward；reverse 还要在本机先 listen
    bool SetUpTunnel(QString *error);

    // 拉起设备上的 server 进程，并盯一眼它有没有立刻退出
    bool StartServerProcess(QString *error);

    // 按当前模式建两条 socket 并读出设备信息
    bool ConnectSockets(ConnectResult *result);

    // reverse 模式：等设备连进来
    bool AcceptForReverse(ConnectResult *result);

    // forward 模式：本机连出去，带重试
    bool ConnectForForward(ConnectResult *result);

    // 读 76 字节设备信息：64 字节设备名 + 4 字节编码器 id + 4 字节宽 + 4 字节高
    bool ReadInfo(QTcpSocket *socket, ConnectResult *result);

    // 拆掉隧道映射，断开后留着会挡住下一次连接
    void RemoveTunnel();

    // 决定本地 jar 的路径：参数里给了就用，没给就用程序目录下的
    QString ResolveLocalServerPath() const;

    // 按 scid 算出设备端的 abstract socket 名
    QString SocketName() const;

    // 把服务端进程这段时间打印的东西抽出来。
    // 这条流程里没有事件循环，readyRead 不会自己触发，只能靠主动读；
    // 失败时把它的输出拼进错误信息，比只报"连接超时"有用得多。
    void DrainServerOutput();
    QString ServerOutputTail() const;

private:
    // 启动参数，Start 里会补上随机 scid
    ServerParams params_;

    // true 表示 reverse 失败、降级到了 forward
    bool forward_mode_ = false;

    // 有没有挂过 adb reverse / forward，Stop 时决定要不要拆
    bool tunnel_enabled_ = false;

    // 只在 reverse 模式下用来等设备连进来
    QTcpServer server_socket_;

    // 设备上的 server 进程，退出即代表服务端挂了
    QProcess *server_process_ = nullptr;

    // 服务端进程到目前为止的输出，失败时拼进错误信息
    QByteArray server_output_;

    // 视频流 socket，成功后交给读线程
    QTcpSocket *video_socket_ = nullptr;

    // 控制 socket，双向
    QTcpSocket *control_socket_ = nullptr;
};

} // namespace qsc::session
