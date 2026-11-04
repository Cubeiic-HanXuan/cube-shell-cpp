#pragma once

// NginxSiteEditor.h — Nginx 站点可视化编辑器（非模态）。
//
// 见 docs/Nginx可视化管理实现方案.md §5.2。两种模式：
//   * 表单模式：分页填写（基本/前端/反向代理/HTTPS/Location/高级），底部实时
//     预览 renderSite() 生成结果；
//   * 文本模式：直接编辑配置文件原文（逃生舱，保留注释）。
// 编辑器不直接写盘：点保存时发 saveRequested(path, text)，由 NginxManagerDialog
// 统一执行「写入 → nginx -t 校验 → 刷新」，成功后回调 markSaved() 关闭本窗口。

#include <QDialog>
#include <QString>
#include <QVector>

#include "nginx/NginxConfig.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QTableWidget;

namespace cubeshell {

class CodeEditor;

class NginxSiteEditor : public QDialog {
    Q_OBJECT
public:
    explicit NginxSiteEditor(QWidget *parent = nullptr);

    // 载入站点。path 为目标文件全路径；currentText 为现有内容（新建为空）；
    // isNew=true 时允许在表单里改名（决定最终写入的文件名）。
    void setSite(const QString &path, const QString &currentText, bool isNew);

    QString targetPath() const { return m_path; }

signals:
    // path 为最终写入路径，text 为待写内容（表单生成或文本原文）。
    void saveRequested(const QString &path, const QString &text);

public slots:
    // 主对话框校验通过后调用：关闭窗口。
    void markSaved();

private slots:
    void onFieldChanged();
    void onAddLocation();
    void onEditLocation();
    void onRemoveLocation();
    void onModeChanged(int index);
    void onSaveClicked();

private:
    void buildUi();
    QWidget *buildFormPage();
    QWidget *buildBaseTab();
    QWidget *buildStaticTab();
    QWidget *buildProxyTab();
    QWidget *buildSslTab();
    QWidget *buildLocationTab();
    QWidget *buildAdvancedTab();

    void loadSpecIntoForm();
    void collectSpecFromForm();
    void rebuildLocationTable();
    void refreshPreview();
    QString renderedText();
    void editLocationAt(int row);

    QString m_path;
    QString m_fileName;   // 既有文件名（改名仅限新建）
    QString m_originalText;
    bool m_isNew = false;
    bool m_formDirty = false;
    nginx::SiteSpec m_spec;
    bool m_switching = false;
    bool m_textInitialized = false;
    bool m_parseFailed = false;
    QVector<nginx::LocationSpec> m_locations;

    QTabWidget *m_modeTabs = nullptr;
    QTabWidget *m_fieldTabs = nullptr;
    CodeEditor *m_preview = nullptr;   // 表单模式下的只读预览
    CodeEditor *m_textEditor = nullptr; // 文本模式下的可写编辑器

    // 基本
    QLineEdit *m_nameEdit = nullptr;
    QLabel *m_dirLabel = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_namesEdit = nullptr;
    QCheckBox *m_defaultChk = nullptr;
    QLineEdit *m_bodySizeEdit = nullptr;
    // 前端
    QLineEdit *m_rootEdit = nullptr;
    QLineEdit *m_indexEdit = nullptr;
    QCheckBox *m_spaChk = nullptr;
    // 反向代理
    QCheckBox *m_proxyAllChk = nullptr;
    QLineEdit *m_proxyPassEdit = nullptr;
    QCheckBox *m_wsChk = nullptr;
    // HTTPS
    QCheckBox *m_sslChk = nullptr;
    QLineEdit *m_certEdit = nullptr;
    QLineEdit *m_keyEdit = nullptr;
    QCheckBox *m_forceHttpsChk = nullptr;
    // Location 规则
    QTableWidget *m_locTable = nullptr;
    // 高级
    QPlainTextEdit *m_extraEdit = nullptr;
};

} // namespace cubeshell
