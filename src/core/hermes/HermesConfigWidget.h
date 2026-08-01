#pragma once

// HermesConfigWidget.h — Hermes Agent configuration tab.
// 对应Python: core/hermes/config_widget.py（ConfigWidget + ConfigLoader +
// SaveWorker + CheckWorker）
//
// Two sub tabs:
//   * 常用设置 — structured form over high-frequency fields; only changed
//     scalars go through `hermes config set` (Hermes rewrites the file safely,
//     keeping structure/comments). .env edits are merged in place.
//   * 原始配置 — raw config.yaml / .env editors as the fallback for every
//     other field; YAML gets a basic syntax check before writing.
//
// config.yaml is parsed text-level (dot-path + indentation tracking), no
// yaml-cpp dependency. All I/O runs on QtConcurrent worker threads; results
// come back to the UI thread via queued QMetaObject::invokeMethod.

#include <QFuture>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

namespace cubeshell {

class HermesBackend;
class YamlHighlighter;
class DotenvHighlighter;

class HermesConfigWidget : public QWidget {
    Q_OBJECT
public:
    explicit HermesConfigWidget(QWidget *parent = nullptr);
    ~HermesConfigWidget() override;

    // 设置后端引用（不拥有、不触发加载）对应Python: set_backend
    void setBackend(HermesBackend *backend);

    // Tab 被选中时调用，触发数据加载 对应Python: refresh
    void refresh();

    // --- pure helpers (unit-testable) ---

    // 按 'a.b.c' 点号路径做文本级 YAML 取值（缩进跟踪，浅层结构够用）。
    // 对应Python: _get_nested(yaml.safe_load 的文本级等价实现)
    static QString getNestedValue(const QString &configText,
                                  const QString &dottedPath);
    // 对应Python: ConfigLoader._parse_env
    static QMap<QString, QString> parseEnv(const QString &content);
    // 就地更新 .env：KEY= 行替换值、新键追加末尾、保留注释与空行。
    // 对应Python: _update_env_content
    static QString updateEnvContent(const QString &raw,
                                    const QMap<QString, QString> &changes);
    // 敏感值遮蔽为 '前4****后4'。对应Python: _mask_key
    static QString maskValue(const QString &value);
    // 对应Python: _is_secret
    static bool isSecretKey(const QString &key);
    // YAML 保存前的基础语法校验（引号配对、键后冒号——启发式而非完整解析）。
    // 对应Python: yaml.safe_load 校验的轻量替代
    static bool validateYamlBasic(const QString &content, QString *error);

private slots:
    void onAddEnvKey();      // 对应Python: _on_add_env_key
    void onSaveForm();       // 对应Python: _on_save_form
    void onSaveRawYaml();    // 对应Python: _on_save_raw_yaml
    void onSaveRawEnv();     // 对应Python: _on_save_raw_env
    void onCheck();          // 对应Python: _on_check

private:
    // 表单字段登记：保存时与 loaded 基准值 diff。对应Python: self._fields
    struct FormField {
        enum Kind { Line, Spin, Combo, Check };
        QString path;
        Kind kind = Line;
        QWidget *widget = nullptr;
        QString loadedText;
        int loadedInt = 0;
        bool loadedBool = false;
    };

    void buildUi();
    QWidget *buildFormTab();
    QWidget *buildRawTab();
    void addLineField(QFormLayout *form, const QString &keyPath,
                      const QString &label);
    void addSpinField(QFormLayout *form, const QString &keyPath,
                      const QString &label, int lo, int hi);
    void addComboField(QFormLayout *form, const QString &keyPath,
                       const QString &label, const QStringList &items,
                       bool editable);
    void addCheckField(QFormLayout *form, const QString &keyPath,
                       const QString &label);
    static void normalizeLabelWidths(const QList<QFormLayout *> &forms);

    void loadConfig();
    void onConfigLoaded(const QString &rawConfig, const QString &rawEnv);
    void populateForm();
    void populateEnvTable();
    void editEnvKey(const QString &key);

    // [(key_path, value_str)] 对应Python: _collect_scalar_changes
    QList<QPair<QString, QString>> collectScalarChanges() const;
    // {KEY: value} 对应Python: _collect_env_changes
    QMap<QString, QString> collectEnvChanges() const;
    void onSaveFinished(const QStringList &okKeys,
                        const QStringList &failedKeys, bool envSaved);
    void showCheckResult(const QString &output);

    void schedule(std::function<void()> job);

    HermesBackend *m_backend = nullptr; // not owned
    QList<QFuture<void>> m_futures;

    // 已加载配置的原始文本与解析结果
    QString m_rawConfig;
    QString m_rawEnv;
    QMap<QString, QString> m_envData;

    QList<FormField> m_fields;

    // 异步操作互斥标志（对应Python: worker.isRunning() 判重）
    bool m_loading = false;
    bool m_saving = false;
    bool m_checking = false;

    QTabWidget *m_tabs = nullptr;
    QTableWidget *m_providerTable = nullptr;
    QPushButton *m_checkBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPlainTextEdit *m_rawConfigEdit = nullptr;
    QPlainTextEdit *m_rawEnvEdit = nullptr;
    YamlHighlighter *m_yamlHighlighter = nullptr;     // owned by document
    DotenvHighlighter *m_envHighlighter = nullptr;    // owned by document
};

} // namespace cubeshell
