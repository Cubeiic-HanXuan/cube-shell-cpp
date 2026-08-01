#pragma once

// ComposeYaml.h — Docker Compose YAML 解析/序列化工具（基于 yaml-cpp）。
//
// 对应Python: core/docker/docker_compose_editor.py 中的 yaml.safe_load /
//             yaml.dump(default_flow_style=False, sort_keys=False) 用法 +
//             load_predefined_services (预定义服务清单读取)。
//
// 纯函数集合，不依赖 UI，可直接单测。
//
// dump 顺序说明：PyYAML 的 sort_keys=False 保留 dict 插入顺序，但 QVariantMap
// 按 key 字典序存储，插入顺序在进入 QVariantMap 时即已丢失。为保证输出稳定
// 且贴近 compose 惯用布局，dumpCompose 按固定优先级排序输出：
//   * 顶层: version, services, networks, volumes 优先，其余按字母序；
//   * service 映射: image, container_name, restart, command, build, ports,
//     environment, volumes, depends_on, networks 优先，其余按字母序。
// 与 PyYAML 输出的字段顺序可能不同，但语义等价（YAML 映射本身无序）。

#include <QList>
#include <QString>
#include <QVariantMap>

namespace cubeshell {

namespace ComposeYaml {

// 对应Python: yaml.safe_load(yaml_text) or {}
// 将 YAML 映射转为 QVariantMap（值为 QVariantMap/QVariantList/QString/
// qlonglong/double/bool/null）。标量类型按 YAML 1.1 语义推断（true/yes/on
// 等视为 bool），贴近 yaml.safe_load 行为。解析失败或根节点不是映射时返回
// 空 map 并写 errorOut。
QVariantMap parseCompose(const QString &yamlText, QString *errorOut = nullptr);

// 对应Python: yaml.dump(config, default_flow_style=False, sort_keys=False,
//             allow_unicode=True)
// 块格式输出；字段顺序策略见文件头注释。
QString dumpCompose(const QVariantMap &config);

// 预定义服务条目。对应Python: load_predefined_services 返回 dict 的一项
// {name: {'description': ..., 'config': ...}}。
struct ComposeServiceInfo {
    QString name;         // services 下的键名
    QString description;  // service.labels.description（缺失为空串）
    QVariantMap config;   // service 完整配置
};

// 对应Python: docker_compose_editor.py::load_predefined_services
// 读取 compose 文件 services 节点。labels 支持 map 形式与 "k=v" 字符串列表
// 两种写法；文件不存在/解析失败返回空列表并写 errorOut。
QList<ComposeServiceInfo> loadComposeServices(const QString &filePath,
                                              QString *errorOut = nullptr);

// 对应Python: load_predefined_services 中的 project_root/conf/docker-compose-full.yml
// 运行期定位内置容器清单：依次尝试可执行文件旁 conf/、安装布局
// share/cube-shell/conf/、macOS bundle Resources/conf/、开发态源码树 ../conf/。
// 返回第一个存在的绝对路径；都不存在返回空串。
QString defaultComposeFullPath();

} // namespace ComposeYaml

} // namespace cubeshell
