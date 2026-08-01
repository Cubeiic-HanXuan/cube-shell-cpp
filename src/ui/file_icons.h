#pragma once

#include <QIcon>
#include <QString>

namespace cubeshell {

// 文件名/目录 → 类型图标（本地与 SFTP 文件浏览器共用）。
// 对应Python: function/util.py::get_default_file_icon / get_default_folder_icon
QIcon iconForFile(const QString &name, bool isDir);

} // namespace cubeshell
