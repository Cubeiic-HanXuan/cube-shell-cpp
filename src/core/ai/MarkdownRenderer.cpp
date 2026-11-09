/**
 * @file MarkdownRenderer.cpp
 * @brief 实现 Markdown 分块缓存、普通文本转义和 Qt 富文本正文提取。
 *
 * 主处理链路为：完整 Markdown → splitBlocks() 划分原文块 → 查询实例缓存 →
 * 对未命中的块调用 renderBlock() → 按输入顺序拼接各块 HTML。分块只决定缓存
 * 的粒度，不承担 Markdown 语法解析；具体标签、表格和代码格式由 Qt 生成。
 *
 * 转换文档在每次 renderBlock() 调用中临时创建，缓存只保存导出的字符串，
 * 不保存 QTextDocument 或控件。escapeHtml() 则是独立的普通文本转义入口，
 * 不属于上述 Markdown 渲染链路，也不接触缓存。
 */

#include "MarkdownRenderer.h"

#include <QGuiApplication>
#include <QTextDocument>

namespace cubeshell {
namespace {
/**
 * @brief 仅用于 Markdown 格式转换、禁止加载外部资源的临时文档。
 *
 * QTextDocument 默认具有资源加载入口，Markdown 图片等内容可能触发该入口。
 * 此处无论资源类型和 URL 是什么，都返回无效 QVariant，不调用基类加载逻辑，
 * 避免转换 AI 输出时由这个临时文档去读取外部资源。
 *
 * 注意：拒绝加载并不等于从 HTML 中删除图片地址或链接；最终展示控件仍需要
 * 自己控制资源加载与链接访问。匿名命名空间将此实现细节限制在当前编译单元。
 */
class MarkdownDocument : public QTextDocument {
protected:
    /**
     * @brief 拒绝通过文档的资源加载入口提供外部资源。
     * @return 无效 QVariant，表示本次请求没有取得可用的资源数据。
     *
     * Qt 在需要图片等资源时可能调用此虚函数。省略两个参数的名字，明确表示
     * 不按资源类型或地址区别放行；也不委托给 QTextDocument 的默认实现，
     * 避免重新进入基类的加载路径。这里不会移除原文中的图片或链接语法。
     */
    QVariant loadResource(int, const QUrl &) override { return {}; }
};
}

/**
 * @brief 通过成员初始化列表直接设置 QCache 的总成本上限。
 * @param cacheBlocks 可缓存的块数上限；插入时每块使用默认成本 1。
 *
 * 这里只配置容量，不预先渲染文本，也不创建 Qt 文档。容量为 0 时，后续成本
 * 为 1 的条目无法留在缓存中，但 render() 仍会正常计算并返回当次的 HTML。
 */
MarkdownRenderer::MarkdownRenderer(int cacheBlocks) : m_cache(cacheBlocks) {}

/**
 * @brief 清空缓存并交由 QCache 释放其拥有的 QString 对象。
 *
 * 不改变构造时设置的容量，也不会使先前已返回给调用者的 QString 失效。
 * 下一次 render() 仍走相同链路，只是此前缓存的块需要重新解析。
 */
void MarkdownRenderer::clearCache() { m_cache.clear(); }

/**
 * @brief 依次复用或生成各块 HTML，最终返回拼接后的正文。
 * @param markdown 本次需要展示的完整 Markdown，不是上次调用之后新增的字符。
 * @return 与本次输入对应的全部正文片段；空输入不产生块，返回空字符串。
 *
 * 每次调用仍需扫描原文、查找缓存并拼接完整结果；缓存节省的是稳定文本块的
 * QTextDocument 解析和 HTML 导出开销，不会消除整个输入长度带来的处理成本。
 *
 * 顺序由 splitBlocks() 返回的列表决定，而非缓存的内部排列。同一个原文块
 * 在输入中出现多次时可共用一份缓存值，但每次出现都会向结果追加对应 HTML，
 * 不会因缓存键相同而去重。传入空文本也不会自动清除此前留下的缓存条目。
 */
QString MarkdownRenderer::render(const QString &markdown)
{
    QString html;
    for (const auto &block : splitBlocks(markdown)) {
        // 按原始块内容查找；命中后立即复制 HTML，不持有可能被后续插入淘汰的指针。
        if (const auto *cached = m_cache.object(block)) html += *cached;
        else {
            // 新内容或已被淘汰的内容才需要解析。流式尾块一旦变化，就会走此分支；
            // 旧版本不会原地更新，而是保留到 QCache 按 LRU 策略将其淘汰。
            const QString rendered = renderBlock(block);

            // 将堆对象所有权移交 QCache。即使因容量不足而插入失败，QCache 也会
            // 删除传入的对象；下面仍使用局部 rendered，保证本次结果不依赖缓存成功。
            m_cache.insert(block, new QString(rendered));
            html += rendered;
        }
    }
    return html;
}

/**
 * @brief 将普通文本转义为 HTML 安全文本，保留原有换行。
 * @param text 未转义的普通文本；不会修改调用者传入的字符串。
 * @return 特殊字符已转换成实体的字符串；空输入返回空字符串。
 *
 * 这是供调用者单独使用的辅助函数；renderBlock() 直接交给 Qt 解析 Markdown，
 * 不会先调用本函数，以免把需要解析的原始内容提前转成 HTML 实体。
 * 例如 <x>& 会变成 &lt;x&gt;&amp;，已有的 &lt; 则会变成 &amp;lt;。
 * 本函数不把换行变成 <br>，因此保留源字符串换行不保证 HTML 中也显示为换行。
 */
QString MarkdownRenderer::escapeHtml(const QString &text)
{
    // Qt 负责将 &、<、> 和双引号转义；先做这一步，避免后面新增实体中的 & 被再转义。
    QString escaped = text.toHtmlEscaped();
    // 补齐单引号转义，便于调用者将结果放入使用单引号包围的 HTML 属性值中。
    // 字符转义并不验证 URL 协议等语义，不能代替对应属性的内容校验。
    escaped.replace(QLatin1Char('\''), QLatin1String("&#39;"));
    return escaped;
}

/**
 * @brief 用一次逐行扫描确定缓存边界，不改写原始字符。
 * @param markdown 待分块的完整原文，可以没有末尾换行，也可以以未闭合围栏结束。
 * @return 保序的原文块列表，满足 blocks.join("") == markdown。
 *
 * 核心状态只有“是否处于三反引号围栏内”：围栏内的空行保留在同一块中，
 * 围栏外的空行则结束当前块。块边界不是完整 Markdown 语法树的节点边界，
 * 列表、引用式链接等跨块语法仍受独立解析的限制。
 *
 * 每轮结束时，已提交的 blocks 与待提交的 current 拼接起来，恰好覆盖
 * markdown 中 [0, pos) 的部分；没有字符被 trim、补写或重复分配给两个块。
 * “提交”统一指将 current 加入 blocks 后清空 current，不改变原文内容。
 *
 * 状态转换规则（按本函数的简化围栏识别方式）：
 * - 围栏外遇到围栏行：先提交前文，再将该行放入新块，进入围栏内状态。
 * - 围栏内遇到围栏行：将该行归入代码块并提交，返回围栏外状态。
 * - 围栏外遇到空白行：将该行归入当前块并提交，仍处于围栏外状态。
 * - 其他行：只累加到 current；扫描结束后统一提交尚未结束的尾块。
 */
QStringList MarkdownRenderer::splitBlocks(const QString &markdown)
{
    QStringList blocks;
    QString current;        // 尚未提交的原文片段，始终保留已有空白和换行。
    bool inFence = false;   // true 表示已遇到起始围栏，正在收集代码块内容。
    int pos = 0;            // 下一行的起始位置，使用 QString 自身的字符索引。
    while (pos < markdown.size()) {
        // 按换行定位整行；有换行时把 '\n' 一并带入，末行无换行时取到字符串末尾。
        // 不使用 trimmed() 的结果作为块内容，确保块拼接后能逐字符还原输入。
        // CRLF 中的 '\r' 随行原样保留；只有 '\r' 而没有 '\n' 时不会在此处分行。
        // pos 始终前进到下一行开头或输入末尾，因此最后一行没有换行也能正常退出。
        const int nl = markdown.indexOf(QLatin1Char('\n'), pos);
        const QString line = nl < 0 ? markdown.mid(pos) : markdown.mid(pos, nl - pos + 1);
        pos = nl < 0 ? markdown.size() : nl + 1;

        // 仅用去空白后的行判断边界：允许围栏行带缩进及语言标记。
        // 这里只认三反引号前缀，不校验开闭长度、结束行后缀，也不处理波浪号围栏；
        // 真正的 Markdown 语法解析在 renderBlock() 中由 Qt 完成。
        const bool fence = line.trimmed().startsWith(QLatin1String("```"));
        if (fence && !inFence && !current.isEmpty()) {
            // 开始新的代码围栏前，先提交紧邻的普通文本，即使二者之间没有空行。
            // 这样代码尾部持续增长时，不必连带重新解析前面的稳定段落。
            blocks.append(current);
            current.clear();
        }

        // 先收集当前行，再处理结束条件，保证空行和闭合围栏归属于刚结束的块。
        current += line;
        if (fence) {
            // 已在围栏内则将本行视为闭合行并提交整块；否则只切换到围栏内状态。
            if (inFence) { blocks.append(current); current.clear(); }
            inFence = !inFence;
        } else if (!inFence && line.trimmed().isEmpty()) {
            // 围栏外的空行（包括只有空白的行）终止当前块；围栏内空行不触发分割。
            // 连续空行会形成独立的空白块，不在分块阶段合并或丢弃它们。
            blocks.append(current);
            current.clear();
        }
    }

    // 提交末尾尚未结束的段落或围栏。流式输入不要求代码块已经闭合；下一次
    // 追加后，其内容会产生新的缓存键。空输入和刚好已提交完的输入不追加空块。
    if (!current.isEmpty()) blocks.append(current);
    return blocks;
}

/**
 * @brief 使用 Qt Markdown 解析器完成一次转换，并剥离完整 HTML 文档外壳。
 * @param text 本次独立解析的 Markdown 原文；不依赖其他块或之前调用的内容。
 * @return 正文 HTML；空输入或正文截取范围无效时返回空字符串。
 *
 * 每次创建独立文档，不共享上一块的解析状态。代码围栏、行内代码中的 Markdown
 * 标记由解析器按字面内容处理，避免手工多轮替换误把代码中的星号等转成富文本。
 * Qt 同时负责处理流式输入中尚未闭合的代码围栏和 GitHub 风格的表格语法。
 *
 * 文档对象采用栈上生命周期：字体配置、Markdown 解析及 HTML 导出均在本次
 * 调用内完成，返回后自动销毁。返回的是独立持有内容的 QString，不依赖该
 * 文档继续存活；调用 renderBlock() 本身不会填充任何渲染器实例的缓存。
 */
QString MarkdownRenderer::renderBlock(const QString &text)
{
    // 避免为空输入创建文档，也避免返回 Qt 为空文档生成的占位段落。
    if (text.isEmpty()) return {};
    MarkdownDocument document;

    // 只有当前应用确实属于 QGuiApplication（或其子类）时，才显式读取 GUI 字体。
    // 使生成的富文本采用应用默认字体；这个判断仅保护字体访问，不是线程同步。
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        document.setDefaultFont(QGuiApplication::font());

    // GitHub 方言提供表格等扩展；MarkdownNoHTML 禁止将输入中的原始 HTML
    // 作为富文本标记解析。它与上面的资源加载拦截分别约束内容解析和资源读取，
    // 并不意味着输出中的链接已经过协议过滤或获得了自动打开的许可。
    document.setMarkdown(text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)
                                   | QTextDocument::MarkdownNoHTML);

    // toHtml() 返回带 head、body 等结构的完整文档；多个完整文档不能直接作为
    // 一段正文拼接，所以这里只截取 body 的内部内容，保留内部标签自身的格式。
    // 同时会舍弃 head 内的样式及 body 标签自身的属性，展示端需提供默认样式。
    // 此处只处理 Qt 刚刚生成的固定结构，不是用于解析任意外部 HTML 的通用算法。
    const QString html = document.toHtml();
    const int body = html.indexOf(QLatin1String("<body"));
    // 从 body 起始标签开始找 '>'，兼容该标签携带字体、颜色等属性的情况。
    const int start = html.indexOf(QLatin1Char('>'), body) + 1;
    const int end = html.lastIndexOf(QLatin1String("</body>"));
    // '>' 未找到时 indexOf() 返回 -1，加 1 后为 0；同时检查结束位置，避免无效截取。
    return start > 0 && end >= start ? html.mid(start, end - start) : QString();
}

} // namespace cubeshell
