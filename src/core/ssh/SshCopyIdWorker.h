#pragma once

// SshCopyIdWorker.h — 一键 ssh-copy-id：把本地公钥追加到对端 ~/.ssh/authorized_keys。
//
// 用设备**已存凭据**（密码或既有密钥，即 DeviceEntry 自身的认证方式）建立一条
// 临时连接，幂等地把公钥写进 authorized_keys——下次即可免密登录。
//
// 连接与远程执行的形态照 ConnectionTester::testSsh + FrpConnectWorker：
// SshClient 建连（带代理）→ CommandExecutor 跑 exec 通道命令。整条流程在 worker
// 线程内阻塞执行，结果经 finishedSignal 回切 UI。
//
// 中断协议同 FrpConnectWorker：run() 纯阻塞，要提前结束须 requestInterruption()，
// 单次阻塞调用（握手/执行）无法被打断，调用方 wait() 应带超时。
//
// 注意：keyboard-interactive / MFA 设备传 nullptr prompt 回调，认证会失败——
// 部署到这类设备不在本功能范围（与「测试连接」的处理一致）。

#include <QThread>

#include "config/DeviceConfigStore.h"

namespace cubeshell {

class SshCopyIdWorker : public QThread {
    Q_OBJECT
public:
    // device 必须是 DeviceConfigStore::resolved() 出来的条目（密码已从钥匙串填好）。
    // publicKeyLine 为完整 authorized_keys 行（type base64 comment）。
    SshCopyIdWorker(const DeviceEntry &device, const QString &publicKeyLine,
                    QObject *parent = nullptr);

signals:
    // success：整体成败；message：人类可读结果/错误；alreadyPresent：公钥已存在（去重命中）。
    void finishedSignal(bool success, const QString &message, bool alreadyPresent);

protected:
    void run() override;

private:
    DeviceEntry m_device;
    QString     m_publicKeyLine;
};

} // namespace cubeshell
