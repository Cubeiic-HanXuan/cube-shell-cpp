// credential_kind_test.cpp — unit tests for SshCredentialKind JSON round-trip
// and backward compatibility (旧 JSON 无 credentialKind 键时的推断)。

#include <QCoreApplication>
#include <QDebug>
#include <QTemporaryFile>

#include "config/DeviceConfigStore.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

static void testStringRoundTrip()
{
    for (SshCredentialKind k : {SshCredentialKind::Password,
                                SshCredentialKind::PrivateKeyFile,
                                SshCredentialKind::SshAgent,
                                SshCredentialKind::KeyboardInteractive}) {
        CHECK(sshCredentialKindFromString(sshCredentialKindToString(k)) == k);
    }
    // 未知字符串回落 fallback，不静默猜。
    CHECK(sshCredentialKindFromString(QStringLiteral("bogus"),
                                      SshCredentialKind::SshAgent)
          == SshCredentialKind::SshAgent);
    // "agent" 是方案早期草稿里的写法，兼容读入。
    CHECK(sshCredentialKindFromString(QStringLiteral("agent"))
          == SshCredentialKind::SshAgent);
}

static void testJsonRoundTrip()
{
    QTemporaryFile tmp;
    CHECK(tmp.open());
    tmp.close();

    DeviceConfigStore store;
    DeviceEntry e;
    e.name = QStringLiteral("agentbox");
    e.username = QStringLiteral("ops");
    e.host = QStringLiteral("10.0.0.1");
    e.port = 22;
    e.credentialKind = SshCredentialKind::SshAgent;
    e.agentForwarding = false;
    store.addDevice(e);

    QString err;
    CHECK(store.saveJson(tmp.fileName(), &err));

    DeviceConfigStore store2;
    CHECK(store2.loadJson(tmp.fileName(), &err));
    const DeviceEntry *back = store2.find(QStringLiteral("agentbox"));
    CHECK(back != nullptr);
    if (back) {
        CHECK(back->credentialKind == SshCredentialKind::SshAgent);
        CHECK(back->agentForwarding == false);
        CHECK(back->usesAgent());
        CHECK(!back->usesKey());
    }
}

static void testLegacyJsonInference()
{
    // 旧格式：没有 credentialKind/agentForwarding 键。
    // keyType/keyFile 齐 → PrivateKeyFile；都没有 → Password。agentForwarding
    // 缺键回落 true（见 loadJson 注释）。
    QTemporaryFile tmp;
    CHECK(tmp.open());
    tmp.write(R"json([
        {"name":"keybox","username":"a","host":"h1","port":22,
         "keyType":"Ed25519Key","keyFile":"/tmp/id_ed25519"},
        {"name":"pwbox","username":"b","host":"h2","port":22}
    ])json");
    tmp.flush();

    DeviceConfigStore store;
    QString err;
    CHECK(store.loadJson(tmp.fileName(), &err));

    const DeviceEntry *keybox = store.find(QStringLiteral("keybox"));
    CHECK(keybox != nullptr);
    if (keybox) {
        CHECK(keybox->credentialKind == SshCredentialKind::PrivateKeyFile);
        CHECK(keybox->agentForwarding == true);
    }
    const DeviceEntry *pwbox = store.find(QStringLiteral("pwbox"));
    CHECK(pwbox != nullptr);
    if (pwbox)
        CHECK(pwbox->credentialKind == SshCredentialKind::Password);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    testStringRoundTrip();
    testJsonRoundTrip();
    testLegacyJsonInference();

    if (failures) {
        qWarning() << "credential_kind_test:" << failures << "failure(s)";
        return 1;
    }
    qDebug() << "credential_kind_test: all passed";
    return 0;
}
