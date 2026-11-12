#include "base/task_runner.h"

#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QThread>

namespace qsc::base
{

namespace
{

// 真正干活的对象：住在后台线程里，任务靠队列事件串行执行。
// 不用 Q_OBJECT：这里不需要信号槽，只需要 invokeMethod 的 functor 重载。
class TaskWorker : public QObject
{
public:
    void Invoke(std::function<void()> task)
    {
        QMetaObject::invokeMethod(this, [task]()
        {
            task();
        }, Qt::QueuedConnection);
    }
};

TaskRunner *g_runner = nullptr;                                                                 // 首次投递时才创建，避免白起线程

} // namespace

TaskRunner::TaskRunner()
{
    thread_ = new QThread();
    thread_->setObjectName(QStringLiteral("qsc-worker"));

    auto *worker = new TaskWorker();
    worker->moveToThread(thread_);
    worker_ = worker;

    thread_->start();
}

// 析构会走 Shutdown，保证线程先停再删
TaskRunner::~TaskRunner()
{
    Shutdown();
}

TaskRunner &TaskRunner::Instance()
{
    if (g_runner == nullptr)
    {
        g_runner = new TaskRunner();
    }
    return *g_runner;
}

void TaskRunner::Post(std::function<void()> task)
{
    if (task == nullptr || worker_ == nullptr)
    {
        return;
    }

    auto *worker = static_cast<TaskWorker *>(worker_);
    worker->Invoke(std::move(task));
}

void TaskRunner::Shutdown()
{
    if (thread_ == nullptr)
    {
        return;
    }

    // quit 让事件循环跑完队列里剩下的任务后退出
    thread_->quit();
    if (!thread_->wait(3000))
    {
        thread_->requestInterruption();
        thread_->wait(1000);
    }

    delete worker_;                                                                              // 线程已停，可以在这里删
    worker_ = nullptr;

    delete thread_;
    thread_ = nullptr;
}

void ShutdownTaskRunner()
{
    if (g_runner == nullptr)
    {
        return;
    }

    g_runner->Shutdown();
    delete g_runner;                                                                            // 析构是私有的，只能在类自己的自由函数里删
    g_runner = nullptr;
}

} // namespace qsc::base
