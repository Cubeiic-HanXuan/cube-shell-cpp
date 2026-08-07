// SensitiveDataMasker unit test: 私钥/证书 PEM 块、敏感键值赋值、IPv4/IPv6
// 地址三类脱敏规则的覆盖与误伤控制。纯逻辑 — 无网络、无 SSH。
//
// 对应模块: src/core/ai/SensitiveDataMasker.{h,cpp}

#include <QDebug>
#include <QString>

#include "ai/SensitiveDataMasker.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 脱敏后原文应被改变，且不再含敏感片段 secret。
static void expectMasked(const QString &in, const QString &secret)
{
    const QString out = SensitiveDataMasker::mask(in);
    if (out == in) {
        qWarning().noquote() << "FAIL(未脱敏): " << in;
        ++failures;
    }
    if (!secret.isEmpty() && out.contains(secret)) {
        qWarning().noquote() << "FAIL(仍含敏感值" << secret << "): " << out;
        ++failures;
    }
}

// 正常内容不应被改动。
static void expectUnchanged(const QString &in)
{
    const QString out = SensitiveDataMasker::mask(in);
    if (out != in) {
        qWarning().noquote() << "FAIL(误伤): in=" << in << " out=" << out;
        ++failures;
    }
}

static void testSecretAssignment()
{
    expectMasked(QStringLiteral("DB password=S3cret123 done"),
                 QStringLiteral("S3cret123"));
    expectMasked(QStringLiteral("passwd: hunter2"),
                 QStringLiteral("hunter2"));
    expectMasked(QStringLiteral("{\"secret\": \"abc123\"}"),
                 QStringLiteral("abc123"));
    expectMasked(QStringLiteral("AWS_SECRET_ACCESS_KEY=AKIAIOSFODNN7EXAMPLE"),
                 QStringLiteral("AKIAIOSFODNN7EXAMPLE"));
    expectMasked(QStringLiteral("api_token = tok_999"),
                 QStringLiteral("tok_999"));
    expectMasked(QStringLiteral("client-secret: xyz"),
                 QStringLiteral("xyz"));
    // 保留 key 名，只打码值，便于模型理解上下文
    const QString out = SensitiveDataMasker::mask(
        QStringLiteral("password=foo"));
    CHECK(out.contains(QStringLiteral("password=")));
    CHECK(!out.contains(QStringLiteral("foo")));
}

static void testPemBlock()
{
    const QString pem = QStringLiteral(
        "-----BEGIN OPENSSH PRIVATE KEY-----\n"
        "b3BlbnNzaC1rZXktdjEAAAAABG5vbmU\n"
        "AAAABHNzaDoAAAAEAAAAA\n"
        "-----END OPENSSH PRIVATE KEY-----");
    const QString out = SensitiveDataMasker::mask(pem);
    CHECK(!out.contains(QStringLiteral("b3BlbnNzaC1rZXktdjEAAAAABG5vbmU")));
    CHECK(out.contains(QStringLiteral("已脱敏")));
}

static void testIpAddresses()
{
    expectMasked(QStringLiteral("connect 192.168.1.100 failed"),
                 QStringLiteral("192.168.1.100"));
    expectMasked(QStringLiteral("dns 8.8.8.8"),
                 QStringLiteral("8.8.8.8"));
    expectMasked(QStringLiteral("addr fe80::1ff:fe23:4567:890a/64"),
                 QStringLiteral("fe80::1ff:fe23:4567:890a"));
    expectMasked(QStringLiteral("listen 10.0.0.1:22"),
                 QStringLiteral("10.0.0.1"));
}

static void testNoFalsePositive()
{
    expectUnchanged(QStringLiteral("total 100, used 50 (50%)"));
    expectUnchanged(QStringLiteral("Linux server 5.15.0-91-generic x86_64"));
    expectUnchanged(QStringLiteral("listening ports: 22, 80, 443"));
    expectUnchanged(QStringLiteral("nginx version 1.2.3"));   // 三段版本号非 IPv4
    expectUnchanged(QStringLiteral("Mem: 16G total, 8G used"));
    expectUnchanged(QString());
}

int main()
{
    testSecretAssignment();
    testPemBlock();
    testIpAddresses();
    testNoFalsePositive();

    if (failures == 0) {
        qInfo() << "ALL PASS";
        return 0;
    }
    qWarning() << failures << "FAILURES";
    return 1;
}
