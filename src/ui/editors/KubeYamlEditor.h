#pragma once

// KubeYamlEditor.h — Kubernetes 资源 YAML 查看 / 编辑应用窗口（非模态）。
//
// 对应 docs/Kubernetes功能实现方案.md §5.2：复用 CodeEditor（行号 + 高亮），
// 只读查看与"编辑并应用"两种模式。YAML 内容不做 C++ 侧解析校验 —— 服务端
// 拒绝时错误原文回显，与 ComposeYaml"文本级传输"的既定取舍一致。

#include <QDialog>
#include <QString>

class QPushButton;

namespace cubeshell {

class CodeEditor;

class KubeYamlEditor : public QDialog {
    Q_OBJECT
public:
    explicit KubeYamlEditor(QWidget *parent = nullptr);

    // editable=false 只读查看；true 显示"应用"按钮，编辑后可下发 apply。
    void showYaml(const QString &title, const QString &yamlText, bool editable);

signals:
    // 用户点击"应用"，内容为编辑器当前全文。
    void applyRequested(const QString &yamlText);

private:
    CodeEditor *m_editor = nullptr;
    QPushButton *m_applyBtn = nullptr;
    bool m_editable = false;
};

} // namespace cubeshell
