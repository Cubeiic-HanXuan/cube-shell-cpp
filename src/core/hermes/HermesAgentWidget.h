#pragma once

// HermesAgentWidget.h — Hermes Agent Profile 管理面板。
// 对应Python: core/hermes/agent_widget.py
//             (AgentWidget / ProfileItemDelegate / ProfileConfigDialog)
//           + core/hermes/config_highlighter.py(YamlHighlighter/DotenvHighlighter)
//
// - Profile 列表 + 搜索过滤   对应Python: _render_profile_list/_filter_profiles
// - 新建/重命名/删除/设为默认  对应Python: _create_profile/_rename_profile/...
// - 网关启停 + 在终端中打开    对应Python: _start_gateway/_stop_gateway/
//                                         _open_in_terminal
// - config.yaml / .env 编辑    对应Python: ProfileConfigDialog
//
// 后台工作一律走 QtConcurrent::run(与 HermesTaskModel/HermesGateway 保持一致,
// 不再引入 QThread 子类),结果用 QMetaObject::invokeMethod 回到 UI 线程。
// Python 侧的 Pygments 高亮在 C++ 无对应库,改用共享的
// hermes/HermesHighlighters.h(YamlHighlighter/DotenvHighlighter)。

#include <QDialog>
#include <QFuture>
#include <QList>
#include <QStyledItemDelegate>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;

namespace cubeshell {

class HermesBackend;

// ---------------------------------------------------------------------------
// ProfileItemDelegate
// ---------------------------------------------------------------------------

// 多行卡片式绘制 Profile 列表项(名称 + 右侧运行状态 + 第二行模型名)。
// 对应Python: agent_widget.ProfileItemDelegate
class ProfileItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // 对应Python: ITEM_HEIGHT / PADDING_* / ACTIVE_BAR_WIDTH / STATUS_DOT_RADIUS
    static constexpr int kItemHeight = 56;
    static constexpr int kPaddingLeft = 12;
    static constexpr int kPaddingRight = 12;
    static constexpr int kPaddingTop = 8;
    static constexpr int kActiveBarWidth = 3;
    static constexpr int kStatusDotRadius = 4;

    // Profile 数据(QVariantMap: name/active/model/gateway/alias/distribution)
    // 存放的 item 角色。对应Python: Qt.UserRole + 1
    static constexpr int kProfileRole = Qt::UserRole + 1;

    explicit ProfileItemDelegate(QObject *parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};

// ---------------------------------------------------------------------------
// ProfileConfigDialog
// ---------------------------------------------------------------------------

// Profile 配置编辑对话框:config.yaml / .env 双 Tab,带语法高亮与 YAML 校验。
// 对应Python: agent_widget.ProfileConfigDialog
//            (+ ProfileConfigLoader / ProfileConfigSaver 两个后台线程)
class ProfileConfigDialog : public QDialog {
    Q_OBJECT
public:
    ProfileConfigDialog(HermesBackend *backend, const QString &profileName,
                        const QString &configPath, const QString &envPath,
                        QWidget *parent = nullptr);
    ~ProfileConfigDialog() override;

    // --- pure helper (unit-testable) ---

    // YAML 语法校验(yaml-cpp)。对应Python: yaml.safe_load 的 YAMLError 分支。
    static bool validateYaml(const QString &text, QString *errorOut = nullptr);

private slots:
    // 对应Python: ProfileConfigDialog._load / _on_save
    void load();
    void onSaveClicked();

private:
    void buildUi();
    // 对应Python: _set_editing_enabled
    void setEditingEnabled(bool enabled);
    // 对应Python: _on_loaded / _on_load_error
    void onLoaded(const QString &configText, const QString &envText);
    // 对应Python: _on_saved / _on_save_error
    void onSaved(const QStringList &savedLabels, const QString &error);
    // 对应Python: _mono_font
    static QFont monoFont();

    HermesBackend *m_backend = nullptr; // not owned
    QString m_profileName;
    QString m_configPath;
    QString m_envPath;
    QString m_loadedConfig;  // 保存基准值,用于判断改动
    QString m_loadedEnv;
    bool m_envExisted = false;
    bool m_busy = false;     // 对应Python: loader/saver isRunning() 判断
    QFuture<void> m_task;

    QTabWidget *m_tabs = nullptr;
    QPlainTextEdit *m_yamlEdit = nullptr;
    QPlainTextEdit *m_envEdit = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_reloadBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
};

// ---------------------------------------------------------------------------
// HermesAgentWidget
// ---------------------------------------------------------------------------

// 对应Python: agent_widget.AgentWidget
class HermesAgentWidget : public QWidget {
    Q_OBJECT
public:
    explicit HermesAgentWidget(QWidget *parent = nullptr);
    ~HermesAgentWidget() override;

    // 对应Python: AgentWidget.set_backend(后端由 Panel 持有,此处不接管所有权)
    void setBackend(HermesBackend *backend);
    HermesBackend *backend() const { return m_backend; }

    // --- pure helper (unit-testable) ---

    // 解析 `hermes profile list` 输出为 QVariantMap 列表。
    // 对应Python: ProfileWorker._parse_profile_list
    static QList<QVariantMap> parseProfileList(const QString &output);

public slots:
    // 对应Python: AgentWidget.refresh(Tab 激活时调用)
    void refresh();

signals:
    // 对应Python: AgentWidget.open_terminal_requested
    void openTerminalRequested(const QString &profileName);

private slots:
    void onCreateClicked();
    void onRenameClicked();
    void onDeleteClicked();
    void onOpenTerminalClicked();
    void onSetDefaultClicked();
    void onEditConfigClicked();
    void onStartGatewayClicked();
    void onStopGatewayClicked();
    // 对应Python: _filter_profiles
    void onFilterTextChanged();
    // 对应Python: _show_profile_detail
    void onCurrentItemChanged(QListWidgetItem *current, QListWidgetItem *previous);

private:
    void buildUi();
    // 对应Python: _render_profile_list(保持刷新前的选中项)
    void renderProfileList();
    // 对应Python: _clear_detail
    void clearDetail();
    // 对应Python: _update_action_buttons
    void updateActionButtons(const QVariantMap &profile);
    // 对应Python: _run_command(执行 CLI 并在完成后 refresh)
    void runCommand(const QStringList &args);
    // 对应Python: _on_error
    void showError(const QString &message);
    // 线程池投递 + 已完成 future 回收。对应Python: self._workers 持有引用
    void schedule(std::function<void()> job);

    QVariantMap currentProfile() const;
    QString currentProfileName() const;

    HermesBackend *m_backend = nullptr; // not owned
    QList<QVariantMap> m_profiles;
    bool m_loading = false;
    QVector<QFuture<void>> m_futures; // joined in destructor

    // 工具栏
    QPushButton *m_btnRefresh = nullptr;
    QPushButton *m_btnCreate = nullptr;
    QPushButton *m_btnRename = nullptr;
    QPushButton *m_btnDelete = nullptr;
    // 左侧
    QLineEdit *m_searchInput = nullptr;
    QListWidget *m_profileList = nullptr;
    // 右侧详情
    QLabel *m_lblName = nullptr;
    QLabel *m_lblModel = nullptr;
    QLabel *m_lblGateway = nullptr;
    QLabel *m_lblAlias = nullptr;
    QLabel *m_lblDistribution = nullptr;
    QLabel *m_lblActive = nullptr;
    // 右侧操作
    QPushButton *m_btnOpenTerminal = nullptr;
    QPushButton *m_btnSetDefault = nullptr;
    QPushButton *m_btnEditConfig = nullptr;
    QPushButton *m_btnStartGateway = nullptr;
    QPushButton *m_btnStopGateway = nullptr;
};

} // namespace cubeshell
