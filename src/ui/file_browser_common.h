#pragma once

// file_browser_common.h — 本地/远程文件树面板共用的条目角色、筛选与排序逻辑。
//
// 两棵树（LocalFileBrowserWidget / SftpBrowserWidget）同为五列平铺列表
// （文件名/文件大小/修改日期/权限/所有者），首行可能是 ".." 返回上级条目。
// 排序与筛选都是纯内存操作：目录数据本就全量在手，不需要任何额外网络往返。
//
// 可测性约定与本工程一致：比较/匹配抽成纯函数（fileMatchesFilter /
// fileTreeSortLess），QTreeWidget 只是它们的一层薄壳（sortFileTree /
// applyFileFilter），真值表钉在 tests/file_browser_test.cpp。

#include <Qt>
#include <QtGlobal>
#include <QString>

class QIcon;
class QPalette;
class QTreeWidget;

namespace cubeshell {

// ---- 条目角色布局（两棵树共用同一套，便于共用函数直接读） -----------------
constexpr int kPathRole = Qt::UserRole;        // 完整路径（远端/本地）
constexpr int kIsDirRole = Qt::UserRole + 1;   // bool
constexpr int kModeRole = Qt::UserRole + 2;    // 权限位 quint32（仅 SFTP 树用）
constexpr int kIsUpRole = Qt::UserRole + 3;    // ".." 返回上级条目
constexpr int kSymlinkTargetRole = Qt::UserRole + 4; // 符号链接目标（仅 SFTP 树用）
constexpr int kSizeRole = Qt::UserRole + 5;    // 裸字节数 qint64（排序用，显示文本是格式化串）
constexpr int kMtimeRole = Qt::UserRole + 6;   // 裸修改时间 qint64 秒级 epoch（同上）

// ---- 筛选 ---------------------------------------------------------------
// 文件名子串匹配（忽略大小写）；空 needle 恒真。抽成纯函数便于单测。
bool fileMatchesFilter(const QString &name, const QString &needle);

// 对整棵树的顶层条目应用筛选：不匹配项隐藏；".." 条目在有筛选词时隐藏
// （它代表的是上级目录，不参与"当前目录里找东西"）。返回可见的普通条目数。
int applyFileFilter(QTreeWidget *tree, const QString &needle);

// ---- 排序 ---------------------------------------------------------------
// 排序所需的条目裸值快照（显示文本已格式化，不能直接比）。
struct FileSortKey {
    QString name;       // 文件名（不含路径）
    bool isDir = false;
    qint64 size = 0;    // 字节
    qint64 mtime = 0;   // 秒级 epoch
    QString perm;       // 权限串（"drwxr-xr-x"）
    QString owner;      // 所有者/组
};

// 树条目排序比较（less 语义，可直接喂 std::sort）：
//  - column: 0 名称（忽略前导点、忽略大小写，沿用原 ls 风格；此列目录恒在
//    文件前，升降序都保持）/ 1 大小 / 2 修改日期 / 3 权限 / 4 所有者；
//  - 非名称列全局混排（目录不提前），保证列值整列单调——目录分组会在
//    目录/文件分界处造成"顺序倒退"，用户看到的就是"排序是乱的"；
//  - 列值平局时以名称排序裁决，保证结果稳定；
//  - 不处理 ".." 条目：由 sortFileTree 先摘出、恒置顶（升降序都不动）。
bool fileTreeSortLess(int column, Qt::SortOrder order,
                      const FileSortKey &a, const FileSortKey &b);

// 对整棵树的顶层条目按 column/order 重排：".." 条目恒置顶，其余按
// fileTreeSortLess 排序后原序插回（不销毁 item，选中态/隐藏态都保留）。
void sortFileTree(QTreeWidget *tree, int column, Qt::SortOrder order);

// ---- 筛选按钮图标 ---------------------------------------------------------
// 手绘放大镜（QPainter），随调色板着色，深浅色主题下都可见；
// 资源包里本没有搜索图标，免得为一个 16px 图标新增二进制资源。
QIcon fileFilterIcon(const QPalette &pal);

} // namespace cubeshell
