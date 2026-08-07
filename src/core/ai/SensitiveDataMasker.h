#pragma once

// SensitiveDataMasker.h — 发往大模型前的敏感信息脱敏（兜底防线）。
//
// 背景：AI 助手的设计原则是「模型只生成命令与总结，永远不应知道服务器的
//       IP、登录密码、私钥等凭据」。服务器画像与系统提示词本就不含这些信息，
//       但命令执行的 stdout/stderr 会原样回填给大模型用于诊断——若某条命令
//       恰好打印了敏感内容（如 cat 了含密码的配置、env 暴露了密钥、私钥文件），
//       这些数据会随请求发往大模型厂商。本模块在回填入口统一脱敏，堵住该通道。
//
// 设计：纯函数、无状态、可独立单测；只影响发往 AI 的内容，不影响终端真实显示。

#include <QString>

namespace cubeshell {

class SensitiveDataMasker {
public:
    // 对一段即将发给大模型的文本做脱敏，返回打码后的副本。
    // 屏蔽三类内容：
    //   1. 私钥 / 证书 PEM 块（-----BEGIN ... PRIVATE KEY----- 等整段）
    //   2. 敏感键值赋值（password=…、secret、token、api_key、passwd 等）
    //   3. IPv4 / IPv6 地址（含画像可能带入的地址，严格遵循「模型不知 IP」原则）
    static QString mask(const QString &text);
};

} // namespace cubeshell
