#pragma once

// ServiceConfigWidget.h — 单个 compose 服务的配置编辑表单（滚动区 + 动态增删行）。
// 对应Python: core/docker/docker_compose_editor.py:19-445 (ServiceConfigWidget)
//
// 与 Python 版差异：Python 通过 parent_window 直接回写主窗口 config 并落盘，
// C++ 版改为发射 configSaved(serviceName, config) 信号，由 DockerComposeEditor
// 统一更新内存配置并走 DockerManager::saveComposeFile 异步保存。

#include <QList>
#include <QPair>
#include <QVariantMap>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QVBoxLayout;

namespace cubeshell {

class ServiceConfigWidget : public QWidget {
    Q_OBJECT
public:
    explicit ServiceConfigWidget(const QString &serviceName,
                                 const QVariantMap &config = QVariantMap(),
                                 QWidget *parent = nullptr);

    QString serviceName() const { return m_serviceName; }

    // 从表单收集配置并剔除空值。对应Python: save_config 392-438 行
    QVariantMap collectConfig() const;

signals:
    // "保存配置"按钮点击后发射。对应Python: save_config 441-445 行
    void configSaved(const QString &serviceName, const QVariantMap &config);

private slots:
    void onSaveClicked();

private:
    // 动态行 helpers。对应Python: add_*_item / remove_* 系列（249-390 行）
    // key=value 双输入框行（构建参数/环境变量）
    void addPairRow(QVBoxLayout *layout, QList<QPair<QLineEdit *, QLineEdit *>> *list,
                    const QString &key, const QString &value);
    // 单输入框行（端口/卷/依赖服务/网络）；deleteText 对应 Python 的 "—" 或 "-"
    void addSingleRow(QVBoxLayout *layout, QList<QLineEdit *> *list,
                      const QString &value, const QString &deleteText);

    QString m_serviceName;
    QVariantMap m_config;

    QLineEdit *m_imageEdit = nullptr;
    QLineEdit *m_containerNameEdit = nullptr;
    QComboBox *m_restartCombo = nullptr;
    QLineEdit *m_commandEdit = nullptr;
    QLineEdit *m_contextEdit = nullptr;
    QLineEdit *m_dockerfileEdit = nullptr;

    QVBoxLayout *m_buildArgsLayout = nullptr;
    QVBoxLayout *m_portsLayout = nullptr;
    QVBoxLayout *m_envLayout = nullptr;
    QVBoxLayout *m_volumesLayout = nullptr;
    QVBoxLayout *m_dependsOnLayout = nullptr;
    QVBoxLayout *m_networksLayout = nullptr;

    QList<QPair<QLineEdit *, QLineEdit *>> m_buildArgsList;
    QList<QLineEdit *> m_portsList;
    QList<QPair<QLineEdit *, QLineEdit *>> m_envList;
    QList<QLineEdit *> m_volumesList;
    QList<QLineEdit *> m_dependsOnList;
    QList<QLineEdit *> m_networksList;
};

} // namespace cubeshell
