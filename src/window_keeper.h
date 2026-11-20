#pragma once

// 窗口与热键：登记主界面/投屏窗、老板键隐藏与恢复、四个窗口快捷键的读写。
// 只管"窗口怎么藏怎么显"，不知道连接和设备。

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

class QWindow;

namespace qsc::app
{

class WindowKeeper : public QObject
{
    Q_OBJECT

public:
    explicit WindowKeeper(QObject *parent = nullptr);

    void SetBossKeyHost(QObject *host);                                                         // main 交进来的 BossKey 组件
    QString BossKey() const;
    bool SetBossKey(const QString &text);                                                       // 解析 + 重新注册 + 写配置

    QString Shortcut(const QString &name) const;                                                // back / home / screenshot / fullscreen
    bool SetShortcut(const QString &name, const QString &sequence);

    void Register(QObject *window);                                                             // QML 窗口自己报到
    void ToggleBossKey();                                                                       // 藏起所有窗口，再按恢复
    void RestoreIfHidden();                                                                     // 断线时把藏起来的窗口放回去

signals:
    void SigBossKeyChanged();
    void SigShortcutsChanged();
    void SigStatus(const QString &text);                                                        // 给状态栏的提示

private:
    QPointer<QObject> boss_key_host_;                                                           // BossKey 组件，改键时重新注册用
    QVector<QPointer<QWindow>> app_windows_;                                                    // 登记过的窗口
    QVector<QPointer<QWindow>> hidden_windows_;                                                 // 老板键藏起来的窗口
    bool boss_hidden_ = false;                                                                  // 现在是"藏起来"的状态吗
};

} // namespace qsc::app