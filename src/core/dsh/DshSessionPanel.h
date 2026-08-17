#pragma once

// DshSessionPanel.h — DeepSeek Harness 会话管理 Tab。
//
// dsh 的会话落在 <DSH_HOME>/sessions/<工作区>/session-<uuid>/session.jsonl.zstd。
// 正文是 zstd 压缩的 JSONL，本面板不解压；但标题/工作目录/轮次不在正文里，而在
// <DSH_HOME>/storages/ 的元信息缓存中，所以这几列是精确值（见 DshManager 的
// listSessions 注释）。
//
// 可做的操作：
//   * 在终端恢复：发 openCliRequested("dsh --profile tui --resume <id>", cwd)，
//     由主窗口在 cwd 下新开本机终端执行。
//     两个坑（都实测过）：
//       - <id> 必须带 "session-" 前缀（即目录名原样），裸 uuid 一律 not found；
//       - cwd 与能否找到会话无关（--resume 全局按 id 查），传它只是为了让
//         agent 的文件操作落在会话原本的项目目录上。
//   * 删除会话：直接删掉该 session-<uuid> 目录。

#include <QWidget>

#include "dsh/DshManager.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace cubeshell {

class DshSessionPanel : public QWidget {
    Q_OBJECT
public:
    explicit DshSessionPanel(DshManager *manager, QWidget *parent = nullptr);

signals:
    // 请求在 workingDir 下的本机终端运行命令（恢复会话）。由 DshPanel 转发到主窗口。
    void openCliRequested(const QString &command, const QString &workingDir);

public slots:
    void refresh();

private slots:
    void onSelectionChanged();
    void onResumeClicked();
    void onDeleteClicked();

private:
    void buildUi();
    // 选中行对应的会话；无选中返回 nullptr。
    const DshManager::SessionInfo *selectedSession() const;
    // 标题列文本：无标题时按 轮次 给出「空会话 / 未命名 / 无元信息」。
    static QString displayTitle(const DshManager::SessionInfo &s);
    // 完整 id → 展示用短名（去 session- 前缀取前 8 位）。仅用于显示。
    static QString shortId(const QString &id);
    static QString prettySize(qint64 bytes);

    DshManager *m_manager = nullptr; // not owned
    QList<DshManager::SessionInfo> m_sessions;

    QTableWidget *m_table = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QComboBox *m_profileCombo = nullptr; // 用哪个 profile 恢复（默认 tui）
    QPushButton *m_resumeBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
};

} // namespace cubeshell
