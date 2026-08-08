// transfer_pool_test.cpp — SftpTransferPool / 并行分片规划的纯逻辑测试。
//
// 不需要网络：连接池在没有可用主连接时必须干净地降级（返回无效租约而不是
// 崩溃或阻塞），这正是这里要钉住的行为。真实多连接并行的吞吐验证需要活的
// sshd，在 uploader_test / sftp_integration_test 里做。
//
// 覆盖点：
//  1) 无主连接 -> lease() 返回无效租约并给出错误文案，canParallelize() == false
//  2) maxConnections 的边界钳制
//  3) 租约 move 语义 / 重复 release 不重复归还
//  4) 分片规划（planChunks）与并行水位推进的语义：乱序完成时水位只跟着
//     连续前缀走 —— 断点续传元数据是标量，这是与 Python 侧互认的前提

#include <QCoreApplication>
#include <QDebug>

#include "ssh/SftpTransferPool.h"
#include "ssh/SftpUploaderCore.h"
#include "ssh/SshClient.h"

using namespace cubeshell;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { qWarning() << "FAIL:" << #cond << "line" << __LINE__; ++failures; } } while (0)

// 未连接的主连接：lease 必须降级为无效租约，不能崩、不能卡。
static void testDegradesWithoutConnection()
{
    SftpTransferPool pool(nullptr);
    QString err;
    auto lease = pool.lease(&err);
    CHECK(!lease.isValid());
    CHECK(!err.isEmpty());
    CHECK(!pool.canParallelize());
    CHECK(pool.activeConnections() == 1); // 至少报 1（降级串行）

    // 未认证的 SshClient 同样不可克隆（isAuthReplayable() == false）。
    SshClient bare;
    pool.setPrimary(&bare);
    CHECK(!bare.isAuthReplayable());
    CHECK(!pool.canParallelize());
    QString err2;
    auto lease2 = pool.lease(&err2);
    CHECK(!lease2.isValid());
}

static void testMaxConnectionsClamp()
{
    SftpTransferPool pool(nullptr);
    CHECK(pool.maxConnections() == SftpTransferPool::kDefaultMaxConnections);
    pool.setMaxConnections(1);
    CHECK(pool.maxConnections() == 1);
    pool.setMaxConnections(0);   // 下界钳到 1
    CHECK(pool.maxConnections() == 1);
    pool.setMaxConnections(-5);
    CHECK(pool.maxConnections() == 1);
    pool.setMaxConnections(999); // 上界钳到 16
    CHECK(pool.maxConnections() == 16);
    pool.setMaxConnections(4);
    CHECK(pool.maxConnections() == 4);
}

// 租约的 move / 重复 release：不能重复归还槽位。
static void testLeaseMoveSemantics()
{
    SftpTransferPool pool(nullptr);
    auto a = pool.lease();
    CHECK(!a.isValid());
    auto b = std::move(a);
    CHECK(!a.isValid()); // 被移走
    CHECK(!b.isValid());
    b.release();
    b.release(); // 幂等，不应崩
    CHECK(!b.isValid());
}

// 分片规划：并行流从同一个 chunks 列表按游标领取，规划本身必须无缝无重叠。
static void testChunkPlanCoversExactly()
{
    const qint64 chunk = SftpUploaderCore::kChunkSize;
    const qint64 total = chunk * 3 + 12345;
    const auto chunks = SftpUploaderCore::planChunks(total, 0);

    qint64 sum = 0;
    qint64 expectedOffset = 0;
    for (const auto &c : chunks) {
        CHECK(c.first == expectedOffset); // 无空洞
        CHECK(c.second > 0);
        CHECK(c.second <= chunk);
        expectedOffset += c.second;
        sum += c.second;
    }
    CHECK(sum == total);          // 无重叠、无遗漏
    CHECK(chunks.size() == 4);

    // 断点续传起点：只规划剩余部分。
    const auto resumed = SftpUploaderCore::planChunks(total, chunk * 2);
    qint64 resumedSum = 0;
    for (const auto &c : resumed)
        resumedSum += c.second;
    CHECK(resumedSum == total - chunk * 2);
    CHECK(resumed.first().first == chunk * 2);
}

// 并行水位语义：乱序完成时，写进元数据的 uploaded_size 只能跟着**连续前缀**
// 走。这是与 Python 侧标量字段互认的前提 —— 水位之后即使已传完也要当作没传，
// 中断后重传一遍（保守但绝不会产生空洞文件）。
//
// 这里复刻 FileTransferState::markDone 的前缀推进算法（该结构体是私有实现，
// 测试直接验证算法本身）。
static void testWatermarkFollowsContiguousPrefix()
{
    auto watermarkAfter = [](const QVector<qint64> &sizes, const QVector<int> &completed) {
        QVector<bool> done(sizes.size(), false);
        for (int idx : completed) {
            if (idx >= 0 && idx < done.size())
                done[idx] = true;
        }
        int i = 0;
        while (i < done.size() && done[i])
            ++i;
        qint64 watermark = 0;
        for (int k = 0; k < i; ++k)
            watermark += sizes[k];
        return watermark;
    };

    const QVector<qint64> sizes{100, 200, 300, 400};

    // 乱序完成 1、2、3（缺 0）-> 水位必须是 0，一个字节都不能算。
    CHECK(watermarkAfter(sizes, {1, 2, 3}) == 0);
    // 完成 0 -> 水位 100。
    CHECK(watermarkAfter(sizes, {0}) == 100);
    // 完成 0、1（连续）-> 300。
    CHECK(watermarkAfter(sizes, {0, 1}) == 300);
    // 完成 0、1、3（3 是乱序超前的）-> 仍只算前缀 0、1 = 300。
    CHECK(watermarkAfter(sizes, {0, 1, 3}) == 300);
    // 补上 2 -> 前缀连成 0..3，全部 1000。
    CHECK(watermarkAfter(sizes, {0, 1, 2, 3}) == 1000);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testDegradesWithoutConnection();
    testMaxConnectionsClamp();
    testLeaseMoveSemantics();
    testChunkPlanCoversExactly();
    testWatermarkFollowsContiguousPrefix();

    if (failures == 0)
        qInfo() << "transfer_pool_test: all checks passed";
    else
        qWarning() << "transfer_pool_test:" << failures << "check(s) failed";
    return failures == 0 ? 0 : 1;
}
