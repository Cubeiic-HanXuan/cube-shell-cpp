#pragma once

// DockerDaemonConfigDialog.h — Docker 守护进程 daemon.json 配置对话框。
// 对应Python: core/docker/docker_compose_editor.py:530-589 (DockerDaemonConfigDialog)
//
// 校验/默认配置复用 DockerManager 的静态方法；应用动作只 accept()，
// 实际写入由调用方（DockerComposeEditor）取 configText() 后交给
// DockerManager::applyDaemonConfig。

#include <QDialog>

class QTextEdit;

namespace cubeshell {

class DockerDaemonConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit DockerDaemonConfigDialog(QWidget *parent = nullptr);

    // 编辑器中的 daemon.json 文本。对应Python: get_config（这里保持原文，
    // 解析/校验交给 DockerManager::validateDaemonJson）
    QString configText() const;

private slots:
    // 对应Python: validate_config（验证成功/失败弹窗）
    void validateConfig();

private:
    QTextEdit *m_editor = nullptr;
};

} // namespace cubeshell
