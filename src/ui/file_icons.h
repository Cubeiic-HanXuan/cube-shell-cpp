#pragma once

#include <QIcon>
#include <QString>
#include <QtGlobal>

namespace cubeshell {

// 文件名/目录 → 类型图标（本地与 SFTP 文件浏览器共用）。
// isSymlink/mode 可选：符号链接与可执行位优先于扩展名匹配（mode 为 st_mode）。
// 对应Python: function/util.py::get_default_file_icon / get_default_folder_icon
QIcon iconForFile(const QString &name, bool isDir, bool isSymlink = false, quint32 mode = 0);

} // namespace cubeshell
