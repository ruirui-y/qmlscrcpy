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
    bool ok_ = false;                                                                           // 整套流程是否走通
    QString error_;                                                                             // 失败原因，失败时才有值
    bool used_forward_ = false;                                                                 // true 说明 reverse 失败后降级到了 forward
    QString device_name_;                                                                       // 服务端上报的设备型号
    QSize frame_size_;                                                                          // 视频帧尺寸，设备旋转前
    quint32 codec_id_ = 0;                                                                      // 服务端用的编码器 id，当前固定 H.264
};

// 把 scrcpy-server 推上设备、拉起来、把视频和控制两条 socket 建好。
// 里面全是阻塞的 waitFor*，必须在工作线程里调用；成功失败都通过 ConnectResult 返回。
class ServerSession
{
public:
    explicit ServerSession(ServerParams params);
    ~ServerSession();

    ConnectResult Start();                                                                      // 阻塞跑完整套流程，成功后可以交给读线程和控制线程
    void Stop();                                                                                // 幂等，不管 Start 成没成功都能调
    QTcpSocket *TakeVideoSocket();                                                              // 成功后由调用方接管，没人接手时本对象负责关闭
    QTcpSocket *TakeControlSocket();                                                            // 同上

private:
    bool PushServer(QString *error);                                                            // 把 server jar 推到设备上
    bool SetUpTunnel(QString *error);                                                           // 挂 adb 隧道：先 reverse，失败降级 forward

    // 拉起设备上的 server 进程，并盯一眼它有没有立刻退出（参数错会秒退）
    bool StartServerProcess(QString *error);

    bool ConnectSockets(ConnectResult *result);                                                 // 按当前模式建两条 socket 并读设备信息
    bool AcceptForReverse(ConnectResult *result);                                               // reverse 模式：等设备连进来
    bool ConnectForForward(ConnectResult *result);                                              // forward 模式：本机连出去，带重试
    bool ReadInfo(QTcpSocket *socket, ConnectResult *result);                                   // 读 76 字节设备信息
    void RemoveTunnel();                                                                        // 拆掉隧道映射，留着会挡住下一次连接
    QString ResolveLocalServerPath() const;                                                     // 参数没给 jar 路径时用程序目录下的
    QString SocketName() const;                                                                 // 按 scid 算出设备端的 abstract socket 名

    // 把服务端进程这段时间打印的东西抽出来。
    // 这条流程里没有事件循环，readyRead 不会自己触发，只能靠主动读；
    // 失败时把它的输出拼进错误信息，比只报"连接超时"有用得多。
    void DrainServerOutput();
    QString ServerOutputTail() const;

private:
    ServerParams params_;                                                                       // 启动参数，Start 里会补上随机 scid

    // 连接方式：正常是 reverse，失败降级到 forward
    bool forward_mode_ = false;                                                                 // true 表示已经降级到 forward
    bool tunnel_enabled_ = false;                                                               // 挂过 adb 隧道就算 true，Stop 时据此决定要不要拆
    QTcpServer server_socket_;                                                                  // 只在 reverse 模式下用来等设备连进来

    // 设备上的 server 进程，退出即代表服务端挂了
    QProcess *server_process_ = nullptr;                                                        // 由本对象负责 kill 和 delete
    QByteArray server_output_;                                                                  // 它到目前为止的输出，失败时拼进错误信息

    // 两条 socket，成功后所有权交给调用方
    QTcpSocket *video_socket_ = nullptr;                                                        // 视频流，只读
    QTcpSocket *control_socket_ = nullptr;                                                      // 控制，双向
};

} // namespace qsc::session
