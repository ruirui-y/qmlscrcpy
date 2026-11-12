#pragma once

#include <functional>

class QObject;
class QThread;

namespace qsc::base
{

// 后台任务队列：一个常驻线程 + 一条队列，串行跑 IO 类任务（写配置、adb push、切无线）。
// 统一从这里投递，别每个功能各自 QThread::create——线程一多就没人说得清谁在跑什么。
class TaskRunner
{
public:
    static TaskRunner &Instance();

    // 主线程调用：任务在后台线程里排到队尾执行，不会阻塞 UI
    void Post(std::function<void()> task);

    // 退出前调一次：跑完队列再停线程
    void Shutdown();

public:
    ~TaskRunner();                                                                              // 由 ShutdownTaskRunner 负责销毁

private:
    TaskRunner();

    TaskRunner(const TaskRunner &) = delete;
    TaskRunner &operator=(const TaskRunner &) = delete;

    QThread *thread_ = nullptr;                                                                 // 常驻后台线程，名字 qsc-worker
    QObject *worker_ = nullptr;                                                                 // 住在那个线程里，靠它投递任务
};

// 进程退出时清理：没创建过就什么都不做
void ShutdownTaskRunner();

} // namespace qsc::base
