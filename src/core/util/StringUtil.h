#pragma once

// StringUtil.h — 字符串/格式化/路径通用工具。
// 对应Python: function/util.py（字符串/路径/格式化工具部分）
//
// 注意：util.py 里的 parse_host_port / format_host_port 已经在
// core/config/DeviceConfigStore.h 中实现为 cubeshell::parseHostPort /
// cubeshell::formatHostPort（另一位同事负责的模块），此处不重复实现，
// 需要时直接 #include "config/DeviceConfigStore.h" 复用。
//
// util.py 中与 GUI 图标相关的 get_default_folder_icon / get_default_file_icon
// 依赖 QIcon 资源系统，属于 UI 层职责，不在本工具层移植范围内。

#include <QString>

namespace cubeshell {
namespace StringUtil {

// 对应Python: function/util.py::MAX_BYTES_SIZE（小于展示字节，大于或等于展示KB）
constexpr qint64 MaxBytesSize = 1024;
// 对应Python: function/util.py::MAX_KB_SIZE（小于展示KB，大于或等于展示MB）
constexpr qint64 MaxKbSize = 1024LL * 1024;
// 对应Python: function/util.py::MAX_MB_SIZE（小于展示MB，大于或等于展示GB）
constexpr qint64 MaxMbSize = 1024LL * 1024 * 1024;

// 对应Python: function/util.py::APP_NAME
QString appName();

// 对应Python: function/util.py::BANNER
QString banner();

// 根据文件大小返回适当的单位展示文件大小
// 对应Python: function/util.py::format_file_size
QString formatFileSize(qint64 sizeInBytes);

// 速度格式化（B/s、KB/s、MB/s）
// 对应Python: function/util.py::format_speed
QString formatSpeed(double bytesPerSecond);

// 检测文件名是否为二进制/归档等"不可预览"类型后缀
// 对应Python: function/util.py::has_valid_suffix
bool hasValidSuffix(const QString &filename);

// 过滤只包含波浪号(或空白)的行
// 对应Python: function/util.py::remove_special_lines
QString removeSpecialLines(const QString &text);

// 判断给定地址是否为 IPv6 地址（可带方括号，不含端口）
// 对应Python: function/util.py::is_ipv6_address
bool isIpv6Address(const QString &address);

// 符号权限转八进制权限，如 "rwxr-xr--" -> 754
// 对应Python: function/util.py::symbolic_to_octal
int symbolicToOctal(const QString &symbolic);

// 路径拼接（os.path.join 语义：name 为绝对路径时直接返回 name）
// 对应Python: function/util.py::copy_config_to_conf 中 os.path.join 用法
QString joinPath(const QString &dir, const QString &name);

// 取路径最后一段文件名
// 对应Python: function/util.py::copy_config_to_conf 中 os.path.basename 用法
QString baseName(const QString &path);

} // namespace StringUtil
} // namespace cubeshell
