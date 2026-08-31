#pragma once

// SshKeyManagerDialog.h — SSH 密钥管理对话框。
//
// 一站式管理本地 SSH 密钥：生成密钥对（Ed25519/RSA/ECDSA）、看指纹、复制公钥、
// 一键 ssh-copy-id 部署到指定设备（用设备已存凭据连接并幂等写入 authorized_keys）。
//
// 密钥本体与索引由 SshKeyStore 管（配置目录 keys/ + ssh_keys.json），本对话框只管
// 呈现与交互。部署需要设备目录与凭据，故构造函数注入 DeviceConfigStore（不持有），
// 用 resolved() 取回带密码的设备条目交给 SshCopyIdWorker 在后台线程跑。

#include <QDialog>
#include <QPointer>

#include "ssh/SshKeyGenerator.h"
#include "ssh/SshKeyStore.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPushButton;

namespace cubeshell {

class DeviceConfigStore;
class SshCopyIdWorker;

class SshKeyManagerDialog : public QDialog {
    Q_OBJECT
public:
    // deviceStore 为部署功能提供设备目录与凭据查询；可为空（此时禁用部署）。
    explicit SshKeyManagerDialog(DeviceConfigStore *deviceStore, QWidget *parent = nullptr);
    ~SshKeyManagerDialog() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void rebuildTree();
    bool selectedKey(SshKeyEntry *out) const;

    void generateKey();      // 生成密钥对（弹表单）
    void copyPublicKey();    // 复制公钥到剪贴板
    void deploySelected();   // ssh-copy-id 到选中设备
    void removeSelected();   // 删除（连密钥文件一起）
    void showDetails();      // 双击：完整指纹 + 公钥行

    // 生成参数表单；确认返回 true 并填好 params/name。
    bool promptGenerate(SshKeyGenParams *params, QString *name);
    // 部署设备选择器：列出 SSH 设备，返回选中的设备名（取消返回空串）。
    QString pickDeployDevice();

    void onDeployFinished(bool ok, const QString &msg, bool alreadyPresent);
    void shutdownWorker();

    SshKeyStore m_keyStore;
    DeviceConfigStore *m_deviceStore = nullptr;   // 不持有
    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_deployBtn = nullptr;
    QPointer<SshCopyIdWorker> m_deployWorker;
};

} // namespace cubeshell
